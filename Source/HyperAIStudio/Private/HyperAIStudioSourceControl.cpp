// Games by Hyper 2026.

#include "HyperAIStudioSourceControl.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "Features/IModularFeatures.h"
#include "HyperAIStudioLore.h"
#include "HyperAIStudioSandbox.h"
#include "HyperAIStudioSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "SourceControlHelpers.h"
#include "HyperAIStudioSourceControlToolset.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/StringOutputDevice.h"
#include "SourceControlOperations.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

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
		/** A Lore submit's commands are running on a worker thread. */
		bool bSubmitting = false;
		FString LoreExecutable;
		FString LorePrefix;
		FString LoreRoot;
		FString LoreRemoteUrl;
		/** Tests: a mock provider standing in for Unreal's. */
		ISourceControlProvider* ProviderForTests = nullptr;
	};

	/** The lore command and the workspace a call runs in: found on the game thread, usable on any. */
	struct FLoreTarget
	{
		FString Executable;
		FString Prefix;
		FString Root;
		/** The workspace's server; empty for a local repository, and when its config could not be read. */
		FString RemoteUrl;
		EHyperAIStudioLoreMode Mode = EHyperAIStudioLoreMode::None;
		/** Only a repository whose config says it has no server: a workspace HyperAI cannot read takes locks and pushes. */
		bool IsLocal() const { return Mode == EHyperAIStudioLoreMode::Local; }
	};

	/** Timeouts: agent edits wait on a lock and a dirty mark on the game thread, so those stay short. */
	constexpr double LockSeconds = 20.0;
	constexpr double DirtySeconds = 30.0;
	constexpr double StatusSeconds = 30.0;
	constexpr double SignInSeconds = 10.0;
	/** A submit runs on a worker thread; staging and committing a large change take a while, a push at most this. */
	constexpr double StageSeconds = 1800.0;
	constexpr double PushSeconds = 600.0;

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

	/**
	 * Unreal's provider when one is set up; its plugins cover Perforce, Subversion, Plastic, Git and Lore's own Unreal
	 * plugin. While a File Sandbox is open the global provider is Epic's proxy ("File Sandbox"), which answers check-outs
	 * and adds without doing them until the sandbox is persisted; the provider configured for the project is the real one.
	 */
	ISourceControlProvider* UnrealProvider()
	{
		if (State().ProviderForTests)
		{
			return State().ProviderForTests;
		}
		ISourceControlModule& Module = ISourceControlModule::Get();
		if (!Module.IsEnabled())
		{
			return nullptr;
		}
		ISourceControlProvider& Active = Module.GetProvider();
		static const FName None(TEXT("None"));
		static const FName SandboxProxy(TEXT("File Sandbox"));
		if (Active.GetName() == None)
		{
			return nullptr;
		}
		if (Active.GetName() != SandboxProxy)
		{
			return &Active;
		}
		FString Configured;
		GConfig->GetString(TEXT("SourceControl.SourceControlSettings"), TEXT("Provider"), Configured, SourceControlHelpers::GetSettingsIni());
		FParse::Value(FCommandLine::Get(), TEXT("SCCProvider="), Configured);
		if (Configured.IsEmpty() || FName(*Configured) == None)
		{
			return nullptr;
		}
		for (ISourceControlProvider* Provider : IModularFeatures::Get().GetModularFeatureImplementations<ISourceControlProvider>(TEXT("SourceControl")))
		{
			if (Provider && Provider != &Active && Provider->GetName() == FName(*Configured))
			{
				return Provider;
			}
		}
		return nullptr;
	}

	/** HyperAI's sandbox holds what is saved: check-outs, locks and adds wait for the user's approval. */
	bool IsDeferredToSandbox()
	{
		return HyperAIStudio::Sandbox::IsHoldingEdits();
	}

	/** Who a lore refusal names: "... is locked by bob@studio". */
	FString LockHolder(const FString& Text)
	{
		for (const TCHAR* Marker : {TEXT("locked by "), TEXT("held by "), TEXT("owned by ")})
		{
			const int32 At = Text.Find(Marker, ESearchCase::IgnoreCase);
			if (At != INDEX_NONE)
			{
				FString Rest = Text.Mid(At + FCString::Strlen(Marker));
				int32 End = 0;
				while (End < Rest.Len() && !FChar::IsWhitespace(Rest[End]) && Rest[End] != TEXT(',') && Rest[End] != TEXT(')'))
				{
					++End;
				}
				Rest.LeftInline(End);
				Rest.RemoveFromEnd(TEXT("."));
				return Rest;
			}
		}
		return FString();
	}

	/**
	 * A Lore workspace around the project, and the lore command, when no Unreal provider is set up. Out.Root and Out.Mode
	 * are filled even when the command is missing. Game thread.
	 */
	bool FindLore(FLoreTarget& Out)
	{
		const FState& Current = State();
		if (!Current.LoreExecutable.IsEmpty())
		{
			Out.Executable = Current.LoreExecutable;
			Out.Prefix = Current.LorePrefix;
			Out.Root = Current.LoreRoot;
			Out.RemoteUrl = Current.LoreRemoteUrl;
			Out.Mode = Current.LoreRemoteUrl.IsEmpty() ? EHyperAIStudioLoreMode::Local : EHyperAIStudioLoreMode::Remote;
			return true;
		}
		// ponytail: looked up on each use; a workspace appears seldom enough that caching buys nothing.
		const FHyperAIStudioLoreWorkspace Workspace = FHyperAIStudioLore::FindWorkspace(FPaths::ProjectDir());
		Out.Root = Workspace.Root;
		Out.RemoteUrl = Workspace.RemoteUrl.TrimStartAndEnd();
		Out.Mode = Workspace.Mode;
		Out.Prefix.Reset();
		return !Out.Root.IsEmpty() && FHyperAIStudioLore::FindExecutable(Out.Executable);
	}

	/** A Lore workspace whose config says its repository is on this computer only (made with --offline). */
	bool IsLocalLore()
	{
		FLoreTarget Lore;
		return FindLore(Lore) && Lore.IsLocal();
	}

	/** One lore command in Lore's workspace, never asking anything, stopped after TimeoutSeconds; OutText is what it said. Any thread. */
	bool RunLore(const FLoreTarget& Lore, const FString& Arguments, FString& OutText, const double TimeoutSeconds)
	{
		const FString Params = FString::Printf(TEXT("%s%s--repository %s --no-pager --non-interactive %s"), *Lore.Prefix, Lore.Prefix.IsEmpty() ? TEXT("") : TEXT(" "),
			*FHyperAIStudioLore::JoinArguments({Lore.Root}), *Arguments);
		int32 Code = -1;
		FHyperAIStudioLore::RunProcess(Lore.Executable, Params, Lore.Root, TimeoutSeconds, OutText, &Code);
		OutText = OutText.Right(MaxOutputCharacters);
		UE_LOG(LogHyperAIStudioSourceControl, Log, TEXT("lore %s -> %d"), *Arguments.Left(300), Code);
		return Code == 0;
	}

	/** RunLore in the project's workspace. Game thread. */
	bool RunLore(const FString& Arguments, FString& OutText, const double TimeoutSeconds)
	{
		FLoreTarget Lore;
		if (!FindLore(Lore))
		{
			OutText = TEXT("No Lore workspace or lore command was found.");
			return false;
		}
		return RunLore(Lore, Arguments, OutText, TimeoutSeconds);
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
			RunLore(TEXT("dirty ") + Quoted(Files), Text, DirtySeconds);
		}
		return false;
	}

	void OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext)
	{
		FState& Current = State();
		// A save into HyperAI's sandbox is not in the project yet; approving it adds and marks what it writes.
		if (!Package || !Current.AgentPackages.Contains(Package->GetName()) || IsDeferredToSandbox())
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

	/** Stages, commits and (on a server) pushes the files, then lets their locks go; the detail for the activity entry. Any thread. */
	bool CommitInLore(const FLoreTarget& Lore, const TArray<FString>& Files, const FString& Description, FString& OutDetail)
	{
		const FString List = Quoted(Files);
		const bool bLocal = Lore.IsLocal();
		FString Step;
		bool bOk = RunLore(Lore, TEXT("stage ") + List, Step, StageSeconds);
		FString Text = Step;
		// A message that starts with '-' follows "--", so lore never reads it as an option.
		bOk = bOk && RunLore(Lore, FString::Printf(TEXT("commit %s%s"), Description.StartsWith(TEXT("-")) ? TEXT("-- ") : TEXT(""),
			*FHyperAIStudioLore::JoinArguments({Description.Replace(TEXT("\""), TEXT("'"))})), Step, StageSeconds);
		Text += TEXT("\n") + Step;
		// A local repository has no server: its commit is the whole submit. One on a server that does not answer has
		// committed here all the same, and pushes later.
		const bool bPushed = bOk && !bLocal && RunLore(Lore, TEXT("push"), Step, PushSeconds);
		if (bOk && !bLocal)
		{
			Text += TEXT("\n") + Step;
			RunLore(Lore, TEXT("lock release ") + List, Step, LockSeconds);
		}
		OutDetail = FString::Printf(TEXT("%s %d file(s) in %s%s. %s"), bOk ? TEXT("Committed") : TEXT("Could not commit"), Files.Num(),
			bLocal ? TEXT("the local Lore repository") : TEXT("Lore"),
			bOk && !bLocal && !bPushed ? TEXT(", but the push did not go through (it stays committed here)") : TEXT(""), *Text.TrimStartAndEnd());
		return bOk;
	}

	/**
	 * Checks the files in with Description. Lore's commands run on a worker thread, so a large commit or a slow server
	 * never stops the editor; the outcome is recorded when they finish, which is what hyper_operation_wait waits for.
	 */
	void Submit(const FString& OperationId, const TArray<FString>& Files, const FString& Description)
	{
		if (ISourceControlProvider* Provider = UnrealProvider())
		{
			const TSharedRef<FCheckIn> CheckIn = ISourceControlOperation::Create<FCheckIn>();
			CheckIn->SetDescription(FText::FromString(Description));
			const FSourceControlChangelistPtr List = State().Changelist;
			const bool bOk = (List.IsValid() ? Provider->Execute(CheckIn, List, Files) : Provider->Execute(CheckIn, Files)) == ECommandResult::Succeeded;
			const FString Text = bOk ? CheckIn->GetSuccessMessage().ToString() : Errors(CheckIn);
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
		FLoreTarget Lore;
		if (!FindLore(Lore))
		{
			RecordOutcome(OperationId, false, TEXT("Could not commit: no Lore workspace or lore command was found."));
			return;
		}
		if (State().bSubmitting)
		{
			RecordOutcome(OperationId, false, TEXT("Could not commit: another submit is still running. Submit again once it has finished."));
			return;
		}
		State().bSubmitting = true;
		Async(EAsyncExecution::Thread, [OperationId, Files, Description, Lore]()
		{
			FString Detail;
			const bool bOk = CommitInLore(Lore, Files, Description, Detail);
			AsyncTask(ENamedThreads::GameThread, [OperationId, Files, bOk, Detail]()
			{
				FState& Current = State();
				Current.bSubmitting = false;
				if (bOk)
				{
					for (const FString& File : Files)
					{
						Current.AgentFiles.Remove(File);
					}
				}
				RecordOutcome(OperationId, bOk, Detail);
			});
		});
	}

	const TCHAR* ModeName(const EHyperAIStudioLoreMode Mode)
	{
		switch (Mode)
		{
		case EHyperAIStudioLoreMode::Local: return TEXT("local");
		case EHyperAIStudioLoreMode::Remote: return TEXT("remote");
		case EHyperAIStudioLoreMode::Unknown: return TEXT("unknown");
		default: return TEXT("none");
		}
	}

	/**
	 * Lore for the status report: the command, the project's workspace and its mode, the server, and who is signed in.
	 * Nothing is started for a project Unreal's provider looks after, the command's version is read once per install, and
	 * sign-ins are read only for a server repository, with the same command (and, in tests, the same stand-in) agent edits use.
	 */
	void DescribeLore(FHyperAISourceControlLoreStatus& Out)
	{
		const FString SetupPlace = TEXT("HyperAI Studio > Settings > Version control");
		Out.ConfiguredServer = GetDefault<UHyperAIStudioSettings>()->LoreServerUrl.TrimStartAndEnd();
		FLoreTarget Lore;
		const bool bLore = FindLore(Lore);
		Out.Workspace = Lore.Root;
		Out.Server = Lore.RemoteUrl;
		Out.Mode = Lore.Root.IsEmpty() ? TEXT("none") : ModeName(Lore.Mode);
		if (Lore.Root.IsEmpty() && UnrealProvider())
		{
			// Unreal's provider is the project's source control; Lore is not needed, and nothing is run to ask about it.
			Out.bCliFound = FHyperAIStudioLore::FindExecutable(Out.CliPath);
			return;
		}
		const FHyperAIStudioLoreCli Cli = FHyperAIStudioLore::DetectCli();
		Out.bCliFound = Cli.bFound;
		Out.CliPath = Cli.Path;
		Out.CliVersion = Cli.Version;
		if (!Cli.bFound && !bLore)
		{
			Out.Next = FString::Printf(TEXT("The lore command is not installed: the user installs it (%s), then sets Lore up in %s."), FHyperAIStudioLore::InstallGuideUrl, *SetupPlace);
		}
		else if (Lore.Root.IsEmpty())
		{
			Out.Next = FString::Printf(TEXT("The project is not in a Lore workspace: the user can set one up in %s (a local repository needs no server)."), *SetupPlace);
		}
		else if (Lore.Mode == EHyperAIStudioLoreMode::Local && !Out.ConfiguredServer.IsEmpty())
		{
			Out.Next = FString::Printf(TEXT("The Lore Server setting names %s, but this repository is local: Lore fixes the server when a repository is made."), *Out.ConfiguredServer);
		}
		else if (Lore.Mode == EHyperAIStudioLoreMode::Unknown)
		{
			Out.Next = TEXT("The workspace's .lore/config.toml could not be read, so it is treated as a server repository: agent edits take locks and a submit pushes.");
		}
		if (bLore && !Lore.IsLocal())
		{
			FString Text;
			if (RunLore(Lore, TEXT("auth list"), Text, SignInSeconds))
			{
				Out.SignedInAs = FHyperAIStudioLore::ParseSignIns(Text);
			}
			if (Out.SignedInAs.IsEmpty() && Lore.Mode == EHyperAIStudioLoreMode::Remote)
			{
				Out.Next = FString::Printf(TEXT("No Lore sign-in is stored. If %s asks for one, the user runs %s in a terminal; agents never sign in."),
					*Out.Server, *FHyperAIStudioLore::LoginCommand(Out.Server));
			}
		}
		if (UnrealProvider() && !Lore.Root.IsEmpty())
		{
			Out.Next += FString::Printf(TEXT("%sAgent edits go through Unreal's %s provider, not the lore command."), Out.Next.IsEmpty() ? TEXT("") : TEXT(" "),
				*UnrealProvider()->GetName().ToString());
		}
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

ISourceControlProvider* FHyperAIStudioSourceControl::GetProvider()
{
	return HyperAIStudio::SourceControl::Private::UnrealProvider();
}

TArray<FHyperAIStudioSourceControl::FHeldFileResult> FHyperAIStudioSourceControl::WriteHeldFiles(const TArray<FHeldFile>& Files,
	TFunctionRef<TSet<FString>(const TArray<FString>&)> Write)
{
	using namespace HyperAIStudio::SourceControl::Private;
	TArray<FHeldFileResult> Results;
	for (const FHeldFile& File : Files)
	{
		FHeldFileResult& Result = Results.AddDefaulted_GetRef();
		Result.Path = FPaths::ConvertRelativePathToFull(File.Path);
		Result.Kind = File.Kind;
	}
	const bool bManaged = GetDefault<UHyperAIStudioSettings>()->bCheckOutBeforeAgentEdits;
	ISourceControlProvider* Provider = bManaged ? UnrealProvider() : nullptr;
	FLoreTarget Lore;
	const bool bLore = bManaged && !Provider && FindLore(Lore);
	FState& Current = State();
	TArray<FString> Clear;

	if (Provider)
	{
		if (!Provider->IsAvailable())
		{
			for (FHeldFileResult& Result : Results)
			{
				Result.Outcome = TEXT("failed");
				Result.Detail = FString::Printf(TEXT("%s is set up but not connected, so nothing was checked out or written. Connect it in the editor's Revision Control menu."),
					*Provider->GetName().ToString());
			}
			return Results;
		}
		TArray<FString> All;
		for (const FHeldFileResult& Result : Results)
		{
			All.Add(Result.Path);
		}
		Provider->Execute(ISourceControlOperation::Create<FUpdateStatus>(), All);
		TArray<int32> ToCheckOut;
		for (int32 Index = 0; Index < Results.Num(); ++Index)
		{
			FHeldFileResult& Result = Results[Index];
			const FHyperAIStudioSourceControlFileState FileState = ReadState(Provider->GetState(Result.Path, EStateCacheUsage::Use));
			FString Error;
			// A new file has nothing to check out, but one a teammate has already added there is theirs.
			const EDecision Decision = Result.Kind == TEXT("added") && !FileState.bCheckedOutOther ? EDecision::Edit
				: Decide(FileState, ProjectRelative(Result.Path), Error);
			if (Decision == EDecision::Refuse)
			{
				Result.Outcome = TEXT("blocked");
				Result.Who = FileState.Who;
				Result.Detail = Error;
			}
			else if (Decision == EDecision::CheckOut && Result.Kind == TEXT("edited"))
			{
				ToCheckOut.Add(Index);
			}
			else
			{
				const FSourceControlStatePtr Held = Provider->GetState(Result.Path, EStateCacheUsage::Use);
				if (Result.Kind == TEXT("edited") && Held.IsValid() && Held->IsCheckedOut())
				{
					// Checked out already, by this workspace.
					Result.Outcome = TEXT("checked_out");
				}
				Clear.Add(Result.Path);
			}
		}
		if (!ToCheckOut.IsEmpty())
		{
			TArray<FString> Batch;
			for (const int32 Index : ToCheckOut)
			{
				Batch.Add(Results[Index].Path);
			}
			const FSourceControlChangelistPtr List = Changelist(*Provider);
			const FSourceControlOperationRef Operation = ISourceControlOperation::Create<FCheckOut>();
			const bool bAll = (List.IsValid() ? Provider->Execute(Operation, List, Batch) : Provider->Execute(Operation, Batch)) == ECommandResult::Succeeded;
			for (const int32 Index : ToCheckOut)
			{
				FHeldFileResult& Result = Results[Index];
				bool bOk = bAll;
				FString Why;
				if (!bAll)
				{
					// One refusal fails the batch: each file again, so only the one a teammate took is held back.
					const FSourceControlOperationRef Single = ISourceControlOperation::Create<FCheckOut>();
					bOk = (List.IsValid() ? Provider->Execute(Single, List, TArray<FString>{Result.Path}) : Provider->Execute(Single, TArray<FString>{Result.Path})) == ECommandResult::Succeeded;
					Why = bOk ? FString() : Errors(Single);
				}
				if (bOk)
				{
					Result.Outcome = TEXT("checked_out");
					Current.AgentFiles.Add(Result.Path);
					Clear.Add(Result.Path);
				}
				else
				{
					const FHyperAIStudioSourceControlFileState After = ReadState(Provider->GetState(Result.Path, EStateCacheUsage::Use));
					Result.Outcome = TEXT("blocked");
					Result.Who = After.Who.IsEmpty() ? LockHolder(Why) : After.Who;
					Result.Detail = FString::Printf(TEXT("%s could not check out %s%s: %s"), *Provider->GetName().ToString(), *ProjectRelative(Result.Path),
						Result.Who.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (held by %s)"), *Result.Who), *Why);
				}
			}
		}
	}
	else if (bLore)
	{
		for (FHeldFileResult& Result : Results)
		{
			// Locks only on a server, and only for files that are there to lock; a local repository has neither.
			if (!Lore.IsLocal() && Result.Kind != TEXT("added") && FPaths::FileExists(Result.Path))
			{
				FString Text;
				if (!RunLore(Lore, TEXT("lock acquire ") + Quoted({Result.Path}), Text, LockSeconds))
				{
					Result.Outcome = TEXT("blocked");
					Result.Who = LockHolder(Text);
					Result.Detail = FString::Printf(TEXT("Lore could not lock %s: %s"), *ProjectRelative(Result.Path), *Text.TrimStartAndEnd().Left(600));
					continue;
				}
				Result.Outcome = TEXT("locked");
				Current.AgentFiles.Add(Result.Path);
			}
			Clear.Add(Result.Path);
		}
	}
	else
	{
		for (const FHeldFileResult& Result : Results)
		{
			Clear.Add(Result.Path);
		}
	}

	const TSet<FString> Written = Clear.IsEmpty() ? TSet<FString>() : Write(Clear);
	TArray<FString> Added;
	TArray<FString> Removed;
	TArray<FString> Dirty;
	for (FHeldFileResult& Result : Results)
	{
		if (!Clear.Contains(Result.Path))
		{
			continue;
		}
		Result.bWritten = Written.Contains(Result.Path);
		if (!Result.bWritten)
		{
			Result.Outcome = TEXT("failed");
			Result.Detail = TEXT("The File Sandbox could not write it into the project, so it stays held; the Output Log says why.");
			continue;
		}
		if (Result.Outcome.IsEmpty())
		{
			Result.Outcome = TEXT("written");
		}
		(Result.Kind == TEXT("added") ? Added : Result.Kind == TEXT("removed") ? Removed : Dirty).Add(Result.Path);
	}

	if (Provider)
	{
		// The File Sandbox adds and deletes through the real provider as it writes; a provider that still does not know
		// a written file (or a stand-in) is told here.
		TArray<FString> Touched = Added;
		Touched.Append(Removed);
		if (!Touched.IsEmpty())
		{
			Provider->Execute(ISourceControlOperation::Create<FUpdateStatus>(), Touched);
		}
		TArray<FString> Add;
		for (const FString& File : Added)
		{
			const FSourceControlStatePtr FileState = Provider->GetState(File, EStateCacheUsage::Use);
			if (FileState.IsValid() && !FileState->IsSourceControlled() && FileState->CanAdd())
			{
				Add.Add(File);
			}
		}
		FString AddError;
		if (!Add.IsEmpty())
		{
			const FSourceControlOperationRef MarkForAdd = ISourceControlOperation::Create<FMarkForAdd>();
			const FSourceControlChangelistPtr List = Changelist(*Provider);
			if ((List.IsValid() ? Provider->Execute(MarkForAdd, List, Add) : Provider->Execute(MarkForAdd, Add)) != ECommandResult::Succeeded)
			{
				AddError = Errors(MarkForAdd);
			}
		}
		TArray<FString> Delete;
		for (const FString& File : Removed)
		{
			const FSourceControlStatePtr FileState = Provider->GetState(File, EStateCacheUsage::Use);
			if (FileState.IsValid() && FileState->IsSourceControlled() && !FileState->IsDeleted() && !FileState->IsAdded())
			{
				Delete.Add(File);
			}
		}
		FString DeleteError;
		if (!Delete.IsEmpty())
		{
			const FSourceControlOperationRef MarkForDelete = ISourceControlOperation::Create<FDelete>();
			if (Provider->Execute(MarkForDelete, Delete) != ECommandResult::Succeeded)
			{
				DeleteError = Errors(MarkForDelete);
			}
		}
		for (FHeldFileResult& Result : Results)
		{
			if (!Result.bWritten)
			{
				continue;
			}
			if (Result.Kind == TEXT("added"))
			{
				const FSourceControlStatePtr FileState = Provider->GetState(Result.Path, EStateCacheUsage::Use);
				const bool bAdded = FileState.IsValid() && FileState->IsAdded();
				Result.Outcome = bAdded ? TEXT("marked_for_add") : TEXT("written");
				Result.Detail = bAdded ? FString() : FString::Printf(TEXT("Written, but not marked for add%s"), AddError.IsEmpty() ? TEXT(".") : *(TEXT(": ") + AddError));
				Current.AgentFiles.Add(Result.Path);
			}
			else if (Result.Kind == TEXT("removed"))
			{
				const FSourceControlStatePtr FileState = Provider->GetState(Result.Path, EStateCacheUsage::Use);
				const bool bDeleted = FileState.IsValid() && FileState->IsDeleted();
				Result.Outcome = bDeleted ? TEXT("marked_for_delete") : TEXT("written");
				Result.Detail = bDeleted || DeleteError.IsEmpty() ? FString() : TEXT("Removed, but not marked for delete: ") + DeleteError;
			}
		}
	}
	else if (bLore)
	{
		TArray<FString> All = Dirty;
		All.Append(Added);
		All.Append(Removed);
		if (!All.IsEmpty())
		{
			FString Text;
			const bool bDirty = RunLore(Lore, TEXT("dirty ") + Quoted(All), Text, DirtySeconds);
			for (FHeldFileResult& Result : Results)
			{
				if (!Result.bWritten)
				{
					continue;
				}
				Current.AgentFiles.Add(Result.Path);
				if (Result.Outcome != TEXT("locked"))
				{
					Result.Outcome = bDirty ? TEXT("marked_dirty") : TEXT("written");
				}
				if (!bDirty)
				{
					Result.Detail = TEXT("Written, but lore dirty did not take it: ") + Text.TrimStartAndEnd().Left(300);
				}
			}
		}
	}
	for (const FHeldFileResult& Result : Results)
	{
		UE_LOG(LogHyperAIStudioSourceControl, Log, TEXT("Sandbox approve: %s %s -> %s%s"), *Result.Kind, *ProjectRelative(Result.Path), *Result.Outcome,
			Result.Who.IsEmpty() ? TEXT("") : *(TEXT(" (") + Result.Who + TEXT(")")));
	}
	return Results;
}

bool FHyperAIStudioSourceControl::MakeEditable(const TArray<FString>& PackageNames, FString& OutError)
{
	using namespace HyperAIStudio::SourceControl::Private;
	ISourceControlProvider* Provider = UnrealProvider();
	FLoreTarget Lore;
	// A project without source control records nothing, so its saves never reach for a provider.
	if (!GetDefault<UHyperAIStudioSettings>()->bCheckOutBeforeAgentEdits || (!Provider && !FindLore(Lore)))
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
		// Sandbox Mode: a teammate's file still turns the edit away now, but the check-out waits for the user's approval,
		// which checks out what it writes (WriteHeldFiles).
		if (IsDeferredToSandbox())
		{
			return true;
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
	// Lore's locks are advisory: taking one tells the team, and one a teammate holds turns the edit away. A local
	// repository has no server to keep locks (lore refuses them: "Unable to acquire lock while offline") and no team to
	// tell, so its files are only recorded, and marked dirty once saved. The edit waits for the lock on the game thread,
	// so a server that does not answer is given up on quickly.
	TArray<FString> Existing = Files.FilterByPredicate([](const FString& File) { return FPaths::FileExists(File); });
	// Sandbox Mode: the lock is taken when the user approves the held files (WriteHeldFiles).
	if (IsDeferredToSandbox())
	{
		return true;
	}
	FString Text;
	if (!Existing.IsEmpty() && !Lore.IsLocal() && !RunLore(Lore, TEXT("lock acquire ") + Quoted(Existing), Text, LockSeconds))
	{
		OutError = FString::Printf(TEXT("Lore could not lock %s for this edit: %s"), *ProjectRelative(Existing[0]), *Text.Left(600));
		if (Text.Contains(TEXT("Disconnected")) || Text.Contains(TEXT("Stopped after")))
		{
			OutError += TEXT(" The Lore server did not answer: start it, or check it in HyperAI Studio > Settings > Version control.");
		}
		return false;
	}
	Current.AgentFiles.Append(Existing);
	return true;
}

bool FHyperAIStudioSourceControl::SaveAsset(UObject& Asset, FString& OutError)
{
	UPackage* Package = Asset.GetOutermost();
	if (!MakeEditable({Package->GetName()}, OutError))
	{
		return false;
	}
	if (!Package->IsFullyLoaded())
	{
		Package->FullyLoad();
	}
	FStringOutputDevice Refusal;
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.Error = &Refusal;
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, &Asset, *File, Args))
	{
		const FString Why = Refusal.TrimStartAndEnd();
		OutError = Why.IsEmpty()
			? FString::Printf(TEXT("%s could not be saved."), *Package->GetName())
			: FString::Printf(TEXT("%s could not be saved: %s"), *Package->GetName(), *Why);
		return false;
	}
	return true;
}

FString FHyperAIStudioSourceControl::DescribeBackend()
{
	using namespace HyperAIStudio::SourceControl::Private;
	if (const ISourceControlProvider* Provider = UnrealProvider())
	{
		return TEXT("unreal: ") + Provider->GetName().ToString();
	}
	FLoreTarget Lore;
	return FindLore(Lore) ? TEXT("lore: ") + Lore.Root : FString(TEXT("none"));
}

TArray<FString> FHyperAIStudioSourceControl::GetAgentFiles()
{
	return HyperAIStudio::SourceControl::Private::State().AgentFiles.Array();
}

bool FHyperAIStudioSourceControl::IsSubmitting()
{
	return HyperAIStudio::SourceControl::Private::State().bSubmitting;
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
void FHyperAIStudioSourceControl::SetLoreForTests(const FString& Executable, const FString& Prefix, const FString& Root, const FString& RemoteUrl)
{
	using namespace HyperAIStudio::SourceControl::Private;
	State().LoreExecutable = Executable;
	State().LorePrefix = Prefix;
	State().LoreRoot = Root;
	State().LoreRemoteUrl = RemoteUrl;
	State().AgentFiles.Reset();
	State().AgentPackages.Reset();
}

void FHyperAIStudioSourceControl::SetProviderForTests(ISourceControlProvider* Provider)
{
	using namespace HyperAIStudio::SourceControl::Private;
	State().ProviderForTests = Provider;
	State().Changelist.Reset();
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
		const bool bLocal = IsLocalLore();
		for (const FString& File : Files)
		{
			Report.Files.Add({ProjectRelative(File), bLocal ? TEXT("changed") : TEXT("locked"), FString()});
		}
		if (Report.Backend.StartsWith(TEXT("lore")))
		{
			RunLore(TEXT("status"), Report.LoreStatus, StatusSeconds);
		}
	}
	DescribeLore(Report.Lore);
	Report.bOk = true;
	Report.Diagnostic = Report.Backend == TEXT("none")
		? TEXT("The project has no source control set up in Unreal and is not in a Lore workspace; agent edits are saved as they are.")
		: FString::Printf(TEXT("%d file(s) changed by agents this session. hyper_source_control_submit checks them in once the user approves."), Report.Files.Num());
	if (Report.Lore.Mode == TEXT("local"))
	{
		Report.Diagnostic += TEXT(" The Lore repository is local: a submit commits here, with no push and no locks.");
	}
	if (FHyperAIStudioSourceControl::IsSubmitting())
	{
		Report.Diagnostic += TEXT(" A submit is still running; hyper_operation_wait follows it.");
	}
	if (!Report.Lore.Next.IsEmpty())
	{
		Report.Diagnostic += TEXT(" ") + Report.Lore.Next;
	}
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
	if (FHyperAIStudioSourceControl::IsSubmitting())
	{
		return Refuse(TEXT("submit_running"), TEXT("A submit is still running in Lore; wait for it with hyper_operation_wait, then submit what is left."));
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
	const bool bLocalLore = !UnrealProvider() && IsLocalLore();
	const FString How = bLocalLore ? TEXT("a commit to the local Lore repository (no server, nothing pushed)") : TEXT("a check-in through ") + Report.Backend;
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("Would submit %d file(s) as %s, \"%s\". Nothing was sent; resubmit with bDryRun false to ask the user."),
			Files.Num(), *How, *Description.Left(200));
		return Report;
	}
	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = PackId;
	Summary.ToolName = SubmitToolName;
	Summary.VariantId = TEXT("source_control.submit.v1");
	Summary.Safety = EHyperAIStudioDomainSafety::ExternalEffect;
	Summary.EffectTarget = Report.Backend;
	Summary.Effects = {FString::Printf(TEXT("Submit %d file(s) as %s: \"%s\""), Files.Num(), *How, *Description.Left(300))};
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
	FHyperAIStudioApprovalGate::DescribeHeld(OperationId, Report.Status, Report.Diagnostic);
	return Report;
}
