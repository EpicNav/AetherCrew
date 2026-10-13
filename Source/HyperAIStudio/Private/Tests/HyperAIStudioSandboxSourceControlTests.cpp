// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Curves/CurveFloat.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioSandbox.h"
#include "HyperAIStudioSourceControl.h"
#include "HyperAIStudioWidgetShot.h"
#include "SHyperAIStudioActivitySidebar.h"
#include "IFileSandboxCoreModule.h"
#include "ISandboxInstance.h"
#include "ISandboxManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "SourceControlOperations.h"
#include "Types/Manager/DeleteSandboxByDirectoryArgs.h"
#include "Types/Manager/DeleteSandboxResult.h"
#include "Types/Manager/LeaveSandboxResult.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/SNullWidget.h"

namespace HyperAIStudio::SandboxSourceControlTests
{
	/** One file as the mock depot sees it. */
	struct FDepotFile
	{
		bool bControlled = false;
		bool bCheckedOut = false;
		bool bAdded = false;
		bool bDeleted = false;
		bool bCurrent = true;
		/** Someone else's check-out or lock. */
		FString OtherUser;
		/** The state reads free, but the check-out fails: someone took it a moment ago. */
		FString TakenOnCheckOut;
	};

	FString Key(const FString& File)
	{
		FString Full = FPaths::ConvertRelativePathToFull(File);
		FPaths::NormalizeFilename(Full);
		return Full;
	}

	class FMockState final : public ISourceControlState
	{
	public:
		FMockState(const FString& InFile, const FDepotFile& InData) : File(InFile), Data(InData) {}
		virtual int32 GetHistorySize() const override { return 0; }
		virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetHistoryItem(int32) const override { return nullptr; }
		virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(int32) const override { return nullptr; }
		virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(const FString&) const override { return nullptr; }
		virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetCurrentRevision() const override { return nullptr; }
#if SOURCE_CONTROL_WITH_SLATE
		virtual FSlateIcon GetIcon() const override { return FSlateIcon(); }
#endif
		virtual FText GetDisplayName() const override { return FText::GetEmpty(); }
		virtual FText GetDisplayTooltip() const override { return FText::GetEmpty(); }
		virtual const FString& GetFilename() const override { return File; }
		virtual const FDateTime& GetTimeStamp() const override { static const FDateTime Zero; return Zero; }
		virtual bool CanCheckIn() const override { return Data.bCheckedOut || Data.bAdded; }
		// As Subversion: a controlled file nobody has locked can be locked.
		virtual bool CanCheckout() const override { return Data.bControlled && !Data.bCheckedOut && !Data.bDeleted && Data.OtherUser.IsEmpty(); }
		virtual bool IsCheckedOut() const override { return Data.bCheckedOut; }
		virtual bool IsCheckedOutOther(FString* Who = nullptr) const override
		{
			if (Who)
			{
				*Who = Data.OtherUser;
			}
			return !Data.OtherUser.IsEmpty();
		}
		virtual bool IsCheckedOutInOtherBranch(const FString& = FString()) const override { return false; }
		virtual bool IsModifiedInOtherBranch(const FString& = FString()) const override { return false; }
		virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString& = FString()) const override { return false; }
		virtual TArray<FString> GetCheckedOutBranches() const override { return {}; }
		virtual FString GetOtherUserBranchCheckedOuts() const override { return FString(); }
		virtual bool GetOtherBranchHeadModification(FString&, FString&, int32&) const override { return false; }
		virtual bool IsCurrent() const override { return Data.bCurrent; }
		virtual bool IsSourceControlled() const override { return Data.bControlled; }
		virtual bool IsAdded() const override { return Data.bAdded; }
		virtual bool IsDeleted() const override { return Data.bDeleted; }
		virtual bool IsIgnored() const override { return false; }
		virtual bool CanEdit() const override { return Data.bCheckedOut || !Data.bControlled; }
		virtual bool CanDelete() const override { return Data.bControlled && Data.OtherUser.IsEmpty(); }
		virtual bool IsUnknown() const override { return false; }
		virtual bool IsModified() const override { return false; }
		virtual bool CanAdd() const override { return !Data.bControlled; }
		virtual bool CanRevert() const override { return Data.bCheckedOut || Data.bAdded; }

	private:
		FString File;
		FDepotFile Data;
	};

	/**
	 * A source control provider with Subversion's rules (Perforce's are the same as far as this goes): CheckOut locks, a
	 * file locked by someone else cannot be locked, MarkForAdd and Delete schedule. It records every operation.
	 */
	class FMockProvider final : public ISourceControlProvider
	{
	public:
		explicit FMockProvider(const TCHAR* InName) : Name(InName) {}

		TMap<FString, FDepotFile> Depot;
		TArray<FString> Log;

		FDepotFile& File(const FString& Path) { return Depot.FindOrAdd(Key(Path)); }

		virtual void Init(bool bForceConnection = true) override {}
		virtual void Close() override {}
		virtual const FName& GetName() const override { return Name; }
		virtual FText GetStatusText() const override { return FText::GetEmpty(); }
		virtual TMap<EStatus, FString> GetStatus() const override { return {}; }
		virtual bool IsEnabled() const override { return true; }
		virtual bool IsAvailable() const override { return true; }
		virtual bool QueryStateBranchConfig(const FString&, const FString&) override { return false; }
		virtual void RegisterStateBranches(const TArray<FString>&, const FString&) override {}
		virtual int32 GetStateBranchIndex(const FString&) const override { return INDEX_NONE; }
		virtual bool GetStateBranchAtIndex(int32, FString&) const override { return false; }
		virtual ECommandResult::Type GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type) override
		{
			for (const FString& InFile : InFiles)
			{
				OutState.Add(MakeShared<FMockState, ESPMode::ThreadSafe>(Key(InFile), Depot.FindRef(Key(InFile))));
			}
			return ECommandResult::Succeeded;
		}
		virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>&, TArray<FSourceControlChangelistStateRef>&, EStateCacheUsage::Type) override
		{
			return ECommandResult::Failed;
		}
		virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)>) const override { return {}; }
		virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate&) override { return FDelegateHandle(); }
		virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle) override {}
		virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr, const TArray<FString>& InFiles,
			EConcurrency::Type = EConcurrency::Synchronous, const FSourceControlOperationComplete& InComplete = FSourceControlOperationComplete()) override
		{
			const FName Operation = InOperation->GetName();
			TArray<FString> Names;
			for (const FString& InFile : InFiles)
			{
				Names.Add(FPaths::GetBaseFilename(InFile));
			}
			Log.Add(Operation.ToString() + TEXT(" ") + FString::Join(Names, TEXT(",")));
			bool bOk = true;
			for (const FString& InFile : InFiles)
			{
				FDepotFile& Data = File(InFile);
				if (Operation == TEXT("CheckOut"))
				{
					const FString Holder = !Data.OtherUser.IsEmpty() ? Data.OtherUser : Data.TakenOnCheckOut;
					if (!Holder.IsEmpty())
					{
						InOperation->AddErrorMessge(FText::FromString(FString::Printf(TEXT("svn: warning: W160035: Path '%s' is already locked by %s"),
							*FPaths::GetCleanFilename(InFile), *Holder)));
						bOk = false;
						continue;
					}
					Data.bCheckedOut = true;
				}
				else if (Operation == TEXT("MarkForAdd"))
				{
					Data.bControlled = true;
					Data.bAdded = true;
				}
				else if (Operation == TEXT("Delete"))
				{
					Data.bDeleted = true;
				}
			}
			const ECommandResult::Type Result = bOk ? ECommandResult::Succeeded : ECommandResult::Failed;
			InComplete.ExecuteIfBound(InOperation, Result);
			return Result;
		}
		virtual bool CanExecuteOperation(const FSourceControlOperationRef&) const override { return true; }
		virtual bool CanCancelOperation(const FSourceControlOperationRef&) const override { return false; }
		virtual void CancelOperation(const FSourceControlOperationRef&) override {}
		virtual TArray<TSharedRef<ISourceControlLabel>> GetLabels(const FString&) const override { return {}; }
		virtual TArray<FSourceControlChangelistRef> GetChangelists(EStateCacheUsage::Type) override { return {}; }
		virtual bool UsesLocalReadOnlyState() const override { return false; }
		virtual bool UsesChangelists() const override { return false; }
		virtual bool UsesUncontrolledChangelists() const override { return false; }
		virtual bool UsesCheckout() const override { return true; }
		virtual bool UsesFileRevisions() const override { return true; }
		virtual bool UsesSnapshots() const override { return false; }
		virtual bool UsesSoftRevertOnDelete() const override { return false; }
		virtual bool AllowsDiffAgainstDepot() const override { return false; }
		virtual TOptional<bool> HasChangesToSync() const override { return TOptional<bool>(); }
		virtual TOptional<bool> HasChangesToCheckIn() const override { return TOptional<bool>(); }
		virtual void Tick() override {}
#if SOURCE_CONTROL_WITH_SLATE
		virtual TSharedRef<SWidget> MakeSettingsWidget() const override { return SNullWidget::NullWidget; }
#endif

	private:
		FName Name;
	};

	/** The lore command for the approve: it records each call, and a file named Locked is bob's. */
	const TCHAR* const FakeLore = TEXT(R"PY(import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
with open(os.path.join(HERE, "calls.jsonl"), "a", encoding="utf-8") as f:
    f.write(json.dumps(args) + "\n")
command = args[args.index("--non-interactive") + 1:] if "--non-interactive" in args else args
if command[:2] == ["lock", "acquire"] and any("Locked" in path for path in command[2:]):
    sys.stderr.write("error: Locked.uasset is locked by bob@studio\n")
    sys.exit(1)
print("ok")
)PY");

	const FHyperAIStudioSourceControl::FHeldFileResult* Find(const TArray<FHyperAIStudioSourceControl::FHeldFileResult>& Results, const TCHAR* Base)
	{
		return Results.FindByPredicate([Base](const FHyperAIStudioSourceControl::FHeldFileResult& Result) { return FPaths::GetBaseFilename(Result.Path) == Base; });
	}

	/** What a File Sandbox persist does, on plain files: writes or deletes each, and reports them written. */
	TSet<FString> WriteFiles(const TArray<FString>& Files, const TArray<FHyperAIStudioSourceControl::FHeldFile>& Held)
	{
		TSet<FString> Written;
		for (const FString& File : Files)
		{
			const FHyperAIStudioSourceControl::FHeldFile* Entry = Held.FindByPredicate([&File](const FHyperAIStudioSourceControl::FHeldFile& Each)
			{
				return Key(Each.Path) == Key(File);
			});
			if (Entry && Entry->Kind == TEXT("removed"))
			{
				IFileManager::Get().Delete(*File, false, true, true);
			}
			else
			{
				FFileHelper::SaveStringToFile(TEXT("approved"), *File);
			}
			Written.Add(File);
		}
		return Written;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSandboxSourceControlProviderTest,
	"HyperAIStudio.SourceControl.SandboxApproveSubversion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSandboxSourceControlProviderTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::SandboxSourceControlTests;
	using FResult = FHyperAIStudioSourceControl::FHeldFileResult;
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / (TEXT("SvnApprove-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
	FMockProvider Svn(TEXT("Subversion"));
	FHyperAIStudioSourceControl::SetProviderForTests(&Svn);
	ON_SCOPE_EXIT
	{
		FHyperAIStudioSourceControl::SetProviderForTests(nullptr);
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
	};
	TestTrue(TEXT("The stand-in is the provider agent edits go through"), FHyperAIStudioSourceControl::GetProvider() == &Svn);

	const auto Path = [&Folder](const TCHAR* Name) { return Folder / (FString(Name) + TEXT(".uasset")); };
	for (const TCHAR* Name : {TEXT("Edited"), TEXT("Held"), TEXT("Behind"), TEXT("Gone"), TEXT("Race")})
	{
		FFileHelper::SaveStringToFile(TEXT("before"), *Path(Name));
		Svn.File(Path(Name)).bControlled = true;
	}
	Svn.File(Path(TEXT("Held"))).OtherUser = TEXT("carol");
	Svn.File(Path(TEXT("Behind"))).bCurrent = false;
	Svn.File(Path(TEXT("Race"))).TakenOnCheckOut = TEXT("dave");

	const TArray<FHyperAIStudioSourceControl::FHeldFile> Held = {
		{Path(TEXT("Edited")), TEXT("edited")},
		{Path(TEXT("Held")), TEXT("edited")},
		{Path(TEXT("Behind")), TEXT("edited")},
		{Path(TEXT("New")), TEXT("added")},
		{Path(TEXT("Gone")), TEXT("removed")},
		{Path(TEXT("Race")), TEXT("edited")},
	};
	TArray<FString> Given;
	bool bCheckedOutBeforeWrite = false;
	const TArray<FResult> Results = FHyperAIStudioSourceControl::WriteHeldFiles(Held, [&](const TArray<FString>& Files)
	{
		Given = Files;
		bCheckedOutBeforeWrite = Svn.File(Path(TEXT("Edited"))).bCheckedOut;
		return WriteFiles(Files, Held);
	});
	AddInfo(TEXT("Provider calls: ") + FString::Join(Svn.Log, TEXT(" | ")));

	TestEqual(TEXT("One result per held file"), Results.Num(), Held.Num());
	const FResult* Edited = Find(Results, TEXT("Edited"));
	TestTrue(TEXT("An edited file is checked out (locked) and written"), Edited && Edited->Outcome == TEXT("checked_out") && Edited->bWritten);
	TestTrue(TEXT("checked out before it was written"), bCheckedOutBeforeWrite);
	const FResult* Teammate = Find(Results, TEXT("Held"));
	TestTrue(TEXT("A file carol has locked stays held"), Teammate && Teammate->Outcome == TEXT("blocked") && !Teammate->bWritten);
	TestTrue(TEXT("naming her"), Teammate && Teammate->Who == TEXT("carol") && Teammate->Detail.Contains(TEXT("carol")));
	TestFalse(TEXT("and is never written"), Given.ContainsByPredicate([&Path](const FString& File) { return Key(File) == Key(Path(TEXT("Held"))); }));
	FString Content;
	FFileHelper::LoadFileToString(Content, *Path(TEXT("Held")));
	TestEqual(TEXT("Her file is as it was"), Content, FString(TEXT("before")));
	const FResult* Behind = Find(Results, TEXT("Behind"));
	TestTrue(TEXT("A file behind the latest revision stays held"), Behind && Behind->Outcome == TEXT("blocked") && Behind->Detail.Contains(TEXT("latest")));
	const FResult* Added = Find(Results, TEXT("New"));
	TestTrue(TEXT("A new file is written and marked for add"), Added && Added->bWritten && Added->Outcome == TEXT("marked_for_add")
		&& Svn.Log.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("MarkForAdd")) && Line.Contains(TEXT("New")); }));
	TestFalse(TEXT("A new file is never checked out"), Svn.Log.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("CheckOut")) && Line.Contains(TEXT("New")); }));
	const FResult* Gone = Find(Results, TEXT("Gone"));
	TestTrue(TEXT("A removed file is deleted and marked for delete"), Gone && Gone->bWritten && Gone->Outcome == TEXT("marked_for_delete")
		&& !FPaths::FileExists(Path(TEXT("Gone"))));
	const FResult* Race = Find(Results, TEXT("Race"));
	TestTrue(TEXT("A file someone locked a moment ago stays held, naming them from the provider's refusal"), Race && Race->Outcome == TEXT("blocked")
		&& Race->Who == TEXT("dave") && !Race->bWritten);
	TestTrue(TEXT("Only that file was held back: the batch was retried one by one"), Edited && Edited->bWritten);
	TestTrue(TEXT("The checked-out and added files are the agents' to submit"), FHyperAIStudioSourceControl::GetAgentFiles().Num() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSandboxSourceControlLoreTest,
	"HyperAIStudio.SourceControl.SandboxApproveLore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSandboxSourceControlLoreTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::SandboxSourceControlTests;
	using FResult = FHyperAIStudioSourceControl::FHeldFileResult;
	if (FHyperAIStudioSourceControl::GetProvider())
	{
		AddInfo(TEXT("Skipped: this editor has an Unreal source control provider, which takes precedence over Lore."));
		return true;
	}
	int32 Code = -1;
	FString Out;
	FString Err;
	FPlatformProcess::ExecProcess(TEXT("python"), TEXT("--version"), &Code, &Out, &Err);
	if (Code != 0)
	{
		AddWarning(TEXT("python is not on PATH, so the scripted lore command cannot run."));
		return true;
	}
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / (TEXT("LoreApprove-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
	const FString Script = Folder / TEXT("fake_lore.py");
	FFileHelper::SaveStringToFile(FakeLore, *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	ON_SCOPE_EXIT
	{
		FHyperAIStudioSourceControl::SetLoreForTests(FString(), FString(), FString());
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
	};
	const auto Path = [&Folder](const TCHAR* Name) { return Folder / (FString(Name) + TEXT(".uasset")); };
	const auto Calls = [&Folder]()
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *(Folder / TEXT("calls.jsonl")));
		return Text.Replace(TEXT("\\\\"), TEXT("/"));
	};
	const auto Normal = [](const FString& File) { return File.Replace(TEXT("\\"), TEXT("/")); };
	for (const TCHAR* Name : {TEXT("Free"), TEXT("Locked")})
	{
		FFileHelper::SaveStringToFile(TEXT("before"), *Path(Name));
	}
	const TArray<FHyperAIStudioSourceControl::FHeldFile> Held = {
		{Path(TEXT("Free")), TEXT("edited")},
		{Path(TEXT("Locked")), TEXT("edited")},
		{Path(TEXT("New")), TEXT("added")},
	};

	// A workspace on a server: lock first, write, then mark dirty for the next commit.
	FHyperAIStudioSourceControl::SetLoreForTests(TEXT("python"), FString::Printf(TEXT("-u \"%s\""), *Script), Folder, TEXT("lore://127.0.0.1:41337"));
	TArray<FString> Given;
	TArray<FResult> Results = FHyperAIStudioSourceControl::WriteHeldFiles(Held, [&](const TArray<FString>& Files)
	{
		Given = Files;
		return WriteFiles(Files, Held);
	});
	const FString ServerCalls = Calls();
	const FResult* Free = Find(Results, TEXT("Free"));
	TestTrue(TEXT("Server: a free file is locked and written"), Free && Free->Outcome == TEXT("locked") && Free->bWritten);
	TestTrue(TEXT("with lore lock acquire"), ServerCalls.Contains(TEXT("\"lock\", \"acquire\"")) && ServerCalls.Contains(Normal(Path(TEXT("Free")))));
	const FResult* Locked = Find(Results, TEXT("Locked"));
	TestTrue(TEXT("Server: a file bob has locked stays held, naming him"), Locked && Locked->Outcome == TEXT("blocked") && Locked->Who == TEXT("bob@studio") && !Locked->bWritten);
	TestEqual(TEXT("Two files were written"), Given.Num(), 2);
	const FResult* New = Find(Results, TEXT("New"));
	TestTrue(TEXT("Server: a new file is not locked, but marked dirty"), New && New->Outcome == TEXT("marked_dirty") && New->bWritten);
	FString LockLine;
	for (const FString& Line : TArray<FString>([&ServerCalls]() { TArray<FString> Lines; ServerCalls.ParseIntoArrayLines(Lines); return Lines; }()))
	{
		if (Line.Contains(TEXT("\"lock\"")) && Line.Contains(Normal(Path(TEXT("New")))))
		{
			LockLine = Line;
		}
	}
	TestTrue(TEXT("no lock is taken on a file that is not there yet"), LockLine.IsEmpty());
	TestTrue(TEXT("written files are marked dirty"), ServerCalls.Contains(TEXT("\"dirty\"")) && ServerCalls.Contains(Normal(Path(TEXT("New")))));

	// A local repository: no server, no locks; everything written is marked dirty.
	IFileManager::Get().Delete(*(Folder / TEXT("calls.jsonl")));
	FFileHelper::SaveStringToFile(TEXT("before"), *Path(TEXT("Locked")));
	FHyperAIStudioSourceControl::SetLoreForTests(TEXT("python"), FString::Printf(TEXT("-u \"%s\""), *Script), Folder, FString());
	Results = FHyperAIStudioSourceControl::WriteHeldFiles(Held, [&](const TArray<FString>& Files) { return WriteFiles(Files, Held); });
	const FString LocalCalls = Calls();
	TestFalse(TEXT("Local: nothing is locked"), LocalCalls.Contains(TEXT("\"lock\"")));
	TestTrue(TEXT("Local: every file is written and marked dirty"), Results.Num() == 3 && !Results.ContainsByPredicate([](const FResult& Result)
	{
		return !Result.bWritten || Result.Outcome != TEXT("marked_dirty");
	}));
	TestTrue(TEXT("with lore dirty"), LocalCalls.Contains(TEXT("\"dirty\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSandboxApproveChecksOutTest,
	"HyperAIStudio.Sandbox.ApproveChecksOutFirst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSandboxApproveChecksOutTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Sandbox;
	using namespace HyperAIStudio::SandboxSourceControlTests;
	namespace FSC = UE::FileSandboxCore;
	if (!FSC::IFileSandboxCoreModule::IsAvailable())
	{
		AddError(TEXT("The File Sandbox plugin is not loaded; HyperAIStudio.uplugin enables it."));
		return false;
	}
	FSC::ISandboxManager& Manager = FSC::IFileSandboxCoreModule::Get().GetSandboxManager();
	if (Manager.HasActiveSandbox())
	{
		AddWarning(TEXT("A sandbox is already active in this editor; the test leaves it alone and does not run."));
		return true;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	struct FProbe
	{
		FString PackageName;
		FString ObjectPath;
		FString File;
	};
	const auto MakeProbe = [](const FString& Name)
	{
		FProbe Probe;
		Probe.PackageName = TEXT("/Game/__HyperAIStudioTests/SandboxScm/") + Name;
		Probe.ObjectPath = Probe.PackageName + TEXT(".") + Name;
		Probe.File = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Probe.PackageName, FPackageName::GetAssetPackageExtension()));
		return Probe;
	};
	const auto SaveCurve = [](const FProbe& Probe, const int32 Keys)
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
	};
	const FProbe Free = MakeProbe(TEXT("ScmFree_") + Suffix);
	const FProbe Taken = MakeProbe(TEXT("ScmTaken_") + Suffix);
	FMockProvider Perforce(TEXT("Perforce"));
	TSet<FString> Roots;
	ON_SCOPE_EXIT
	{
		FHyperAIStudioSourceControl::SetProviderForTests(nullptr);
		SetModeForTests(false);
		OnModeChanged();
		if (Manager.HasActiveSandbox())
		{
			const FSC::ISandboxInstance* Active = Manager.GetActiveSandboxInstance();
			Roots.Add(Active ? FString(Active->GetRootDirectory()) : FString());
			Manager.LeaveSandbox();
		}
		for (const FString& Root : Roots)
		{
			if (!Root.IsEmpty())
			{
				Manager.DeleteSandbox(FSC::FDeleteSandboxByDirectoryArgs(Root));
			}
		}
		SetModeForTests(TOptional<bool>());
		TArray<UObject*> Made;
		for (const FProbe* Probe : {&Free, &Taken})
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
		for (const FProbe* Probe : {&Free, &Taken})
		{
			IPlatformFile::GetPlatformPhysical().DeleteFile(*Probe->File);
		}
	};

	// Both assets are in the project and the depot before the sandbox opens; carol has one of them checked out.
	SetModeForTests(false);
	TestTrue(TEXT("The first versions save"), SaveCurve(Free, 2) && SaveCurve(Taken, 2));
	const int64 FreeBefore = IPlatformFile::GetPlatformPhysical().FileSize(*Free.File);
	const int64 TakenBefore = IPlatformFile::GetPlatformPhysical().FileSize(*Taken.File);
	FHyperAIStudioSourceControl::SetProviderForTests(&Perforce);
	Perforce.File(Free.File).bControlled = true;
	Perforce.File(Taken.File).bControlled = true;
	Perforce.File(Taken.File).OtherUser = TEXT("carol");

	SetModeForTests(true);
	FString Error;
	TestTrue(TEXT("The sandbox opens"), CheckAgentEdit(Error));
	if (const FSC::ISandboxInstance* Active = Manager.GetActiveSandboxInstance())
	{
		Roots.Add(Active->GetRootDirectory());
	}
	TestTrue(TEXT("Both edits are held"), SaveCurve(Free, 5) && SaveCurve(Taken, 6));
	TestEqual(TEXT("Two files held"), GetStatus().Changes.Num(), 2);
	TestTrue(TEXT("Nothing was checked out while held"), !Perforce.Log.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("CheckOut")); }));

	TestFalse(TEXT("Approve All writes what it can, and says one stayed"), ApproveAll(Error));
	AddInfo(TEXT("Approve All: ") + Error);
	TestTrue(TEXT("naming who holds it"), Error.Contains(TEXT("carol")));
	TestTrue(TEXT("The free file was checked out"), Perforce.File(Free.File).bCheckedOut);
	TestTrue(TEXT("and written to the project"), IPlatformFile::GetPlatformPhysical().FileSize(*Free.File) != FreeBefore);
	TestEqual(TEXT("Carol's file is untouched in the project"), IPlatformFile::GetPlatformPhysical().FileSize(*Taken.File), TakenBefore);
	FStatus Status = GetStatus();
	TestTrue(TEXT("and stays held"), Status.State == EState::Active && Status.Changes.Num() == 1
		&& FPaths::GetBaseFilename(Status.Changes[0].Path) == FPaths::GetBaseFilename(Taken.File));
	const TArray<FFileOutcome>& Outcomes = GetLastOutcomes();
	const FFileOutcome* FreeOutcome = Outcomes.FindByPredicate([&Free](const FFileOutcome& Each) { return FPaths::GetBaseFilename(Each.Path) == FPaths::GetBaseFilename(Free.File); });
	const FFileOutcome* TakenOutcome = Outcomes.FindByPredicate([&Taken](const FFileOutcome& Each) { return FPaths::GetBaseFilename(Each.Path) == FPaths::GetBaseFilename(Taken.File); });
	TestTrue(TEXT("The card shows the free file checked out"), FreeOutcome && FreeOutcome->Outcome == TEXT("checked_out") && FreeOutcome->bWritten);
	TestTrue(TEXT("and the other held by carol"), TakenOutcome && TakenOutcome->Outcome == TEXT("blocked") && TakenOutcome->Who == TEXT("carol"));
	const FString CardShot = HyperAIStudio::Tests::SaveWidgetShot(SNew(SHyperAIStudioChatActivitySidebar), FIntPoint(340, 640), TEXT("sandbox-card-checkout"));
	AddInfo(TEXT("Sandbox card: ") + (CardShot.IsEmpty() ? FString(TEXT("not rendered here")) : CardShot));

	// Carol checks it in; approving that one file now checks it out and writes it.
	Perforce.File(Taken.File).OtherUser.Reset();
	TestTrue(TEXT("Approving the one file goes through"), Status.Changes.Num() == 1 && ApproveFiles({Status.Changes[0].Path}, Error));
	TestTrue(TEXT("checked out first"), Perforce.File(Taken.File).bCheckedOut);
	TestTrue(TEXT("and written"), IPlatformFile::GetPlatformPhysical().FileSize(*Taken.File) != TakenBefore);
	TestEqual(TEXT("Nothing is held"), GetStatus().Changes.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
