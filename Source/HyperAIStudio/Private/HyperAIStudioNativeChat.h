// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"

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
 * A chat HyperAIStudio hosts itself instead of in a terminal, over the CLI's stdin and stdout: Codex through its
 * app-server, and Gemini, GitHub Copilot, OpenCode and other agents through the Agent Client Protocol, all
 * newline-delimited JSON-RPC, and Claude Code through the stream-json protocol its Agent SDK uses. Game thread only.
 */
class FHyperAIStudioNativeChat : public TSharedFromThis<FHyperAIStudioNativeChat>
{
public:
	static bool IsSupported(const FString& AgentName);
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
	void Stop();
	/** Sends now, or once the running turn ends. Images are files the agent sees with the text. */
	void Send(const FString& Text, const TArray<FString>& Images = {});
	void Interrupt();
	/** Answers an open approval with one of its choices. */
	void Answer(const FString& ItemId, int32 Choice);

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

	/** One message to the agent, as a line. */
	void Write(const TSharedRef<FJsonObject>& Message);
	using FOnResult = TFunction<void(const TSharedPtr<FJsonObject>& Result, const FString& Error)>;
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
	FString ResumeSessionId;
	FString SessionId;
	FString Usage;
	TArray<FHyperAIStudioChatPlanStep> Plan;
	bool bHeadless = false;
	FString HeadlessInstructions;

private:
	struct FQueuedMessage
	{
		FString Text;
		TArray<FString> Images;
	};

	/** Adds an item without announcing it; AddItem and AskApproval announce it once it is complete. */
	TSharedRef<FHyperAIStudioChatItem> NewItem(FHyperAIStudioChatItem::EKind Kind, const FString& Id, const FString& Title,
		const FString& Text, const FString& Status);
	bool Pump(float DeltaTime);
	void Dispatch(const FString& Line);
	void ClosePipes();
	/** What project memory knows about Text and the skills that fit it, that this chat has not had yet, to send with it. */
	FString MemoryFor(const FString& Text);
	/** Records the turn that just ended in the project's task history. */
	void RecordTurn();

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
};
