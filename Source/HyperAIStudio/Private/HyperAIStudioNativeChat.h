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
 * A chat HyperAIStudio hosts itself instead of in a terminal: Codex through its app-server and Gemini through the
 * Agent Client Protocol, both newline-delimited JSON-RPC over the CLI's stdin and stdout. Claude Code has no
 * documented protocol for this and stays in the terminal. Game thread only.
 */
class FHyperAIStudioNativeChat : public TSharedFromThis<FHyperAIStudioNativeChat>
{
public:
	static bool IsSupported(const FString& AgentName);
#if WITH_DEV_AUTOMATION_TESTS
	/** Runs Command instead of the agent's CLI, so protocol tests can use a scripted stand-in. Empty restores it. */
	static void SetCommandForTests(const FString& Command);
#endif
	/** Null for an agent without a native protocol. ResumeSessionId continues an earlier chat. */
	static TSharedPtr<FHyperAIStudioNativeChat> Create(const FString& AgentName, const FString& ProjectRoot, const FString& Model,
		const FString& ResumeSessionId);

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

	using FOnResult = TFunction<void(const TSharedPtr<FJsonObject>& Result, const FString& Error)>;
	void Request(const FString& Method, const TSharedRef<FJsonObject>& Params, FOnResult OnResult = nullptr);
	void Notify(const FString& Method, const TSharedRef<FJsonObject>& Params);
	void Respond(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result);
	void RespondError(const TSharedPtr<FJsonValue>& Id, const FString& Message);

	void Ready(const FString& InSessionId);
	void TurnStarted();
	void TurnFinished();
	TSharedPtr<FHyperAIStudioChatItem> FindItem(const FString& Id) const;
	TSharedRef<FHyperAIStudioChatItem> AddItem(FHyperAIStudioChatItem::EKind Kind, const FString& Id, const FString& Title,
		const FString& Text, const FString& Status = FString());
	/** An approval the user answers later with Answer. */
	void AskApproval(const TSharedPtr<FJsonValue>& RequestId, const FString& Id, const FString& Title, const FString& Text,
		const TArray<FString>& Choices, const TArray<FString>& ChoiceIds);
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

	bool Pump(float DeltaTime);
	void Dispatch(const FString& Line);
	void Write(const TSharedRef<FJsonObject>& Message);
	void ClosePipes();

	FProcHandle Process;
	void* StdOutRead = nullptr;
	void* StdOutWrite = nullptr;
	void* StdInRead = nullptr;
	void* StdInWrite = nullptr;
	TArray<uint8> Pending;
	int64 NextId = 1;
	TMap<int64, FOnResult> Waiting;
	TMap<FString, TSharedPtr<FJsonValue>> OpenApprovals;
	TArray<TSharedPtr<FHyperAIStudioChatItem>> Items;
	TArray<FQueuedMessage> Queued;
	FTSTicker::FDelegateHandle Ticker;
	bool bReady = false;
	bool bBusy = false;
	bool bStopped = false;
};
