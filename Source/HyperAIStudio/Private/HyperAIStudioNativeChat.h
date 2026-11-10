// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioChatModes.h"

class FHyperAIStudioHostStreamReader;

/** One line of a native chat's transcript. */
struct FHyperAIStudioChatItem
{
	enum class EKind : uint8
	{
		User,
		Agent,
		Reasoning,
		Tool,
		Command,
		FileChange,
		Approval,
		Error,
		Notice,
		/** A picture the agent made or looked at; Images holds its file. */
		Image
	};

	FString Id;
	EKind Kind = EKind::Notice;
	/** A tool, command or file list; an approval's question. */
	FString Title;
	/** Message text, or a tool's output. */
	FString Text;
	/** running, completed, failed, or the answer an approval got. */
	FString Status;
	/** Approval choices as shown, and what the agent expects back for each. */
	TArray<FString> Choices;
	TArray<FString> ChoiceIds;
	bool bAnswered = false;
	/** Image files on disk: what the user attached, or the picture an Image item shows. */
	TArray<FString> Images;
};

struct FHyperAIStudioChatPlanStep
{
	FString Text;
	/** pending, inProgress or completed. */
	FString Status;
};

/**
 * What a message says about itself beyond its words, as a prompt preset does: its packs, skills and project agent
 * go with it as they are named, instead of the task router's guess from the words.
 */
struct FHyperAIStudioMessageHints
{
	/** The preset it was written from, named in the line that says what went with the message. */
	FString PresetId;
	/** Catalog pack ids, the main one first. */
	TArray<FString> Packs;
	/** The hyper_* tools its prompt names: listed before the rest of its packs' tools, each with its toolset_name. */
	TArray<FString> Tools;
	TArray<FString> Skills;
	/** A project agent's name, or "team". */
	FString Agent;
	/** The task route (by its category's name) a new chat's first message takes. */
	FString Route;

	bool IsEmpty() const { return PresetId.IsEmpty() && Packs.IsEmpty() && Tools.IsEmpty() && Skills.IsEmpty() && Agent.IsEmpty() && Route.IsEmpty(); }
};

/**
 * A chat HyperAIStudio hosts itself instead of in a terminal, over the CLI's stdin and stdout: Codex through its
 * app-server, and Gemini, GitHub Copilot, OpenCode and other agents through the Agent Client Protocol, all
 * newline-delimited JSON-RPC, and Claude Code through the stream-json protocol its Agent SDK uses. Game thread only.
 */
class FHyperAIStudioNativeChat : public TSharedFromThis<FHyperAIStudioNativeChat>
{
public:
	static bool IsSupported(const FString& AgentName);
	static bool IsDirectApiAgent(const FString& AgentName);
	/** Direct provider routes whose API key and bundled Python runtime are available; no agent CLI required. */
	static TArray<FString> GetAvailableApiAgents();
	/** Same direct API engine in either the native protocol or its interactive terminal presentation. No key in arguments. */
	static FString GetDirectApiLaunchCommand(const FString& AgentName, const FString& ProjectRoot, const FString& Model,
		const FString& SessionId, bool bTerminal, bool bReadOnly = false);
	/**
	 * Agents HyperAI hosts through the Agent Client Protocol whose command is installed: each chat hands them Unreal
	 * MCP, so they need no settings of their own. Gemini is not among them; it has its own route and settings file.
	 */
	static TArray<FString> GetInstalledAcpAgents();
	/** The command that runs such an agent's own terminal interface; empty for any other agent. */
	static FString GetAcpTerminalCommand(const FString& AgentName);
#if WITH_DEV_AUTOMATION_TESTS
	/** Runs Command instead of the agent's CLI, so protocol tests can use a scripted stand-in. Empty restores it. */
	static void SetCommandForTests(const FString& Command);
	/** Sends ContextFor with messages in tests too, which otherwise send exactly what they script. */
	static void SetContextInTestsForTests(bool bEnabled);
#endif
	/**
	 * Null for an agent without a native protocol. ResumeSessionId continues an earlier chat. Subagent is for Claude
	 * Code: a new chat starts as that project subagent.
	 */
	static TSharedPtr<FHyperAIStudioNativeChat> Create(const FString& AgentName, const FString& ProjectRoot, const FString& Model,
		const FString& ResumeSessionId, const FString& Subagent = FString());

	virtual ~FHyperAIStudioNativeChat();

	/**
	 * Before Start: run with no one watching, for a tool that wants one answer, such as a picture. Codex gets an
	 * ephemeral read-only thread that never asks for approval, with Instructions as its developer instructions.
	 * Gemini's permission requests are answered by the chat itself, and only image tools are allowed.
	 */
	void SetHeadless(const FString& Instructions);
	bool Start(FString& OutError);
	/**
	 * Instead of Start: takes up a chat whose agent HyperAI's background host runs (FHyperAIStudioAgentHost), one an
	 * editor that closed or crashed left there. Everything the agent said since it started is replayed into the
	 * transcript, its turn and open approvals included, and the chat goes on live from there.
	 */
	bool Attach(const FString& HostSessionId, FString& OutError);
	/** Lets go of a hosted chat without stopping its agent, as the editor closes; another editor attaches to it later. */
	void Detach();
	void Stop();
	/** The background host's session this chat runs in; empty when its agent runs inside the editor. */
	const FString& GetHostSession() const { return HostSession; }
	/** For the host's note of the chat: the tab it is in, and its title and subagent, for an editor that takes it up later. */
	void SetHostNote(int32 TabIndex, const FString& Title, const FString& Subagent);
	/** Sends now, or once the running turn ends. Images are files the agent sees with the text; Hints go with it either way. */
	void Send(const FString& Text, const TArray<FString>& Images = {}, const FHyperAIStudioMessageHints& Hints = {});
	void Interrupt();
	/** Answers an open approval with one of its choices. */
	void Answer(const FString& ItemId, int32 Choice);
	/** A line in the transcript from HyperAI itself, such as which agent a task was routed to. */
	void AddNotice(const FString& Text);
	/**
	 * The chat's mode: its instructions go with the next message, its packs, tools and skills with every one, and a
	 * read-only mode makes HyperAI refuse edit plans while this chat runs. Label names the chat in refusals.
	 */
	void SetMode(const FHyperAIStudioChatMode& InMode, const FString& Label);
	const FHyperAIStudioChatMode& GetMode() const { return Mode; }
	/**
	 * What goes with a message beside what was typed: the project notes and past tasks it matches, the project agent its
	 * task is for (unless the chat runs as one, StartedAs), the skills made for its kind of work and the HyperAI tools
	 * for it, each once per chat (Given). ChatAgent is the agent the chat runs on. OutSummary says in a few words what was added.
	 */
	static FString ContextFor(const FString& Root, const FString& Text, const FString& ChatAgent, const FString& StartedAs, TSet<FString>& Given,
		FString& OutSummary);
	/**
	 * ContextFor a message that names its own packs, skills and agent (Hints): their tools and skills go with it whatever
	 * its words, and its agent takes the hand-off when the project has it (the team when it is installed). Anything
	 * Hints leave out is found from the words as above.
	 */
	static FString ContextFor(const FString& Root, const FString& Text, const FHyperAIStudioMessageHints& Hints, const FString& ChatAgent,
		const FString& StartedAs, TSet<FString>& Given, FString& OutSummary);

	bool IsRunning() const;
	bool IsReady() const { return bReady; }
	bool IsBusy() const { return bBusy; }
	bool HasOpenApproval() const { return !OpenApprovals.IsEmpty(); }
	int32 NumQueued() const { return Queued.Num(); }
	const FString& GetAgentName() const { return AgentName; }
	const FString& GetModel() const { return Model; }
	/** The agent's own id for this chat, for resuming it later. */
	const FString& GetSessionId() const { return SessionId; }
	const TArray<TSharedPtr<FHyperAIStudioChatItem>>& GetItems() const { return Items; }
	const TArray<FHyperAIStudioChatPlanStep>& GetPlan() const { return Plan; }
	/** Token and rate-limit use, as one line. */
	const FString& GetUsage() const { return Usage; }
	/** Where pictures an agent sends as data are written. */
	static FString ImageFolder();

	/** After any change to the transcript, the plan, the usage or the state. */
	FSimpleMulticastDelegate OnChanged;

protected:
	FHyperAIStudioNativeChat(const FString& InAgentName, const FString& InProjectRoot, const FString& InModel, const FString& InResumeSessionId);

	virtual FString CommandLine() const = 0;
	/** Codex leaves the jsonrpc field out; the Agent Client Protocol requires it. */
	virtual bool WritesJsonRpcVersion() const = 0;
	/** Begins the handshake; call Ready once the chat can take a message. */
	virtual void Handshake() = 0;
	virtual void SendNow(const FString& Text, const TArray<FString>& Images) = 0;
	virtual void InterruptNow() = 0;
	virtual void HandleMessage(const FString& Method, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonValue>& RequestId) = 0;
	virtual TSharedRef<FJsonObject> AnswerPayload(const FHyperAIStudioChatItem& Item, int32 Choice) = 0;
	/** Sees every message before it is read as JSON-RPC; true when it was handled. */
	virtual bool HandleLine(const TSharedRef<FJsonObject>& /*Line*/) { return false; }
	/** A resumed chat brings back what was said before, for an agent that does not send it back itself. */
	virtual void LoadHistory() {}
	/**
	 * Replaying a hosted chat: what handles the answer to a request the editor sent before (Method), so a turn that
	 * ends with that answer ends in the replay too. Null for requests whose answer only matters live.
	 */
	using FOnResult = TFunction<void(const TSharedPtr<FJsonObject>& Result, const FString& Error)>;
	virtual FOnResult ReplayCallback(const FString& /*Method*/) { return nullptr; }
	/** Replaying a hosted chat: the user's message just shown starts a new turn, as SendNow does live. */
	virtual void ReplayUserMessage() {}
	/** The id of a request the agent sent (a JSON-RPC request; Claude Code's control_request), or empty. */
	virtual FString AgentRequestKey(const TSharedRef<FJsonObject>& Line) const;
	/** The id of the agent's request this line of the editor's answers, or empty. */
	virtual FString EditorAnswerKey(const TSharedRef<FJsonObject>& Line) const;
	/** The id of a request the agent withdrew with this line, or empty. */
	virtual FString AgentWithdrawKey(const TSharedRef<FJsonObject>& /*Line*/) const { return FString(); }
	/** A request id as text, a number or a string alike. */
	static FString KeyOf(const TSharedPtr<FJsonValue>& Id);
	/**
	 * Shows an earlier part of the chat above everything on screen: Replay adds its items as the live chat would, and
	 * they move to the top. bMoreBefore says the conversation goes back further than what is shown.
	 */
	void ShowEarlier(TFunctionRef<void()> Replay, bool bMoreBefore);

	/** One message to the agent, as a line. */
	void Write(const TSharedRef<FJsonObject>& Message);
	void Request(const FString& Method, const TSharedRef<FJsonObject>& Params, FOnResult OnResult = nullptr);
	void Notify(const FString& Method, const TSharedRef<FJsonObject>& Params);
	/** Answers a request the agent sent; Claude Code answers with control responses instead. */
	virtual void Respond(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result);
	virtual void RespondError(const TSharedPtr<FJsonValue>& Id, const FString& Message);

	void Ready(const FString& InSessionId);
	void TurnStarted();
	void TurnFinished();
	TSharedPtr<FHyperAIStudioChatItem> FindItem(const FString& Id) const;
	TSharedRef<FHyperAIStudioChatItem> AddItem(FHyperAIStudioChatItem::EKind Kind, const FString& Id, const FString& Title,
		const FString& Text, const FString& Status = FString());
	/** An approval the user answers later with Answer. */
	void AskApproval(const TSharedPtr<FJsonValue>& RequestId, const FString& Id, const FString& Title, const FString& Text,
		const TArray<FString>& Choices, const TArray<FString>& ChoiceIds);
	/** Closes an approval the agent no longer waits for, with Status as its answer. */
	void CloseApproval(const FString& ItemId, const FString& Status);
	void Changed();
	/** Writes an agent's base64 picture under ImageFolder and returns the file; empty when it is not an image. */
	static FString SaveImageData(const FString& Base64, const FString& Name);

	FString AgentName;
	FString ProjectRoot;
	FString Model;
	/** The project agent the chat runs as (Claude Code's --agent), so its tasks are that agent's own. */
	FString StartedAs;
	FString ResumeSessionId;
	FString SessionId;
	FString Usage;
	TArray<FHyperAIStudioChatPlanStep> Plan;
	bool bHeadless = false;
	FString HeadlessInstructions;
	/** Set while ShowEarlier replays: nothing is announced, and pictures are not written to disk again. */
	bool bReplaying = false;
	/** Set while a hosted chat's stream replays: nothing reaches the agent, no request is sent, and pictures are kept. */
	bool bStreamReplay = false;

private:
	struct FQueuedMessage
	{
		FString Text;
		TArray<FString> Images;
		FHyperAIStudioMessageHints Hints;
	};

	/** Adds an item without announcing it; AddItem and AskApproval announce it once it is complete. */
	TSharedRef<FHyperAIStudioChatItem> NewItem(FHyperAIStudioChatItem::EKind Kind, const FString& Id, const FString& Title,
		const FString& Text, const FString& Status);
	bool Pump(float DeltaTime);
	/** A hosted chat's pump: the lines its host wrote since the last tick. */
	bool PumpHost();
	void Dispatch(const FString& Line);
	void ClosePipes();
	/** Starts the agent in the background host; false (and nothing started) when the host cannot run. */
	bool StartHosted(const FString& Command, FString& OutError);
	/** Everything a hosted chat's stream holds, run through the chat as it happened, with nothing sent to the agent. */
	void ReplayStream(const TArray<FString>& Lines);
	/** The host says the agent ended (exited, stopped or lost): Body is its event. */
	void HostEnded(const FString& Body);
	/** A note kept in a hosted chat's stream for an editor that replays it later. */
	void Note(const TSharedRef<FJsonObject>& Fields);
	/** Lets go of the chat's ticker and turn, the agent left alone. */
	void EndLocally();
	/** ContextFor Text and its Hints, with what this chat has not had yet, to send with it; a notice says what went. */
	FString MemoryFor(const FString& Text, const FHyperAIStudioMessageHints& Hints);
	/** Records the turn that just ended in the project's task history. */
	void RecordTurn();

	/** The background host's session, and its stream as read so far; empty when the agent runs inside the editor. */
	FString HostSession;
	TUniquePtr<FHyperAIStudioHostStreamReader> HostStream;
	/** When the host was asked to start the agent; its first word is waited for a while, the host restarted if gone. */
	double HostAskedAt = 0.0;
	double HostCheckedAt = 0.0;
	bool bHostHeard = false;
	FProcHandle Process;
	void* StdOutRead = nullptr;
	void* StdOutWrite = nullptr;
	void* StdInRead = nullptr;
	void* StdInWrite = nullptr;
	TArray<uint8> Pending;
	/** The last line that was not a message: often why the agent stopped. */
	FString LastLogLine;
	int64 NextId = 1;
	TMap<int64, FOnResult> Waiting;
	TMap<FString, TSharedPtr<FJsonValue>> OpenApprovals;
	TArray<TSharedPtr<FHyperAIStudioChatItem>> Items;
	TArray<FQueuedMessage> Queued;
	FTSTicker::FDelegateHandle Ticker;
	bool bReady = false;
	bool bBusy = false;
	bool bStopped = false;
	/** The notes and past tasks this chat was given, and the tasks it finished: none is sent to it again. */
	TSet<FString> GivenMemory;
	FHyperAIStudioChatMode Mode;
	/** The mode whose instructions the agent was last sent, so a change is announced once. */
	FString SentModeKey;
};
