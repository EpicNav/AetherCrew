// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

enum class EHyperAIStudioAgentJobState : uint8
{
	/** The chat tab is open and its agent is starting. */
	Starting,
	Working,
	/** The owner judged the goal reached. */
	Succeeded,
	/** Stopped by the user, the owner, or the turn limit, before the goal. */
	Stopped,
	/** The chat could not do the job: the tab closed, or no turn made progress. */
	Failed
};

/** What a job's owner decides after each turn the agent finishes. */
struct FHyperAIStudioAgentJobVerdict
{
	/** The next message to send; empty ends the job. */
	FString NextMessage;
	/** One line for the job's status, such as "best 71 of 80 after 9 attempts". */
	FString Status;
	/** The job reached what it was for. */
	bool bSucceeded = false;
};

/**
 * A task an agent does in its own chat tab while HyperAI watches: after each turn the owner looks at the result and
 * either sends the agent back to work or ends the job. That is how "keep going until it matches" holds even for an
 * agent that would stop early.
 */
struct FHyperAIStudioAgentJobSpec
{
	/** Shown on the job, such as "Match the lighting to dusk.png". */
	FString Title;
	/** A task route (Agent Workflow > Routing), such as Lighting or Level Design: it picks the agent and model. Empty or unknown uses the preferred agent. */
	FString Category;
	FString Prompt;
	/** Files the agent sees with the first message in HyperAI's chat view; a terminal chat gets their paths in the text. */
	TArray<FString> Images;
	/** Lets the owner find its job again, such as the reference image's path. */
	FString Key;
	/** Messages after the first; the job stops there even when the goal is not reached. */
	int32 MaxFollowUps = 6;
	/**
	 * Packs whose plain edits are approved for the user while the job runs: starting the job asked for that work.
	 * Destructive and external-effect edits still wait for a click.
	 */
	TArray<FString> AutoApprovePacks;
	/** Called on the game thread after each finished turn, with how many turns are done. Unbound: the job ends after one turn. */
	TFunction<FHyperAIStudioAgentJobVerdict(int32 /*TurnsDone*/)> OnTurnEnded;
	/** Called once the chat's agent has launched: the chat tab may go behind another page from then on, such as the one following the job. */
	TFunction<void()> OnStarted;
};

struct FHyperAIStudioAgentJobInfo
{
	int32 Id = 0;
	FString Title;
	FString Category;
	FString Key;
	/** The chat tab doing the work. */
	int32 TabIndex = 0;
	FString Agent;
	/** Finished turns. */
	int32 Turns = 0;
	EHyperAIStudioAgentJobState State = EHyperAIStudioAgentJobState::Starting;
	FString Status;

	bool IsRunning() const { return State == EHyperAIStudioAgentJobState::Starting || State == EHyperAIStudioAgentJobState::Working; }
};

/** Supervised agent jobs. Game thread only. */
class HYPERAISTUDIO_API FHyperAIStudioAgentJobs
{
public:
	/** Opens a free chat tab for the job and sends it the prompt. Returns the job's id, or 0 with OutError set. */
	static int32 Start(FHyperAIStudioAgentJobSpec Spec, FString& OutError);
	/** Stops supervising, and interrupts the agent's turn in HyperAI's chat view. */
	static void Stop(int32 Id, const FString& Why = FString());
	/** Newest first. */
	static TArray<FHyperAIStudioAgentJobInfo> List();
	/** The newest job with this key, if any. */
	static TOptional<FHyperAIStudioAgentJobInfo> FindByKey(const FString& Key);
	/** Brings the job's chat tab forward. */
	static void ShowChat(int32 Id);
	/** A job started, changed state or status, or ended. */
	static FSimpleMulticastDelegate& OnChanged();
};
