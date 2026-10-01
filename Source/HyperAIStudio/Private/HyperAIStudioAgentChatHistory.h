// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** A Claude Code session running right now, as `claude agents --json` lists it. */
struct FHyperAIStudioLiveChat
{
	/** Short id that `claude attach`, `stop` and `logs` take. */
	FString ShortId;
	FString SessionId;
	/** "background", or "interactive" for a session open in some terminal. */
	FString Kind;
	/** "busy", "idle" or "waiting". */
	FString Status;

	bool IsRunning() const { return !ShortId.IsEmpty(); }
	bool IsBackground() const { return Kind == TEXT("background"); }
	bool IsBusy() const { return Status == TEXT("busy"); }
};

/** One past agent conversation that ran in the project root, where the project's MCP config connects it to Unreal. */
struct FHyperAIStudioChatSession
{
	/** Built-in agent route name: "Claude Code" or "Codex". */
	FString AgentName;
	FString SessionId;
	FString Title;
	FDateTime LastActiveUtc;
	/** An Unreal MCP tool call was found in the part of the log that was read. */
	bool bUsedUnrealMcp = false;
	/** Set while the chat is running (Claude Code only). */
	FHyperAIStudioLiveChat Live;
	/** The agent's own transcript file. */
	FString Path;
};

/**
 * Reads the agents' own session stores; nothing is copied or persisted by the plugin.
 * Claude Code: <CLAUDE_CONFIG_DIR or ~/.claude>/projects/<root with non-alphanumerics as '-'>/<id>.jsonl.
 * Codex: <CODEX_HOME or ~/.codex>/sessions/**\/rollout-*.jsonl whose session_meta cwd is the root, leaving out the
 * threads Codex runs for itself (subagents such as its approval reviewer); titles from session_index.jsonl. Gemini
 * only resumes by list index, which shifts, so it is not offered.
 * Safe to call off the game thread.
 */
namespace HyperAIStudio::ChatHistory
{
	/** Newest first. What each file holds is remembered while it is unchanged, so listing again reads only what moved. */
	TArray<FHyperAIStudioChatSession> ListSessions(const FString& ProjectRoot, int32 MaxSessions = 400);

	/** The agent's own log of a chat, found from its id; empty when it has none. */
	FString SessionFile(const FString& AgentName, const FString& ProjectRoot, const FString& SessionId);
	/**
	 * Deletes a chat: its log goes to the Recycle Bin, where it can be restored, and for Claude Code the folder of
	 * subagent logs beside it. False, with OutError, when nothing could be moved.
	 */
	bool DeleteSession(const FHyperAIStudioChatSession& Session, FString& OutError);
	/** A message as it was typed, without the project memory, agent, skill and tool notes HyperAI's chat sends along with it. */
	FString StripInjectedContext(const FString& Message);
	/** The whole lines in the last MaxBytes of a log; bOutCut says the log goes back further. */
	TArray<FString> ReadRecentLines(const FString& Path, int64 MaxBytes, bool& bOutCut);

	/** CLI arguments that resume the session, e.g. "--resume <id>"; empty when the id or agent is not resumable. */
	FString BuildResumeArguments(const FString& AgentName, const FString& SessionId);

	bool IsSafeSessionId(const FString& SessionId);
	/** True when the text records a call to an Unreal or Hyper MCP tool, not merely a tool listing. */
	bool MentionsUnrealMcpCall(const FString& JsonlText);
	FString ClaudeProjectDirectoryName(const FString& ProjectRoot);
	/** Title from Claude Code jsonl text: last custom title, else last AI title, else the first typed prompt. */
	FString ExtractClaudeTitle(const FString& JsonlText);
	/** First typed prompt from Codex jsonl text, skipping injected AGENTS.md, environment and slash-command turns. */
	FString ExtractCodexFirstPrompt(const FString& JsonlText);

	/** A random RFC 4122 version 4 id for `claude --session-id`, which refuses any other shape. */
	FString NewClaudeSessionId();
	/** Remote Control arguments. The chat title names the remote session; a chat without one gets Claude's own name, prefixed with the project. */
	FString BuildClaudeRemoteArguments(const FString& ProjectName, const FString& ChatTitle);
	/** Short id from `claude --bg` output ("backgrounded, then the id"); empty when nothing started. */
	FString ParseBackgroundedId(const FString& Output);
	/** Sessions in `claude agents --json` output that run in ProjectRoot. */
	TArray<FHyperAIStudioLiveChat> ParseLiveClaudeChats(const FString& Json, const FString& ProjectRoot);
	/** Copies each running session onto its listed chat. */
	void MergeLiveChats(TArray<FHyperAIStudioChatSession>& Sessions, const TArray<FHyperAIStudioLiveChat>& Live);
	/** True once Claude Code has written the session's log, so `--resume` can find it. */
	bool ClaudeSessionExists(const FString& ProjectRoot, const FString& SessionId);

	/**
	 * These run the claude CLI and block for a second or more, so call them off the game thread. ClaudeCommand is
	 * the resolved launcher (FHyperAIStudioService::GetAgentTerminalLaunchCommand). Processes they start are not tied
	 * to the editor, so background sessions outlive it.
	 */
	TArray<FHyperAIStudioLiveChat> ListLiveClaudeChats(const FString& ClaudeCommand, const FString& ProjectRoot);
	/** Starts `claude --bg <Arguments>` in ProjectRoot and returns its short id. EnvPrefix is cmd `set` commands ending in "&& ". */
	FString StartClaudeBackgroundChat(const FString& ClaudeCommand, const FString& ProjectRoot, const FString& EnvPrefix,
		const FString& Arguments, FString& OutError);
	/** `claude stop <ShortId>`: the session ends and its conversation stays resumable. */
	bool StopClaudeBackgroundChat(const FString& ClaudeCommand, const FString& ShortId);
}
