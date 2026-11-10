// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentHost.h"
#include "HyperAIStudioEditorRestart.h"
#include "HyperAIStudioNativeChat.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace HyperAIStudio::AgentHostTests
{
	/** An agent that says hello, then answers each line slowly: three lines a third of a second apart. */
	const TCHAR* const SlowEcho = TEXT(R"PY(import sys, time
print("hello", flush=True)
for line in iter(sys.stdin.readline, ""):
    for k in range(3):
        print("got %s %d" % (line.strip(), k), flush=True)
        time.sleep(0.35)
)PY");

	/** An agent that says hello and then nothing at all, to be stopped as idle. */
	const TCHAR* const Silent = TEXT(R"PY(import sys, time
print("hello", flush=True)
time.sleep(120)
)PY");

	/**
	 * An agent over the Agent Client Protocol whose turns take a while: "hi" streams five chunks 0.4 s apart, "ask" asks
	 * permission first and says what it was given, "again" answers at once.
	 */
	const TCHAR* const SlowAcp = TEXT(R"PY(import json, sys, time
def send(o):
    o["jsonrpc"] = "2.0"; sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
def say(t):
    send({"method": "session/update", "params": {"sessionId": "s1", "update": {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": t}}}})
asking = None
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id"); p = m.get("params") or {}
    if method == "initialize":
        send({"id": i, "result": {"protocolVersion": 1, "agentCapabilities": {"loadSession": False, "mcpCapabilities": {"http": True}}}})
    elif method == "session/new":
        send({"id": i, "result": {"sessionId": "s1"}})
    elif method == "session/prompt":
        text = p["prompt"][0]["text"].split("\n")[0]
        if text == "hi":
            for k in range(5):
                say("part%d " % k); time.sleep(0.4)
            send({"id": i, "result": {"stopReason": "end_turn"}})
        elif text == "ask":
            time.sleep(0.8)
            asking = i
            send({"id": 77, "method": "session/request_permission", "params": {"sessionId": "s1", "toolCall": {"toolCallId": "t1", "title": "Write notes.md"},
                  "options": [{"optionId": "yes", "name": "Allow", "kind": "allow_once"}, {"optionId": "no", "name": "Reject", "kind": "reject_once"}]}})
        else:
            say("again done"); send({"id": i, "result": {"stopReason": "end_turn"}})
    elif i == 77 and "result" in m:
        say("allowed=" + m["result"]["outcome"].get("optionId", "?"))
        send({"id": asking, "result": {"stopReason": "end_turn"}})
)PY");

	/** Claude Code's stream-json, far enough for a slow reply: four text pieces 0.4 s apart, then the result. */
	const TCHAR* const SlowClaude = TEXT(R"PY(import json, sys, time
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); t = m.get("type")
    if t == "control_request" and m["request"]["subtype"] == "initialize":
        send({"type": "control_response", "response": {"subtype": "success", "request_id": m["request_id"], "response": {}}})
    elif t == "user":
        sid = m.get("session_id")
        send({"type": "stream_event", "event": {"type": "message_start", "message": {"id": "m1"}}, "parent_tool_use_id": None, "session_id": sid})
        for k in range(4):
            send({"type": "stream_event", "event": {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "piece%d " % k}},
                  "parent_tool_use_id": None, "session_id": sid})
            time.sleep(0.4)
        send({"type": "result", "subtype": "success", "is_error": False, "result": "done", "session_id": sid})
)PY");

	struct FScratch
	{
		FString Dir = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()), TEXT("AgentHost-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FString Root = Dir / TEXT("agent-host");

		FScratch()
		{
			IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		}
		~FScratch()
		{
			// Whatever a failed check left running stops with the host, which then exits.
			FFileHelper::SaveStringToFile(TEXT("stop"), *(Root / TEXT("stop")));
			for (int32 Try = 0; Try < 100 && FHyperAIStudioAgentHost::IsHostRunning(); ++Try)
			{
				FPlatformProcess::Sleep(0.1f);
			}
			FHyperAIStudioAgentHost::SetRootForTests(FString());
			FHyperAIStudioNativeChat::SetCommandForTests(FString());
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/true);
		}

		FString Write(const TCHAR* Name, const TCHAR* Text) const
		{
			const FString Path = Dir / Name;
			FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			return Path;
		}
	};

	bool HasPython()
	{
		int32 Code = -1;
		FString Out;
		FString Err;
		FPlatformProcess::ExecProcess(TEXT("python"), TEXT("--version"), &Code, &Out, &Err);
		return Code == 0;
	}

	bool WaitUntil(const TFunction<bool()>& Done, const double Seconds = 15.0)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (!Done() && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.05f);
			FPlatformProcess::Sleep(0.03f);
		}
		return Done();
	}

	/** The agent's own lines (o) of a session's whole stream, read from its start as a returning editor reads it. */
	TArray<FString> AgentLines(const FString& Id)
	{
		FHyperAIStudioHostStreamReader Reader(FHyperAIStudioAgentHost::StreamPath(Id));
		TArray<FString> Lines;
		Reader.Read(Lines);
		TArray<FString> Out;
		for (const FString& Raw : Lines)
		{
			FHyperAIStudioHostLine Line;
			if (FHyperAIStudioAgentHost::ParseLine(Raw, Line) && Line.Kind == TEXT('o'))
			{
				Out.Add(Line.Body);
			}
		}
		return Out;
	}

	FString AgentText(const FHyperAIStudioNativeChat& Chat)
	{
		FString Text;
		for (const TSharedPtr<FHyperAIStudioChatItem>& Item : Chat.GetItems())
		{
			if (Item->Kind == FHyperAIStudioChatItem::EKind::Agent)
			{
				Text += Item->Text;
			}
		}
		return Text;
	}

	int32 Count(const FHyperAIStudioNativeChat& Chat, const FHyperAIStudioChatItem::EKind Kind)
	{
		int32 Found = 0;
		for (const TSharedPtr<FHyperAIStudioChatItem>& Item : Chat.GetItems())
		{
			Found += Item->Kind == Kind ? 1 : 0;
		}
		return Found;
	}

	/** Says the editor is gone: no editor answers for the host's agents any more. */
	void ForgetEditor(const FString& Root)
	{
		FFileHelper::SaveStringToFile(TEXT("{\"pid\": 0}"), *(Root / TEXT("editor.json")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentHostLifecycleTest,
	"HyperAIStudio.AgentHost.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentHostLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AgentHostTests;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the stand-in agents cannot run."));
		return true;
	}
	const FScratch Scratch;
	// A short linger and a 6 s idle limit, so the host's own ends happen within the test.
	FHyperAIStudioAgentHost::SetRootForTests(Scratch.Root, /*LingerSeconds=*/3, /*IdleMinutes=*/0.1);
	FString Error;
	if (!TestTrue(TEXT("The host starts and answers"), FHyperAIStudioAgentHost::EnsureRunning(Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("A second ask finds the same host running"), FHyperAIStudioAgentHost::EnsureRunning(Error, 0.0) && FHyperAIStudioAgentHost::IsHostRunning());

	// An agent that prints slowly: its first word, then an answer streamed over a second.
	const FString Echo = Scratch.Write(TEXT("slow_echo.py"), SlowEcho);
	const FString Id = FHyperAIStudioAgentHost::NewSessionId();
	TestTrue(TEXT("The host is asked to start the agent"), FHyperAIStudioAgentHost::StartSession(Id, FString::Printf(TEXT("python -u \"%s\""), *Echo), Scratch.Dir,
		{}, {{TEXT("HYPERAI_HOST_TEST"), TEXT("1")}}, Error));
	FHyperAIStudioHostStreamReader Live(FHyperAIStudioAgentHost::StreamPath(Id));
	TArray<FString> Seen;
	auto SeenLine = [&Seen](const TCHAR* Text) { return Seen.ContainsByPredicate([Text](const FString& Line) { return Line.Contains(Text); }); };
	TestTrue(TEXT("Its output reaches the stream"), WaitUntil([&]() { Live.Read(Seen); return SeenLine(TEXT("o\thello")); }));
	FHyperAIStudioHostSessionState State;
	TestTrue(TEXT("The host says it runs"), FHyperAIStudioAgentHost::ReadState(Id, State) && State.IsRunning() && State.ProcessId > 0);
	TestEqual(TEXT("One agent runs"), FHyperAIStudioAgentHost::CountRunning(), 1);

	// The editor sends a line and goes away before the answer is done: the agent keeps going all the same.
	TestTrue(TEXT("A line goes to the agent"), FHyperAIStudioAgentHost::Append(Id, TEXT('a'), TEXT("{\"n\":1}")));
	TestTrue(TEXT("and its answer starts"), WaitUntil([&]() { Live.Read(Seen); return SeenLine(TEXT("got {\"n\":1} 0")); }));
	ForgetEditor(Scratch.Root);
	FPlatformProcess::Sleep(1.5f);
	// A returning editor reads the stream from its start: everything, including what came while it was away.
	const TArray<FString> Replayed = AgentLines(Id);
	AddInfo(FString::Printf(TEXT("Replayed %d agent line(s): %s"), Replayed.Num(), *FString::Join(Replayed, TEXT(" | "))));
	TestTrue(TEXT("The answer finished while no editor read it, and replays whole on reattach"), Replayed.Contains(TEXT("hello"))
		&& Replayed.Contains(TEXT("got {\"n\":1} 1")) && Replayed.Contains(TEXT("got {\"n\":1} 2")));
	TArray<FString> All;
	FHyperAIStudioHostStreamReader(FHyperAIStudioAgentHost::StreamPath(Id)).Read(All);
	TestTrue(TEXT("The stream keeps what was sent, in order with what came back"), All.IndexOfByKey(TEXT("i\t{\"n\":1}")) != INDEX_NONE
		&& All.IndexOfByKey(TEXT("i\t{\"n\":1}")) < All.IndexOfByKey(TEXT("o\tgot {\"n\":1} 0")));

	// The editor is back (its note says so again), and stops the agent: the host ends it and says why.
	FHyperAIStudioAgentHost::EnsureRunning(Error, 0.0);
	FHyperAIStudioAgentHost::StopSession(Id, TEXT("test"), /*bForget=*/false);
	TestTrue(TEXT("A stopped agent ends, and the stream says why"), WaitUntil([&]() { Live.Read(Seen); return SeenLine(TEXT("\"event\":\"stopped\"")); }));
	TestTrue(TEXT("with its state stopped"), FHyperAIStudioAgentHost::ReadState(Id, State) && State.State == TEXT("stopped") && State.Reason == TEXT("test"));

	// An agent that does nothing while no editor is there is stopped as idle.
	const FString Quiet = Scratch.Write(TEXT("silent.py"), Silent);
	const FString IdleId = FHyperAIStudioAgentHost::NewSessionId();
	FHyperAIStudioAgentHost::StartSession(IdleId, FString::Printf(TEXT("python -u \"%s\""), *Quiet), Scratch.Dir, {}, {}, Error);
	ForgetEditor(Scratch.Root);
	TestTrue(TEXT("An agent idle with no editor attached is stopped after the idle limit"), WaitUntil([&]()
	{
		FHyperAIStudioHostSessionState Idle;
		return FHyperAIStudioAgentHost::ReadState(IdleId, Idle) && Idle.State == TEXT("stopped") && Idle.Reason.Contains(TEXT("idle"));
	}, 20.0));

	// Nothing runs any more: the host exits by itself once it lingered.
	TestTrue(TEXT("With no agent left, the host exits"), WaitUntil([]() { return !FHyperAIStudioAgentHost::IsHostRunning(); }, 15.0)
		&& !FPaths::FileExists(Scratch.Root / TEXT("host.json")));

	// The stop file ends everything without an editor.
	TestTrue(TEXT("A host starts again for a new agent"), FHyperAIStudioAgentHost::EnsureRunning(Error));
	const FString LastId = FHyperAIStudioAgentHost::NewSessionId();
	FHyperAIStudioAgentHost::StartSession(LastId, FString::Printf(TEXT("python -u \"%s\""), *Quiet), Scratch.Dir, {}, {}, Error);
	TestTrue(TEXT("and runs it"), WaitUntil([&]() { FHyperAIStudioHostSessionState Running; return FHyperAIStudioAgentHost::ReadState(LastId, Running) && Running.IsRunning(); }));
	TestEqual(TEXT("Stop Background Agents counts the agent it stops"), FHyperAIStudioAgentHost::StopAll(TEXT("test")), 1);
	TestTrue(TEXT("and the agent stops"), WaitUntil([&]() { FHyperAIStudioHostSessionState Done; return FHyperAIStudioAgentHost::ReadState(LastId, Done) && Done.State == TEXT("stopped"); }));
	TestTrue(TEXT("then the host exits"), WaitUntil([]() { return !FHyperAIStudioAgentHost::IsHostRunning(); }, 15.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentHostReattachTest,
	"HyperAIStudio.AgentHost.ChatReattach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentHostReattachTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AgentHostTests;
	using EKind = FHyperAIStudioChatItem::EKind;
	if (!HasPython())
	{
		AddWarning(TEXT("python is not on PATH, so the stand-in agents cannot run."));
		return true;
	}
	const FScratch Scratch;
	FHyperAIStudioAgentHost::SetRootForTests(Scratch.Root, /*LingerSeconds=*/3);
	TestTrue(TEXT("Chats run under the host when it is on"), FHyperAIStudioAgentHost::ShouldHost());
	FHyperAIStudioNativeChat::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\""), *Scratch.Write(TEXT("slow_acp.py"), SlowAcp)));

	// A turn that streams for two seconds; the editor lets go of the chat a moment in.
	TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(TEXT("OpenCode"), Scratch.Dir, FString(), FString());
	FString Error;
	if (!TestTrue(TEXT("The chat starts"), Chat.IsValid() && Chat->Start(Error)))
	{
		AddError(Error);
		return false;
	}
	const FString Session = Chat->GetHostSession();
	TestFalse(TEXT("in the background host"), Session.IsEmpty());
	TestTrue(TEXT("and becomes ready"), WaitUntil([&Chat]() { return Chat->IsReady(); }));
	Chat->SetHostNote(3, TEXT("Notes"), FString());
	Chat->Send(TEXT("hi"));
	TestTrue(TEXT("Its reply starts"), WaitUntil([&Chat]() { return AgentText(*Chat).Contains(TEXT("part0")); }));
	TestTrue(TEXT("while the turn runs"), Chat->IsBusy());
	Chat->Detach();
	Chat.Reset();
	FHyperAIStudioHostSessionMeta Meta;
	TestTrue(TEXT("Let go mid-turn, the chat's note says so"), FHyperAIStudioAgentHost::ReadMeta(Session, Meta) && Meta.bBusyAtDetach && Meta.EditorProcessId == 0
		&& Meta.TabIndex == 3 && Meta.SessionId == TEXT("s1"));
	TestEqual(TEXT("A later editor finds it to take up"), FHyperAIStudioAgentHost::FindDetachedSessions(Meta.ProjectFile, {}).Num(), 1);
	const FHyperAIStudioRestartChat Restored = FHyperAIStudioEditorRestart::ChatFromHostSession(Meta);
	TestTrue(TEXT("as a chat for its tab that reattaches and is told the editor is back"), Restored.TabIndex == 3 && Restored.HostSession == Session
		&& Restored.bWorking && Restored.Message.Contains(TEXT("background host")));
	FPlatformProcess::Sleep(2.5f);

	// Another chat object, as a new editor makes: it replays the turn that finished meanwhile and goes on live.
	Chat = FHyperAIStudioNativeChat::Create(TEXT("OpenCode"), Scratch.Dir, FString(), FString());
	if (!TestTrue(TEXT("The chat reattaches to its session"), Chat->Attach(Session, Error)))
	{
		AddError(Error);
		return false;
	}
	AddInfo(TEXT("After reattaching: ") + AgentText(*Chat));
	TestTrue(TEXT("The whole reply is there, the parts that came while no editor watched included"), AgentText(*Chat).Contains(TEXT("part0 part1 part2 part3 part4")));
	TestEqual(TEXT("The message sent before is in the transcript once"), Count(*Chat, EKind::User), 1);
	TestTrue(TEXT("The turn that ended while away has ended here too, and the chat is ready"), !Chat->IsBusy() && Chat->IsReady() && Chat->GetSessionId() == TEXT("s1"));
	Chat->Send(TEXT("again"));
	TestTrue(TEXT("A new message goes to the same agent, live"), WaitUntil([&Chat]() { return !Chat->IsBusy() && AgentText(*Chat).Contains(TEXT("again done")); }));

	// An approval the agent asked for while no editor was there waits, and is answered after the reattach.
	Chat->Send(TEXT("ask"));
	TestTrue(TEXT("Another turn starts"), WaitUntil([&Chat]() { return Chat->IsBusy(); }));
	Chat->Detach();
	Chat.Reset();
	FPlatformProcess::Sleep(1.5f);
	Chat = FHyperAIStudioNativeChat::Create(TEXT("OpenCode"), Scratch.Dir, FString(), FString());
	TestTrue(TEXT("It reattaches again"), Chat->Attach(Session, Error));
	TestTrue(TEXT("with the approval asked while away open"), Chat->HasOpenApproval() && Chat->IsBusy());
	TSharedPtr<FHyperAIStudioChatItem> Approval;
	for (const TSharedPtr<FHyperAIStudioChatItem>& Item : Chat->GetItems())
	{
		Approval = Item->Kind == EKind::Approval && !Item->bAnswered ? Item : Approval;
	}
	if (TestTrue(TEXT("and shown"), Approval.IsValid()))
	{
		Chat->Answer(Approval->Id, 0);
	}
	TestTrue(TEXT("Answered now, it reaches the agent, which finishes"), WaitUntil([&Chat]() { return !Chat->IsBusy() && AgentText(*Chat).Contains(TEXT("allowed=yes")); }));
	TestEqual(TEXT("Each message shows once after two reattaches"), Count(*Chat, EKind::User), 3);

	// Closing the chat stops its agent and drops its files.
	Chat->Stop();
	TestTrue(TEXT("A closed chat's agent stops and its session goes"), WaitUntil([&Session]() { return !FPaths::DirectoryExists(FHyperAIStudioAgentHost::GetRoot() / TEXT("sessions") / Session); }));

	// Claude Code too: its turn ends with a result line, and its readiness comes back from the stream.
	FHyperAIStudioNativeChat::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\""), *Scratch.Write(TEXT("slow_claude.py"), SlowClaude)));
	Chat = FHyperAIStudioNativeChat::Create(TEXT("Claude Code"), Scratch.Dir, FString(), FString());
	TestTrue(TEXT("A Claude Code chat starts in the host"), Chat->Start(Error) && !Chat->GetHostSession().IsEmpty() && WaitUntil([&Chat]() { return Chat->IsReady(); }));
	const FString ClaudeSession = Chat->GetHostSession();
	const FString ClaudeId = Chat->GetSessionId();
	Chat->Send(TEXT("hello"));
	TestTrue(TEXT("Its reply starts"), WaitUntil([&Chat]() { return AgentText(*Chat).Contains(TEXT("piece0")); }));
	Chat->Detach();
	Chat.Reset();
	FPlatformProcess::Sleep(2.0f);
	Chat = FHyperAIStudioNativeChat::Create(TEXT("Claude Code"), Scratch.Dir, FString(), FString());
	TestTrue(TEXT("It reattaches"), Chat->Attach(ClaudeSession, Error));
	TestTrue(TEXT("with the reply finished while away, the turn over and the same conversation"), AgentText(*Chat).Contains(TEXT("piece3"))
		&& !Chat->IsBusy() && Chat->IsReady() && Chat->GetSessionId() == ClaudeId);
	Chat->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentHostRulesTest,
	"HyperAIStudio.AgentHost.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentHostRulesTest::RunTest(const FString& Parameters)
{
	FHyperAIStudioHostLine Line;
	TestTrue(TEXT("A stream line is its kind, a tab and its body"), FHyperAIStudioAgentHost::ParseLine(TEXT("o\t{\"a\":1}\tz"), Line) && Line.Kind == TEXT('o')
		&& Line.Body == TEXT("{\"a\":1}\tz"));
	TestFalse(TEXT("Anything else is not one"), FHyperAIStudioAgentHost::ParseLine(TEXT("q\tx"), Line) || FHyperAIStudioAgentHost::ParseLine(TEXT("o x"), Line));

	const FString Project = TEXT("E:/Unreal Projects/Game/Game.uproject");
	FHyperAIStudioHostSessionMeta Meta;
	Meta.Id = TEXT("s");
	Meta.Agent = TEXT("Codex");
	Meta.ProjectFile = Project;
	Meta.EditorProcessId = 4242;
	FHyperAIStudioHostSessionState Running;
	Running.State = TEXT("running");
	FHyperAIStudioHostSessionState Ended;
	Ended.State = TEXT("exited");
	TestTrue(TEXT("A running agent whose editor is gone is taken up"), FHyperAIStudioAgentHost::IsDetached(Meta, Running, true, Project, /*bOwnerRunning=*/false));
	TestFalse(TEXT("one another running editor shows is not"), FHyperAIStudioAgentHost::IsDetached(Meta, Running, true, Project, /*bOwnerRunning=*/true));
	TestFalse(TEXT("nor another project's"), FHyperAIStudioAgentHost::IsDetached(Meta, Running, true, TEXT("E:/Other/Other.uproject"), false));
	TestTrue(TEXT("An agent that ended while its editor was away is, to show how it ended"), FHyperAIStudioAgentHost::IsDetached(Meta, Ended, true, Project, false));
	Meta.bClosed = true;
	TestFalse(TEXT("A chat closed in the editor never comes back"), FHyperAIStudioAgentHost::IsDetached(Meta, Running, true, Project, false));
	Meta.bClosed = false;
	Meta.EditorProcessId = 0;
	TestFalse(TEXT("nor one that ended with no editor ever let go of it"), FHyperAIStudioAgentHost::IsDetached(Meta, Ended, true, Project, false));
	TestFalse(TEXT("nor one the host never started"), FHyperAIStudioAgentHost::IsDetached(Meta, Running, false, Project, false));

	Meta.TabIndex = 0;
	Meta.bBusyAtDetach = false;
	Meta.ResumeSessionId = TEXT("thread-7");
	const FHyperAIStudioRestartChat Idle = FHyperAIStudioEditorRestart::ChatFromHostSession(Meta);
	TestTrue(TEXT("An idle one reattaches in a tab, on its conversation, told nothing"), Idle.TabIndex == 1 && Idle.SessionId == TEXT("thread-7") && Idle.Message.IsEmpty()
		&& !Idle.bWorking && Idle.HostSession == TEXT("s"));
	return true;
}

#endif
