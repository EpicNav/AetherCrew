// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Curves/CurveFloat.h"
#include "Data/SandboxMetaData.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioSandbox.h"
#include "HyperAIStudioWidgetShot.h"
#include "SHyperAIStudioActivitySidebar.h"
#include "SHyperAIStudioAutoApprovalDialog.h"
#include "IFileSandboxCoreModule.h"
#include "ISandboxInstance.h"
#include "ISandboxManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HyperAIStudioNativeReadToolset.h"
#include "ObjectTools.h"
#include "Tests/HyperAIStudioLogCapture.h"
#include "Types/Manager/DeleteSandboxByDirectoryArgs.h"
#include "Types/Manager/DeleteSandboxResult.h"
#include "Types/Manager/LeaveSandboxResult.h"
#include "Types/Manager/NewSandboxArgs.h"
#include "Types/Manager/NewSandboxError.h"
#include "Types/Manager/SandboxCreationResult.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSandboxModeTest,
	"HyperAIStudio.Sandbox.HoldApproveDiscard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace HyperAIStudio::Sandbox::Tests
{
	namespace FSC = UE::FileSandboxCore;

	struct FProbe
	{
		FString PackageName;
		FString ObjectPath;
		FString File;
	};

	FProbe MakeProbe(const FString& Name)
	{
		FProbe Probe;
		Probe.PackageName = TEXT("/Game/__HyperAIStudioTests/Sandbox/") + Name;
		Probe.ObjectPath = Probe.PackageName + TEXT(".") + Name;
		Probe.File = FPaths::ConvertRelativePathToFull(
			FPackageName::LongPackageNameToFilename(Probe.PackageName, FPackageName::GetAssetPackageExtension()));
		return Probe;
	}

	/** Saves a curve with Keys keys, into the sandbox while one is active. */
	bool SaveCurve(const FProbe& Probe, const int32 Keys)
	{
		UPackage* Package = CreatePackage(*Probe.PackageName);
		const FString Name = FPackageName::GetShortName(Probe.PackageName);
		UCurveFloat* Curve = FindObject<UCurveFloat>(Package, *Name);
		if (!Curve)
		{
			Curve = NewObject<UCurveFloat>(Package, *Name, RF_Public | RF_Standalone);
		}
		Curve->FloatCurve.Reset();
		for (int32 Key = 0; Key < Keys; ++Key)
		{
			Curve->FloatCurve.AddKey(static_cast<float>(Key), 1.0f);
		}
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.Error = GWarn;
		return UPackage::SavePackage(Package, Curve, *Probe.File, Args);
	}

	int32 KeysInMemory(const FProbe& Probe)
	{
		const UCurveFloat* Curve = FindObject<UCurveFloat>(nullptr, *Probe.ObjectPath);
		return Curve ? Curve->FloatCurve.GetNumKeys() : INDEX_NONE;
	}

	/** The project's own file, beneath any sandbox. */
	int64 ProjectFileSize(const FProbe& Probe)
	{
		return IPlatformFile::GetPlatformPhysical().FileSize(*Probe.File);
	}

	bool Holds(const FStatus& Status, const FProbe& Probe, const TCHAR* Kind)
	{
		const FString Name = FPaths::GetCleanFilename(Probe.File);
		return Status.Changes.ContainsByPredicate([&Name, Kind](const FChange& Change)
		{
			return Change.Kind == Kind && FPaths::GetCleanFilename(Change.Path) == Name;
		});
	}

	FSC::ISandboxManager& Manager()
	{
		return FSC::IFileSandboxCoreModule::Get().GetSandboxManager();
	}

	TArray<FString> UnsavedPackages()
	{
		TArray<UPackage*> Packages;
		UEditorLoadingAndSavingUtils::GetDirtyMapPackages(Packages);
		UEditorLoadingAndSavingUtils::GetDirtyContentPackages(Packages);
		TArray<FString> Names;
		for (const UPackage* Package : Packages)
		{
			Names.Add(Package->GetName());
		}
		return Names;
	}

	FString ActiveRoot()
	{
		const FSC::ISandboxInstance* Active = Manager().GetActiveSandboxInstance();
		return Active ? FString(Active->GetRootDirectory()) : FString();
	}

	/** Auto-approval's failed sandbox writes in Activity so far. */
	int32 FailedWrites()
	{
		return FHyperAIStudioAgentActivityLog::GetSnapshot().FilterByPredicate([](const FHyperAIStudioActivityEntry& Entry)
		{
			return Entry.Kind == EHyperAIStudioActivityKind::Failed && Entry.StatusCode == TEXT("sandbox_write_failed");
		}).Num();
	}

#if PLATFORM_WINDOWS
	/**
	 * Holds a project file open with no sharing, as another program can: nothing replaces or deletes it until Release,
	 * so writing the sandbox fails, and releasing it changes nothing the sandbox reports.
	 */
	struct FHeldOpen
	{
		HANDLE Handle = INVALID_HANDLE_VALUE;

		bool Hold(const FString& File)
		{
			Release();
			Handle = ::CreateFileW(*File, GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			return Handle != INVALID_HANDLE_VALUE;
		}

		void Release()
		{
			if (Handle != INVALID_HANDLE_VALUE)
			{
				::CloseHandle(Handle);
				Handle = INVALID_HANDLE_VALUE;
			}
		}

		~FHeldOpen()
		{
			Release();
		}
	};
#endif
}

bool FHyperAIStudioSandboxModeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Sandbox;
	using namespace HyperAIStudio::Sandbox::Tests;
	if (!FSC::IFileSandboxCoreModule::IsAvailable())
	{
		AddError(TEXT("The File Sandbox plugin is not loaded; HyperAIStudio.uplugin enables it."));
		return false;
	}
	if (Manager().HasActiveSandbox())
	{
		AddWarning(TEXT("A sandbox is already active in this editor; the Sandbox Mode test leaves it alone and does not run."));
		return true;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FProbe Edited = MakeProbe(TEXT("SandboxEdited_") + Suffix);
	const FProbe Added = MakeProbe(TEXT("SandboxAdded_") + Suffix);
	TSet<FString> Roots;
	ON_SCOPE_EXIT
	{
		SetModeForTests(false);
		OnModeChanged();
		if (Manager().HasActiveSandbox())
		{
			Roots.Add(ActiveRoot());
			Manager().LeaveSandbox();
		}
		for (const FString& Root : Roots)
		{
			Manager().DeleteSandbox(FSC::FDeleteSandboxByDirectoryArgs(Root));
		}
		SetModeForTests(TOptional<bool>());
		TArray<UObject*> Made;
		for (const FProbe* Probe : {&Edited, &Added})
		{
			if (UObject* Object = FindObject<UObject>(nullptr, *Probe->ObjectPath))
			{
				Made.Add(Object);
			}
		}
		if (!Made.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Made, /*ShowConfirmation=*/false);
		}
		for (const FProbe* Probe : {&Edited, &Added})
		{
			IPlatformFile::GetPlatformPhysical().DeleteFile(*Probe->File);
		}
	};

	const TArray<FString> UnsavedAtStart = UnsavedPackages();
	if (!UnsavedAtStart.IsEmpty())
	{
		AddInfo(TEXT("Unsaved before the test: ") + FString::Join(UnsavedAtStart, TEXT(", ")));
	}

	// The asset exists in the project before the sandbox opens: two keys.
	SetModeForTests(false);
	TestTrue(TEXT("The first version is saved to the project"), SaveCurve(Edited, 2));
	const int64 SizeBefore = ProjectFileSize(Edited);
	TestTrue(TEXT("The project holds the first version"), SizeBefore > 0);
	TestTrue(TEXT("Mode off reads as off"), GetStatus().State == EState::Off);
	FString Error;
	TestTrue(TEXT("Mode off never blocks an agent edit"), CheckAgentEdit(Error));
	TestFalse(TEXT("Mode off opens no sandbox"), Manager().HasActiveSandbox());

	// Mode on: the first agent edit opens HyperAI's sandbox, tagged so Epic's Sandboxes browser adopts it.
	SetModeForTests(true);
	TestTrue(TEXT("An agent edit opens the sandbox"), CheckAgentEdit(Error));
	TestTrue(TEXT("It is HyperAI's"), IsHoldingEdits());
	Roots.Add(ActiveRoot());
	if (const FSC::ISandboxInstance* Active = Manager().GetActiveSandboxInstance())
	{
		TestTrue(TEXT("Tagged for Epic's Sandboxes browser"), Active->GetInitialMetaData().Tags.Contains(FName(TEXT("SandboxedEditingTag"))));
	}
	TestEqual(TEXT("Active with nothing held"), GetStatus().Changes.Num(), 0);

	// An edit saved now lands in the sandbox; the project's file is untouched.
	TestTrue(TEXT("The edit saves"), SaveCurve(Edited, 5));
	FStatus Status = GetStatus();
	TestTrue(TEXT("Active"), Status.State == EState::Active);
	TestTrue(TEXT("The edited file is held"), Holds(Status, Edited, TEXT("edited")));
	TestEqual(TEXT("The project's file is unchanged"), ProjectFileSize(Edited), SizeBefore);
	TestTrue(TEXT("Held paths are project-relative"), Status.Changes.Num() > 0 && FPaths::IsRelative(Status.Changes[0].Path)
		&& Status.Changes[0].Path.StartsWith(TEXT("Content/")));
	AddInfo(FString::Printf(TEXT("Held: %s %s"), Status.Changes.Num() > 0 ? *Status.Changes[0].Kind : TEXT("-"),
		Status.Changes.Num() > 0 ? *Status.Changes[0].Path : TEXT("-")));

	// Discard throws the held file away and reloads the project's version into memory.
	TestEqual(TEXT("Memory has the edit before discarding"), KeysInMemory(Edited), 5);
	TestTrue(TEXT("Discard All succeeds"), DiscardAll(Error));
	TestEqual(TEXT("Discard reloads the project's version"), KeysInMemory(Edited), 2);
	TestEqual(TEXT("The project's file is still the first version"), ProjectFileSize(Edited), SizeBefore);
	TestTrue(TEXT("A fresh sandbox holds the next edits"), IsHoldingEdits());
	Roots.Add(ActiveRoot());
	TestEqual(TEXT("Nothing is held after discarding"), GetStatus().Changes.Num(), 0);

	// Approve never closes the sandbox over unsaved work, which the File Sandbox would reload from disk.
	if (UCurveFloat* Curve = FindObject<UCurveFloat>(nullptr, *Edited.ObjectPath))
	{
		Curve->FloatCurve.AddKey(9.0f, 1.0f);
		Curve->MarkPackageDirty();
	}
	TestFalse(TEXT("Approve refuses while an asset is unsaved"), ApproveAll(Error));
	TestTrue(TEXT("and names it"), Error.Contains(Edited.PackageName));
	TestEqual(TEXT("the unsaved edit is still in memory"), KeysInMemory(Edited), 3);
	TestTrue(TEXT("Saved, the edit is held"), SaveCurve(Edited, 4));

	// Approve writes the held files to the project.
	TestTrue(TEXT("A new asset saves"), SaveCurve(Added, 3));
	Status = GetStatus();
	TestTrue(TEXT("The new file is held as added"), Holds(Status, Added, TEXT("added")));
	TestTrue(TEXT("The project does not have it yet"), ProjectFileSize(Added) < 0);
	// The File Sandbox reports changes at the end of the frame, which is when the count follows.
	FCoreDelegates::OnEndFrame.Broadcast();
	TestEqual(TEXT("The Activity button counts it"), GetHeldCount(), Status.Changes.Num());
	TestTrue(TEXT("Approve All succeeds"), ApproveAll(Error));
	TestTrue(TEXT("The project has it now"), ProjectFileSize(Added) > 0);
	TestTrue(TEXT("and the approved edit"), ProjectFileSize(Edited) != SizeBefore);
	TestEqual(TEXT("Memory keeps the approved edit"), KeysInMemory(Edited), 4);
	TestEqual(TEXT("Nothing is held after approving"), GetStatus().Changes.Num(), 0);
	Roots.Add(ActiveRoot());

	// Someone else's sandbox: HyperAI refuses agent edits and never takes it over.
	SetModeForTests(false);
	OnModeChanged();
	TestFalse(TEXT("Turning the mode off leaves the sandbox"), Manager().HasActiveSandbox());
	FFileSandboxCore_SandboxMetaData Foreign(TEXT("Someone else's sandbox"), TEXT("HyperAI test"));
	const FSC::FNewSandboxResult Created = Manager().CreateNewSandbox(FSC::FNewSandboxArgs(MoveTemp(Foreign)));
	TestTrue(TEXT("A foreign sandbox opens"), Created.HasValue());
	Roots.Add(ActiveRoot());
	SetModeForTests(true);
	TestFalse(TEXT("Agent edits are refused"), CheckAgentEdit(Error));
	TestTrue(TEXT("The refusal says why"), Error.Contains(TEXT("Another sandbox")));
	TestTrue(TEXT("Foreign"), GetStatus().State == EState::Foreign);
	TestFalse(TEXT("It is not HyperAI's"), IsHoldingEdits());
	TestFalse(TEXT("Approve is refused on a foreign sandbox"), ApproveAll(Error));
	TestTrue(TEXT("The foreign sandbox is still active"), Manager().HasActiveSandbox());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSandboxAutoApprovalTest,
	"HyperAIStudio.Sandbox.AutoApprovalWritesHeldSaves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSandboxAutoApprovalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Sandbox;
	using namespace HyperAIStudio::Sandbox::Tests;
	using FAccess = FHyperAIStudioAutoApprovalTestAccess;
	using ECategory = EHyperAIStudioApprovalCategory;
	if (!FSC::IFileSandboxCoreModule::IsAvailable())
	{
		AddError(TEXT("The File Sandbox plugin is not loaded; HyperAIStudio.uplugin enables it."));
		return false;
	}
	if (Manager().HasActiveSandbox())
	{
		AddWarning(TEXT("A sandbox is already active in this editor; this test leaves it alone and does not run."));
		return true;
	}
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	if (const TArray<FString> Unsaved = UnsavedPackages(); !Unsaved.IsEmpty())
	{
		AddWarning(TEXT("Assets are unsaved in this editor, so a sandbox write would wait for them; this test does not run: ") + FString::Join(Unsaved, TEXT(", ")));
		return true;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FProbe Added = MakeProbe(TEXT("SandboxAutoAdded_") + Suffix);
	const FProbe Removed = MakeProbe(TEXT("SandboxAutoRemoved_") + Suffix);
	TSet<FString> Roots;
	FAccess::Reset();
	ON_SCOPE_EXIT
	{
		FAccess::Reset();
		SetModeForTests(false);
		OnModeChanged();
		if (Manager().HasActiveSandbox())
		{
			Roots.Add(ActiveRoot());
			Manager().LeaveSandbox();
		}
		for (const FString& Root : Roots)
		{
			Manager().DeleteSandbox(FSC::FDeleteSandboxByDirectoryArgs(Root));
		}
		SetModeForTests(TOptional<bool>());
		TArray<UObject*> Made;
		for (const FProbe* Probe : {&Added, &Removed})
		{
			if (UObject* Object = FindObject<UObject>(nullptr, *Probe->ObjectPath))
			{
				Made.Add(Object);
			}
		}
		if (!Made.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Made, /*ShowConfirmation=*/false);
		}
		for (const FProbe* Probe : {&Added, &Removed})
		{
			IPlatformFile::GetPlatformPhysical().DeleteFile(*Probe->File);
		}
	};

	// One asset is in the project before the sandbox opens; the sandbox will hold its removal.
	SetModeForTests(false);
	TestTrue(TEXT("An asset is saved to the project first"), SaveCurve(Removed, 2));
	TestTrue(TEXT("The project holds it"), ProjectFileSize(Removed) > 0);
	SetModeForTests(true);
	FString Error;
	TestTrue(TEXT("Sandbox Mode opens its sandbox"), CheckAgentEdit(Error));
	Roots.Add(ActiveRoot());
	const FString Name = GetStatus().SandboxName;

	// Turned on without naming this sandbox, a test's auto-approval may write nothing at all.
	TestTrue(TEXT("Sandbox Mode's held saves are offered in the dialog"), FAccess::OffersSandboxSaves(*FAccess::MakeDialog()));
	TestTrue(TEXT("A new asset is saved, into the sandbox"), SaveCurve(Added, 3));
	FCoreDelegates::OnEndFrame.Broadcast();
	TestTrue(TEXT("It is held"), Holds(GetStatus(), Added, TEXT("added")));
	{
		// For review: the dialog while Sandbox Mode holds a file, with its row ticked.
		const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = FAccess::MakeDialog();
		FAccess::SetTicked(*Dialog, ECategory::SandboxSaves, true);
		const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(Dialog, FIntPoint(660, 1240), TEXT("auto-approval-dialog-sandbox"));
		if (!Shot.IsEmpty())
		{
			AddInfo(TEXT("Screenshot: ") + Shot);
		}
		TestTrue(TEXT("Its native confirmation names Sandbox Mode's held saves"),
			FAccess::GetConfirmation(*Dialog).ToString().Contains(TEXT("Sandbox Mode's held saves")));
	}
	TestTrue(TEXT("A test's auto-approval that does not name this sandbox turns on"), FAccess::TurnOn({ECategory::Edits, ECategory::SandboxSaves}, {}));
	TestFalse(TEXT("and does not cover it"), FHyperAIStudioAutoApproval::CoversSandboxSaves());
	FAccess::PumpSandbox();
	TestTrue(TEXT("Nothing is written"), ProjectFileSize(Added) < 0 && Holds(GetStatus(), Added, TEXT("added")));
	TestTrue(TEXT("and Activity's card says nothing about a sandbox it does not reach"), FHyperAIStudioAutoApproval::DescribeSandboxWait().IsEmpty());
	FAccess::Reset();

	// Turned on without Sandbox Mode's held saves, as by default: the files keep waiting, and every place says so.
	{
		const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = FAccess::MakeDialog();
		const FString Unticked = FAccess::GetSandboxNote(*Dialog).ToString();
		TestTrue(TEXT("The dialog says what stays unticked means, with the file it holds: ") + Unticked,
			Unticked.Contains(TEXT("writes none of it")) && Unticked.Contains(TEXT("1 file")));
		FAccess::SetTicked(*Dialog, ECategory::SandboxSaves, true);
		const FString Ticked = FAccess::GetSandboxNote(*Dialog).ToString();
		TestTrue(TEXT("and what ticked means: ") + Ticked, Ticked.Contains(TEXT("written too")));
	}
	{
		HyperAIStudio::Tests::FLogCapture Log(TEXT("LogHyperAIStudioAutoApproval"));
		TestTrue(TEXT("Auto-approval for Edits only, reaching this sandbox, turns on"), FAccess::TurnOn({ECategory::Edits}, {TEXT("sandbox:") + Name}));
		TestEqual(TEXT("Its log line says Sandbox Mode is on and its held saves are not covered"),
			Log.Count(TEXT("held saves are NOT covered (not ticked in its dialog)")), 1);
		TestFalse(TEXT("It does not cover the held saves"), FHyperAIStudioAutoApproval::CoversSandboxSaves());
		TestTrue(TEXT("and knows it leaves them waiting"), FHyperAIStudioAutoApproval::LeavesSandboxSavesWaiting());
		const FString Card = FHyperAIStudioAutoApproval::DescribeSandboxWait().ToString();
		TestTrue(TEXT("Activity's Sandbox card says auto-approval does not write them: ") + Card, Card.Contains(TEXT("does not write these")));
		const FString CardShot = HyperAIStudio::Tests::SaveWidgetShot(SNew(SHyperAIStudioChatActivitySidebar), FIntPoint(300, 640),
			TEXT("auto-approval-activity-sandbox-not-covered"));
		if (!CardShot.IsEmpty())
		{
			AddInfo(TEXT("Screenshot: ") + CardShot);
		}
		FAccess::PumpSandbox();
		FAccess::PumpSandbox();
		TestEqual(TEXT("The log says so once for this activation"), Log.Count(TEXT("does not write the 1 file(s) Sandbox Mode holds")), 1);
		TestTrue(TEXT("Nothing is written"), ProjectFileSize(Added) < 0);
		const FString Agents = UHyperAIStudioNativeReadToolset::hyper_sandbox_status().Diagnostic;
		TestTrue(TEXT("hyper_sandbox_status tells agents too: ") + Agents, Agents.Contains(TEXT("does not cover these")));
		FAccess::Reset();
	}

	// A held removal deletes the project's file, so it needs Destructive as well.
	TestTrue(TEXT("The project file is removed in the sandbox"), IFileManager::Get().Delete(*Removed.File, false, true, true));
	FCoreDelegates::OnEndFrame.Broadcast();
	const FStatus WithRemoval = GetStatus();
	TestTrue(TEXT("The removal is held"), Holds(WithRemoval, Removed, TEXT("removed")));
	TestTrue(TEXT("while the project keeps its file"), ProjectFileSize(Removed) > 0);
	TestTrue(TEXT("Auto-approval for Edits and Sandbox Mode's held saves turns on"),
		FAccess::TurnOn({ECategory::Edits, ECategory::SandboxSaves}, {TEXT("sandbox:") + Name}));
	TestTrue(TEXT("It covers this sandbox"), FHyperAIStudioAutoApproval::CoversSandboxSaves());
	FAccess::PumpSandbox();
	TestTrue(TEXT("Without Destructive nothing is written"), ProjectFileSize(Added) < 0 && ProjectFileSize(Removed) > 0);
	const FString RemovalWait = FHyperAIStudioAutoApproval::DescribeSandboxWait().ToString();
	TestTrue(TEXT("and Activity says why: ") + RemovalWait, RemovalWait.Contains(TEXT("removals")));

	// With Destructive too, it still never closes the sandbox over unsaved work.
	FAccess::Reset();
	TestTrue(TEXT("With Destructive too, it turns on"),
		FAccess::TurnOn({ECategory::Edits, ECategory::Destructive, ECategory::SandboxSaves}, {TEXT("sandbox:") + Name}));
	if (UCurveFloat* Curve = FindObject<UCurveFloat>(nullptr, *Added.ObjectPath))
	{
		Curve->FloatCurve.AddKey(9.0f, 1.0f);
		Curve->MarkPackageDirty();
	}
	FAccess::PumpSandbox();
	TestTrue(TEXT("While an asset is unsaved nothing is written"), ProjectFileSize(Added) < 0 && ProjectFileSize(Removed) > 0);
	const FString UnsavedWait = FHyperAIStudioAutoApproval::DescribeSandboxWait().ToString();
	TestTrue(TEXT("and Activity names it: ") + UnsavedWait, UnsavedWait.Contains(TEXT("unsaved")) && UnsavedWait.Contains(Added.PackageName));
	TestEqual(TEXT("the unsaved edit is still in memory"), KeysInMemory(Added), 4);

	// Saved, the sandbox is written at the next quiet look, as Approve All writes it.
	TestTrue(TEXT("The edit is saved, into the sandbox"), SaveCurve(Added, 4));
	FCoreDelegates::OnEndFrame.Broadcast();
	FAccess::PumpSandbox();
	TestTrue(TEXT("The project has the new asset now"), ProjectFileSize(Added) > 0);
	TestTrue(TEXT("and the removal is written"), ProjectFileSize(Removed) < 0);
	TestEqual(TEXT("Nothing is held any more"), GetStatus().Changes.Num(), 0);
	TestEqual(TEXT("It counts as one approval"), FHyperAIStudioAutoApproval::GetStatus().Approved, 1);
	TestEqual(TEXT("and as one sandbox write"), FHyperAIStudioAutoApproval::GetStatus().SandboxWrites, 1);
	const FString Tooltip = FHyperAIStudioAutoApproval::DescribeBannerTooltip().ToString();
	TestTrue(TEXT("The banner says how many of its approvals were sandbox writes: ") + Tooltip, Tooltip.Contains(TEXT("1 of them writes")));
	TestTrue(TEXT("A fresh sandbox holds what is saved next"), IsHoldingEdits());
	Roots.Add(ActiveRoot());
	const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestTrue(TEXT("Activity marks the write auto-approved, naming the files and the approval it used"), Entries.ContainsByPredicate([&Added](const FHyperAIStudioActivityEntry& Entry)
	{
		return Entry.Kind == EHyperAIStudioActivityKind::ApprovalApproved && Entry.AutoApprovedAs == EHyperAIStudioApprovalCategory::SandboxSaves
			&& Entry.ToolName == TEXT("Sandbox Mode") && Entry.Detail.Contains(FPaths::GetBaseFilename(Added.File))
			&& Entry.Detail.Contains(FString::Printf(TEXT("approval 1 of %d"), FHyperAIStudioAutoApproval::GetMaxApprovals()));
	}));

	// Someone else's sandbox is never written, and nothing tells agents or the user that it will be.
	FAccess::Reset();
	SetModeForTests(false);
	OnModeChanged();
	TestFalse(TEXT("Turning the mode off leaves HyperAI's empty sandbox"), Manager().HasActiveSandbox());
	const FString ForeignName = TEXT("Someone else's sandbox ") + Suffix;
	FFileSandboxCore_SandboxMetaData Foreign(ForeignName, TEXT("HyperAI test"));
	const FSC::FNewSandboxResult Created = Manager().CreateNewSandbox(FSC::FNewSandboxArgs(MoveTemp(Foreign)));
	TestTrue(TEXT("A foreign sandbox opens"), Created.HasValue());
	Roots.Add(ActiveRoot());
	SetModeForTests(true);
	TestTrue(TEXT("Auto-approval that names it turns on"), FAccess::TurnOn({ECategory::Edits, ECategory::SandboxSaves}, {TEXT("sandbox:") + ForeignName}));
	TestFalse(TEXT("but does not cover someone else's sandbox"), FHyperAIStudioAutoApproval::CoversSandboxSaves());
	TestFalse(TEXT("nor counts it as left waiting"), FHyperAIStudioAutoApproval::LeavesSandboxSavesWaiting());
	TestTrue(TEXT("Activity's card says nothing about it"), FHyperAIStudioAutoApproval::DescribeSandboxWait().IsEmpty());
	const FString ForeignStatus = UHyperAIStudioNativeReadToolset::hyper_sandbox_status().Diagnostic;
	TestFalse(TEXT("and hyper_sandbox_status does not tell agents it is written: ") + ForeignStatus, ForeignStatus.Contains(TEXT("writes them")));
	FAccess::Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSandboxAutoApprovalRetryTest,
	"HyperAIStudio.Sandbox.AutoApprovalRetriesFailedWrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * A sandbox write that fails because another program holds a project file is tried again when the held files change,
 * and after a minute when they do not, since the cause is fixed outside the sandbox where nothing reports it. Each failed
 * write counts toward auto-approval's failures in a row, which turn it off at three.
 */
bool FHyperAIStudioSandboxAutoApprovalRetryTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Sandbox;
	using namespace HyperAIStudio::Sandbox::Tests;
	using FAccess = FHyperAIStudioAutoApprovalTestAccess;
	using ECategory = EHyperAIStudioApprovalCategory;
#if !PLATFORM_WINDOWS
	AddInfo(TEXT("Holding a file open with no sharing needs Windows; nothing to check."));
	return true;
#else
	if (!FSC::IFileSandboxCoreModule::IsAvailable())
	{
		AddError(TEXT("The File Sandbox plugin is not loaded; HyperAIStudio.uplugin enables it."));
		return false;
	}
	if (Manager().HasActiveSandbox())
	{
		AddWarning(TEXT("A sandbox is already active in this editor; this test leaves it alone and does not run."));
		return true;
	}
	if (FAccess::SkipWhileUserActivationOn(*this))
	{
		return true;
	}
	if (const TArray<FString> Unsaved = UnsavedPackages(); !Unsaved.IsEmpty())
	{
		AddWarning(TEXT("Assets are unsaved in this editor, so a sandbox write would wait for them; this test does not run: ") + FString::Join(Unsaved, TEXT(", ")));
		return true;
	}
	// What the File Sandbox and auto-approval say when a write fails is this test's point.
	AddExpectedMessagePlain(TEXT("when persisting sandbox state"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessagePlain(TEXT("Auto-approval could not write the"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FProbe Edited = MakeProbe(TEXT("SandboxRetryEdited_") + Suffix);
	const FProbe Added = MakeProbe(TEXT("SandboxRetryAdded_") + Suffix);
	TSet<FString> Roots;
	FHeldOpen Held;
	FAccess::Reset();
	ON_SCOPE_EXIT
	{
		Held.Release();
		FAccess::Reset();
		SetModeForTests(false);
		OnModeChanged();
		if (Manager().HasActiveSandbox())
		{
			Roots.Add(ActiveRoot());
			Manager().LeaveSandbox();
		}
		for (const FString& Root : Roots)
		{
			Manager().DeleteSandbox(FSC::FDeleteSandboxByDirectoryArgs(Root));
		}
		SetModeForTests(TOptional<bool>());
		TArray<UObject*> Made;
		for (const FProbe* Probe : {&Edited, &Added})
		{
			if (UObject* Object = FindObject<UObject>(nullptr, *Probe->ObjectPath))
			{
				Made.Add(Object);
			}
		}
		if (!Made.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Made, /*ShowConfirmation=*/false);
		}
		for (const FProbe* Probe : {&Edited, &Added})
		{
			IPlatformFile::GetPlatformPhysical().DeleteFile(*Probe->File);
		}
	};

	SetModeForTests(false);
	TestTrue(TEXT("An asset is saved to the project first"), SaveCurve(Edited, 2));
	const int64 SizeBefore = ProjectFileSize(Edited);
	SetModeForTests(true);
	FString Error;
	TestTrue(TEXT("Sandbox Mode opens its sandbox"), CheckAgentEdit(Error));
	Roots.Add(ActiveRoot());
	TestTrue(TEXT("Its edit is saved, into the sandbox"), SaveCurve(Edited, 5));
	FCoreDelegates::OnEndFrame.Broadcast();
	TestTrue(TEXT("and held"), Holds(GetStatus(), Edited, TEXT("edited")));
	const int32 FailedBefore = FailedWrites();

	// Another program holds the project's file: the write fails, and Activity and the card say what happens next.
	TestTrue(TEXT("The project's file is held open"), Held.Hold(Edited.File));
	TestTrue(TEXT("Auto-approval for Sandbox Mode's held saves turns on"),
		FAccess::TurnOn({ECategory::Edits, ECategory::SandboxSaves}, {TEXT("sandbox:") + GetStatus().SandboxName}));
	FAccess::PumpSandbox();
	TestEqual(TEXT("The write fails"), FailedWrites() - FailedBefore, 1);
	TestEqual(TEXT("and the project's file is as it was"), ProjectFileSize(Edited), SizeBefore);
	const FString Card = FHyperAIStudioAutoApproval::DescribeSandboxWait().ToString();
	TestTrue(TEXT("The card says when it tries again: ") + Card, Card.Contains(TEXT("tries again in a minute")));
	FAccess::PumpSandbox();
	TestEqual(TEXT("With nothing changed, it does not try again at once"), FailedWrites() - FailedBefore, 1);

	// The held files change: it tries again at once.
	TestTrue(TEXT("A new asset is saved, into the sandbox"), SaveCurve(Added, 3));
	FCoreDelegates::OnEndFrame.Broadcast();
	FAccess::PumpSandbox();
	TestEqual(TEXT("A change to the held files is tried at once, and fails again"), FailedWrites() - FailedBefore, 2);
	TestTrue(TEXT("Two failures in a row leave it on"), FHyperAIStudioAutoApproval::IsActive());
	FAccess::PumpSandbox();
	TestEqual(TEXT("What that write left held is not tried again at once"), FailedWrites() - FailedBefore, 2);

	// The cause is fixed outside the sandbox, which reports nothing: the back-off tries again.
	Held.Release();
	FAccess::PumpSandbox();
	TestTrue(TEXT("Before the back-off nothing is written"), Holds(GetStatus(), Edited, TEXT("edited")) && ProjectFileSize(Edited) == SizeBefore);
	FAccess::AdvanceClock(FAccess::GetSandboxRetrySeconds() + 1.0);
	FAccess::PumpSandbox();
	TestTrue(TEXT("After it, the write is tried again and succeeds"), ProjectFileSize(Edited) != SizeBefore && ProjectFileSize(Added) > 0);
	TestEqual(TEXT("Nothing is held any more"), GetStatus().Changes.Num(), 0);
	TestTrue(TEXT("It is still on"), FHyperAIStudioAutoApproval::IsActive());
	TestEqual(TEXT("with one write"), FHyperAIStudioAutoApproval::GetStatus().SandboxWrites, 1);
	Roots.Add(ActiveRoot());

	// Three failed writes in a row turn it off, as three failed requests do.
	const int64 SizeApproved = ProjectFileSize(Edited);
	FAccess::Reset();
	TestTrue(TEXT("It turns on for the new sandbox"),
		FAccess::TurnOn({ECategory::Edits, ECategory::SandboxSaves}, {TEXT("sandbox:") + GetStatus().SandboxName}));
	TestTrue(TEXT("Another edit is saved, into the sandbox"), SaveCurve(Edited, 7));
	FCoreDelegates::OnEndFrame.Broadcast();
	TestTrue(TEXT("The project's file is held open again"), Held.Hold(Edited.File));
	const int32 GuardBefore = FailedWrites();
	FAccess::PumpSandbox();
	FAccess::AdvanceClock(FAccess::GetSandboxRetrySeconds() + 1.0);
	FAccess::PumpSandbox();
	TestTrue(TEXT("Two failures leave it on"), FHyperAIStudioAutoApproval::IsActive());
	FAccess::AdvanceClock(FAccess::GetSandboxRetrySeconds() + 1.0);
	FAccess::PumpSandbox();
	TestEqual(TEXT("Each retry is a failed write"), FailedWrites() - GuardBefore, 3);
	TestFalse(TEXT("The third turns it off"), FHyperAIStudioAutoApproval::IsActive());
	TestTrue(TEXT("for repeated failures"), FAccess::GetLastEnd() == EHyperAIStudioAutoApprovalEnd::RepeatedFailures);
	TestEqual(TEXT("and the project's file is as it was"), ProjectFileSize(Edited), SizeApproved);
	return true;
#endif
}

#endif
