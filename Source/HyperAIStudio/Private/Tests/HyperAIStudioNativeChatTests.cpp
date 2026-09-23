// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioNativeChat.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::NativeChatTests
{
	/** Codex's app-server as far as one turn goes: a plan, a streamed reply, a command that needs approval, usage. */
	const TCHAR* const FakeCodex = TEXT(R"PY(import json, sys
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id")
    if "jsonrpc" in m:
        send({"method": "error", "params": {"error": {"message": "codex messages carry no jsonrpc field"}, "willRetry": False}})
    if method == "initialize":
        send({"id": i, "result": {"userAgent": "fake"}})
    elif method == "thread/resume":
        send({"id": i, "error": {"code": -32600, "message": "no rollout found"}})
    elif method == "thread/start":
        send({"id": i, "result": {"thread": {"id": "thread-1"}, "model": "fake"}})
    elif method == "turn/start":
        text = m["params"]["input"][0]["text"]
        send({"id": i, "result": {"turn": {"id": "turn-1", "status": "inProgress", "items": []}}})
        send({"method": "turn/plan/updated", "params": {"plan": [{"step": "Read", "status": "completed"}, {"step": "Answer", "status": "inProgress"}]}})
        send({"method": "item/started", "params": {"item": {"type": "agentMessage", "id": "m1", "text": ""}}})
        send({"method": "item/agentMessage/delta", "params": {"itemId": "m1", "delta": "You said: "}})
        send({"method": "item/agentMessage/delta", "params": {"itemId": "m1", "delta": text}})
        send({"method": "item/completed", "params": {"item": {"type": "agentMessage", "id": "m1", "text": "You said: " + text}}})
        send({"id": 900, "method": "item/commandExecution/requestApproval", "params": {"itemId": "c1", "command": "echo hi", "reason": "to test"}})
    elif i == 900 and "result" in m:
        decision = m["result"]["decision"]
        send({"method": "item/started", "params": {"item": {"type": "commandExecution", "id": "c1", "command": "echo hi", "status": "inProgress"}}})
        send({"method": "item/commandExecution/outputDelta", "params": {"itemId": "c1", "delta": "decision=" + decision}})
        send({"method": "item/completed", "params": {"item": {"type": "commandExecution", "id": "c1", "command": "echo hi", "status": "completed", "exitCode": 0, "aggregatedOutput": "decision=" + decision}}})
        send({"method": "thread/tokenUsage/updated", "params": {"tokenUsage": {"total": {"totalTokens": 1500}, "last": {"totalTokens": 1000}, "modelContextWindow": 10000}}})
        send({"method": "turn/completed", "params": {"turn": {"id": "turn-1", "status": "completed", "items": []}}})
)PY");

	/** Gemini over the Agent Client Protocol: a reply, a tool call that asks permission, then the end of the turn. */
	const TCHAR* const FakeGemini = TEXT(R"PY(import json, sys
def send(o):
    o["jsonrpc"] = "2.0"; sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def update(u):
    send({"method": "session/update", "params": {"sessionId": "s1", "update": u}})
prompt = None
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id")
    if m.get("jsonrpc") != "2.0":
        update({"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "MISSING jsonrpc"}})
    if method == "initialize":
        send({"id": i, "result": {"protocolVersion": 1, "agentCapabilities": {"loadSession": False}}})
    elif method == "session/new":
        send({"id": i, "result": {"sessionId": "s1"}})
    elif method == "session/prompt":
        prompt = i
        update({"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "Checking "}})
        update({"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": m["params"]["prompt"][0]["text"]}})
        update({"sessionUpdate": "tool_call", "toolCallId": "t1", "title": "Read file", "kind": "read", "status": "pending"})
        send({"id": 700, "method": "session/request_permission", "params": {"sessionId": "s1", "toolCall": {"toolCallId": "t1", "title": "Read file"},
              "options": [{"optionId": "yes", "name": "Allow", "kind": "allow_once"}, {"optionId": "no", "name": "Deny", "kind": "reject_once"}]}})
    elif i == 700 and "result" in m:
        picked = m["result"]["outcome"]["optionId"]
        update({"sessionUpdate": "tool_call_update", "toolCallId": "t1", "status": "completed", "content": [{"type": "content", "content": {"type": "text", "text": "picked=" + picked}}]})
        update({"sessionUpdate": "plan", "entries": [{"content": "Read", "status": "completed", "priority": "high"}]})
        update({"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "done"}})
        send({"id": prompt, "result": {"stopReason": "end_turn"}})
)PY");

	struct FScratch
	{
		FString Dir = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("NativeChat-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		~FScratch()
		{
			FHyperAIStudioNativeChat::SetCommandForTests(FString());
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/true);
		}
	};

	/** Ticks the chat's pump until Done holds, a few seconds at most. */
	bool PumpUntil(const TFunction<bool()>& Done, const double Seconds = 15.0)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (!Done() && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.05f);
			FPlatformProcess::Sleep(0.02f);
		}
		return Done();
	}

	TSharedPtr<FHyperAIStudioChatItem> FindKind(const FHyperAIStudioNativeChat& Chat, const FHyperAIStudioChatItem::EKind Kind, const int32 Nth = 0)
	{
		int32 Seen = 0;
		for (const TSharedPtr<FHyperAIStudioChatItem>& Item : Chat.GetItems())
		{
			if (Item->Kind == Kind && Seen++ == Nth)
			{
				return Item;
			}
		}
		return nullptr;
	}

	bool HasPython()
	{
		int32 Code = -1;
		FString Out;
		FString Err;
		FPlatformProcess::ExecProcess(TEXT("python"), TEXT("--version"), &Code, &Out, &Err);
		return Code == 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeChatCodexTest,
	"HyperAIStudio.NativeChat.CodexProtocol",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeChatCodexTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	using EKind = FHyperAIStudioChatItem::EKind;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted Codex stand-in cannot run."));
		return true;
	}
	const FScratch Scratch;
	const FString Script = FPaths::Combine(Scratch.Dir, TEXT("fake_codex.py"));
	FFileHelper::SaveStringToFile(FakeCodex, *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FHyperAIStudioNativeChat::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\""), *Script));

	TestFalse(TEXT("Claude Code has no native chat"), FHyperAIStudioNativeChat::IsSupported(TEXT("Claude Code")));
	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Codex"), Scratch.Dir, FString(), TEXT("gone-thread"));
	FString Error;
	if (!TestTrue(TEXT("The Codex chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	Chat->Send(TEXT("hello"));
	TestTrue(TEXT("A message sent before the handshake waits its turn"), Chat->NumQueued() == 1 || Chat->IsBusy());
	TestTrue(TEXT("A chat that cannot be resumed starts fresh and becomes ready"), PumpUntil([&Chat]() { return Chat->IsReady(); }));
	TestEqual(TEXT("It has Codex's thread id"), Chat->GetSessionId(), FString(TEXT("thread-1")));
	TestTrue(TEXT("The queued message goes out, and Codex asks to run a command"), PumpUntil([&Chat]() { return Chat->HasOpenApproval(); }));
	const TSharedPtr<FHyperAIStudioChatItem> Approval = FindKind(*Chat, EKind::Approval);
	TestTrue(TEXT("The approval shows the command and three choices"), Approval.IsValid() && Approval->Text.Contains(TEXT("echo hi")) && Approval->Choices.Num() == 3);
	TestTrue(TEXT("The turn is still running while it waits"), Chat->IsBusy());
	Chat->Answer(Approval->Id, 0);
	TestTrue(TEXT("The turn completes"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));

	const TSharedPtr<FHyperAIStudioChatItem> User = FindKind(*Chat, EKind::User);
	const TSharedPtr<FHyperAIStudioChatItem> Agent = FindKind(*Chat, EKind::Agent);
	const TSharedPtr<FHyperAIStudioChatItem> Command = FindKind(*Chat, EKind::Command);
	TestTrue(TEXT("The user's message is in the transcript"), User.IsValid() && User->Text == TEXT("hello"));
	TestTrue(TEXT("The reply streamed in"), Agent.IsValid() && Agent->Text == TEXT("You said: hello"));
	TestTrue(TEXT("The approval reached Codex as accept"), Command.IsValid() && Command->Text == TEXT("decision=accept") && Command->Status == TEXT("completed"));
	TestTrue(TEXT("The approval shows its answer"), Approval->bAnswered && Approval->Status == TEXT("Run"));
	TestTrue(TEXT("The plan arrived"), Chat->GetPlan().Num() == 2 && Chat->GetPlan()[1].Status == TEXT("inProgress"));
	TestTrue(TEXT("Usage reads as tokens and context"), Chat->GetUsage().Contains(TEXT("1.5k tokens")) && Chat->GetUsage().Contains(TEXT("10%")));
	TestFalse(TEXT("No jsonrpc field was sent to Codex"), Chat->GetItems().ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
	{
		return Item->Text.Contains(TEXT("jsonrpc"));
	}));
	Chat->Stop();
	TestFalse(TEXT("Stop ends the process"), Chat->IsRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeChatGeminiTest,
	"HyperAIStudio.NativeChat.GeminiProtocol",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeChatGeminiTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	using EKind = FHyperAIStudioChatItem::EKind;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted Gemini stand-in cannot run."));
		return true;
	}
	const FScratch Scratch;
	const FString Script = FPaths::Combine(Scratch.Dir, TEXT("fake_gemini.py"));
	FFileHelper::SaveStringToFile(FakeGemini, *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FHyperAIStudioNativeChat::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\""), *Script));

	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Gemini"), Scratch.Dir, FString(), FString());
	FString Error;
	if (!TestTrue(TEXT("The Gemini chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("It becomes ready with Gemini's session id"), PumpUntil([&Chat]() { return Chat->IsReady(); }) && Chat->GetSessionId() == TEXT("s1"));
	Chat->Send(TEXT("the level"));
	TestTrue(TEXT("Gemini asks permission for its tool"), PumpUntil([&Chat]() { return Chat->HasOpenApproval(); }));
	const TSharedPtr<FHyperAIStudioChatItem> Approval = FindKind(*Chat, EKind::Approval);
	TestTrue(TEXT("with its own option names"), Approval.IsValid() && Approval->Choices == TArray<FString>{TEXT("Allow"), TEXT("Deny")});
	Chat->Answer(Approval->Id, 0);
	TestTrue(TEXT("The prompt ends when Gemini answers it"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));

	const TSharedPtr<FHyperAIStudioChatItem> First = FindKind(*Chat, EKind::Agent, 0);
	const TSharedPtr<FHyperAIStudioChatItem> Second = FindKind(*Chat, EKind::Agent, 1);
	const TSharedPtr<FHyperAIStudioChatItem> Tool = FindKind(*Chat, EKind::Tool);
	TestTrue(TEXT("Chunks join into one message"), First.IsValid() && First->Text == TEXT("Checking the level"));
	TestTrue(TEXT("Text after a tool call is a new message"), Second.IsValid() && Second->Text == TEXT("done"));
	TestTrue(TEXT("The chosen option reached Gemini"), Tool.IsValid() && Tool->Text.Contains(TEXT("picked=yes")) && Tool->Status == TEXT("completed"));
	TestTrue(TEXT("The plan arrived"), Chat->GetPlan().Num() == 1);
	TestFalse(TEXT("Every message carried jsonrpc 2.0"), Chat->GetItems().ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
	{
		return Item->Text.Contains(TEXT("MISSING"));
	}));
	Chat->Stop();
	return true;
}

#endif
