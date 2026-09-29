// Games by Hyper 2026.

#include "HyperAIStudioSourceControl.h"

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSourceControlToolset.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioSourceControl, Log, All);

namespace HyperAIStudio::SourceControl::Private
{
	constexpr const TCHAR* PackId = TEXT("source_control");
	constexpr const TCHAR* CohortId = TEXT("cohort.source.hyperaistudiosourcecontroltoolset.v1");
	constexpr const TCHAR* StatusToolName = TEXT("hyper_source_control_status");
	constexpr const TCHAR* SubmitToolName = TEXT("hyper_source_control_submit");
	constexpr const TCHAR* QualifiedToolset = TEXT("HyperAIStudio.HyperAIStudioSourceControlToolset");
	constexpr int32 MaxOutputCharacters = 4000;

	struct FState
	{
		/** Packages agents made editable: their saves are the ones whose new files are added. */
		TSet<FString> AgentPackages;
		TSet<FString> AgentFiles;
		/** HyperAI's changelist this session, for a provider that uses them. */
		FSourceControlChangelistPtr Changelist;
		TArray<FString> PendingAdds;
		FDelegateHandle Saved;
		FDelegateHandle PostEngineInit;
		bool bOwnsToolset = false;
		FString LoreExecutable;
		FString LorePrefix;
		FString LoreRoot;
	};

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	FString ProjectRelative(const FString& Path)
	{
		FString Relative = Path;
		FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return Relative;
	}

	FString Filename(const FString& PackageName)
	{
		FString File;
		if (!FPackageName::DoesPackageExist(PackageName, &File))
		{
			File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		}
		return FPaths::ConvertRelativePathToFull(File);
	}

	/** Unreal's provider when one is set up; its plugin covers Perforce, Plastic, Git and Lore's own Unreal plugin. */
	ISourceControlProvider* UnrealProvider()
	{
		ISourceControlModule& Module = ISourceControlModule::Get();
		return Module.IsEnabled() && Module.GetProvider().GetName() != FName(TEXT("None")) ? &Module.GetProvider() : nullptr;
	}

	/** A Lore workspace around the project, and the lore command, when no Unreal provider is set up. */
	bool FindLore(FString& OutExecutable, FString& OutPrefix, FString& OutRoot)
	{
		const FState& Current = State();
		if (!Current.LoreExecutable.IsEmpty())
		{
			OutExecutable = Current.LoreExecutable;
			OutPrefix = Current.LorePrefix;
			OutRoot = Current.LoreRoot;
			return true;
		}
		// ponytail: looked up on each use; a workspace appears seldom enough that caching buys nothing.
		for (FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()); !Folder.IsEmpty(); Folder = FPaths::GetPath(Folder))
		{
			Folder.RemoveFromEnd(TEXT("/"));
			if (IFileManager::Get().DirectoryExists(*(Folder / TEXT(".lore"))))
			{
				OutRoot = Folder;
				break;
			}
			if (FPaths::GetPath(Folder) == Folder)
			{
				break;
			}
		}
		OutPrefix.Reset();
		return !OutRoot.IsEmpty() && FHyperAIStudioService::FindExecutableOnPath(TEXT("lore"), OutExecutable);
	}

	/** One lore command, never asking anything; OutText is what it said. */
	bool RunLore(const FString& Arguments, FString& OutText)
	{
		FString Executable;
		FString Prefix;
		FString Root;
		if (!FindLore(Executable, Prefix, Root))
		{
			OutText = TEXT("No Lore workspace or lore command was found.");
			return false;
		}
		int32 Code = -1;
		FString Out;
		FString Err;
		const FString Params = FString::Printf(TEXT("%s%s--repository \"%s\" --non-interactive %s"), *Prefix, Prefix.IsEmpty() ? TEXT("") : TEXT(" "), *Root, *Arguments);
		FPlatformProcess::ExecProcess(*Executable, *Params, &Code, &Out, &Err, *Root);
		OutText = (Out + TEXT("\n") + Err).TrimStartAndEnd().Right(MaxOutputCharacters);
		UE_LOG(LogHyperAIStudioSourceControl, Log, TEXT("lore %s -> %d"), *Arguments, Code);
		return Code == 0;
	}

	FString Quoted(const TArray<FString>& Files)
	{
		TArray<FString> Parts;
		for (const FString& File : Files)
		{
			Parts.Add(TEXT("\"") + File + TEXT("\""));
		}
		return FString::Join(Parts, TEXT(" "));
	}

	FString Errors(const FSourceControlOperationRef& Operation)
	{
		TArray<FString> Lines;
		for (const FText& Message : Operation->GetResultInfo().ErrorMessages)
		{
			Lines.Add(Message.ToString());
		}
		return FString::Join(Lines, TEXT(" ")).Left(MaxOutputCharacters);
	}

	/** One changelist for the session's agent edits, kept apart from the user's own work. */
	FSourceControlChangelistPtr Changelist(ISourceControlProvider& Provider)
	{
		FState& Current = State();
		if (!Current.Changelist.IsValid() && Provider.UsesChangelists())
		{
			const TSharedRef<FNewChangelist> New = ISourceControlOperation::Create<FNewChangelist>();
			New->SetDescription(FText::FromString(FString::Printf(TEXT("HyperAI Studio: agent edits, %s"), *FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M")))));
			if (Provider.Execute(New) == ECommandResult::Succeeded)
			{
				Current.Changelist = New->GetNewChangelist();
			}
		}
		return Current.Changelist;
	}

	FHyperAIStudioSourceControlFileState ReadState(const FSourceControlStatePtr& State)
	{
		FHyperAIStudioSourceControlFileState Out;
		if (State.IsValid())
		{
			Out.bSourceControlled = State->IsSourceControlled();
			Out.bCheckedOutOther = State->IsCheckedOutOther(&Out.Who);
			Out.bCurrent = State->IsCurrent();
			Out.bDeleted = State->IsDeleted();
			Out.bCanCheckOut = State->CanCheckout();
		}
		return Out;
	}

	/** New files agents saved: marked for add, or in Lore marked dirty so a commit picks them up. */
	bool AddPending(float)
	{
		TArray<FString> Files = MoveTemp(State().PendingAdds);
		State().PendingAdds.Reset();
		if (Files.IsEmpty())
		{
			return false;
		}
		if (ISourceControlProvider* Provider = UnrealProvider())
		{
			Provider->Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files);
			TArray<FString> Add;
			for (const FString& File : Files)
			{
				const FSourceControlStatePtr FileState = Provider->GetState(File, EStateCacheUsage::Use);
				if (FileState.IsValid() && !FileState->IsSourceControlled() && FileState->CanAdd())
				{
					Add.Add(File);
				}
			}
			if (!Add.IsEmpty())
			{
				const FSourceControlOperationRef MarkForAdd = ISourceControlOperation::Create<FMarkForAdd>();
				const FSourceControlChangelistPtr List = Changelist(*Provider);
				const ECommandResult::Type Result = List.IsValid() ? Provider->Execute(MarkForAdd, List, Add) : Provider->Execute(MarkForAdd, Add);
				UE_LOG(LogHyperAIStudioSourceControl, Log, TEXT("Marked %d new file(s) for add: %s"), Add.Num(),
					Result == ECommandResult::Succeeded ? TEXT("done") : *Errors(MarkForAdd));
			}
		}
		else
		{
			FString Text;
			RunLore(TEXT("dirty ") + Quoted(Files), Text);
		}
		return false;
	}

	void OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext)
	{
		FState& Current = State();
		if (!Package || !Current.AgentPackages.Contains(Package->GetName()))
		{
			return;
		}
		const FString File = FPaths::ConvertRelativePathToFull(PackageFilename);
		Current.AgentFiles.Add(File);
		// Not during the save itself: source control is asked on the next tick.
		if (Current.PendingAdds.IsEmpty())
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&AddPending));
		}
		Current.PendingAdds.AddUnique(File);
	}

	void RecordOutcome(const FString& OperationId, const bool bOk, const FString& Detail)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = PackId;
		Entry.ToolName = SubmitToolName;
		Entry.OperationId = OperationId;
		Entry.StatusCode = bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Detail.Left(MaxOutputCharacters);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	/** Checks the files in with Description, or in Lore stages, commits and pushes them, then lets their locks go. */
	void Submit(const FString& OperationId, const TArray<FString>& Files, const FString& Description)
	{
		FString Text;
		if (ISourceControlProvider* Provider = UnrealProvider())
		{
			const TSharedRef<FCheckIn> CheckIn = ISourceControlOperation::Create<FCheckIn>();
			CheckIn->SetDescription(FText::FromString(Description));
			const FSourceControlChangelistPtr List = State().Changelist;
			const bool bOk = (List.IsValid() ? Provider->Execute(CheckIn, List, Files) : Provider->Execute(CheckIn, Files)) == ECommandResult::Succeeded;
			Text = bOk ? CheckIn->GetSuccessMessage().ToString() : Errors(CheckIn);
			if (bOk)
			{
				State().Changelist.Reset();
				for (const FString& File : Files)
				{
					State().AgentFiles.Remove(File);
				}
			}
			RecordOutcome(OperationId, bOk, FString::Printf(TEXT("%s %d file(s): %s"), bOk ? TEXT("Submitted") : TEXT("Could not submit"), Files.Num(), *Text));
			return;
		}
		const FString List = Quoted(Files);
		FString Step;
		bool bOk = RunLore(TEXT("stage ") + List, Step);
		Text = Step;
		bOk = bOk && RunLore(FString::Printf(TEXT("commit \"%s\""), *Description.Replace(TEXT("\""), TEXT("'"))), Step);
		Text += TEXT("\n") + Step;
		// A workspace with no server to push to has committed locally all the same.
		const bool bPushed = bOk && RunLore(TEXT("push"), Step);
		Text += TEXT("\n") + Step;
		if (bOk)
		{
			RunLore(TEXT("lock release ") + List, Step);
			for (const FString& File : Files)
			{
				State().AgentFiles.Remove(File);
			}
		}
		RecordOutcome(OperationId, bOk, FString::Printf(TEXT("%s %d file(s) in Lore%s. %s"), bOk ? TEXT("Committed") : TEXT("Could not commit"), Files.Num(),
			bOk && !bPushed ? TEXT(", but the push did not go through (it stays committed here)") : TEXT(""), *Text.TrimStartAndEnd()));
	}

	void RegisterToolset()
	{
		FState& Current = State();
		if (Current.bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
		{
			return;
		}
		if (!FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, CohortId, {StatusToolName, SubmitToolName},
			FHyperAIStudioExtensionRuntime::AreSourceCandidateToolsEnabled()))
		{
			UE_LOG(LogHyperAIStudioSourceControl, Verbose, TEXT("The source control tools are not admitted here (Extended Hyper Tools are off)."));
			return;
		}
		FString Error;
		Current.bOwnsToolset = FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioSourceControlToolset::StaticClass(), QualifiedToolset, Error);
		if (!Current.bOwnsToolset)
		{
			UE_LOG(LogHyperAIStudioSourceControl, Error, TEXT("The source control tools were not registered: %s"), *Error);
		}
	}
}

FHyperAIStudioSourceControl::EDecision FHyperAIStudioSourceControl::Decide(const FHyperAIStudioSourceControlFileState& FileState, const FString& DisplayName, FString& OutError)
{
	if (FileState.bCheckedOutOther)
	{
		OutError = FString::Printf(TEXT("%s is checked out or locked by %s. Ask them to check it in, or work on another asset."), *DisplayName,
			FileState.Who.IsEmpty() ? TEXT("someone else") : *FileState.Who);
		return EDecision::Refuse;
	}
	if (FileState.bSourceControlled && !FileState.bCurrent)
	{
		OutError = FString::Printf(TEXT("%s is behind the latest revision. Get the latest version first, so the edit does not overwrite a teammate's."), *DisplayName);
		return EDecision::Refuse;
	}
	if (FileState.bDeleted)
	{
		OutError = FString::Printf(TEXT("%s is marked for delete in source control."), *DisplayName);
		return EDecision::Refuse;
	}
	return FileState.bCanCheckOut ? EDecision::CheckOut : EDecision::Edit;
}

bool FHyperAIStudioSourceControl::MakeEditable(const TArray<FString>& PackageNames, FString& OutError)
{
	using namespace HyperAIStudio::SourceControl::Private;
	ISourceControlProvider* Provider = UnrealProvider();
	FString Executable;
	FString Prefix;
	FString Root;
	// A project without source control records nothing, so its saves never reach for a provider.
	if (!GetDefault<UHyperAIStudioSettings>()->bCheckOutBeforeAgentEdits || (!Provider && !FindLore(Executable, Prefix, Root)))
	{
		return true;
	}
	FState& Current = State();
	TArray<FString> Files;
	for (const FString& PackageName : PackageNames)
	{
		if (PackageName.StartsWith(TEXT("/Game/")) && FPackageName::IsValidLongPackageName(PackageName))
		{
			Current.AgentPackages.Add(PackageName);
			Files.Add(Filename(PackageName));
		}
	}
	if (Files.IsEmpty())
	{
		return true;
	}
	if (Provider)
	{
		if (!Provider->IsAvailable())
		{
			OutError = FString::Printf(TEXT("%s is set up but not connected, so the asset cannot be checked out. Connect it in the editor's Revision Control menu, ")
				TEXT("or turn off Check Out Before Agent Edits."), *Provider->GetName().ToString());
			return false;
		}
		Provider->Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files);
		TArray<FString> CheckOut;
		for (const FString& File : Files)
		{
			switch (Decide(ReadState(Provider->GetState(File, EStateCacheUsage::Use)), ProjectRelative(File), OutError))
			{
			case EDecision::Refuse: return false;
			case EDecision::CheckOut: CheckOut.Add(File); break;
			default: break;
			}
		}
		if (!CheckOut.IsEmpty())
		{
			const FSourceControlOperationRef Operation = ISourceControlOperation::Create<FCheckOut>();
			const FSourceControlChangelistPtr List = Changelist(*Provider);
			if ((List.IsValid() ? Provider->Execute(Operation, List, CheckOut) : Provider->Execute(Operation, CheckOut)) != ECommandResult::Succeeded)
			{
				OutError = FString::Printf(TEXT("%s could not check out %s: %s"), *Provider->GetName().ToString(), *ProjectRelative(CheckOut[0]), *Errors(Operation));
				return false;
			}
		}
		Current.AgentFiles.Append(CheckOut);
		return true;
	}
	// Lore's locks are advisory: taking one tells the team, and one a teammate holds turns the edit away.
	TArray<FString> Existing = Files.FilterByPredicate([](const FString& File) { return FPaths::FileExists(File); });
	FString Text;
	if (!Existing.IsEmpty() && !RunLore(TEXT("lock acquire ") + Quoted(Existing), Text))
	{
		OutError = FString::Printf(TEXT("Lore could not lock %s for this edit: %s"), *ProjectRelative(Existing[0]), *Text.Left(600));
		return false;
	}
	Current.AgentFiles.Append(Existing);
	return true;
}

FString FHyperAIStudioSourceControl::DescribeBackend()
{
	using namespace HyperAIStudio::SourceControl::Private;
	if (const ISourceControlProvider* Provider = UnrealProvider())
	{
		return TEXT("unreal: ") + Provider->GetName().ToString();
	}
	FString Executable;
	FString Prefix;
	FString Root;
	return FindLore(Executable, Prefix, Root) ? TEXT("lore: ") + Root : FString(TEXT("none"));
}

TArray<FString> FHyperAIStudioSourceControl::GetAgentFiles()
{
	return HyperAIStudio::SourceControl::Private::State().AgentFiles.Array();
}

void FHyperAIStudioSourceControl::Startup()
{
	using namespace HyperAIStudio::SourceControl::Private;
	FState& Current = State();
	Current.Saved = UPackage::PackageSavedWithContextEvent.AddStatic(&OnPackageSaved);
	Current.PostEngineInit = FCoreDelegates::GetOnPostEngineInit().AddStatic(&RegisterToolset);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterToolset();
	}
}

void FHyperAIStudioSourceControl::Shutdown()
{
	using namespace HyperAIStudio::SourceControl::Private;
	FState& Current = State();
	UPackage::PackageSavedWithContextEvent.Remove(Current.Saved);
	FCoreDelegates::GetOnPostEngineInit().Remove(Current.PostEngineInit);
	if (Current.bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioSourceControlToolset::StaticClass(), QualifiedToolset, Error);
	}
	Current.bOwnsToolset = false;
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioSourceControl::SetLoreForTests(const FString& Executable, const FString& Prefix, const FString& Root)
{
	using namespace HyperAIStudio::SourceControl::Private;
	State().LoreExecutable = Executable;
	State().LorePrefix = Prefix;
	State().LoreRoot = Root;
	State().AgentFiles.Reset();
	State().AgentPackages.Reset();
}
#endif

FHyperAISourceControlStatusReport UHyperAIStudioSourceControlToolset::hyper_source_control_status(const FHyperAISourceControlStatusRequest& Request)
{
	using namespace HyperAIStudio::SourceControl::Private;
	FHyperAISourceControlStatusReport Report;
	Report.Backend = FHyperAIStudioSourceControl::DescribeBackend();
	const TArray<FString> Files = FHyperAIStudioSourceControl::GetAgentFiles();
	if (ISourceControlProvider* Provider = UnrealProvider())
	{
		if (Request.bRefresh && !Files.IsEmpty())
		{
			Provider->Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files);
		}
		for (const FString& File : Files)
		{
			const FSourceControlStatePtr State = Provider->GetState(File, EStateCacheUsage::Use);
			FHyperAISourceControlFile& Entry = Report.Files.AddDefaulted_GetRef();
			Entry.Path = ProjectRelative(File);
			Entry.State = !State.IsValid() ? TEXT("unknown") : State->IsCheckedOutOther(&Entry.Who) ? TEXT("checked_out_by_other")
				: State->IsAdded() ? TEXT("added") : State->IsCheckedOut() ? TEXT("checked_out") : !State->IsCurrent() ? TEXT("not_current")
				: State->IsModified() ? TEXT("modified") : TEXT("current");
		}
	}
	else
	{
		for (const FString& File : Files)
		{
			Report.Files.Add({ProjectRelative(File), TEXT("locked"), FString()});
		}
		if (Report.Backend.StartsWith(TEXT("lore")))
		{
			RunLore(TEXT("status"), Report.LoreStatus);
		}
	}
	Report.bOk = true;
	Report.Diagnostic = Report.Backend == TEXT("none")
		? TEXT("The project has no source control set up in Unreal and is not in a Lore workspace; agent edits are saved as they are.")
		: FString::Printf(TEXT("%d file(s) changed by agents this session. hyper_source_control_submit checks them in once the user approves."), Report.Files.Num());
	return Report;
}

FHyperAISourceControlSubmitReport UHyperAIStudioSourceControlToolset::hyper_source_control_submit(const FHyperAISourceControlSubmitRequest& Request)
{
	using namespace HyperAIStudio::SourceControl::Private;
	FHyperAISourceControlSubmitReport Report;
	Report.Backend = FHyperAIStudioSourceControl::DescribeBackend();
	auto Refuse = [&Report](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.Status = Status;
		Report.Diagnostic = Diagnostic;
		return Report;
	};
	if (Report.Backend == TEXT("none"))
	{
		return Refuse(TEXT("no_source_control"), TEXT("The project has no source control set up in Unreal and is not in a Lore workspace."));
	}
	const FString Description = Request.Description.TrimStartAndEnd();
	if (Description.IsEmpty() || Description.Len() > 2000)
	{
		return Refuse(TEXT("invalid_request"), TEXT("description says what the change does, in up to 2000 characters."));
	}
	TArray<FString> Files;
	const TArray<FString> Changed = FHyperAIStudioSourceControl::GetAgentFiles();
	for (const FString& Path : Request.Files)
	{
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::ProjectDir() / Path : Path);
		if (!Changed.Contains(File))
		{
			return Refuse(TEXT("not_an_agent_file"), FString::Printf(TEXT("%s is not a file agents changed this session; submit only those, and leave the user's own work to them."), *Path));
		}
		Files.Add(File);
	}
	Files = Files.IsEmpty() ? Changed : Files;
	if (Files.IsEmpty())
	{
		return Refuse(TEXT("nothing_to_submit"), TEXT("Agents changed no files under source control this session."));
	}
	for (const FString& File : Files)
	{
		Report.Files.Add(ProjectRelative(File));
	}
	Report.OperationId = Request.OperationId.IsEmpty() ? TEXT("submit-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower() : Request.OperationId;
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Report.OperationId))
	{
		return Refuse(TEXT("invalid_operation_id"), TEXT("operation_id must be a short journal-safe id, or empty to have one made."));
	}
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("Would submit %d file(s) through %s as \"%s\". Nothing was sent; resubmit with bDryRun false to ask the user."),
			Files.Num(), *Report.Backend, *Description.Left(200));
		return Report;
	}
	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = PackId;
	Summary.ToolName = SubmitToolName;
	Summary.VariantId = TEXT("source_control.submit.v1");
	Summary.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
	Summary.EffectTarget = Report.Backend;
	Summary.Effects = {FString::Printf(TEXT("Submit %d file(s) through %s: \"%s\""), Files.Num(), *Report.Backend, *Description.Left(300))};
	Summary.Touches = Report.Files;
	FString Error;
	const FString OperationId = Report.OperationId;
	if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [OperationId, Files, Description]() { Submit(OperationId, Files, Description); }, Error))
	{
		return Refuse(TEXT("approval_not_queued"), Error);
	}
	Report.bOk = true;
	Report.Status = TEXT("awaiting_user_approval");
	Report.Diagnostic = TEXT("Waiting for the user's approval in HyperAI Chat, Activity; nothing was sent. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
	return Report;
}
