// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioEditorRestart.h"
#include "HyperAIStudioNativeChat.h"
#include "HyperAIStudioWidgetShot.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SHyperAIStudioNativeChatView.h"
#include "SHyperAIStudioQuickActionWindow.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace HyperAIStudio::EditorRestartTests
{
	FString NewFolder()
	{
		const FString Folder = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("EditorRestart-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		return Folder;
	}

	/** What the editor writes before it closes, and what the helper writes after the build. */
	void WriteRestart(const FString& Folder, const FString& Token, const FDateTime& RequestedAt, const FString& Build)
	{
		const FString Plan = FString::Printf(TEXT(R"JSON({"token": "%s", "requestedAt": "%s",
			"note": "Rebuilt the jump ability; next, try it in PIE.", "savedPackages": ["/Game/Maps/Arena"],
			"build": {"enabled": true, "target": "HyperAIDevEditor", "logRelative": "Saved/HyperAIStudio/Restart/build.log"},
			"chats": [
				{"tab": 1, "agent": "Claude Code", "sessionId": "s-1", "model": "opus", "subagent": "builder", "title": "Jump", "backgroundId": "", "working": true},
				{"tab": 2, "agent": "Codex", "sessionId": "thread-2", "model": "", "subagent": "", "title": "", "backgroundId": "", "working": false}]})JSON"),
			*Token, *RequestedAt.ToIso8601());
		FFileHelper::SaveStringToFile(Plan, *(Folder / TEXT("request.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FFileHelper::SaveStringToFile(FString::Printf(TEXT(R"JSON({"token": "%s", "state": "relaunched", "build": %s})JSON"), *Token, *Build),
			*(Folder / TEXT("result.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString HelperScript()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/Scripts/HyperAIStudioRestart.ps1")) : FString();
	}

	/** Claude Code picking its chat up after a restart whose build failed: it reads the error, fixes it and says so. */
	const TCHAR* const FakeResumedClaude = TEXT(R"PY(import json, sys
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def event(sid, e):
    send({"type": "stream_event", "event": e, "parent_tool_use_id": None, "session_id": sid})
def whole(sid, b):
    send({"type": "assistant", "message": {"id": "m1", "role": "assistant", "content": [b],
          "usage": {"input_tokens": 8, "cache_read_input_tokens": 41000, "output_tokens": 900}}, "parent_tool_use_id": None, "session_id": sid})
REPLY = """The restart came back with one build error: `Bar` is undeclared in `Foo.cpp`, line 12. It is the jump ability's cooldown field, which I renamed but missed here.

| Step | Result |
|---|---|
| Build after the restart | FAIL 1 error in Foo.cpp |
| Fix | PASS `Bar` is now `JumpCooldown` |

I'll restart once more to build it, then try the jump in PIE."""
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); t = m.get("type")
    if t == "control_request" and m["request"]["subtype"] == "initialize":
        send({"type": "control_response", "response": {"subtype": "success", "request_id": m["request_id"], "response": {}}})
    elif t == "user":
        sid = m.get("session_id")
        event(sid, {"type": "message_start", "message": {"id": "m1"}})
        thought = "The build failed on a name I changed; fix that use, then build again."
        event(sid, {"type": "content_block_delta", "index": 0, "delta": {"type": "thinking_delta", "thinking": thought}})
        whole(sid, {"type": "thinking", "thinking": thought})
        edit = {"type": "tool_use", "id": "e1", "name": "Edit", "input": {"file_path": "E:/Unreal Projects/Aetherfall/Source/LyraGame/Abilities/Foo.cpp",
                "old_string": "    Cooldown = Bar;", "new_string": "    Cooldown = JumpCooldown;"}}
        event(sid, {"type": "content_block_start", "index": 1, "content_block": {"type": "tool_use", "id": "e1", "name": "Edit", "input": {}}})
        whole(sid, edit)
        send({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "e1", "content": "The file has been updated."}]},
              "parent_tool_use_id": None, "session_id": sid})
        event(sid, {"type": "message_start", "message": {"id": "m2"}})
        for k in range(0, len(REPLY), 60):
            event(sid, {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": REPLY[k:k + 60]}})
        send({"type": "result", "subtype": "success", "is_error": False, "result": "done", "session_id": sid,
              "modelUsage": {"claude-opus-5": {"contextWindow": 200000}}})
)PY");

	FString Native(const FString& Path)
	{
		FString Out = Path;
		FPaths::MakePlatformFilename(Out);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioEditorRestartResumeTest,
	"HyperAIStudio.EditorRestart.Resume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioEditorRestartResumeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EditorRestartTests;
	const FString Folder = NewFolder();

	WriteRestart(Folder, TEXT("t1"), FDateTime::UtcNow(), TEXT(R"JSON({"enabled": true, "exitCode": 0, "seconds": 42, "errors": []})JSON"));
	TestTrue(TEXT("A restart's own editor takes its chats back"), FHyperAIStudioEditorRestart::LoadResume(Folder, TEXT("t1"), 3600.0));
	TestFalse(TEXT("and uses its files up, so it resumes once"), FPaths::FileExists(Folder / TEXT("request.json")) || FPaths::FileExists(Folder / TEXT("result.json")));
	const TOptional<FHyperAIStudioRestartChat> First = FHyperAIStudioEditorRestart::TakeChat(1);
	if (!TestTrue(TEXT("Tab 1 has its chat"), First.IsSet()))
	{
		return false;
	}
	AddInfo(TEXT("Told: ") + First->Message.Replace(TEXT("\n"), TEXT(" / ")));
	TestTrue(TEXT("with its agent, conversation, model, subagent and title"), First->Agent == TEXT("Claude Code") && First->SessionId == TEXT("s-1")
		&& First->Model == TEXT("opus") && First->Subagent == TEXT("builder") && First->Title == TEXT("Jump"));
	TestTrue(TEXT("The chat that asked is told the editor restarted, the build, what was saved and its own note"),
		First->Message.StartsWith(TEXT("The Unreal Editor restarted")) && First->Message.Contains(TEXT("HyperAIDevEditor built in 42 s"))
		&& First->Message.Contains(TEXT("Saved before it closed: 1 level or asset")) && First->Message.Contains(TEXT("Your note: Rebuilt the jump ability"))
		&& First->Message.EndsWith(TEXT("Continue where you left off.")));
	TestFalse(TEXT("A tab's chat is handed over once"), FHyperAIStudioEditorRestart::TakeChat(1).IsSet());
	const TOptional<FHyperAIStudioRestartChat> Second = FHyperAIStudioEditorRestart::TakeChat(2);
	TestTrue(TEXT("A chat that did not ask resumes, and is told nothing"), Second.IsSet() && Second->SessionId == TEXT("thread-2") && Second->Message.IsEmpty());
	FString Continue;
	TestTrue(TEXT("A terminal chat's details wait in continue.md"), FFileHelper::LoadFileToString(Continue, *(Folder / TEXT("continue.md"))) && Continue == First->Message);

	WriteRestart(Folder, TEXT("t2"), FDateTime::UtcNow(),
		TEXT(R"JSON({"enabled": true, "exitCode": 6, "seconds": 13, "errors": ["E:\\Game\\Foo.cpp(12): error C2065: 'Bar': undeclared identifier"]})JSON"));
	FHyperAIStudioEditorRestart::LoadResume(Folder, TEXT("t2"), 3600.0);
	const TOptional<FHyperAIStudioRestartChat> Failed = FHyperAIStudioEditorRestart::TakeChat(1);
	TestTrue(TEXT("A failed build is told with its first errors and where the whole log is"), Failed.IsSet()
		&& Failed->Message.Contains(TEXT("failed (exit code 6) after 13 s")) && Failed->Message.Contains(TEXT("binaries from before the build"))
		&& Failed->Message.Contains(TEXT("error C2065: 'Bar'")) && Failed->Message.Contains(TEXT("The whole log: Saved/HyperAIStudio/Restart/build.log")));
	FHyperAIStudioEditorRestart::TakeChat(2);

	WriteRestart(Folder, TEXT("t3"), FDateTime::UtcNow(), TEXT(R"JSON({"enabled": false})JSON"));
	FHyperAIStudioEditorRestart::LoadResume(Folder, TEXT("t3"), 3600.0);
	const TOptional<FHyperAIStudioRestartChat> Unbuilt = FHyperAIStudioEditorRestart::TakeChat(1);
	TestTrue(TEXT("A restart without a build says so"), Unbuilt.IsSet() && Unbuilt->Message.Contains(TEXT("Build: none, as you asked.")));
	FHyperAIStudioEditorRestart::TakeChat(2);

	WriteRestart(Folder, TEXT("t4"), FDateTime::UtcNow(), TEXT(R"JSON({"enabled": false})JSON"));
	TestFalse(TEXT("Another restart's files give nothing back"), FHyperAIStudioEditorRestart::LoadResume(Folder, TEXT("someone-else"), 3600.0));
	TestFalse(TEXT("and are used up all the same"), FPaths::FileExists(Folder / TEXT("request.json")));
	WriteRestart(Folder, TEXT("t5"), FDateTime::UtcNow() - FTimespan::FromHours(2.0), TEXT(R"JSON({"enabled": false})JSON"));
	TestFalse(TEXT("A stale restart gives nothing back"), FHyperAIStudioEditorRestart::LoadResume(Folder, TEXT("t5"), 3600.0));
	TestFalse(TEXT("so no tab gets a chat"), FHyperAIStudioEditorRestart::TakeChat(1).IsSet());

	const FString Prompt = FHyperAIStudioEditorRestart::ContinuePrompt();
	bool bShellSafe = true;
	for (const TCHAR Character : FString(TEXT("\"%^&|<>`")))
	{
		int32 Unused = INDEX_NONE;
		bShellSafe = bShellSafe && !Prompt.FindChar(Character, Unused);
	}
	TestTrue(TEXT("A terminal chat's line has nothing a shell reads, and points at continue.md"), bShellSafe && Prompt.Contains(TEXT("continue.md")));
	TestEqual(TEXT("Chat tabs' page ids"), SHyperAIStudioQuickActionWindow::GetPageId(1), FName(TEXT("Chat")));
	TestEqual(TEXT("and the next ones"), SHyperAIStudioQuickActionWindow::GetPageId(3), FName(TEXT("Chat3")));
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioEditorRestartDryRunTest,
	"HyperAIStudio.EditorRestart.DryRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioEditorRestartDryRunTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioEditorRestart::FOptions Options;
	Options.bDryRun = true;
	// Other tests leave untitled levels behind, which a restart that saves refuses; one that does not save still plans.
	Options.bSaveDirty = false;
	const FHyperAIEditorRestartReport Checked = FHyperAIStudioEditorRestart::Restart(Options);
	AddInfo(FString::Printf(TEXT("Dry run: %s: %s"), *Checked.Status, *Checked.Message));
	AddInfo(TEXT("Build: ") + Checked.BuildCommand);
	AddInfo(TEXT("Reopens with: ") + Checked.RelaunchCommand);
	TestTrue(TEXT("A dry run only reports"), Checked.bOk && Checked.Status == TEXT("checked") && !FHyperAIStudioEditorRestart::IsRestarting());
	TestTrue(TEXT("It would build this editor's own target for the project"), Checked.BuildCommand.Contains(TEXT("Build.bat"))
		&& Checked.BuildCommand.Contains(FString(TEXT(" ")) + FPlatformMisc::GetUBTTargetName() + TEXT(" ")) && Checked.BuildCommand.Contains(TEXT("-Project="))
		&& Checked.BuildCommand.Contains(TEXT("-WaitMutex")));
	const FString Relaunch = Checked.RelaunchCommand;
	TestTrue(TEXT("and open the same project with one restart token"), Relaunch.Contains(FPaths::GetCleanFilename(FPaths::GetProjectFilePath()))
		&& Relaunch.Contains(TEXT("-HyperAIResume=")) && Relaunch.Find(TEXT("-HyperAIResume=")) == Relaunch.Find(TEXT("-HyperAIResume="), ESearchCase::IgnoreCase, ESearchDir::FromEnd));

	Options.bDryRun = false;
	const FHyperAIEditorRestartReport Refused = FHyperAIStudioEditorRestart::Restart(Options);
	TestTrue(TEXT("For real it refuses to restart under automation tests"), !Refused.bOk && Refused.Status == TEXT("refused")
		&& Refused.Message.Contains(TEXT("Automation tests")) && !FHyperAIStudioEditorRestart::IsRestarting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioEditorRestartHelperTest,
	"HyperAIStudio.EditorRestart.Helper",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioEditorRestartHelperTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EditorRestartTests;
	const FString Script = HelperScript();
	if (!TestTrue(TEXT("The helper ships in the plugin"), FPaths::FileExists(Script)))
	{
		return false;
	}
	const FString Folder = NewFolder();
	// A stand-in editor that closes after a second, a build with a compiler error, and a relaunch that leaves a marker.
	uint32 EditorId = 0;
	FProcHandle Editor = FPlatformProcess::CreateProc(TEXT("powershell.exe"), TEXT("-NoProfile -Command Start-Sleep -Seconds 1"), false, true, true,
		&EditorId, 0, nullptr, nullptr);
	const FString BuildScript = Folder / TEXT("build.cmd");
	FFileHelper::SaveStringToFile(TEXT("@echo off\r\necho Building HyperAIDevEditor...\r\necho Foo.cpp(12): error C2065: 'Bar': undeclared identifier\r\nexit /b 6\r\n"), *BuildScript);
	const FString Marker = Folder / TEXT("relaunched.txt");
	const FString Plan = FString::Printf(TEXT(R"JSON({"token": "tok", "project": "Stand-in", "editorProcessId": %u, "closeTimeoutSeconds": 30, "lingerSeconds": 0,
		"build": {"enabled": true, "target": "HyperAIDevEditor", "script": "%s", "log": "%s"},
		"relaunch": {"executable": "cmd.exe", "arguments": "/c echo relaunched -HyperAIResume=tok> \"%s\"", "workingDirectory": "%s"}})JSON"),
		EditorId, *Native(BuildScript).ReplaceCharWithEscapedChar(), *Native(Folder / TEXT("build.log")).ReplaceCharWithEscapedChar(),
		*Native(Marker).ReplaceCharWithEscapedChar(), *Native(Folder).ReplaceCharWithEscapedChar());
	FFileHelper::SaveStringToFile(Plan, *(Folder / TEXT("request.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	int32 Code = -1;
	FString Out;
	FString Err;
	FPlatformProcess::ExecProcess(TEXT("powershell.exe"), *FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -Request \"%s\""),
		*Native(Script), *Native(Folder / TEXT("request.json"))), &Code, &Out, &Err);
	TestFalse(TEXT("It waited for the editor to close"), FPlatformProcess::IsProcRunning(Editor));
	FPlatformProcess::CloseProc(Editor);
	FString ResultText;
	TSharedPtr<FJsonObject> Result;
	const bool bResult = FFileHelper::LoadFileToString(ResultText, *(Folder / TEXT("result.json")))
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResultText), Result) && Result.IsValid();
	AddInfo(TEXT("Result: ") + ResultText.Replace(TEXT("\r\n"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")));
	if (!TestTrue(TEXT("It wrote its result"), bResult))
	{
		AddInfo(TEXT("Helper said: ") + Out + Err);
		return false;
	}
	const TSharedPtr<FJsonObject>* Build = nullptr;
	double ExitCode = 0.0;
	TArray<FString> Errors;
	if (Result->TryGetObjectField(TEXT("build"), Build) && Build)
	{
		(*Build)->TryGetNumberField(TEXT("exitCode"), ExitCode);
		(*Build)->TryGetStringArrayField(TEXT("errors"), Errors);
	}
	TestTrue(TEXT("with the restart's token, relaunched"), Result->GetStringField(TEXT("token")) == TEXT("tok") && Result->GetStringField(TEXT("state")) == TEXT("relaunched"));
	TestTrue(TEXT("The failed build's exit code and its compiler error are kept"), ExitCode == 6.0 && Errors.Num() == 1 && Errors[0].Contains(TEXT("error C2065")));
	FString Relaunched;
	for (int32 Try = 0; Try < 50 && !FFileHelper::LoadFileToString(Relaunched, *Marker); ++Try)
	{
		FPlatformProcess::Sleep(0.1f);
	}
	TestTrue(TEXT("and the project opens again with the token, even after a failed build"), Relaunched.Contains(TEXT("-HyperAIResume=tok")));
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensRestartResumeTest,
	"HyperAIStudio.Screens.RestartResume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioScreensRestartResumeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EditorRestartTests;
	int32 Code = -1;
	FString Out;
	FString Err;
	FPlatformProcess::ExecProcess(TEXT("python"), TEXT("--version"), &Code, &Out, &Err);
	if (Code != 0)
	{
		AddWarning(TEXT("python is not on PATH, so the scripted Claude Code stand-in cannot run."));
		return true;
	}
	const FString Folder = NewFolder();
	const FString Script = Folder / TEXT("fake_claude_resumed.py");
	FFileHelper::SaveStringToFile(FakeResumedClaude, *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FHyperAIStudioNativeChat::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\""), *Script));

	// What the chat that asked is told after a restart whose build failed.
	TSharedPtr<FJsonObject> Plan;
	TSharedPtr<FJsonObject> Result;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(TEXT(R"JSON({"token": "t",
		"note": "Rebuilt the jump ability with its new cooldown; next, try it in PIE.", "savedPackages": ["/Game/Maps/Arena", "/Game/Abilities/GA_Jump"],
		"build": {"enabled": true, "target": "LyraEditor", "logRelative": "Saved/HyperAIStudio/Restart/build.log"}})JSON"))), Plan);
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(TEXT(R"JSON({"token": "t", "state": "relaunched", "build": {"enabled": true,
		"exitCode": 6, "seconds": 48, "errors": ["E:\\Unreal Projects\\Aetherfall\\Source\\LyraGame\\Abilities\\Foo.cpp(12): error C2065: 'Bar': undeclared identifier"]}})JSON"))), Result);
	const FString Told = FHyperAIStudioEditorRestart::DescribeRestart(Plan, Result);

	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Claude Code"), Folder, FString(), FString());
	FString Error;
	if (!TestTrue(TEXT("The Claude Code chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		FHyperAIStudioNativeChat::SetCommandForTests(FString());
		return false;
	}
	const TSharedRef<SHyperAIStudioNativeChatView> View = SNew(SHyperAIStudioNativeChatView).Chat(Chat);
	Chat->Send(Told);
	const double End = FPlatformTime::Seconds() + 20.0;
	auto Answered = [&Chat]()
	{
		return Chat->IsReady() && !Chat->IsBusy() && Chat->GetItems().ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
		{
			return Item->Kind == FHyperAIStudioChatItem::EKind::Agent;
		});
	};
	while (!Answered() && FPlatformTime::Seconds() < End)
	{
		FTSTicker::GetCoreTicker().Tick(0.05f);
		FPlatformProcess::Sleep(0.02f);
	}
	const TSharedPtr<FHyperAIStudioChatItem>* First = Chat->GetItems().FindByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
	{
		return Item->Kind == FHyperAIStudioChatItem::EKind::User;
	});
	TestTrue(TEXT("The resumed chat's first message is the restart, with the failed build's error"), First && (*First)->Text == Told
		&& Told.Contains(TEXT("error C2065")));
	TestTrue(TEXT("and the agent answers it"), Answered());
	const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(View, FIntPoint(900, 1100), TEXT("step19-restart-resume"), 10);
	if (!Shot.IsEmpty())
	{
		AddInfo(TEXT("Screenshot: ") + Shot);
	}
	Chat->Stop();
	FHyperAIStudioNativeChat::SetCommandForTests(FString());
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

#endif
