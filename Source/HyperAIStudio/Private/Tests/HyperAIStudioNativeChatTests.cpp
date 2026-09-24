// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentImage.h"
#include "HyperAIStudioNativeChat.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioWidgetShot.h"
#include "ImageUtils.h"
#include "SHyperAIStudioNativeChatView.h"
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

	/** Writes a small valid PNG; every stand-in below makes its pictures with it. */
	const TCHAR* const PngWriter = TEXT(R"PY(import zlib, struct
def png(path, w=4, h=4, rgb=(200, 40, 40)):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)
    return data
)PY");

	/** Codex drawing in a chat: attachments it can see, a saved picture, a viewed one, one sent as data, and a spent limit. */
	const TCHAR* const FakeCodexImages = TEXT(R"PY(import json, sys, os, base64
from pngwriter import png
HERE = os.path.dirname(os.path.abspath(__file__))
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def done(item):
    send({"method": "item/completed", "params": {"item": item}})
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id")
    if method == "initialize":
        send({"id": i, "result": {"userAgent": "fake"}})
    elif method == "thread/start":
        send({"id": i, "result": {"thread": {"id": "thread-img"}, "model": "fake"}})
    elif method == "turn/start":
        images = [x["path"] for x in m["params"]["input"] if x.get("type") == "localImage"]
        send({"id": i, "result": {"turn": {"id": "turn-1", "status": "inProgress", "items": []}}})
        done({"type": "agentMessage", "id": "m1", "text": "saw %d of %d images" % (len([p for p in images if os.path.exists(p)]), len(images))})
        saved = os.path.join(HERE, "codex_generated.png"); png(saved)
        send({"method": "item/started", "params": {"item": {"type": "imageGeneration", "id": "ig1", "status": "generating", "revisedPrompt": None, "result": "", "failure": None}}})
        done({"type": "imageGeneration", "id": "ig1", "status": "completed", "revisedPrompt": "a red cube on grey", "result": "", "failure": None, "savedPath": saved})
        done({"type": "imageView", "id": "iv1", "path": saved})
        data = base64.b64encode(png(os.path.join(HERE, "inline.png"), rgb=(20, 200, 40))).decode()
        done({"type": "imageGeneration", "id": "ig2", "status": "completed", "revisedPrompt": "inline", "result": "data:image/png;base64," + data, "failure": None})
        done({"type": "imageGeneration", "id": "ig3", "status": "failed", "revisedPrompt": None, "result": "", "failure": {"type": "usageLimitExceeded", "limitId": "images", "resetsAt": 1790000000}})
        send({"method": "turn/completed", "params": {"turn": {"id": "turn-1", "status": "completed", "items": []}}})
)PY");

	/** Gemini taking a picture as an image block, and answering with one. */
	const TCHAR* const FakeGeminiImages = TEXT(R"PY(import json, sys, os, base64
from pngwriter import png
HERE = os.path.dirname(os.path.abspath(__file__))
def send(o):
    o["jsonrpc"] = "2.0"; sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def update(u):
    send({"method": "session/update", "params": {"sessionId": "s1", "update": u}})
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id")
    if method == "initialize":
        send({"id": i, "result": {"protocolVersion": 1, "agentCapabilities": {"loadSession": False, "promptCapabilities": {"image": True}}}})
    elif method == "session/new":
        send({"id": i, "result": {"sessionId": "s1"}})
    elif method == "session/prompt":
        blocks = [b for b in m["params"]["prompt"] if b.get("type") == "image"]
        good = [b for b in blocks if base64.b64decode(b["data"])[:4] == b"\x89PNG"]
        update({"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "got %d image(s) as %s" % (len(good), ",".join(b["mimeType"] for b in good))}})
        data = base64.b64encode(png(os.path.join(HERE, "gemini_reply.png"), rgb=(40, 40, 220))).decode()
        update({"sessionUpdate": "agent_message_chunk", "content": {"type": "image", "mimeType": "image/png", "data": data}})
        update({"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "after"}})
        send({"id": i, "result": {"stopReason": "end_turn"}})
)PY");

	/** Codex as the image delegate: it refuses to start unless the thread is headless, then draws or runs out of allowance. */
	const TCHAR* const FakeCodexHeadless = TEXT(R"PY(import json, sys, os
from pngwriter import png
HERE = os.path.dirname(os.path.abspath(__file__))
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id")
    if method == "initialize":
        send({"id": i, "result": {"userAgent": "fake"}})
    elif method == "thread/start":
        p = m["params"]
        if not (p.get("ephemeral") is True and p.get("approvalPolicy") == "never" and p.get("sandbox") == "read-only"
                and "exactly one image" in (p.get("developerInstructions") or "")):
            send({"id": i, "error": {"code": -32600, "message": "not headless: " + json.dumps(p)}})
            continue
        send({"id": i, "result": {"thread": {"id": "thread-headless"}, "model": "fake"}})
    elif method == "turn/start":
        text = m["params"]["input"][0]["text"]
        send({"id": i, "result": {"turn": {"id": "turn-1", "status": "inProgress", "items": []}}})
        if "LIMIT" in text:
            item = {"type": "imageGeneration", "id": "ig1", "status": "failed", "revisedPrompt": None, "result": "", "failure": {"type": "usageLimitExceeded", "limitId": "images", "resetsAt": None}}
        else:
            saved = os.path.join(HERE, "delegated_%d.png" % len(os.listdir(HERE))); png(saved, 8, 6)
            item = {"type": "imageGeneration", "id": "ig1", "status": "completed", "revisedPrompt": "revised: " + text, "result": "", "failure": None, "savedPath": saved}
        send({"method": "item/completed", "params": {"item": item}})
        send({"method": "item/completed", "params": {"item": {"type": "agentMessage", "id": "m1", "text": "Drew it."}}})
        send({"method": "turn/completed", "params": {"turn": {"id": "turn-1", "status": "completed", "items": []}}})
)PY");

	/** Gemini with an image extension that writes its picture into the folder it runs in, behind two permission requests. */
	const TCHAR* const FakeGeminiHeadless = TEXT(R"PY(import json, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngwriter import png
def send(o):
    o["jsonrpc"] = "2.0"; sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def ask(i, title):
    send({"id": i, "method": "session/request_permission", "params": {"sessionId": "s1", "toolCall": {"toolCallId": title, "title": title},
          "options": [{"optionId": "allow", "name": "Allow", "kind": "allow_once"}, {"optionId": "reject", "name": "Reject", "kind": "reject_once"}]}})
prompt = None; first = None
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id")
    if method == "initialize":
        send({"id": i, "result": {"protocolVersion": 1, "agentCapabilities": {"loadSession": False}}})
    elif method == "session/new":
        send({"id": i, "result": {"sessionId": "s1"}})
    elif method == "session/prompt":
        prompt = i; ask(801, "Shell: rm -rf build")
    elif i == 801 and "result" in m:
        first = m["result"]["outcome"].get("optionId"); ask(802, "generate_image")
    elif i == 802 and "result" in m:
        second = m["result"]["outcome"].get("optionId")
        if first == "reject" and second == "allow":
            os.makedirs("nanobanana-output", exist_ok=True); png(os.path.join("nanobanana-output", "pic.png"))
        send({"method": "session/update", "params": {"sessionId": "s1", "update": {"sessionUpdate": "agent_message_chunk",
              "content": {"type": "text", "text": "answers were %s then %s" % (first, second)}}}})
        send({"id": prompt, "result": {"stopReason": "end_turn"}})
)PY");

	/** Codex answering a request with a reference attached: a reply, a picture it drew, and a follow-up. */
	const TCHAR* const FakeCodexShowcase = TEXT(R"PY(import json, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def done(item):
    send({"method": "item/completed", "params": {"item": item}})
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id")
    if method == "initialize":
        send({"id": i, "result": {"userAgent": "fake"}})
    elif method == "thread/start":
        send({"id": i, "result": {"thread": {"id": "thread-show"}, "model": "gpt-5-codex"}})
    elif method == "turn/start":
        send({"id": i, "result": {"turn": {"id": "turn-1", "status": "inProgress", "items": []}}})
        done({"type": "agentMessage", "id": "m1", "text": "I looked at your reference: cool blue sky, warm sand, soft light. I'll keep that softness but move it to dusk."})
        done({"type": "imageGeneration", "id": "ig1", "status": "completed", "revisedPrompt": "A stylized lighthouse on a rocky shore at dusk, warm orange sky fading to violet, low sun behind thin haze, calm dark sea, soft rim light, title-screen composition with space on the right.", "result": "", "failure": None, "savedPath": os.path.join(HERE, "scene.png")})
        done({"type": "agentMessage", "id": "m2", "text": "Here is a first pass. Save it to the project and I can match the level's lighting to it."})
        send({"method": "thread/tokenUsage/updated", "params": {"tokenUsage": {"total": {"totalTokens": 18400}, "last": {"totalTokens": 12000}, "modelContextWindow": 272000}}})
        send({"method": "turn/completed", "params": {"turn": {"id": "turn-1", "status": "completed", "items": []}}})
)PY");

	/**
	 * Codex checking a level: a thought, an Unreal MCP tool call that answers JSON, a shell command, and a reply that
	 * streams in as a report with a table, code and a picture.
	 */
	const TCHAR* const FakeCodexReport = TEXT(R"PY(import json, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def done(item):
    send({"method": "item/completed", "params": {"item": item}})
REPORT = """Checked the arena for the three things you asked about; one needs a fix.

| Check | Result |
|---|---|
| Player start above the floor | PASS 96 cm clear |
| Lights cast shadows | PASS 4 of 4 |
| Nav mesh covers the arena | FAIL 2 areas cannot be reached |

```json
{ "unreachable": 2, "area_m2": 14.5 }
```

![The arena from above](SCENE)
"""
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id")
    if method == "initialize":
        send({"id": i, "result": {"userAgent": "fake"}})
    elif method == "thread/start":
        send({"id": i, "result": {"thread": {"id": "thread-report"}, "model": "gpt-5-codex"}})
    elif method == "turn/start":
        send({"id": i, "result": {"turn": {"id": "turn-1", "status": "inProgress", "items": []}}})
        done({"type": "reasoning", "id": "r1", "summary": ["Check the player start, the lights and the nav mesh, then report."]})
        args = {"toolset_name": "editor_toolset.toolsets.scene.SceneTools", "tool_name": "find_actors", "arguments": {"name": "PlayerStart"}}
        answer = {"returnValue": {"summary": "Found 1 actor named PlayerStart.", "count": 1, "actors": [{"name": "PlayerStart", "class": "PlayerStart", "location": "X=0 Y=0 Z=96"}]}}
        done({"type": "mcpToolCall", "id": "t1", "server": "unreal-mcp", "tool": "call_tool", "status": "completed", "arguments": args,
              "result": {"content": [{"type": "text", "text": json.dumps(answer)}]}})
        done({"type": "commandExecution", "id": "c1", "command": "git status --short", "status": "completed", "aggregatedOutput": " M Content/Maps/Arena.umap\n", "exitCode": 0})
        send({"method": "item/started", "params": {"item": {"type": "agentMessage", "id": "m1", "text": ""}}})
        text = REPORT.replace("SCENE", os.path.join(HERE, "scene.png").replace("\\", "/"))
        for k in range(0, len(text), 48):
            send({"method": "item/agentMessage/delta", "params": {"itemId": "m1", "delta": text[k:k + 48]}})
        done({"type": "agentMessage", "id": "m1", "text": text})
        send({"method": "turn/completed", "params": {"turn": {"id": "turn-1", "status": "completed", "items": []}}})
)PY");

	void WritePng(const FString& Path)
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(200, 40, 40), 16);
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(4, 4, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		FFileHelper::SaveArrayToFile(Png, *Path);
	}

	/** Writes a stand-in and the PNG helper it imports into Dir, and routes every agent CLI to it. */
	void UseStandIn(const FString& Dir, const TCHAR* Script, const TCHAR* Name)
	{
		FFileHelper::SaveStringToFile(PngWriter, *FPaths::Combine(Dir, TEXT("pngwriter.py")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const FString Path = FPaths::Combine(Dir, Name);
		FFileHelper::SaveStringToFile(Script, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FHyperAIStudioNativeChat::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\""), *Path));
	}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeChatCodexImagesTest,
	"HyperAIStudio.NativeChat.CodexImages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeChatCodexImagesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	using EKind = FHyperAIStudioChatItem::EKind;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted Codex stand-in cannot run."));
		return true;
	}
	const FScratch Scratch;
	UseStandIn(Scratch.Dir, FakeCodexImages, TEXT("fake_codex_images.py"));
	const FString Attached = FPaths::Combine(Scratch.Dir, TEXT("attached.png"));
	WritePng(Attached);

	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Codex"), Scratch.Dir, FString(), FString());
	FString Error;
	if (!TestTrue(TEXT("The Codex chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("It becomes ready"), PumpUntil([&Chat]() { return Chat->IsReady(); }));
	Chat->Send(TEXT("draw a red cube"), {Attached, FPaths::Combine(Scratch.Dir, TEXT("missing.png"))});
	TestTrue(TEXT("The turn completes"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));

	const TSharedPtr<FHyperAIStudioChatItem> User = FindKind(*Chat, EKind::User);
	TestTrue(TEXT("The user's message keeps the picture that exists, and only that one"), User.IsValid() && User->Images == TArray<FString>{Attached});
	const TSharedPtr<FHyperAIStudioChatItem> Agent = FindKind(*Chat, EKind::Agent);
	TestTrue(TEXT("Codex was sent the picture as a local image it could open"), Agent.IsValid() && Agent->Text == TEXT("saw 1 of 1 images"));

	const TSharedPtr<FHyperAIStudioChatItem> Drawn = FindKind(*Chat, EKind::Image, 0);
	TestTrue(TEXT("A generated picture is an image item with Codex's file"), Drawn.IsValid() && Drawn->Images.Num() == 1
		&& Drawn->Images[0].EndsWith(TEXT("codex_generated.png")) && IFileManager::Get().FileExists(*Drawn->Images[0]));
	TestTrue(TEXT("with the prompt as Codex rewrote it, completed"), Drawn.IsValid() && Drawn->Text == TEXT("a red cube on grey") && Drawn->Status == TEXT("completed"));
	const TSharedPtr<FHyperAIStudioChatItem> Viewed = FindKind(*Chat, EKind::Image, 1);
	TestTrue(TEXT("A picture Codex looked at is shown too"), Viewed.IsValid() && Viewed->Title == TEXT("Looked at an image") && Viewed->Images.Num() == 1);
	const TSharedPtr<FHyperAIStudioChatItem> Inline = FindKind(*Chat, EKind::Image, 2);
	TestTrue(TEXT("A picture sent as data is written to the chat's image folder"), Inline.IsValid() && Inline->Images.Num() == 1
		&& Inline->Images[0].StartsWith(FHyperAIStudioNativeChat::ImageFolder()) && IFileManager::Get().FileExists(*Inline->Images[0]));
	const TSharedPtr<FHyperAIStudioChatItem> Limit = FindKind(*Chat, EKind::Image, 3);
	TestTrue(TEXT("A spent allowance fails in words, with when it comes back"), Limit.IsValid() && Limit->Status == TEXT("failed")
		&& Limit->Text.Contains(TEXT("used up until")) && Limit->Images.IsEmpty());
	if (Inline.IsValid() && !Inline->Images.IsEmpty())
	{
		IFileManager::Get().Delete(*Inline->Images[0]);
	}
	Chat->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeChatGeminiImagesTest,
	"HyperAIStudio.NativeChat.GeminiImages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeChatGeminiImagesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	using EKind = FHyperAIStudioChatItem::EKind;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted Gemini stand-in cannot run."));
		return true;
	}
	const FScratch Scratch;
	UseStandIn(Scratch.Dir, FakeGeminiImages, TEXT("fake_gemini_images.py"));
	const FString Attached = FPaths::Combine(Scratch.Dir, TEXT("attached.png"));
	WritePng(Attached);

	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Gemini"), Scratch.Dir, FString(), FString());
	FString Error;
	if (!TestTrue(TEXT("The Gemini chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("It becomes ready"), PumpUntil([&Chat]() { return Chat->IsReady(); }));
	Chat->Send(TEXT("what is this?"), {Attached});
	TestTrue(TEXT("The prompt ends"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));

	const TSharedPtr<FHyperAIStudioChatItem> First = FindKind(*Chat, EKind::Agent, 0);
	TestTrue(TEXT("Gemini, which says it takes pictures, got the attachment as a PNG image block"),
		First.IsValid() && First->Text == TEXT("got 1 image(s) as image/png"));
	const TSharedPtr<FHyperAIStudioChatItem> Reply = FindKind(*Chat, EKind::Image);
	TestTrue(TEXT("A picture in Gemini's reply becomes an image item with a file"), Reply.IsValid() && Reply->Images.Num() == 1
		&& IFileManager::Get().FileExists(*Reply->Images[0]));
	const TSharedPtr<FHyperAIStudioChatItem> After = FindKind(*Chat, EKind::Agent, 1);
	TestTrue(TEXT("Text after the picture is a new message"), After.IsValid() && After->Text == TEXT("after"));
	if (Reply.IsValid() && !Reply->Images.IsEmpty())
	{
		IFileManager::Get().Delete(*Reply->Images[0]);
	}
	Chat->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentImageTest,
	"HyperAIStudio.NativeChat.AgentImageDelegation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentImageTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted stand-ins cannot run."));
		return true;
	}
	FString Issue;
	FString Found;
	TestEqual(TEXT("Codex can draw exactly when its CLI is installed"), FHyperAIStudioAgentImage::CanDraw(TEXT("codex"), Issue),
		FHyperAIStudioService::FindExecutableOnPath(TEXT("codex"), Found));
	TestFalse(TEXT("Claude cannot draw"), FHyperAIStudioAgentImage::CanDraw(TEXT("claude"), Issue));
	if (!FHyperAIStudioAgentImage::CanDraw(TEXT("gemini"), Issue))
	{
		TestTrue(TEXT("Gemini without its image extension says how to get one"), Issue.Contains(TEXT("nanobanana")) || Issue.Contains(TEXT("not installed")));
	}

	const FScratch Scratch;
	UseStandIn(Scratch.Dir, FakeCodexHeadless, TEXT("fake_codex_headless.py"));
	FString Error;
	TSharedPtr<FHyperAIStudioAgentImage> Image = FHyperAIStudioAgentImage::Start(TEXT("codex"), TEXT("a lighthouse at dusk"), FIntPoint(1536, 1024), Error);
	if (!TestTrue(TEXT("A Codex picture starts"), Image.IsValid()))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("and finishes"), PumpUntil([&Image]() { return Image->IsDone(); }, 30.0));
	TestTrue(TEXT("Codex drew it on a headless thread, and the picture is there"), !Image->GetImagePath().IsEmpty()
		&& IFileManager::Get().FileExists(*Image->GetImagePath()));
	TestTrue(TEXT("The prompt asked for a landscape picture and said what to draw"), Image->GetRevisedPrompt().Contains(TEXT("landscape"))
		&& Image->GetRevisedPrompt().Contains(TEXT("a lighthouse at dusk")));
	TestTrue(TEXT("with no error"), Image->GetError().IsEmpty());

	Image = FHyperAIStudioAgentImage::Start(TEXT("codex"), TEXT("LIMIT"), FIntPoint(1024, 1024), Error);
	TestTrue(TEXT("A second picture starts"), Image.IsValid() && PumpUntil([&Image]() { return Image->IsDone(); }, 30.0));
	TestTrue(TEXT("A spent allowance ends with no picture, saying so"), Image.IsValid() && Image->GetImagePath().IsEmpty()
		&& Image->GetError().Contains(TEXT("used up")));

	UseStandIn(Scratch.Dir, FakeGeminiHeadless, TEXT("fake_gemini_headless.py"));
	Image = FHyperAIStudioAgentImage::Start(TEXT("gemini"), TEXT("a red door"), FIntPoint(1024, 1024), Error);
	TestTrue(TEXT("A Gemini picture starts"), Image.IsValid() && PumpUntil([&Image]() { return Image->IsDone(); }, 30.0));
	TestTrue(TEXT("With nobody watching, Gemini's shell request was refused and its image tool allowed, and the file it wrote is found"),
		Image.IsValid() && Image->GetImagePath().EndsWith(TEXT("pic.png")) && IFileManager::Get().FileExists(*Image->GetImagePath()));
	if (Image.IsValid() && Image->GetImagePath().IsEmpty())
	{
		AddInfo(TEXT("Gemini said: ") + Image->GetError());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensNativeChatImagesTest,
	"HyperAIStudio.Screens.NativeChatImages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioScreensNativeChatImagesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted Codex stand-in cannot run."));
		return true;
	}
	const FScratch Scratch;
	HyperAIStudio::Tests::WriteScenePng(FPaths::Combine(Scratch.Dir, TEXT("scene.png")), FIntPoint(480, 300));
	const FString Reference = FPaths::Combine(Scratch.Dir, TEXT("reference.png"));
	HyperAIStudio::Tests::WriteScenePng(Reference, FIntPoint(320, 200), FLinearColor(0.35f, 0.55f, 0.95f), FLinearColor(0.92f, 0.82f, 0.62f));
	UseStandIn(Scratch.Dir, FakeCodexShowcase, TEXT("fake_codex_showcase.py"));

	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Codex"), Scratch.Dir, FString(), FString());
	FString Error;
	if (!TestTrue(TEXT("The Codex chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("It becomes ready"), PumpUntil([&Chat]() { return Chat->IsReady(); }));
	Chat->Send(TEXT("Draw a lighthouse at dusk for the title screen, keeping the softness of this reference."), {Reference});
	TestTrue(TEXT("The turn completes"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));

	const TSharedRef<SHyperAIStudioNativeChatView> View = SNew(SHyperAIStudioNativeChatView).Chat(Chat);
	const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(View, FIntPoint(900, 1080), TEXT("step10-native-chat-images"));
	if (Shot.IsEmpty())
	{
		AddWarning(TEXT("This editor cannot render, so no screenshot was taken."));
	}
	else
	{
		AddInfo(TEXT("Screenshot: ") + Shot);
	}
	Chat->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensChatReportTest,
	"HyperAIStudio.Screens.ChatReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioScreensChatReportTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted Codex stand-in cannot run."));
		return true;
	}
	const FScratch Scratch;
	HyperAIStudio::Tests::WriteScenePng(FPaths::Combine(Scratch.Dir, TEXT("scene.png")), FIntPoint(480, 300),
		FLinearColor(0.45f, 0.6f, 0.9f), FLinearColor(0.2f, 0.3f, 0.2f));
	UseStandIn(Scratch.Dir, FakeCodexReport, TEXT("fake_codex_report.py"));

	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Codex"), Scratch.Dir, FString(), FString());
	FString Error;
	if (!TestTrue(TEXT("The Codex chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("It becomes ready"), PumpUntil([&Chat]() { return Chat->IsReady(); }));
	const TSharedRef<SHyperAIStudioNativeChatView> View = SNew(SHyperAIStudioNativeChatView).Chat(Chat);
	Chat->Send(TEXT("Check the arena: player start, shadows and nav mesh. Tell me what needs fixing."));
	TestTrue(TEXT("The turn completes"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));

	const TSharedPtr<FHyperAIStudioChatItem> Tool = FindKind(*Chat, FHyperAIStudioChatItem::EKind::Tool);
	const TSharedPtr<FHyperAIStudioChatItem> Command = FindKind(*Chat, FHyperAIStudioChatItem::EKind::Command);
	const TSharedPtr<FHyperAIStudioChatItem> Reply = FindKind(*Chat, FHyperAIStudioChatItem::EKind::Agent);
	if (!TestTrue(TEXT("The thought, the tool call, the command and the reply all arrive"), Tool.IsValid() && Command.IsValid() && Reply.IsValid()))
	{
		Chat->Stop();
		return false;
	}
	TestTrue(TEXT("The reply streamed in whole"), Reply->Text.Contains(TEXT("| Nav mesh covers the arena | FAIL")) && Reply->Text.Contains(TEXT("scene.png")));

	// A tool's JSON answer reads as a report: its summary first, then its fields and records as tables.
	const FString ToolDetails = SHyperAIStudioNativeChatView::StepMarkdown(*Tool);
	AddInfo(TEXT("Tool details: ") + ToolDetails.Left(400).Replace(TEXT("\n"), TEXT(" / ")));
	TestTrue(TEXT("The tool's answer leads with its summary"), ToolDetails.StartsWith(TEXT("Found 1 actor named PlayerStart.")));
	TestTrue(TEXT("its records become a table"), ToolDetails.Contains(TEXT("| Name | Class | Location |")) && ToolDetails.Contains(TEXT("| PlayerStart | PlayerStart |")));
	TestTrue(TEXT("and the arguments follow as JSON"), ToolDetails.Contains(TEXT("```json")) && ToolDetails.Contains(TEXT("\"tool_name\": \"find_actors\"")));
	TestTrue(TEXT("A command's output is a code block"), SHyperAIStudioNativeChatView::StepMarkdown(*Command).StartsWith(TEXT("```\n M Content/Maps/Arena.umap")));

	// The transcript follows the reply as it grows, and stays at its end.
	View->SetOpen(Tool->Id, true);
	HyperAIStudio::Tests::SaveWidgetShot(View, FIntPoint(900, 560), TEXT("step15-chat-follow-1"), 10);
	TestTrue(TEXT("It follows the conversation"), View->IsFollowing() && View->IsAtEnd());
	Reply->Text += TEXT("\n\nOne more thing: the east ramp is 3 degrees too steep for the bot to climb.\n\n- Lower it to 35 degrees.\n- Or add steps.");
	Chat->OnChanged.Broadcast();
	// A reply is read again a few times a second as it streams; give it that long, as the editor's frames would.
	FPlatformProcess::Sleep(0.15f);
	HyperAIStudio::Tests::SaveWidgetShot(View, FIntPoint(900, 560), TEXT("step15-chat-follow-2"), 10);
	TestTrue(TEXT("Text added to the last reply stays in view"), View->IsAtEnd());

	const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(View, FIntPoint(900, 1200), TEXT("step15-chat-report"), 10);
	if (!Shot.IsEmpty())
	{
		AddInfo(TEXT("Screenshot: ") + Shot);
	}
	Chat->Stop();
	return true;
}

#endif
