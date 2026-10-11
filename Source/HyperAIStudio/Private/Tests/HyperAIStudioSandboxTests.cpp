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
#include "ObjectTools.h"
#include "Types/Manager/DeleteSandboxByDirectoryArgs.h"
#include "Types/Manager/DeleteSandboxResult.h"
#include "Types/Manager/LeaveSandboxResult.h"
#include "Types/Manager/NewSandboxArgs.h"
#include "Types/Manager/NewSandboxError.h"
#include "Types/Manager/SandboxCreationResult.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

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
	FAccess::Reset();

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
	TestTrue(TEXT("A fresh sandbox holds what is saved next"), IsHoldingEdits());
	Roots.Add(ActiveRoot());
	const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
	TestTrue(TEXT("Activity marks the write auto-approved, naming the files"), Entries.ContainsByPredicate([&Added](const FHyperAIStudioActivityEntry& Entry)
	{
		return Entry.Kind == EHyperAIStudioActivityKind::ApprovalApproved && Entry.AutoApprovedAs == EHyperAIStudioApprovalCategory::SandboxSaves
			&& Entry.ToolName == TEXT("Sandbox Mode") && Entry.Detail.Contains(FPaths::GetBaseFilename(Added.File));
	}));
	return true;
}

#endif
