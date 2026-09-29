// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioScriptingToolset.h"

#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioSettings.h"
#include "IPythonScriptPlugin.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::Scripting::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	bool PythonReady(FAutomationTestBase& Test)
	{
		const IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
		if (!Python || !Python->IsPythonAvailable())
		{
			Test.AddInfo(TEXT("Skipped: editor Python is not available."));
			return false;
		}
		return true;
	}

	FHyperAIScriptRunReport RunScript(const FString& Code, const int32 TimeoutSeconds = 10)
	{
		FHyperAIScriptRunRequest Request;
		Request.Code = Code;
		Request.TimeoutSeconds = TimeoutSeconds;
		return FHyperAIStudioScriptingContracts::Run(Request);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScriptingAnalyzeTest,
	"HyperAIStudio.NativeTools.Scripting.AnalyzeEscalations",
	HyperAIStudio::Scripting::Tests::Flags)

bool FHyperAIStudioScriptingAnalyzeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!HyperAIStudio::Scripting::Tests::PythonReady(*this)) return true;
	bool bHasRun = false;
	TArray<FString> Reasons;
	FString Error;
	TestTrue(TEXT("plain editor Python analyzes: ") + Error,
		FHyperAIStudioScriptingContracts::Analyze(TEXT("import unreal\ndef run():\n    return {}\n"), bHasRun, Reasons, Error));
	TestTrue(TEXT("it defines run() and needs no approval"), bHasRun && Reasons.IsEmpty());
	TestTrue(TEXT("a risky script analyzes"), FHyperAIStudioScriptingContracts::Analyze(
		TEXT("import os\nimport unreal\ndef run():\n    unreal.EditorAssetLibrary.delete_asset('/Game/X')\n    open('x.txt', 'w')\n    return {}\n"),
		bHasRun, Reasons, Error));
	TestTrue(TEXT("importing os needs approval"), Reasons.Contains(TEXT("imports os")));
	TestTrue(TEXT("deleting an asset needs approval"), Reasons.ContainsByPredicate([](const FString& Reason) { return Reason.StartsWith(TEXT("calls delete_asset")); }));
	TestTrue(TEXT("writing a file needs approval"), Reasons.Contains(TEXT("opens a file for writing")));

	// What agents reach for in their own work needs no click: screenshots, computed lookups, a computed write path
	// (checked as it runs), and a level change while the open level has nothing unsaved to lose.
	TestTrue(TEXT("safe paths analyze"), FHyperAIStudioScriptingContracts::Analyze(TEXT(
		"import unreal\ndef run():\n"
		"    unreal.SystemLibrary.execute_console_command(None, 'shot showui')\n"
		"    open(unreal.Paths.project_saved_dir() + 'HyperAIStudio/Captures/x.png', 'wb')\n"
		"    return {'names': [getattr(unreal, n, None) for n in ('Vector', 'Rotator')]}\n"), bHasRun, Reasons, Error));
	TestTrue(TEXT("and need no approval: ") + FString::Join(Reasons, TEXT("; ")), Reasons.IsEmpty());
	TestTrue(TEXT("a chained console command still does"), FHyperAIStudioScriptingContracts::Analyze(
		TEXT("import unreal\ndef run():\n    unreal.SystemLibrary.execute_console_command(None, 'shot|exit')\n"), bHasRun, Reasons, Error)
		&& Reasons.Num() == 1);
	TArray<UPackage*> DirtyWorlds;
	FEditorFileUtils::GetDirtyWorldPackages(DirtyWorlds);
	TestTrue(TEXT("a level change analyzes"), FHyperAIStudioScriptingContracts::Analyze(TEXT(
		"import unreal\ndef run():\n    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).new_level('/Game/X')\n"), bHasRun, Reasons, Error));
	TestEqual(TEXT("and needs approval only over unsaved level changes: ") + FString::Join(Reasons, TEXT("; ")),
		Reasons.IsEmpty(), DirtyWorlds.IsEmpty());
	TestFalse(TEXT("a syntax error is reported"), FHyperAIStudioScriptingContracts::Analyze(TEXT("def run(:\n"), bHasRun, Reasons, Error));
	TestTrue(TEXT("with its line: ") + Error, Error.Contains(TEXT("line 1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScriptingRunTest,
	"HyperAIStudio.NativeTools.Scripting.RunRollbackTimeoutAndGuard",
	HyperAIStudio::Scripting::Tests::Flags)

bool FHyperAIStudioScriptingRunTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Scripting::Tests;
	if (!PythonReady(*this)) return true;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("an editor world"), World)) return false;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT { Settings->bRequireApprovalForAgentEdits = bApproval; };

	FHyperAIScriptRunReport Report = RunScript(TEXT("def run():\n    print('adding')\n    return {'sum': 1 + 2}\n"));
	TestEqual(*FString::Printf(TEXT("a plain script completes (%s)"), *Report.Diagnostic), Report.Status, FString(TEXT("completed")));
	TestTrue(TEXT("with its result as JSON: ") + Report.ResultJson, Report.ResultJson.Contains(TEXT("\"sum\": 3")));
	TestTrue(TEXT("and its printed output"), Report.Output.Contains(TEXT("adding")));
	TestEqual(TEXT("it ran as undoable"), Report.Safety, FString(TEXT("undoable")));

	// A failing script's edits are undone: it renames an actor, then raises.
	const FString Label = TEXT("HyperAIScriptTest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transactional;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!TestNotNull(TEXT("a test actor"), Actor)) return false;
	ON_SCOPE_EXIT { if (IsValid(Actor)) World->DestroyActor(Actor); };
	Actor->SetActorLabel(Label);
	Report = RunScript(FString::Printf(TEXT(
		"import unreal\n"
		"def run():\n"
		"    for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():\n"
		"        if actor.get_actor_label() == '%s':\n"
		"            actor.set_actor_label('Renamed')\n"
		"    raise RuntimeError('boom')\n"), *Label));
	TestEqual(TEXT("the failing script fails"), Report.Status, FString(TEXT("failed")));
	TestTrue(TEXT("its traceback names the error: ") + Report.Diagnostic, Report.Traceback.Contains(TEXT("boom")));
	TestTrue(TEXT("its edits were undone"), Report.bRolledBack);
	TestEqual(TEXT("the actor keeps its label"), Actor->GetActorLabel(), Label);

	Report = RunScript(TEXT("def run():\n    while True:\n        pass\n"), /*TimeoutSeconds=*/1);
	TestEqual(TEXT("a runaway script stops at its deadline"), Report.Status, FString(TEXT("timed_out")));
	TestTrue(TEXT("and says so"), Report.bTimedOut);

	// The static check misses an aliased open; the audit hook still refuses the write.
	const FString Blocked = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Scripts/blocked.txt"));
	Report = RunScript(FString::Printf(TEXT("def run():\n    f = open\n    f(r'%s', 'w')\n"), *Blocked));
	TestEqual(TEXT("an undoable script cannot write files"), Report.Status, FString(TEXT("failed")));
	TestTrue(TEXT("the guard refused it: ") + Report.Diagnostic, Report.Traceback.Contains(TEXT("PermissionError")));
	TestFalse(TEXT("and no file appeared"), IFileManager::Get().FileExists(*Blocked));

	// Captures are an agent's own scratch: writing one needs no approval, even by a path computed at run time.
	const FString Capture = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Captures/hyperai_script_test.txt"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Capture), /*Tree=*/true);
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Capture, false, true, true); };
	Report = RunScript(FString::Printf(TEXT("def run():\n    path = r'%s'\n    open(path, 'w').write('ok')\n    return {}\n"), *Capture));
	TestEqual(*FString::Printf(TEXT("a script writes its captures (%s)"), *Report.Diagnostic), Report.Status, FString(TEXT("completed")));
	TestTrue(TEXT("and the file is there"), IFileManager::Get().FileExists(*Capture));
	Report = RunScript(TEXT("import unreal\ndef run():\n    getattr(unreal.EditorAssetLibrary, 'delete_' + 'asset')\n"));
	TestTrue(TEXT("a computed name cannot reach what the check escalates: ") + Report.Diagnostic, Report.Traceback.Contains(TEXT("PermissionError")));

	// A guessed name comes back with the names it was near, so the next try needs no dir() probe.
	Report = RunScript(TEXT("import unreal\ndef run():\n    return unreal.KismetSystemLibrary\n"));
	TestTrue(TEXT("a missed attribute names its neighbours: ") + Report.Diagnostic, Report.Diagnostic.Contains(TEXT("SystemLibrary")));

	FHyperAIScriptRunRequest Request;
	Request.Code = TEXT("def run():\n    return {}\n");
	Request.OperationId = Report.OperationId;
	TestEqual(TEXT("an operation id runs once"), FHyperAIStudioScriptingContracts::Run(Request).Status, FString(TEXT("operation_id_used")));
	Request.OperationId.Reset();
	Request.Code = TEXT("x = 1\n");
	TestEqual(TEXT("a script needs run()"), FHyperAIStudioScriptingContracts::Run(Request).Status, FString(TEXT("invalid_script")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScriptingApprovalTest,
	"HyperAIStudio.NativeTools.Scripting.RiskyScriptsWaitForApproval",
	HyperAIStudio::Scripting::Tests::Flags)

bool FHyperAIStudioScriptingApprovalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Scripting::Tests;
	if (!PythonReady(*this)) return true;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT { Settings->bRequireApprovalForAgentEdits = bApproval; };

	const FString Code = TEXT("import os\ndef run():\n    return {'cwd': os.getcwd()}\n");
	FHyperAIScriptRunRequest Check;
	Check.Code = Code;
	Check.bDryRun = true;
	const FHyperAIScriptRunReport Checked = FHyperAIStudioScriptingContracts::Run(Check);
	TestEqual(TEXT("a dry run only checks"), Checked.Status, FString(TEXT("checked")));
	TestTrue(TEXT("and names the reason"), Checked.EscalationReasons.Contains(TEXT("imports os")));

	// Even with agent-edit approval off, a script an undo cannot take back waits for a click.
	FHyperAIScriptRunReport Queued = RunScript(Code);
	TestEqual(TEXT("a risky script waits"), Queued.Status, FString(TEXT("awaiting_user_approval")));
	TestTrue(TEXT("in the approval queue"), FHyperAIStudioApprovalGate::HasPending(Queued.OperationId));
	FHyperAIStudioApprovalGate::Reject(Queued.OperationId, TEXT("test"));
	TestFalse(TEXT("rejecting drops it"), FHyperAIStudioApprovalGate::HasPending(Queued.OperationId));
	TestFalse(TEXT("and nothing ran"), IFileManager::Get().FileExists(*(FPaths::ProjectDir() / Queued.ScriptFolder / TEXT("result.json"))));

	Queued = RunScript(Code);
	FString Error;
	TestTrue(TEXT("approving runs it: ") + Error, FHyperAIStudioApprovalGate::Approve(Queued.OperationId, Error));
	TestTrue(TEXT("it left its result"), IFileManager::Get().FileExists(*(FPaths::ProjectDir() / Queued.ScriptFolder / TEXT("result.json"))));
	const TArray<FHyperAIStudioActivityEntry> Activity = FHyperAIStudioAgentActivityLog::GetSnapshot();
	const FHyperAIStudioActivityEntry* Latest = Activity.FindByPredicate([&Queued](const FHyperAIStudioActivityEntry& Entry)
	{
		return Entry.OperationId == Queued.OperationId;
	});
	TestTrue(*FString::Printf(TEXT("and the activity log records it completed (%s)"), Latest ? *Latest->Detail : TEXT("no entry")),
		Latest && Latest->Kind == EHyperAIStudioActivityKind::Completed && Latest->StatusCode == TEXT("completed"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
