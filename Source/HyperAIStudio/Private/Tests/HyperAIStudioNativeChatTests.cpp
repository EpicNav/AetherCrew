// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "HyperAIStudioAgentImage.h"
#include "HyperAIStudioNativeChat.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioWidgetShot.h"
#include "Misc/ScopeExit.h"
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

	/**
	 * Claude Code headless, as its Agent SDK drives it: a control handshake after the hooks report, messages that stream
	 * and then come again whole, a permission request, tool results with a picture, the to-do and task tools, a subagent's
	 * own message, a message that never streamed, and an interrupt that withdraws an open question.
	 */
	const TCHAR* const FakeClaude = TEXT(R"PY(import json, sys, os, base64
from pngwriter import png
HERE = os.path.dirname(os.path.abspath(__file__))
SID = None
USAGE = {"input_tokens": 12, "cache_creation_input_tokens": 3000, "cache_read_input_tokens": 17000, "output_tokens": 988}
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def ev(e):
    send({"type": "stream_event", "event": e, "parent_tool_use_id": None, "session_id": SID, "uuid": "u"})
def whole(mid, b):
    send({"type": "assistant", "message": {"id": mid, "role": "assistant", "content": [b], "usage": USAGE}, "parent_tool_use_id": None, "session_id": SID})
def streamed(mid, blocks):
    # Each block streams, then comes again whole in an assistant message of its own, under the same message id.
    ev({"type": "message_start", "message": {"id": mid, "role": "assistant", "content": [], "usage": {"input_tokens": 12}}})
    for i, b in enumerate(blocks):
        if b["type"] == "tool_use":
            ev({"type": "content_block_start", "index": i, "content_block": {"type": "tool_use", "id": b["id"], "name": b["name"], "input": {}}})
            ev({"type": "content_block_delta", "index": i, "delta": {"type": "input_json_delta", "partial_json": json.dumps(b["input"])}})
        else:
            kind = b["type"]
            ev({"type": "content_block_start", "index": i, "content_block": {"type": kind, kind: ""}})
            for k in range(0, len(b[kind]), 9):
                ev({"type": "content_block_delta", "index": i, "delta": {"type": kind + "_delta", kind: b[kind][k:k + 9]}})
        ev({"type": "content_block_stop", "index": i})
        whole(mid, b)
    ev({"type": "message_delta", "delta": {"stop_reason": "end_turn"}, "usage": {"output_tokens": 988}})
    ev({"type": "message_stop"})
def result(tool, content, structured=None):
    m = {"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": tool, "content": content, "is_error": False}]},
         "parent_tool_use_id": None, "session_id": SID}
    if structured:
        m["tool_use_result"] = structured
    send(m)
def finish(text):
    send({"type": "result", "subtype": "success", "is_error": False, "result": text, "session_id": SID, "usage": {"input_tokens": 40, "output_tokens": 2000},
          "modelUsage": {"claude-opus-5": {"inputTokens": 40, "outputTokens": 2000, "contextWindow": 200000}}})
turn = 0
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); t = m.get("type"); r = m.get("request") or {}
    if t == "control_request" and r.get("subtype") == "initialize":
        # Hooks report first, under an id that is not the chat's.
        send({"type": "system", "subtype": "hook_started", "hook_name": "SessionStart:startup", "session_id": "hook-session"})
        send({"type": "control_response", "response": {"subtype": "success", "request_id": m["request_id"], "response": {"commands": []}}})
    elif t == "control_request" and r.get("subtype") == "interrupt":
        send({"type": "control_response", "response": {"subtype": "success", "request_id": m["request_id"], "response": {}}})
        send({"type": "control_cancel_request", "request_id": "perm-late"})
        send({"type": "result", "subtype": "error_during_execution", "is_error": True, "errors": ["Request was aborted."], "session_id": SID})
    elif t == "user":
        turn += 1
        SID = m.get("session_id")
        content = m["message"]["content"]
        text = [b["text"] for b in content if b["type"] == "text"][0]
        pictures = [b for b in content if b["type"] == "image" and b["source"]["media_type"] == "image/png"
                    and base64.b64decode(b["source"]["data"])[:4] == b"\x89PNG"]
        if turn == 1:
            send({"type": "system", "subtype": "init", "session_id": SID, "model": "claude-opus-5", "tools": ["Bash", "Edit"],
                  "mcp_servers": [{"name": "unreal-mcp", "status": "connected"}, {"name": "nwiro", "status": "failed"}]})
            streamed("msg_1", [
                {"type": "thinking", "thinking": "Check the level, then answer."},
                {"type": "text", "text": "You said: %s, with %d picture(s)." % (text, len(pictures))},
                {"type": "tool_use", "id": "toolu_todo", "name": "TodoWrite", "input": {"todos": [
                    {"content": "Read the level", "status": "completed", "activeForm": "Reading the level"},
                    {"content": "Answer", "status": "in_progress", "activeForm": "Answering"}]}},
                {"type": "tool_use", "id": "toolu_bash", "name": "Bash", "input": {"command": "echo hi", "description": "Say hi"}}])
            result("toolu_todo", "Todos have been modified successfully.")
            send({"type": "control_request", "request_id": "perm-1", "request": {"subtype": "can_use_tool", "tool_name": "Bash",
                  "input": {"command": "echo hi", "description": "Say hi"}, "tool_use_id": "toolu_bash", "title": "Claude wants to run echo hi",
                  "decision_reason": "\u001b[1mecho\u001b[0m is not in your allow list",
                  "permission_suggestions": [{"type": "addRules", "rules": [{"toolName": "Bash", "ruleContent": "echo:*"}], "behavior": "allow", "destination": "localSettings"}]}})
        elif turn == 2:
            streamed("msg_3", [{"type": "tool_use", "id": "toolu_task", "name": "TaskCreate", "input": {"subject": "Build the arena", "description": "Block it out"}}])
            result("toolu_task", "Task #7 created successfully: Build the arena", structured={"task": {"id": "7", "subject": "Build the arena"}})
            streamed("msg_4", [{"type": "tool_use", "id": "toolu_task2", "name": "TaskUpdate", "input": {"taskId": "7", "status": "in_progress"}}])
            result("toolu_task2", "Updated task #7 status")
            finish("Started on the arena.")
        else:
            # Waits on a question until it is interrupted.
            send({"type": "control_request", "request_id": "perm-late", "request": {"subtype": "can_use_tool", "tool_name": "Write",
                  "input": {"file_path": os.path.join(HERE, "late.txt"), "content": "late"}, "tool_use_id": "toolu_late"}})
    elif t == "control_response" and m["response"]["request_id"] == "perm-1":
        answer = m["response"]["response"]
        decision = answer["behavior"] + ("+rules" if answer.get("updatedPermissions") else "") \
            + ("+input" if (answer.get("updatedInput") or {}).get("command") == "echo hi" else "")
        result("toolu_bash", "decision=" + decision)
        streamed("msg_2", [
            {"type": "tool_use", "id": "toolu_mcp", "name": "mcp__unreal-mcp__call_tool", "input": {"toolset_name": "editor_toolset.toolsets.scene.SceneTools",
             "tool_name": "find_actors", "arguments": {"name": "PlayerStart"}}},
            {"type": "tool_use", "id": "toolu_edit", "name": "Edit", "input": {"file_path": os.path.join(HERE, "Notes.md"), "old_string": "old line", "new_string": "new line"}}])
        found = {"summary": "Found 1 actor named PlayerStart.", "count": 1, "actors": [{"name": "PlayerStart", "class": "PlayerStart", "location": "X=0 Y=0 Z=96"}]}
        data = base64.b64encode(png(os.path.join(HERE, "shot.png"), rgb=(30, 160, 60))).decode()
        result("toolu_mcp", [{"type": "text", "text": json.dumps(found)}, {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": data}}])
        result("toolu_edit", "The file has been updated successfully.")
        # A subagent's own steps carry its card's id, and stay out of the transcript.
        send({"type": "assistant", "message": {"id": "msg_sub", "role": "assistant", "content": [{"type": "text", "text": "SUBAGENT"}]}, "parent_tool_use_id": "toolu_agent", "session_id": SID})
        # A message that comes whole, without streaming, still shows.
        whole("msg_5", {"type": "text", "text": "Done: the start is fine."})
        finish("Done: the start is fine.")
)PY");

	/** Claude Code checking a level: a plan, an Unreal MCP tool, a command it asks to run, an edit, and a report. */
	const TCHAR* const FakeClaudeShowcase = TEXT(R"PY(import json, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
SID = None
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def streamed(mid, blocks):
    send({"type": "stream_event", "event": {"type": "message_start", "message": {"id": mid}}, "parent_tool_use_id": None, "session_id": SID})
    for i, b in enumerate(blocks):
        kind = b["type"]
        if kind == "tool_use":
            event = {"type": "content_block_start", "index": i, "content_block": {"type": "tool_use", "id": b["id"], "name": b["name"], "input": {}}}
            send({"type": "stream_event", "event": event, "parent_tool_use_id": None, "session_id": SID})
        else:
            for k in range(0, len(b[kind]), 40):
                event = {"type": "content_block_delta", "index": i, "delta": {"type": kind + "_delta", kind: b[kind][k:k + 40]}}
                send({"type": "stream_event", "event": event, "parent_tool_use_id": None, "session_id": SID})
        send({"type": "assistant", "message": {"id": mid, "role": "assistant", "content": [b],
              "usage": {"input_tokens": 6, "cache_creation_input_tokens": 2400, "cache_read_input_tokens": 31000, "output_tokens": 1200}},
              "parent_tool_use_id": None, "session_id": SID})
def result(tool, content):
    send({"type": "user", "message": {"role": "user", "content": [{"type": "tool_result", "tool_use_id": tool, "content": content}]},
          "parent_tool_use_id": None, "session_id": SID})
REPORT = """Checked the arena. The player start and the lights are fine; the nav mesh was not, and I fixed its cause.

| Check | Result |
|---|---|
| Player start above the floor | PASS 96 cm clear |
| Lights cast shadows | PASS 4 of 4 |
| Nav mesh covers the arena | FAIL 2 areas cannot be reached |

The east ramp was 3 degrees too steep for the bot to climb. I lowered it to 35 degrees in `ArenaNotes.md`; rebuild the nav mesh to reach both areas.

![The arena from above](SCENE)
"""
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); t = m.get("type")
    if t == "control_request" and m["request"]["subtype"] == "initialize":
        send({"type": "control_response", "response": {"subtype": "success", "request_id": m["request_id"], "response": {}}})
    elif t == "user":
        SID = m.get("session_id")
        streamed("msg_1", [
            {"type": "thinking", "thinking": "Find the player start and the lights, check the nav mesh, then fix what fails."},
            {"type": "text", "text": "I'll check the arena: the player start, the lights and the nav mesh."},
            {"type": "tool_use", "id": "todo", "name": "TodoWrite", "input": {"todos": [
                {"content": "Find the player start", "status": "completed", "activeForm": "Finding the player start"},
                {"content": "Check the lights", "status": "completed", "activeForm": "Checking the lights"},
                {"content": "Check the nav mesh", "status": "in_progress", "activeForm": "Checking the nav mesh"},
                {"content": "Fix what fails", "status": "pending", "activeForm": "Fixing what fails"}]}},
            {"type": "tool_use", "id": "t1", "name": "mcp__unreal-mcp__call_tool", "input": {"toolset_name": "editor_toolset.toolsets.scene.SceneTools",
             "tool_name": "find_actors", "arguments": {"name": "PlayerStart"}}}])
        found = {"summary": "Found 1 actor named PlayerStart.", "count": 1, "actors": [{"name": "PlayerStart", "class": "PlayerStart", "location": "X=0 Y=0 Z=96"}]}
        result("todo", "Todos have been modified successfully.")
        result("t1", [{"type": "text", "text": json.dumps(found)}])
        streamed("msg_2", [{"type": "tool_use", "id": "c1", "name": "Bash", "input": {"command": "git status --short", "description": "Show changed files"}}])
        send({"type": "control_request", "request_id": "p1", "request": {"subtype": "can_use_tool", "tool_name": "Bash",
              "input": {"command": "git status --short", "description": "Show changed files"}, "tool_use_id": "c1",
              "permission_suggestions": [{"type": "addRules", "rules": [{"toolName": "Bash", "ruleContent": "git status:*"}], "behavior": "allow", "destination": "localSettings"}]}})
    elif t == "control_response":
        result("c1", " M Content/Maps/Arena.umap")
        streamed("msg_3", [{"type": "tool_use", "id": "e1", "name": "Edit", "input": {"file_path": os.path.join(HERE, "ArenaNotes.md"),
                 "old_string": "Ramp slope: 38 degrees", "new_string": "Ramp slope: 35 degrees"}}])
        result("e1", "The file has been updated successfully.")
        streamed("msg_4", [{"type": "text", "text": REPORT.replace("SCENE", os.path.join(HERE, "scene.png").replace("\\", "/"))}])
        send({"type": "result", "subtype": "success", "is_error": False, "result": "done", "session_id": SID,
              "modelUsage": {"claude-opus-5": {"contextWindow": 200000}}})
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

	/**
	 * An agent over the Agent Client Protocol, such as OpenCode: it lists its models as a config option (grouped), or the
	 * older way with "legacy", takes no HTTP MCP with "nohttp", and answers a prompt with the MCP servers it was handed
	 * and the model it is on.
	 */
	const TCHAR* const FakeAcpAgent = TEXT(R"PY(import json, sys
legacy = "legacy" in sys.argv; nohttp = "nohttp" in sys.argv
def send(o):
    o["jsonrpc"] = "2.0"; sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
mcp = []; model = "m-small"
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id"); p = m.get("params") or {}
    if method == "initialize":
        send({"id": i, "result": {"protocolVersion": 1, "agentCapabilities": {"loadSession": False, "mcpCapabilities": {"http": not nohttp, "sse": False}}}})
    elif method == "session/new":
        mcp = p.get("mcpServers", [])
        if legacy:
            send({"id": i, "result": {"sessionId": "s1", "models": {"currentModelId": model,
                  "availableModels": [{"modelId": "m-small", "name": "Small"}, {"modelId": "m-large", "name": "Large"}]}}})
        else:
            send({"id": i, "result": {"sessionId": "s1", "configOptions": [{"id": "mode", "name": "Mode", "category": "mode", "type": "select", "currentValue": "ask",
                  "options": [{"value": "ask", "name": "Ask"}]}, {"id": "model", "name": "Model", "category": "model", "type": "select", "currentValue": model,
                  "options": [{"value": "m-small", "name": "Small"}, {"group": "big", "name": "Big", "options": [{"value": "m-large", "name": "Large"}]}]}]}})
    elif method == "session/set_config_option" and p.get("configId") == "model":
        model = p["value"]; send({"id": i, "result": {"configOptions": []}})
    elif method == "session/set_model":
        model = p["modelId"]; send({"id": i, "result": {}})
    elif method == "session/prompt":
        servers = ",".join("%s:%s=%s" % (s.get("type"), s.get("name"), s.get("url")) for s in mcp)
        send({"method": "session/update", "params": {"sessionId": "s1", "update": {"sessionUpdate": "agent_message_chunk",
              "content": {"type": "text", "text": "mcp=[%s] model=%s" % (servers, model)}}}})
        send({"id": i, "result": {"stopReason": "end_turn"}})
    elif i is not None:
        send({"id": i, "error": {"code": -32601, "message": "unknown " + str(method)}})
)PY");

	/** Codex's app-server, far enough to show the provider and model a thread started with, as its id. */
	const TCHAR* const FakeCodexThread = TEXT(R"PY(import json, sys
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id"); p = m.get("params") or {}
    if method == "initialize":
        send({"id": i, "result": {"userAgent": "fake"}})
    elif method == "thread/start":
        send({"id": i, "result": {"thread": {"id": "%s|%s" % (p.get("modelProvider"), p.get("model"))}, "model": p.get("model")}})
)PY");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeChatAcpAgentsTest,
	"HyperAIStudio.NativeChat.AcpAgents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeChatAcpAgentsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	using EKind = FHyperAIStudioChatItem::EKind;
	TestTrue(TEXT("GitHub Copilot, OpenCode, Kimi and the other ACP agents chat natively"), FHyperAIStudioNativeChat::IsSupported(TEXT("GitHub Copilot"))
		&& FHyperAIStudioNativeChat::IsSupported(TEXT("OpenCode")) && FHyperAIStudioNativeChat::IsSupported(TEXT("Kimi"))
		&& FHyperAIStudioNativeChat::IsSupported(TEXT("Gemini")) && !FHyperAIStudioNativeChat::IsSupported(TEXT("Notepad")));
	TestFalse(TEXT("Gemini keeps its own route, so it is not listed with them"), FHyperAIStudioNativeChat::GetInstalledAcpAgents().Contains(TEXT("Gemini")));
	TestTrue(TEXT("In a terminal, such an agent runs its own interface"), FHyperAIStudioNativeChat::GetAcpTerminalCommand(TEXT("OpenCode")).Contains(TEXT("opencode"))
		&& FHyperAIStudioNativeChat::GetAcpTerminalCommand(TEXT("Codex")).IsEmpty());
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted agents cannot run."));
		return true;
	}
	const FScratch Scratch;
	const FString Script = FPaths::Combine(Scratch.Dir, TEXT("fake_acp.py"));
	FFileHelper::SaveStringToFile(FakeAcpAgent, *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	// The agent's models are saved as its Model menu; the user's own menus come back afterwards.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioAgentModelRoute> Routes = Settings->AgentModelRoutes;
	ON_SCOPE_EXIT
	{
		Settings->AgentModelRoutes = Routes;
		Settings->SaveConfig();
	};

	// One turn with the stand-in in Mode, starting on Model; the chat is stopped, its items kept.
	auto Turn = [&](const TCHAR* Mode, const FString& Model)
	{
		FHyperAIStudioNativeChat::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\" %s"), *Script, Mode));
		const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("OpenCode"), Scratch.Dir, Model, FString());
		FString Error;
		if (!Chat.IsValid() || !Chat->Start(Error))
		{
			AddError(TEXT("The OpenCode chat did not start: ") + Error);
			return Chat;
		}
		PumpUntil([&Chat]() { return Chat->IsReady(); });
		Chat->Send(TEXT("hi"));
		PumpUntil([&Chat]() { return !Chat->IsBusy() && FindKind(*Chat, EKind::Agent).IsValid(); });
		Chat->Stop();
		return Chat;
	};
	auto Reply = [](const TSharedPtr<FHyperAIStudioNativeChat>& Chat)
	{
		const TSharedPtr<FHyperAIStudioChatItem> Item = Chat.IsValid() ? FindKind(*Chat, FHyperAIStudioChatItem::EKind::Agent) : nullptr;
		return Item.IsValid() ? Item->Text : FString();
	};
	auto Told = [](const TSharedPtr<FHyperAIStudioNativeChat>& Chat, const TCHAR* Text)
	{
		return Chat.IsValid() && Chat->GetItems().ContainsByPredicate([Text](const TSharedPtr<FHyperAIStudioChatItem>& Item)
		{
			return Item->Kind == FHyperAIStudioChatItem::EKind::Notice && Item->Text.Contains(Text);
		});
	};

	const FString Endpoint = FHyperAIStudioService::GetEndpoint(Settings->UnrealMCPPort, Settings->UnrealMCPUrlPath);
	const FString Chosen = Reply(Turn(TEXT("options"), TEXT("m-large")));
	AddInfo(TEXT("OpenCode said: ") + Chosen);
	TestTrue(TEXT("The session is handed Unreal MCP over HTTP"), Chosen.Contains(FString::Printf(TEXT("mcp=[http:unreal-mcp=%s]"), *Endpoint)));
	TestTrue(TEXT("and moves to the chosen model through the agent's model option"), Chosen.EndsWith(TEXT("model=m-large")));
	const FHyperAIStudioAgentModelRoute* Route = Settings->FindAgentModelRoute(TEXT("OpenCode"));
	TestTrue(TEXT("The agent's own models, groups and all, become its Model menu"), Route && Route->Models == TArray<FString>{TEXT("m-small"), TEXT("m-large")});

	TestTrue(TEXT("An agent that lists models the older way is switched the older way"), Reply(Turn(TEXT("legacy"), TEXT("m-large"))).EndsWith(TEXT("model=m-large")));
	const TSharedPtr<FHyperAIStudioNativeChat> Unknown = Turn(TEXT("options"), TEXT("m-huge"));
	TestTrue(TEXT("A model the agent does not offer leaves it on its own, and says so"), Reply(Unknown).EndsWith(TEXT("model=m-small")) && Told(Unknown, TEXT("does not offer m-huge")));
	const TSharedPtr<FHyperAIStudioNativeChat> NoHttp = Turn(TEXT("nohttp"), FString());
	TestTrue(TEXT("An agent that takes no HTTP MCP gets none, and the chat says so"), Reply(NoHttp).StartsWith(TEXT("mcp=[]"))
		&& Told(NoHttp, TEXT("cannot take Unreal MCP from HyperAI")));

	// A model Ollama serves reaches Codex through its own provider for it.
	const FString CodexScript = FPaths::Combine(Scratch.Dir, TEXT("fake_codex_thread.py"));
	FFileHelper::SaveStringToFile(FakeCodexThread, *CodexScript, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FHyperAIStudioNativeChat::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\""), *CodexScript));
	const TSharedPtr<FHyperAIStudioNativeChat> Codex = FHyperAIStudioNativeChat::Create(TEXT("Codex"), Scratch.Dir, TEXT("ollama/qwen3.6:latest"), FString());
	FString Error;
	TestTrue(TEXT("A Codex chat on a local model starts"), Codex.IsValid() && Codex->Start(Error));
	TestTrue(TEXT("with Ollama as its provider and the model by Ollama's name"), PumpUntil([&Codex]() { return Codex->IsReady(); })
		&& Codex->GetSessionId() == TEXT("ollama|qwen3.6:latest"));
	Codex->Stop();
	return true;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeChatClaudeTest,
	"HyperAIStudio.NativeChat.ClaudeProtocol",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeChatClaudeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	using EKind = FHyperAIStudioChatItem::EKind;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted Claude Code stand-in cannot run."));
		return true;
	}
	const FScratch Scratch;
	UseStandIn(Scratch.Dir, FakeClaude, TEXT("fake_claude.py"));
	const FString Attached = FPaths::Combine(Scratch.Dir, TEXT("attached.png"));
	WritePng(Attached);

	TestTrue(TEXT("Claude Code has a native chat"), FHyperAIStudioNativeChat::IsSupported(TEXT("Claude Code")));
	TestFalse(TEXT("A terminal-only agent has none"), FHyperAIStudioNativeChat::IsSupported(TEXT("Cursor")));
	const FString Gone = TEXT("0f0e0d0c-0b0a-4000-8000-000000000001");
	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Claude Code"), Scratch.Dir, FString(), Gone);
	FString Error;
	if (!TestTrue(TEXT("The Claude Code chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	const FString ChatId = Chat->GetSessionId();
	TestTrue(TEXT("A chat Claude never wrote down starts again, under a new id it has from the start"), ChatId.Len() == 36 && ChatId != Gone);
	// The view builds a row as soon as it hears of an item: an approval announced before its choices would have no buttons.
	bool bBareApproval = false;
	Chat->OnChanged.AddLambda([&Chat, &bBareApproval]()
	{
		const TArray<TSharedPtr<FHyperAIStudioChatItem>>& Items = Chat->GetItems();
		bBareApproval = bBareApproval || (!Items.IsEmpty() && Items.Last()->Kind == EKind::Approval && Items.Last()->Choices.IsEmpty());
	});
	auto ById = [&Chat](const FString& Id)
	{
		const TSharedPtr<FHyperAIStudioChatItem>* Found = Chat->GetItems().FindByPredicate([&Id](const TSharedPtr<FHyperAIStudioChatItem>& Item) { return Item->Id == Id; });
		return Found ? *Found : TSharedPtr<FHyperAIStudioChatItem>();
	};

	Chat->Send(TEXT("hello"), {Attached});
	TestEqual(TEXT("A message sent before the handshake waits its turn"), Chat->NumQueued(), 1);
	TestTrue(TEXT("The handshake makes it ready"), PumpUntil([&Chat]() { return Chat->IsReady(); }));
	TestTrue(TEXT("The queued message goes out, and Claude asks to run a command"), PumpUntil([&Chat]() { return Chat->HasOpenApproval(); }));
	const TSharedPtr<FHyperAIStudioChatItem> Approval = FindKind(*Chat, EKind::Approval);
	if (!TestTrue(TEXT("The question is in the transcript"), Approval.IsValid()))
	{
		Chat->Stop();
		return false;
	}
	TestEqual(TEXT("It has Claude's own title"), Approval->Title, FString(TEXT("Claude wants to run echo hi")));
	TestTrue(TEXT("It shows the command, and the reason without terminal colour codes"), Approval->Text.Contains(TEXT("echo hi"))
		&& Approval->Text.Contains(TEXT("echo is not in your allow list")) && !Approval->Text.Contains(TEXT("\x1b")));
	TestEqual(TEXT("It offers Allow, Allow and don't ask again, and Deny"), Approval->Choices.Num(), 3);
	TestFalse(TEXT("It was announced with its choices in, so the view shows its buttons"), bBareApproval);
	TestTrue(TEXT("A command's own description comes with it"), Approval->Text.StartsWith(TEXT("Say hi")));
	Chat->Answer(Approval->Id, 1);
	TestTrue(TEXT("The turn completes"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));
	TestEqual(TEXT("The chat keeps its id; the hooks' own is not taken for it"), Chat->GetSessionId(), ChatId);

	const TSharedPtr<FHyperAIStudioChatItem> User = FindKind(*Chat, EKind::User);
	TestTrue(TEXT("The user's message keeps its picture"), User.IsValid() && User->Text == TEXT("hello") && User->Images == TArray<FString>{Attached});
	const TSharedPtr<FHyperAIStudioChatItem> Thought = FindKind(*Chat, EKind::Reasoning);
	TestTrue(TEXT("Thinking streams in as its own step"), Thought.IsValid() && Thought->Text == TEXT("Check the level, then answer."));
	const TSharedPtr<FHyperAIStudioChatItem> Reply = FindKind(*Chat, EKind::Agent);
	TestTrue(TEXT("The reply streams in once, and Claude got the picture as a PNG"), Reply.IsValid() && Reply->Text == TEXT("You said: hello, with 1 picture(s)."));
	TestTrue(TEXT("TodoWrite becomes the plan, not a card"), Chat->GetPlan().Num() == 2 && Chat->GetPlan()[1].Status == TEXT("inProgress")
		&& !ById(TEXT("toolu_todo")).IsValid());
	const TSharedPtr<FHyperAIStudioChatItem> Command = ById(TEXT("toolu_bash"));
	TestTrue(TEXT("Bash is a command card, and Claude got the answer with its rule and the input"), Command.IsValid() && Command->Kind == EKind::Command
		&& Command->Title == TEXT("echo hi") && Command->Text == TEXT("decision=allow+rules+input") && Command->Status == TEXT("completed"));
	TestTrue(TEXT("The question shows its answer"), Approval->bAnswered && Approval->Status == TEXT("Allow, and don't ask again"));
	const TSharedPtr<FHyperAIStudioChatItem> Tool = ById(TEXT("toolu_mcp"));
	TestTrue(TEXT("Unreal's call_tool is titled as the tool it ran"), Tool.IsValid() && Tool->Title == TEXT("editor_toolset.toolsets.scene.SceneTools.find_actors")
		&& Tool->Status == TEXT("completed"));
	TestTrue(TEXT("and its JSON answer reads as a report"), Tool.IsValid() && SHyperAIStudioNativeChatView::StepMarkdown(*Tool).StartsWith(TEXT("Found 1 actor named PlayerStart.")));
	const TSharedPtr<FHyperAIStudioChatItem> Picture = ById(TEXT("toolu_mcp#image0"));
	TestTrue(TEXT("A picture in a tool's answer is an image item with its file"), Picture.IsValid() && Picture->Kind == EKind::Image
		&& Picture->Images.Num() == 1 && IFileManager::Get().FileExists(*Picture->Images[0]));
	const TSharedPtr<FHyperAIStudioChatItem> Change = ById(TEXT("toolu_edit"));
	TestTrue(TEXT("An edit is a file change with its diff"), Change.IsValid() && Change->Kind == EKind::FileChange && Change->Title == TEXT("Edit Notes.md")
		&& Change->Text.Contains(TEXT("-old line")) && Change->Text.Contains(TEXT("+new line")) && Change->Status == TEXT("completed"));
	const TSharedPtr<FHyperAIStudioChatItem> Whole = FindKind(*Chat, EKind::Agent, 1);
	TestTrue(TEXT("A message that did not stream still shows, once"), Whole.IsValid() && Whole->Text == TEXT("Done: the start is fine.")
		&& !FindKind(*Chat, EKind::Agent, 2).IsValid());
	TestFalse(TEXT("A subagent's own steps stay out of the transcript"), Chat->GetItems().ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
	{
		return Item->Text.Contains(TEXT("SUBAGENT"));
	}));
	TestTrue(TEXT("An MCP server that did not connect is named, and only that one"), Chat->GetItems().ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
	{
		return Item->Kind == EKind::Notice && Item->Text.Contains(TEXT("nwiro (failed)")) && !Item->Text.Contains(TEXT("unreal-mcp"));
	}));
	AddInfo(TEXT("Usage: ") + Chat->GetUsage());
	TestTrue(TEXT("Usage reads as the size of the context"), Chat->GetUsage().StartsWith(TEXT("21.0k tokens")) && Chat->GetUsage().Contains(TEXT("of context used")));

	Chat->Send(TEXT("plan the arena"));
	TestTrue(TEXT("The second turn completes"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));
	TestTrue(TEXT("Claude's task list becomes the plan, under the id its result gave"), Chat->GetPlan().Num() == 1
		&& Chat->GetPlan()[0].Text == TEXT("Build the arena") && Chat->GetPlan()[0].Status == TEXT("inProgress"));
	const TSharedPtr<FHyperAIStudioChatItem> Local = FindKind(*Chat, EKind::Agent, 2);
	TestTrue(TEXT("A turn that answers in its result alone still shows the answer"), Local.IsValid() && Local->Text == TEXT("Started on the arena."));

	Chat->Send(TEXT("write it down"));
	TestTrue(TEXT("Claude asks to write a file"), PumpUntil([&Chat]() { return Chat->HasOpenApproval(); }));
	const TSharedPtr<FHyperAIStudioChatItem> Late = FindKind(*Chat, EKind::Approval, 1);
	TestTrue(TEXT("Without a title from Claude it names the file, shows what goes in, and offers no don't-ask-again without a suggestion"),
		Late.IsValid() && Late->Title == TEXT("Write late.txt?") && Late->Text.Contains(TEXT("+late")) && Late->Choices.Num() == 2);
	Chat->Interrupt();
	TestTrue(TEXT("Stop ends the turn"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));
	TestTrue(TEXT("and the question Claude withdrew is closed"), !Chat->HasOpenApproval() && Late.IsValid() && Late->bAnswered && Late->Status == TEXT("Cancelled"));
	TestTrue(TEXT("It says it stopped"), Chat->GetItems().Last()->Kind == EKind::Notice && Chat->GetItems().Last()->Text == TEXT("Stopped."));
	TestFalse(TEXT("and nothing failed"), Chat->GetItems().ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item) { return Item->Kind == EKind::Error; }));
	if (Picture.IsValid() && !Picture->Images.IsEmpty())
	{
		IFileManager::Get().Delete(*Picture->Images[0]);
	}
	Chat->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensClaudeChatTest,
	"HyperAIStudio.Screens.ClaudeChat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioScreensClaudeChatTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	using EKind = FHyperAIStudioChatItem::EKind;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the scripted Claude Code stand-in cannot run."));
		return true;
	}
	const FScratch Scratch;
	HyperAIStudio::Tests::WriteScenePng(FPaths::Combine(Scratch.Dir, TEXT("scene.png")), FIntPoint(480, 300),
		FLinearColor(0.45f, 0.6f, 0.9f), FLinearColor(0.2f, 0.3f, 0.2f));
	UseStandIn(Scratch.Dir, FakeClaudeShowcase, TEXT("fake_claude_showcase.py"));

	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Claude Code"), Scratch.Dir, FString(), FString());
	FString Error;
	if (!TestTrue(TEXT("The Claude Code chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("It becomes ready"), PumpUntil([&Chat]() { return Chat->IsReady(); }));
	const TSharedRef<SHyperAIStudioNativeChatView> View = SNew(SHyperAIStudioNativeChatView).Chat(Chat);
	Chat->Send(TEXT("Check the arena: player start, shadows and nav mesh. Fix what fails."));
	TestTrue(TEXT("Claude asks before it runs a command"), PumpUntil([&Chat]() { return Chat->HasOpenApproval(); }));
	const TSharedPtr<FHyperAIStudioChatItem> Approval = FindKind(*Chat, EKind::Approval);
	TestTrue(TEXT("A command's question is titled as one"), Approval.IsValid() && Approval->Title == TEXT("Run this command?")
		&& Approval->Text.StartsWith(TEXT("Show changed files")));
	HyperAIStudio::Tests::SaveWidgetShot(View, FIntPoint(900, 1000), TEXT("step18-claude-approval"), 10);
	if (Approval.IsValid())
	{
		Chat->Answer(Approval->Id, 0);
	}
	TestTrue(TEXT("The turn completes"), PumpUntil([&Chat]() { return !Chat->IsBusy(); }));
	TestTrue(TEXT("The plan, the tool, the command, the edit and the report are all there"), Chat->GetPlan().Num() == 4
		&& FindKind(*Chat, EKind::Tool).IsValid() && FindKind(*Chat, EKind::Command).IsValid() && FindKind(*Chat, EKind::FileChange).IsValid()
		&& FindKind(*Chat, EKind::Agent, 1).IsValid());
	const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(View, FIntPoint(900, 1400), TEXT("step18-claude-chat"), 10);
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
	FHyperAIStudioNativeChatAcpLiveTest,
	"HyperAIStudio.NativeChat.AcpLiveHandshake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeChatAcpLiveTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	// Installed ACP agents, started the way the chat tab starts them, as far as a session: nothing is sent, so no model usage.
	TArray<FString> Agents;
	FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAI_TEST_ACP_CLI")).ParseIntoArray(Agents, TEXT(","));
	if (Agents.IsEmpty())
	{
		AddInfo(TEXT("Skipped: set HYPERAI_TEST_ACP_CLI to installed agents, such as OpenCode,Kimi."));
		return true;
	}
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioAgentModelRoute> Routes = Settings->AgentModelRoutes;
	ON_SCOPE_EXIT
	{
		Settings->AgentModelRoutes = Routes;
		Settings->SaveConfig();
	};
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/AcpHandshakeTest"));
	IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);
	for (const FString& Agent : Agents)
	{
		const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(Agent.TrimStartAndEnd(), Directory, FString(), FString());
		FString Error;
		if (!TestTrue(Agent + TEXT(" starts"), Chat.IsValid() && Chat->Start(Error)))
		{
			AddError(Error);
			continue;
		}
		PumpUntil([&Chat]() { return Chat->IsReady() || !Chat->IsRunning(); }, 90.0);
		for (const TSharedPtr<FHyperAIStudioChatItem>& Item : Chat->GetItems())
		{
			AddInfo(Agent + TEXT(": ") + Item->Text);
		}
		const FHyperAIStudioAgentModelRoute* Route = Settings->FindAgentModelRoute(Chat->GetAgentName());
		AddInfo(FString::Printf(TEXT("%s offers %d models: %s"), *Agent, Route ? Route->Models.Num() : 0,
			Route ? *FString::Join(TArray<FString>(Route->Models.GetData(), FMath::Min(Route->Models.Num(), 12)), TEXT(", ")) : TEXT("none")));
		TestTrue(Agent + TEXT(" answers the handshake and opens a session"), Chat->IsReady() && !Chat->GetSessionId().IsEmpty());
		Chat->Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNativeChatClaudeLiveTest,
	"HyperAIStudio.NativeChat.ClaudeLiveHandshake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNativeChatClaudeLiveTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::NativeChatTests;
	// The real CLI, started the way the chat tab starts it, as far as the handshake: nothing is sent, so no model usage.
	if (FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAI_TEST_CLAUDE_CLI")) != TEXT("1"))
	{
		AddInfo(TEXT("Skipped: set HYPERAI_TEST_CLAUDE_CLI=1 to run against an installed, logged-in Claude Code."));
		return true;
	}
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/ClaudeHandshakeTest"));
	IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);
	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("Claude Code"), Directory, FString(), FString());
	FString Error;
	if (!TestTrue(TEXT("claude starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	PumpUntil([&Chat]() { return Chat->IsReady() || !Chat->IsRunning(); }, 90.0);
	for (const TSharedPtr<FHyperAIStudioChatItem>& Item : Chat->GetItems())
	{
		AddInfo(TEXT("Transcript: ") + Item->Text);
	}
	TestTrue(TEXT("It takes the command line and answers the handshake"), Chat->IsReady());
	TestTrue(TEXT("and keeps running, waiting for a message"), Chat->IsRunning());
	Chat->Stop();
	TestFalse(TEXT("A chat that sent nothing leaves no session behind"), HyperAIStudio::ChatHistory::ClaudeSessionExists(Directory, Chat->GetSessionId()));
	return true;
}

#endif
