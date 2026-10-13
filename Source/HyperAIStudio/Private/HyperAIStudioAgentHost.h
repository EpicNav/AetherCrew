// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** One line of a hosted agent's stream.log: its kind (o, e, i, u or x) and what follows the tab. */
struct FHyperAIStudioHostLine
{
	TCHAR Kind = 0;
	FString Body;
};

/** The host's view of one session, from its state.json. */
struct FHyperAIStudioHostSessionState
{
	/** starting, running, exited, stopped or lost. */
	FString State;
	int32 ProcessId = 0;
	int32 ExitCode = 0;
	bool bHasExitCode = false;
	FString Reason;
	FString LastActivityAt;

	bool IsRunning() const { return State == TEXT("running") || State == TEXT("starting"); }
};

/** The editor's note of a hosted chat, its meta.json: what an editor needs to take the chat up again. */
struct FHyperAIStudioHostSessionMeta
{
	FString Id;
	FString Agent;
	FString Model;
	/** The agent's own id for the conversation, once it is known. */
	FString SessionId;
	/** The conversation the chat resumed when it started, if any. */
	FString ResumeSessionId;
	FString Subagent;
	FString Title;
	FString ProjectFile;
	int32 TabIndex = 0;
	/** The editor showing the chat; 0 while none does. */
	uint32 EditorProcessId = 0;
	/** The chat was closed in the editor: the host forgets it once its agent is gone. */
	bool bClosed = false;
	/** Its turn ran, or an approval waited, when its editor went away. */
	bool bBusyAtDetach = false;
	FString DetachedAt;
};

/**
 * HyperAI Studio's agent host, seen from the editor. Resources/Scripts/HyperAIStudioAgentHost.ps1 runs apart from the
 * editor and owns the native chats' agent processes, so a turn keeps going when the editor closes, restarts or crashes;
 * an editor that comes back reattaches each chat and replays what it missed. Everything passes through files under
 * .hyperai/runtime/agent-host, which the host limits to this user; no port is opened. Game thread only, Windows only.
 */
class FHyperAIStudioAgentHost
{
public:
	/** Native chats run under the host: the setting is on, this is an interactive Windows editor, the script ships; or a test set a root. */
	static bool ShouldHost();
	/** .hyperai/runtime/agent-host in the project, or a test's folder. */
	static FString GetRoot();
#if WITH_DEV_AUTOMATION_TESTS
	/** Hosts chats under Root with these limits (seconds, minutes); empty restores the project's host and the setting. */
	static void SetRootForTests(const FString& Root, int32 LingerSeconds = 3, double IdleMinutes = 30.0);
#endif
	/** A host answers for GetRoot: its heartbeat is fresh and its process alive. */
	static bool IsHostRunning();
	/** Starts the host unless one runs, and waits up to WaitSeconds for its heartbeat. */
	static bool EnsureRunning(FString& OutError, double WaitSeconds = 8.0);

	static FString NewSessionId();
	/**
	 * Asks the host to start Command for session Id (through cmd, in WorkingDirectory), with Environment added and each
	 * (variable, Credential Manager target) of Targets read by the host into the agent's environment alone.
	 */
	static bool StartSession(const FString& Id, const FString& Command, const FString& WorkingDirectory, const TArray<TPair<FString, FString>>& Targets,
		const TMap<FString, FString>& Environment, FString& OutError);
	/** Stops the agent; bForget also drops its files. */
	static void StopSession(const FString& Id, const FString& Reason, bool bForget);
	/** Asks every running agent to stop; returns how many ran. The Stop Background Agents button. */
	static int32 StopAll(const FString& Reason);
	/** One line for session Id: Kind 'a' goes to the agent's stdin, 'u' is a note kept in its stream for a later replay. */
	static bool Append(const FString& Id, TCHAR Kind, const FString& Line);

	static bool ReadState(const FString& Id, FHyperAIStudioHostSessionState& Out);
	static bool ReadMeta(const FString& Id, FHyperAIStudioHostSessionMeta& Out);
	static bool WriteMeta(const FHyperAIStudioHostSessionMeta& Meta);
	static TArray<FString> ListSessions();
	/** Sessions whose agent runs now. */
	static int32 CountRunning();
	static FString StreamPath(const FString& Id);
	/** A stream line as kind and body; false for a line that is not one. */
	static bool ParseLine(const FString& Line, FHyperAIStudioHostLine& Out);

	/**
	 * The sessions of ProjectFile no running editor shows and nobody closed: an agent still running, or one that ended
	 * while its editor was away. Skip names sessions already taken (a restart's own chats). Oldest tab first.
	 */
	static TArray<FHyperAIStudioHostSessionMeta> FindDetachedSessions(const FString& ProjectFile, const TSet<FString>& Skip);
	/** Whether a chat with these sessions should reattach (see FindDetachedSessions): pure, for tests. */
	static bool IsDetached(const FHyperAIStudioHostSessionMeta& Meta, const FHyperAIStudioHostSessionState& State, bool bHasState, const FString& ProjectFile,
		bool bOwnerRunning);

	/** The editor closes: a hosted chat that works detaches rather than stopping. */
	static bool IsEditorExiting();
	static void SetEditorExiting(bool bExiting = true);

	/** Module lifetime: the closing hooks and the HyperAIStudio.StopBackgroundAgents console command. */
	static void Startup();
	static void Shutdown();
};

/** Reads a stream.log as it grows, from where it left off: whole lines only. The file may not exist yet. */
class FHyperAIStudioHostStreamReader
{
public:
	explicit FHyperAIStudioHostStreamReader(const FString& InPath);
	~FHyperAIStudioHostStreamReader();
	/** Appends the complete lines written since the last call. */
	void Read(TArray<FString>& OutLines);

private:
	FString Path;
	void* Handle = nullptr;
	TArray<uint8> Pending;
};
