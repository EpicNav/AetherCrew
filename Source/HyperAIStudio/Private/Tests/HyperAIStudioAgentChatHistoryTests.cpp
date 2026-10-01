// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "HyperAIStudioChatHistoryPrefs.h"
#include "HyperAIStudioWidgetShot.h"
#include "SHyperAIStudioChatHistorySidebar.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioService.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Internationalization/Regex.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentChatHistoryTest,
	"HyperAIStudio.Chat.AgentChatHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentChatHistoryTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChatHistory;
	const FString ClaudeId = TEXT("7350db99-a8d7-4f9d-9c85-9b141c4163a6");
	const FString CodexId = TEXT("01a0975b-2c7e-7e72-a9ce-13cfc8c4a95d");

	TestEqual(TEXT("Claude resume flag"), BuildResumeArguments(TEXT("Claude Code"), ClaudeId), TEXT("--resume ") + ClaudeId);
	TestEqual(TEXT("Codex resume subcommand"), BuildResumeArguments(TEXT("Codex"), CodexId), TEXT("resume ") + CodexId);
	TestTrue(TEXT("Gemini is not resumable"), BuildResumeArguments(TEXT("Gemini"), ClaudeId).IsEmpty());
	TestFalse(TEXT("Shell metacharacters are refused"), IsSafeSessionId(TEXT("abcdef12 & del x")));
	TestFalse(TEXT("Short ids are refused"), IsSafeSessionId(TEXT("abc")));
	TestEqual(TEXT("Claude project folder name"), ClaudeProjectDirectoryName(TEXT("E:/Unreal Projects/Aetherfall/")), FString(TEXT("E--Unreal-Projects-Aetherfall")));

	const FString ClaudeLines = FString::Join(TArray<FString>{
		TEXT("{\"type\":\"user\",\"isMeta\":true,\"message\":{\"role\":\"user\",\"content\":\"Caveat: injected\"}}"),
		TEXT("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"<command-name>/model</command-name>\"}}"),
		TEXT("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"Fix the knockdown get-up\\nand camera shake\"}]}}"),
		TEXT("{\"type\":\"ai-title\",\"aiTitle\":\"Combat polish\"}"),
		TEXT("{\"type\":\"custom-ti")}, TEXT("\n"));
	TestEqual(TEXT("AI title beats the first prompt; a cut line is ignored"), ExtractClaudeTitle(ClaudeLines), FString(TEXT("Combat polish")));
	TestEqual(TEXT("Custom title beats the AI title"),
		ExtractClaudeTitle(ClaudeLines + TEXT("\n{\"type\":\"custom-title\",\"customTitle\":\"Knockdown pass\"}")), FString(TEXT("Knockdown pass")));
	TestEqual(TEXT("First typed prompt, first line only"),
		ExtractClaudeTitle(ClaudeLines.Left(ClaudeLines.Find(TEXT("{\"type\":\"ai-title\"")))), FString(TEXT("Fix the knockdown get-up")));

	const FString CodexLines = FString::Join(TArray<FString>{
		TEXT("{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"# AGENTS.md instructions\"}]}}"),
		TEXT("{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"/fast off\"}]}}"),
		TEXT("{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"Build the title screen\"}]}}")}, TEXT("\n"));
	TestEqual(TEXT("Codex skips injected and slash turns"), ExtractCodexFirstPrompt(CodexLines), FString(TEXT("Build the title screen")));

	// Fake agent stores, pointed at through the agents' own override variables.
	const FString Scratch = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudioTests/ChatHistory"));
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	const FString Root = TEXT("E:/Some Project");
	const FString OtherRoot = TEXT("E:/Other Project");
	FFileHelper::SaveStringToFile(ClaudeLines, *(Scratch / TEXT("claude/projects") / ClaudeProjectDirectoryName(Root) / ClaudeId + TEXT(".jsonl")));
	FFileHelper::SaveStringToFile(TEXT("{\"type\":\"mode\"}"), *(Scratch / TEXT("claude/projects") / ClaudeProjectDirectoryName(Root) / TEXT("aaaaaaaa-0000-0000-0000-000000000000.jsonl")));
	auto CodexMeta = [](const FString& Id, const FString& Cwd)
	{
		return FString::Printf(TEXT("{\"type\":\"session_meta\",\"payload\":{\"id\":\"%s\",\"cwd\":\"%s\"}}\n"), *Id, *Cwd.Replace(TEXT("/"), TEXT("\\\\")));
	};
	FFileHelper::SaveStringToFile(CodexMeta(CodexId, Root) + CodexLines, *(Scratch / TEXT("codex/sessions/2026/09/12/rollout-a.jsonl")));
	FFileHelper::SaveStringToFile(CodexMeta(TEXT("bbbbbbbb-0000-0000-0000-000000000000"), OtherRoot) + CodexLines, *(Scratch / TEXT("codex/sessions/2026/09/12/rollout-b.jsonl")));
	FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"id\":\"%s\",\"thread_name\":\"Title screen UMG\"}\n"), *CodexId), *(Scratch / TEXT("codex/session_index.jsonl")));

	const FString PreviousClaude = FPlatformMisc::GetEnvironmentVariable(TEXT("CLAUDE_CONFIG_DIR"));
	const FString PreviousCodex = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_HOME"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CLAUDE_CONFIG_DIR"), *(Scratch / TEXT("claude")));
	FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_HOME"), *(Scratch / TEXT("codex")));
	const TArray<FHyperAIStudioChatSession> Sessions = ListSessions(Root + TEXT("/"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CLAUDE_CONFIG_DIR"), *PreviousClaude);
	FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_HOME"), *PreviousCodex);
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);

	TestEqual(TEXT("One chat per agent in this project; empty and foreign sessions are skipped"), Sessions.Num(), 2);
	const FHyperAIStudioChatSession* Claude = Sessions.FindByPredicate([](const FHyperAIStudioChatSession& S) { return S.AgentName == TEXT("Claude Code"); });
	const FHyperAIStudioChatSession* Codex = Sessions.FindByPredicate([](const FHyperAIStudioChatSession& S) { return S.AgentName == TEXT("Codex"); });
	TestTrue(TEXT("Claude chat listed with its title"), Claude && Claude->SessionId == ClaudeId && Claude->Title == TEXT("Combat polish"));
	TestTrue(TEXT("Codex chat listed with its thread name"), Codex && Codex->SessionId == CodexId && Codex->Title == TEXT("Title screen UMG"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioRemoteChatsTest,
	"HyperAIStudio.Chat.RemoteChats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioRemoteChatsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChatHistory;

	// claude --session-id takes only RFC 4122 ids: version 4, variant 10xx.
	const FString Id = NewClaudeSessionId();
	const FRegexPattern UuidV4(TEXT("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
	FRegexMatcher Matcher(UuidV4, Id);
	TestTrue(FString::Printf(TEXT("Session id %s is a v4 UUID"), *Id), Matcher.FindNext());
	TestNotEqual(TEXT("Session ids are random"), NewClaudeSessionId(), Id);
	TestTrue(TEXT("Session ids pass the command-line check"), IsSafeSessionId(Id));

	// Remote session names go into a quoted cmd.exe argument.
	TestEqual(TEXT("Title names the remote session; cmd metacharacters are dropped"),
		BuildClaudeRemoteArguments(TEXT("Aetherfall"), TEXT("Fix \"quotes\" & 100% ^pipes|")),
		FString(TEXT("--remote-control \"Aetherfall - Fix quotes 100 pipes\"")));
	FString Accented = TEXT("Caf");
	Accented.AppendChar(TCHAR(0x00E9));
	TestEqual(TEXT("Non-ASCII is dropped"), BuildClaudeRemoteArguments(TEXT("Aetherfall"), Accented),
		FString(TEXT("--remote-control \"Aetherfall - Caf\"")));
	TestEqual(TEXT("No title: Claude names it, prefixed with the project"), BuildClaudeRemoteArguments(TEXT("Aetherfall"), FString()),
		FString(TEXT("--remote-control --remote-control-session-name-prefix \"Aetherfall\"")));
	TestEqual(TEXT("Nothing usable: bare flag"), BuildClaudeRemoteArguments(TEXT("%%"), TEXT("&")), FString(TEXT("--remote-control")));

	// claude --bg output, with its real middle-dot separators.
	FString Started = TEXT("Starting background service...\nbackgrounded ");
	Started.AppendChar(TCHAR(0x00B7));
	Started += TEXT(" 583C0fce ");
	Started.AppendChar(TCHAR(0x00B7));
	Started += TEXT(" hyperai-bg (idle - send a prompt to start)\n  claude attach 583c0fce    open in this terminal\n");
	TestEqual(TEXT("Short id after the marker, lower-cased"), ParseBackgroundedId(Started), FString(TEXT("583c0fce")));
	TestTrue(TEXT("No marker, no id"), ParseBackgroundedId(TEXT("Error: Remote Control requires a claude.ai login\n")).IsEmpty());
	TestTrue(TEXT("Only an eight-digit run on the marker's line counts"),
		ParseBackgroundedId(TEXT("backgrounded - 1234567890ab - x\nattach deadbeef\n")).IsEmpty());

	const FString Json = TEXT("[")
		TEXT("{\"pid\":1,\"id\":\"f3c6bc98\",\"cwd\":\"E:\\\\Unreal Projects\\\\Aetherfall\",\"kind\":\"background\",")
		TEXT("\"sessionId\":\"f3c6bc98-3bbb-4979-96ae-e6eb02d43ed8\",\"status\":\"busy\"},")
		TEXT("{\"pid\":2,\"cwd\":\"E:/Unreal Projects/Aetherfall/\",\"kind\":\"interactive\",")
		TEXT("\"sessionId\":\"7350db99-a8d7-4f9d-9c85-9b141c4163a6\",\"status\":\"idle\"},")
		TEXT("{\"pid\":3,\"id\":\"583c0fce\",\"cwd\":\"E:/IdeaProjects/Terra\",\"kind\":\"background\",")
		TEXT("\"sessionId\":\"583c0fce-bd17-4767-9995-ae2e2715caef\",\"status\":\"idle\"},")
		TEXT("{\"pid\":4,\"id\":\"x & y\",\"cwd\":\"E:/Unreal Projects/Aetherfall\",\"sessionId\":\"not safe; rm\"}")
		TEXT("]");
	const TArray<FHyperAIStudioLiveChat> Live = ParseLiveClaudeChats(Json, TEXT("E:/Unreal Projects/Aetherfall"));
	TestEqual(TEXT("Only this project's chats with safe ids"), Live.Num(), 2);
	if (Live.Num() == 2)
	{
		TestTrue(TEXT("Background chat keeps its short id"), Live[0].ShortId == TEXT("f3c6bc98") && Live[0].IsBackground() && Live[0].IsBusy());
		TestTrue(TEXT("Interactive chat gets its id's first eight digits"), Live[1].ShortId == TEXT("7350db99") && !Live[1].IsBackground());
	}
	TestEqual(TEXT("Output that is not JSON lists nothing"), ParseLiveClaudeChats(TEXT("claude: not found"), TEXT("E:/x")).Num(), 0);

	TArray<FHyperAIStudioChatSession> Sessions;
	Sessions.Add({TEXT("Claude Code"), TEXT("f3c6bc98-3bbb-4979-96ae-e6eb02d43ed8"), TEXT("Running one"), FDateTime::UtcNow(), false});
	Sessions.Add({TEXT("Claude Code"), TEXT("0a0a0a0a-3bbb-4979-96ae-e6eb02d43ed8"), TEXT("Stopped one"), FDateTime::UtcNow(), false});
	Sessions.Add({TEXT("Codex"), TEXT("7350db99-a8d7-4f9d-9c85-9b141c4163a6"), TEXT("Same id, other agent"), FDateTime::UtcNow(), false});
	MergeLiveChats(Sessions, Live);
	TestTrue(TEXT("A running chat is marked"), Sessions[0].Live.IsRunning() && Sessions[0].Live.ShortId == TEXT("f3c6bc98"));
	TestFalse(TEXT("A stopped chat is not"), Sessions[1].Live.IsRunning());
	TestFalse(TEXT("Only Claude Code chats are matched"), Sessions[2].Live.IsRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioRemoteChatsLiveTest,
	"HyperAIStudio.Chat.RemoteChatsLive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioRemoteChatsLiveTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChatHistory;
	// Starts a real background Claude Code session (no prompt, so no model usage), finds it, and stops it.
	if (FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAI_TEST_CLAUDE_CLI")) != TEXT("1"))
	{
		AddInfo(TEXT("Skipped: set HYPERAI_TEST_CLAUDE_CLI=1 to run against an installed, logged-in Claude Code."));
		return true;
	}
	const FString Claude = FHyperAIStudioService::GetAgentTerminalLaunchCommand(TEXT("Claude Code"));
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/RemoteChatTest"));
	IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);

	FString Error;
	const FString ShortId = StartClaudeBackgroundChat(Claude, Directory, TEXT("set \"HYPERAI_REMOTE_TEST=1\" && "),
		TEXT("-n hyperai-remote-test"), Error);
	if (!TestFalse(FString::Printf(TEXT("claude --bg started (%s)"), *Error), ShortId.IsEmpty()))
	{
		return false;
	}
	const TArray<FHyperAIStudioLiveChat> Live = ListLiveClaudeChats(Claude, Directory);
	const FHyperAIStudioLiveChat* Started = Live.FindByPredicate([&ShortId](const FHyperAIStudioLiveChat& Chat) { return Chat.ShortId == ShortId; });
	TestTrue(TEXT("The background session outlives the command that started it and is listed"), Started && Started->IsBackground());
	TestTrue(TEXT("Its full id starts with the short id"), Started && Started->SessionId.StartsWith(ShortId));
	TestTrue(TEXT("claude stop succeeds"), StopClaudeBackgroundChat(Claude, ShortId));
	TestFalse(TEXT("Stopped sessions are no longer listed"),
		ListLiveClaudeChats(Claude, Directory).ContainsByPredicate([&ShortId](const FHyperAIStudioLiveChat& Chat) { return Chat.ShortId == ShortId; }));

	// Leave nothing behind in `claude agents --all`.
	int32 ReturnCode = 0;
	FString Output;
	FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *FString::Printf(TEXT("/d /s /c \"%s rm %s\""), *Claude, *ShortId), &ReturnCode, &Output, nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioChatListTest,
	"HyperAIStudio.Chat.ChatList",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioChatListTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChatHistory;
	const FString Scratch = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudioTests/ChatList"));
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	const FString Root = TEXT("E:/Some Project");
	const FString ClaudeDir = Scratch / TEXT("claude/projects") / ClaudeProjectDirectoryName(Root);
	auto ClaudeLog = [](const FString& Prompt)
	{
		return FString::Printf(TEXT("{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"%s\"}}\n"), *Prompt);
	};
	// More chats than the list used to keep. The oldest, like a long-running main chat, must still be listed.
	TArray<FString> Ids;
	for (int32 Index = 0; Index < 60; ++Index)
	{
		Ids.Add(FString::Printf(TEXT("%08x-0000-4000-8000-000000000000"), Index));
		const FString Path = ClaudeDir / Ids.Last() + TEXT(".jsonl");
		FFileHelper::SaveStringToFile(ClaudeLog(FString::Printf(TEXT("Chat number %d"), Index)), *Path);
		IFileManager::Get().SetTimeStamp(*Path, FDateTime::UtcNow() - FTimespan::FromMinutes(60 - Index));
	}
	FFileHelper::SaveStringToFile(TEXT("{}"), *(ClaudeDir / Ids[0] / TEXT("subagents/agent-a.jsonl")));
	auto CodexLog = [](const FString& Id, const FString& Cwd, const bool bReviewer)
	{
		return FString::Printf(TEXT("{\"type\":\"session_meta\",\"payload\":{\"id\":\"%s\",\"cwd\":\"%s\"%s}}\n"), *Id, *Cwd.Replace(TEXT("/"), TEXT("\\\\")),
			bReviewer ? TEXT(",\"thread_source\":\"subagent\",\"source\":{\"subagent\":{\"other\":\"guardian\"}}") : TEXT(""))
			+ TEXT("{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"Build the title screen\"}]}}\n");
	};
	const FString CodexId = TEXT("01a0975b-2c7e-7e72-a9ce-13cfc8c4a95d");
	const FString ReviewerId = TEXT("01a0975b-2c7e-7e72-a9ce-13cfc8c4a95e");
	FFileHelper::SaveStringToFile(CodexLog(CodexId, Root, false), *(Scratch / TEXT("codex/sessions/2026/09/12/rollout-2026-09-12T10-00-00-") + CodexId + TEXT(".jsonl")));
	FFileHelper::SaveStringToFile(CodexLog(ReviewerId, Root, true), *(Scratch / TEXT("codex/sessions/2026/09/12/rollout-2026-09-12T10-05-00-") + ReviewerId + TEXT(".jsonl")));

	const FString PreviousClaude = FPlatformMisc::GetEnvironmentVariable(TEXT("CLAUDE_CONFIG_DIR"));
	const FString PreviousCodex = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_HOME"));
	FPlatformMisc::SetEnvironmentVar(TEXT("CLAUDE_CONFIG_DIR"), *(Scratch / TEXT("claude")));
	FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_HOME"), *(Scratch / TEXT("codex")));
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("CLAUDE_CONFIG_DIR"), *PreviousClaude);
		FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_HOME"), *PreviousCodex);
		IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	};
	auto Listed = [&Root](const FString& Id)
	{
		return ListSessions(Root).FindByPredicate([&Id](const FHyperAIStudioChatSession& Session) { return Session.SessionId == Id; }) != nullptr;
	};

	TArray<FHyperAIStudioChatSession> Sessions = ListSessions(Root);
	TestEqual(TEXT("Every chat is listed, past the old cap of 50"), Sessions.Num(), 61);
	TestTrue(TEXT("the oldest too"), Listed(Ids[0]));
	TestFalse(TEXT("The thread Codex runs to review its own approvals is not a chat"), Listed(ReviewerId));

	// A rename lands at the end of the log: listing again reads the changed file instead of what it remembered.
	FFileHelper::SaveStringToFile(ClaudeLog(TEXT("Chat number 59")) + TEXT("{\"type\":\"custom-title\",\"customTitle\":\"Main Chat\"}\n"), *(ClaudeDir / Ids[59] + TEXT(".jsonl")));
	Sessions = ListSessions(Root);
	const FHyperAIStudioChatSession* Renamed = Sessions.FindByPredicate([&Ids](const FHyperAIStudioChatSession& Session) { return Session.SessionId == Ids[59]; });
	TestTrue(TEXT("A changed log is read again"), Renamed && Renamed->Title == TEXT("Main Chat"));

	TestTrue(TEXT("A Claude Code chat's log is found from its id"), FPaths::IsSamePath(SessionFile(TEXT("Claude Code"), Root, Ids[3]), ClaudeDir / Ids[3] + TEXT(".jsonl")));
	TestTrue(TEXT("and a Codex thread's"), SessionFile(TEXT("Codex"), Root, CodexId).EndsWith(CodexId + TEXT(".jsonl")));
	TestTrue(TEXT("and a chat with no log has none"), SessionFile(TEXT("Claude Code"), Root, TEXT("ffffffff-0000-4000-8000-000000000000")).IsEmpty());

	const FHyperAIStudioChatSession* Oldest = Sessions.FindByPredicate([&Ids](const FHyperAIStudioChatSession& Session) { return Session.SessionId == Ids[0]; });
	FString Error;
	TestTrue(TEXT("A chat is deleted: ") + Error, Oldest && DeleteSession(*Oldest, Error));
	TestFalse(TEXT("its log is gone"), IFileManager::Get().FileExists(*(ClaudeDir / Ids[0] + TEXT(".jsonl"))));
	TestFalse(TEXT("with its subagents' logs"), IFileManager::Get().DirectoryExists(*(ClaudeDir / Ids[0])));
	TestFalse(TEXT("and it is no longer listed"), Listed(Ids[0]));

	// Only a log inside the agent's own store, named for the chat, is ever touched.
	FHyperAIStudioChatSession Stray = Sessions[0];
	Stray.Path = Scratch / TEXT("elsewhere") / Stray.SessionId + TEXT(".jsonl");
	FFileHelper::SaveStringToFile(TEXT("{}"), *Stray.Path);
	TestFalse(TEXT("A log outside the agent's store is refused"), DeleteSession(Stray, Error));
	TestTrue(TEXT("and left where it is"), IFileManager::Get().FileExists(*Stray.Path));

	TestEqual(TEXT("What HyperAI sent along with a message is not part of what was typed"),
		StripInjectedContext(TEXT("Fix the ramp\n\n<project-memory>\nNotes\n</project-memory>\n\n<skills>\nA skill\n</skills>")), FString(TEXT("Fix the ramp")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioChatGroupsTest,
	"HyperAIStudio.Chat.ChatGroups",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioChatGroupsTest::RunTest(const FString& Parameters)
{
	using FPrefs = UHyperAIStudioChatHistoryPrefs;
	FPrefs* Prefs = GetMutableDefault<FPrefs>();
	// The user's own groups, folds and names are kept aside and put back.
	const TArray<FString> Groups = Prefs->Groups;
	const TMap<FString, FString> ChatGroups = Prefs->ChatGroups;
	const TArray<FString> Collapsed = Prefs->Collapsed;
	const FString AgentFilter = Prefs->AgentFilter;
	const TMap<FString, FString> Titles = Prefs->Titles;
	const TArray<FString> Pinned = Prefs->Pinned;
	const TArray<FString> Remote = Prefs->Remote;
	ON_SCOPE_EXIT
	{
		Prefs->Groups = Groups;
		Prefs->ChatGroups = ChatGroups;
		Prefs->Collapsed = Collapsed;
		Prefs->AgentFilter = AgentFilter;
		Prefs->Titles = Titles;
		Prefs->Pinned = Pinned;
		Prefs->Remote = Remote;
		Prefs->SaveConfig();
	};
	Prefs->Groups.Reset();
	Prefs->ChatGroups.Reset();
	Prefs->Collapsed.Reset();
	const FString Chat = TEXT("7350db99-a8d7-4f9d-9c85-9b141c4163a6");

	TestTrue(TEXT("A group is made"), FPrefs::AddGroup(TEXT("Combat")));
	TestFalse(TEXT("not twice, whatever the case"), FPrefs::AddGroup(TEXT(" combat ")));
	TestFalse(TEXT("nor blank"), FPrefs::AddGroup(TEXT("   ")));
	TestFalse(TEXT("nor too long"), FPrefs::AddGroup(FString::ChrN(FPrefs::MaxGroupNameChars + 1, TEXT('x'))));
	FPrefs::SetGroup(Chat, TEXT("Combat"));
	TestEqual(TEXT("A chat moves into it"), FPrefs::GetGroup(Chat), FString(TEXT("Combat")));
	FPrefs::SetCollapsed(TEXT("group:Combat"), true);
	TestTrue(TEXT("A renamed group keeps its chats and stays folded"), FPrefs::RenameGroup(TEXT("Combat"), TEXT("Melee"))
		&& FPrefs::GetGroup(Chat) == TEXT("Melee") && FPrefs::IsCollapsed(TEXT("group:Melee")) && !FPrefs::IsCollapsed(TEXT("group:Combat")));
	FPrefs::RemoveGroup(TEXT("Melee"));
	TestTrue(TEXT("A deleted group sends its chats back to their dates"), FPrefs::GetGroup(Chat).IsEmpty() && !FPrefs::IsCollapsed(TEXT("group:Melee")));
	FPrefs::SetGroup(Chat, TEXT("Nowhere"));
	TestTrue(TEXT("A chat cannot move into a group that does not exist"), FPrefs::GetGroup(Chat).IsEmpty());

	FPrefs::AddGroup(TEXT("Combat"));
	FPrefs::SetGroup(Chat, TEXT("Combat"));
	FPrefs::SetTitle(Chat, TEXT("Main Chat"));
	if (!FPrefs::IsPinned(Chat))
	{
		FPrefs::TogglePinned(Chat);
	}
	FPrefs::Forget(Chat);
	TestTrue(TEXT("A deleted chat leaves no name, pin or group behind"),
		FPrefs::GetGroup(Chat).IsEmpty() && FPrefs::GetTitle(Chat).IsEmpty() && !FPrefs::IsPinned(Chat));

	FPrefs::SetAgentFilter(TEXT("Codex"));
	TestEqual(TEXT("The list remembers whose chats it shows"), FPrefs::GetAgentFilter(), FString(TEXT("Codex")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioChatSidebarShotTest,
	"HyperAIStudio.Chat.SidebarShot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioChatSidebarShotTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChatHistory;
	using FPrefs = UHyperAIStudioChatHistoryPrefs;
	const FString Scratch = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudioTests/ChatSidebar"));
	IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	const FString Root = FHyperAIStudioService::GetProjectRoot();
	struct FChat
	{
		const TCHAR* Title;
		bool bCodex;
		double HoursAgo;
	};
	const FChat Chats[] = {
		{TEXT("Arena ramp and cover pass"), false, 0.2}, {TEXT("Knockdown get-up timing"), false, 1.5}, {TEXT("Title screen UMG"), true, 2.0},
		{TEXT("Main Chat"), false, 30.0}, {TEXT("Boss roar layers"), false, 50.0}, {TEXT("Combat VFX: sword trails"), true, 100.0},
		{TEXT("Parry window tuning"), false, 120.0}, {TEXT("Level streaming setup"), false, 300.0}, {TEXT("Inventory widget"), true, 400.0},
		{TEXT("Dodge roll i-frames"), false, 900.0}, {TEXT("Lighting look dev"), false, 1500.0}};
	TMap<FString, FString> Ids;
	int32 Index = 0;
	for (const FChat& Chat : Chats)
	{
		const FString Id = FString::Printf(TEXT("%08x-0000-4000-8000-0000000000%02d"), 0x51de0000 + Index, Index);
		++Index;
		Ids.Add(Chat.Title, Id);
		const FString Path = Chat.bCodex
			? Scratch / TEXT("codex/sessions/2026/09/30/rollout-2026-09-30T10-00-00-") + Id + TEXT(".jsonl")
			: Scratch / TEXT("claude/projects") / ClaudeProjectDirectoryName(Root) / Id + TEXT(".jsonl");
		const FString Log = Chat.bCodex
			? FString::Printf(TEXT("{\"type\":\"session_meta\",\"payload\":{\"id\":\"%s\",\"cwd\":\"%s\"}}\n{\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"%s\"}]}}\n"),
				*Id, *Root.Replace(TEXT("/"), TEXT("\\\\")), Chat.Title)
			: FString::Printf(TEXT("{\"type\":\"ai-title\",\"aiTitle\":\"%s\"}\n{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"%s\"}}\n%s"), Chat.Title, Chat.Title,
				Index % 3 == 0 ? TEXT("{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"name\":\"mcp__unreal-mcp__call_tool\"}]}}\n") : TEXT(""));
		FFileHelper::SaveStringToFile(Log, *Path);
		IFileManager::Get().SetTimeStamp(*Path, FDateTime::UtcNow() - FTimespan::FromHours(Chat.HoursAgo));
	}

	// A pinned chat, a group of two, and a folded section, kept aside from the user's own and put back after.
	FPrefs* Prefs = GetMutableDefault<FPrefs>();
	const TArray<FString> Groups = Prefs->Groups;
	const TMap<FString, FString> ChatGroups = Prefs->ChatGroups;
	const TArray<FString> Collapsed = Prefs->Collapsed;
	const FString AgentFilter = Prefs->AgentFilter;
	const TArray<FString> Pinned = Prefs->Pinned;
	const FString PreviousClaude = FPlatformMisc::GetEnvironmentVariable(TEXT("CLAUDE_CONFIG_DIR"));
	const FString PreviousCodex = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_HOME"));
	ON_SCOPE_EXIT
	{
		Prefs->Groups = Groups;
		Prefs->ChatGroups = ChatGroups;
		Prefs->Collapsed = Collapsed;
		Prefs->AgentFilter = AgentFilter;
		Prefs->Pinned = Pinned;
		Prefs->SaveConfig();
		FPlatformMisc::SetEnvironmentVar(TEXT("CLAUDE_CONFIG_DIR"), *PreviousClaude);
		FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_HOME"), *PreviousCodex);
		IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	};
	Prefs->Groups.Reset();
	Prefs->ChatGroups.Reset();
	Prefs->Collapsed.Reset();
	Prefs->AgentFilter.Reset();
	FPrefs::AddGroup(TEXT("Combat"));
	FPrefs::SetGroup(Ids[TEXT("Combat VFX: sword trails")], TEXT("Combat"));
	FPrefs::SetGroup(Ids[TEXT("Parry window tuning")], TEXT("Combat"));
	if (!FPrefs::IsPinned(Ids[TEXT("Main Chat")]))
	{
		FPrefs::TogglePinned(Ids[TEXT("Main Chat")]);
	}
	FPrefs::SetCollapsed(TEXT("month"), true);
	FPlatformMisc::SetEnvironmentVar(TEXT("CLAUDE_CONFIG_DIR"), *(Scratch / TEXT("claude")));
	FPlatformMisc::SetEnvironmentVar(TEXT("CODEX_HOME"), *(Scratch / TEXT("codex")));

	const TSharedRef<SHyperAIStudioChatHistorySidebar> Sidebar = SNew(SHyperAIStudioChatHistorySidebar)
		.ActiveSessionId(Ids[TEXT("Arena ramp and cover pass")]);
	Sidebar->Refresh();
	const double End = FPlatformTime::Seconds() + 30.0;
	while (Sidebar->IsLoading() && FPlatformTime::Seconds() < End)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FTSTicker::GetCoreTicker().Tick(0.05f);
		FPlatformProcess::Sleep(0.02f);
	}
	TestFalse(TEXT("The chats are read"), Sidebar->IsLoading());
	const FString All = HyperAIStudio::Tests::SaveWidgetShot(Sidebar, FIntPoint(300, 760), TEXT("step20-chat-sidebar"));
	FPrefs::SetAgentFilter(TEXT("Codex"));
	Sidebar->RebuildFiltered();
	const FString Codex = HyperAIStudio::Tests::SaveWidgetShot(Sidebar, FIntPoint(300, 420), TEXT("step20-chat-sidebar-codex"));
	if (All.IsEmpty())
	{
		AddWarning(TEXT("This editor cannot draw off screen, so the chat list was not pictured."));
		return true;
	}
	AddInfo(TEXT("Picture chat-sidebar: ") + All);
	AddInfo(TEXT("Picture chat-sidebar-codex: ") + Codex);
	return true;
}

#endif
