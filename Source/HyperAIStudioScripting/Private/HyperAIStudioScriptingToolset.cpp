// Games by Hyper 2026.

#include "HyperAIStudioScriptingToolset.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioRequestFit.h"
#include "HyperAIStudioSandbox.h"
#include "IPythonScriptPlugin.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PythonScriptTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioScripting"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioScripting, Log, All);

namespace HyperAIStudio::Scripting::Private
{
	FString ScriptsRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Scripts"));
	}

	FString ProjectRelative(const FString& Path)
	{
		FString Relative = Path;
		FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		return Relative;
	}

	FString RunnerPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		return Plugin.IsValid()
			? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/Python/hyperai_script_runner.py"))
			: FString();
	}

	FString Quote(const FString& Path)
	{
		FString Normalized = Path;
		FPaths::NormalizeFilename(Normalized);
		return TEXT("\"") + Normalized + TEXT("\"");
	}

	FString LastLine(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);
		return Lines.IsEmpty() ? FString() : Lines.Last().TrimStartAndEnd().Left(300);
	}

	bool RunRunner(const FString& Arguments, FString& OutError)
	{
		IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
		const FString Runner = RunnerPath();
		if (!Python || !Python->IsPythonAvailable() || Runner.IsEmpty() || !IFileManager::Get().FileExists(*Runner))
		{
			OutError = TEXT("Editor Python is unavailable, or the plugin's Resources/Python/hyperai_script_runner.py is missing.");
			return false;
		}
		FPythonCommandEx Command;
		Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		Command.FileExecutionScope = EPythonFileExecutionScope::Private;
		Command.Flags = EPythonCommandFlags::Unattended;
		// Always a quoted absolute path: given anything else, ExecuteFile guesses whether it holds a path or code.
		Command.Command = Quote(Runner) + TEXT(" ") + Arguments;
		if (!Python->ExecPythonCommandEx(Command))
		{
			OutError = Command.CommandResult.Left(4000);
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> ReadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
		}
		return Object;
	}

	TSet<FString> DirtyPackageNames(const bool bWorldsOnly = false)
	{
		TArray<UPackage*> Packages;
		if (!bWorldsOnly) FEditorFileUtils::GetDirtyContentPackages(Packages);
		FEditorFileUtils::GetDirtyWorldPackages(Packages);
		TSet<FString> Names;
		for (const UPackage* Package : Packages)
		{
			if (Package) Names.Add(Package->GetName());
		}
		return Names;
	}

	/**
	 * Why a script cannot run as one undoable step, and the level-changing calls it makes. The editor never overwrites a
	 * level, so a change loses nothing while the open one has no unsaved changes; only then does it run without approval.
	 */
	bool AnalyzeFolder(const FString& Folder, bool& bOutHasRun, TArray<FString>& OutReasons, TArray<FString>& OutLevelChanges, FString& OutError)
	{
		const FString Out = Folder / TEXT("analysis.json");
		// "levels": this build checks level changes for unsaved levels, so the runner reports them apart from its reasons.
		if (!RunRunner(FString::Printf(TEXT("analyze %s %s levels"), *Quote(Folder / TEXT("script.py")), *Quote(Out)), OutError)) return false;
		const TSharedPtr<FJsonObject> Analysis = ReadJson(Out);
		if (!Analysis.IsValid())
		{
			OutError = TEXT("The script check did not report.");
			return false;
		}
		if (!Analysis->GetBoolField(TEXT("ok")))
		{
			OutError = TEXT("Syntax error at ") + Analysis->GetStringField(TEXT("syntax_error"));
			return false;
		}
		bOutHasRun = Analysis->GetBoolField(TEXT("has_run"));
		OutReasons.Reset();
		OutLevelChanges.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
		if (Analysis->TryGetArrayField(TEXT("reasons"), Reasons))
		{
			for (const TSharedPtr<FJsonValue>& Reason : *Reasons) OutReasons.Add(Reason->AsString());
		}
		const TArray<TSharedPtr<FJsonValue>>* LevelChanges = nullptr;
		if (Analysis->TryGetArrayField(TEXT("level_changes"), LevelChanges))
		{
			for (const TSharedPtr<FJsonValue>& Call : *LevelChanges) OutLevelChanges.Add(Call->AsString());
		}
		if (!OutLevelChanges.IsEmpty() && !DirtyPackageNames(/*bWorldsOnly=*/true).IsEmpty())
		{
			OutReasons.Add(FString::Printf(TEXT("calls %s while the open level has unsaved changes"), *FString::Join(OutLevelChanges, TEXT(", "))));
		}
		return true;
	}

	void RecordActivity(const EHyperAIStudioActivityKind Kind, const FString& OperationId, const FString& Status, const FString& Detail)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Kind;
		Entry.PackId = FHyperAIStudioScriptingContracts::PackId;
		Entry.ToolName = FHyperAIStudioScriptingContracts::ToolName;
		Entry.Target = TEXT("Python script");
		Entry.OperationId = OperationId;
		Entry.StatusCode = Status;
		Entry.Detail = Detail.Left(512);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	/**
	 * Runs script.py in Folder inside one undo step, undone if it fails. Guarded runs also refuse deletes, and level loads
	 * unless bChangesLevel: the analysis then found no unsaved level to lose, and the undo step ends where the level changes.
	 */
	FHyperAIScriptRunReport Execute(const FString& OperationId, const FString& Folder, const int32 TimeoutSeconds, const bool bGuarded,
		const bool bChangesLevel = false)
	{
		FHyperAIScriptRunReport Report;
		Report.OperationId = OperationId;
		Report.Safety = !bGuarded ? TEXT("needs_approval") : bChangesLevel ? TEXT("changes_level") : TEXT("undoable");
		Report.ScriptFolder = ProjectRelative(Folder);
		if (!HyperAIStudio::Sandbox::CheckAgentEdit(Report.Diagnostic))
		{
			Report.Status = TEXT("sandbox_unavailable");
			RecordActivity(EHyperAIStudioActivityKind::Failed, OperationId, Report.Status, Report.Diagnostic);
			return Report;
		}
		// Checked again here, since an approved run starts later; nothing can veto a new level the way a load is vetoed.
		if (bChangesLevel && !DirtyPackageNames(/*bWorldsOnly=*/true).IsEmpty())
		{
			Report.Status = TEXT("level_unsaved");
			Report.Diagnostic = TEXT("The open level has unsaved changes, which changing the level would lose, so nothing ran. Save or revert them first.");
			RecordActivity(EHyperAIStudioActivityKind::Failed, OperationId, Report.Status, Report.Diagnostic);
			return Report;
		}
		const FString ResultPath = Folder / TEXT("result.json");
		IFileManager::Get().Delete(*ResultPath, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
		const TSet<FString> DirtyBefore = DirtyPackageNames();
		const int32 UndoBefore = UToolsetLibrary::GetActiveUndoCount();
		FDelegateHandle DeleteVeto;
		FDelegateHandle MapVeto;
		if (bGuarded)
		{
			// An undo cannot restore a deleted asset, and a level load would replace the world this undo step lives in.
			DeleteVeto = FEditorDelegates::OnAssetsCanDelete.AddLambda([](const TArray<UObject*>&, FCanDeleteAssetResult& Result) { Result.Set(false); });
			if (!bChangesLevel)
			{
				MapVeto = FEditorDelegates::OnMapLoad.AddLambda([](const FString&, FCanLoadMap& Result) { Result.SetFalse(); });
			}
		}
		const int32 PriorActions = GEditor ? GEditor->BeginTransaction(TEXT("HyperAIStudio"), LOCTEXT("RunScript", "HyperAI: Run Script"), nullptr) : 0;
		FString Error;
		const bool bRan = RunRunner(FString::Printf(TEXT("run %s %d %s %d"), *Quote(Folder / TEXT("script.py")), TimeoutSeconds,
			*Quote(ResultPath), bGuarded ? 1 : 0), Error);
		if (GEditor) GEditor->EndTransaction();
		if (bGuarded)
		{
			FEditorDelegates::OnAssetsCanDelete.Remove(DeleteVeto);
			if (MapVeto.IsValid()) FEditorDelegates::OnMapLoad.Remove(MapVeto);
		}
		const TSharedPtr<FJsonObject> Result = bRan ? ReadJson(ResultPath) : nullptr;
		const bool bOk = Result.IsValid() && Result->GetBoolField(TEXT("ok"));
		if (Result.IsValid())
		{
			Report.bTimedOut = Result->GetBoolField(TEXT("timed_out"));
			Report.ResultJson = Result->GetStringField(TEXT("result_json")).Left(FHyperAIStudioScriptingContracts::MaxResultCharacters);
			Report.Output = Result->GetStringField(TEXT("output"));
			Report.Traceback = Result->GetStringField(TEXT("traceback"));
			Report.Seconds = static_cast<float>(Result->GetNumberField(TEXT("seconds")));
		}
		// Only the outermost transaction may be undone; an enclosing caller owns the history below it.
		if (!bOk && PriorActions == 0 && UToolsetLibrary::GetActiveUndoCount() > UndoBefore)
		{
			Report.bRolledBack = UToolsetLibrary::UndoTransaction(/*bCanRedo=*/false);
		}
		for (const FString& Name : DirtyPackageNames())
		{
			if (!DirtyBefore.Contains(Name)) Report.DirtyPackages.Add(Name);
		}
		Report.DirtyPackages.Sort();
		Report.bOk = bOk;
		Report.Status = bOk ? TEXT("completed") : Report.bTimedOut ? TEXT("timed_out") : TEXT("failed");
		Report.Diagnostic = !Result.IsValid()
			? TEXT("The runner did not report: ") + Error
			: bOk
			? FString::Printf(TEXT("Ran in %.2f s%s."), Report.Seconds, Report.DirtyPackages.IsEmpty()
				? TEXT("") : *FString::Printf(TEXT("; %d changed packages are unsaved"), Report.DirtyPackages.Num()))
			: LastLine(Report.Traceback) + (Report.bRolledBack ? TEXT(" Its edits were undone.") : TEXT(""));
		RecordActivity(bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed, OperationId, Report.Status, Report.Diagnostic);
		return Report;
	}
}

FString FHyperAIStudioScriptingContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioScripting.HyperAIStudioScriptingToolset");
}

TArray<FString> FHyperAIStudioScriptingContracts::GetToolNames()
{
	return {ToolName};
}

bool FHyperAIStudioScriptingContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(
		PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

bool FHyperAIStudioScriptingContracts::Analyze(const FString& Code, bool& bOutHasRun, TArray<FString>& OutReasons, FString& OutError)
{
	using namespace HyperAIStudio::Scripting::Private;
	const FString Folder = ScriptsRoot() / TEXT("_checks") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const bool bWritten = IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true)
		&& FFileHelper::SaveStringToFile(Code, *(Folder / TEXT("script.py")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TArray<FString> LevelChanges;
	const bool bAnalyzed = bWritten && AnalyzeFolder(Folder, bOutHasRun, OutReasons, LevelChanges, OutError);
	if (!bWritten) OutError = TEXT("Could not write the script under Saved/HyperAIStudio/Scripts.");
	IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists=*/false, /*Tree=*/true);
	return bAnalyzed;
}

FHyperAIScriptRunReport FHyperAIStudioScriptingContracts::Run(const FHyperAIScriptRunRequest& Request)
{
	using namespace HyperAIStudio::Scripting::Private;
	FHyperAIScriptRunReport Report;
	auto Reject = [&Report](const TCHAR* Status, const FString& Diagnostic)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Diagnostic.Left(2048);
		return Report;
	};
	if (!IsInGameThread())
	{
		return Reject(TEXT("game_thread_required"), TEXT("Scripts run on the game thread."));
	}
	if (Request.Code.TrimStartAndEnd().IsEmpty() || Request.Code.Len() > MaxCodeCharacters
		|| Request.TimeoutSeconds < 1 || Request.TimeoutSeconds > MaxTimeoutSeconds)
	{
		return Reject(TEXT("invalid_request"), TEXT("code is 1 character to 64 KB of Python; timeout_seconds is 1-60."));
	}
	Report.OperationId = Request.OperationId.IsEmpty()
		? TEXT("script-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower() : Request.OperationId;
	if (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Report.OperationId))
	{
		return Reject(TEXT("invalid_operation_id"), TEXT("operation_id must be a short journal-safe id, or empty to have one made."));
	}
	const FString Folder = ScriptsRoot() / Report.OperationId;
	if (IFileManager::Get().DirectoryExists(*Folder))
	{
		return Reject(TEXT("operation_id_used"), TEXT("That operation_id already ran; pass a new one or leave it empty."));
	}
	Report.ScriptFolder = ProjectRelative(Folder);
	if (!IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true)
		|| !FFileHelper::SaveStringToFile(Request.Code, *(Folder / TEXT("script.py")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return Reject(TEXT("unavailable"), TEXT("Could not write the script under Saved/HyperAIStudio/Scripts."));
	}
	bool bHasRun = false;
	TArray<FString> Reasons;
	TArray<FString> LevelChanges;
	FString Error;
	if (!AnalyzeFolder(Folder, bHasRun, Reasons, LevelChanges, Error))
	{
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
		return Reject(Error.StartsWith(TEXT("Syntax error")) ? TEXT("invalid_script") : TEXT("unavailable"), Error);
	}
	if (!bHasRun)
	{
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
		return Reject(TEXT("invalid_script"), TEXT("Define run() and return a JSON-serializable dict."));
	}
	Report.EscalationReasons = Reasons;
	const bool bUndoable = Reasons.IsEmpty();
	const bool bChangesLevel = bUndoable && !LevelChanges.IsEmpty();
	Report.Safety = !bUndoable ? TEXT("needs_approval") : bChangesLevel ? TEXT("changes_level") : TEXT("undoable");
	// Undoable scripts follow the agent-edit approval setting like any plan; the rest always wait for a click.
	const bool bNeedsApproval = !bUndoable || FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit);
	if (Request.bDryRun)
	{
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = bUndoable
			? (bNeedsApproval ? TEXT("Undoable; runs inside one undo step once the user approves it (agent edits need approval here).")
				: TEXT("Undoable; runs at once, inside one undo step."))
			: TEXT("Waits for the user's approval because it ") + FString::Join(Reasons, TEXT("; ")) + TEXT(".");
		if (bChangesLevel)
		{
			Report.Diagnostic += TEXT(" It changes the level, where its undo step ends; the open level has nothing unsaved to lose.");
		}
		return Report;
	}
	if (!bNeedsApproval)
	{
		return Execute(Report.OperationId, Folder, Request.TimeoutSeconds, /*bGuarded=*/true, bChangesLevel);
	}

	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = PackId;
	Summary.ToolName = ToolName;
	Summary.VariantId = TEXT("script.run.v1");
	Summary.Safety = bUndoable ? EHyperAIStudioDomainSafety::Edit : EHyperAIStudioDomainSafety::Destructive;
	Summary.EffectTarget = Report.ScriptFolder / TEXT("script.py");
	Summary.Effects.Add(bUndoable ? FString(TEXT("Run a Python script inside one undo step"))
		: TEXT("Run a Python script that ") + FString::Join(Reasons, TEXT("; ")));
	TArray<FString> Lines;
	Request.Code.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
	constexpr int32 ShownLines = 24;
	for (int32 Index = 0; Index < Lines.Num() && Index < ShownLines; ++Index)
	{
		Summary.Effects.Add(TEXT("| ") + Lines[Index].Left(160));
	}
	if (Lines.Num() > ShownLines)
	{
		Summary.Effects.Add(FString::Printf(TEXT("| ... %d more lines in %s"), Lines.Num() - ShownLines, *Summary.EffectTarget));
	}
	const FString OperationId = Report.OperationId;
	const int32 Timeout = Request.TimeoutSeconds;
	if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary,
		[OperationId, Folder, Timeout, bUndoable, bChangesLevel]() { Execute(OperationId, Folder, Timeout, /*bGuarded=*/bUndoable, bChangesLevel); }, Error))
	{
		return Reject(TEXT("approval_not_queued"), Error);
	}
	Report.bOk = true;
	Report.Status = TEXT("awaiting_user_approval");
	Report.Diagnostic = (bUndoable ? FString(TEXT("Agent edits need approval here."))
		: TEXT("The script ") + FString::Join(Reasons, TEXT("; ")) + TEXT("."))
		+ TEXT(" Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
	return Report;
}

FHyperAIScriptRunReport UHyperAIStudioScriptingToolset::hyper_script_run(const FHyperAIScriptRunRequest& InRequest)
{
	using C = FHyperAIStudioScriptingContracts;
	FHyperAIScriptRunRequest Request = InRequest;
	FHyperAIStudioRequestFit Fit;
	Fit.ClampOrDefault(Request.TimeoutSeconds, 1, C::MaxTimeoutSeconds, FHyperAIScriptRunRequest().TimeoutSeconds, TEXT("timeout_seconds"));
	// A script's id names its folder under Saved, which outlives the session, so a reused id runs under the next free one.
	FHyperAIScriptRunReport Report = Fit.FreshOperationId(Request, [](const FHyperAIScriptRunRequest& Planned) { return C::Run(Planned); },
		TEXT("operation_id_used"));
	Report.Diagnostic = Fit.AppendTo(Report.Diagnostic);
	return Report;
}

void FHyperAIStudioScriptingRegistration::Startup()
{
	if (bStarted) return;
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioScriptingRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized()) RegisterAfterEngineInit();
}

void FHyperAIStudioScriptingRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioScriptingToolset::StaticClass(),
			FHyperAIStudioScriptingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioScripting, Warning, TEXT("Could not unregister the scripting toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioScriptingRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioScriptingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioScripting, Verbose, TEXT("Scripting source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioScriptingToolset::StaticClass(),
		FHyperAIStudioScriptingContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioScripting, Error, TEXT("Scripting cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
