// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAgentJobs.h"
#include "HyperAIStudioLightingToolset.h"
#include "HyperAIStudioMatchStudies.h"
#include "HyperAIStudioPreviewPanel.h"

class UWorld;

/**
 * Match it: a reference image handed to the preferred agent, which changes the level until hyper_image_compare scores
 * it at the target. HyperAI reads the match session after each turn and sends the agent back to work with what still
 * differs, until the target, two rounds without a gain, or the round limit. When the job ends, HyperAI keeps a study of
 * it (Studies), and the next job starts from the studies that bear on it.
 */
namespace HyperAIStudio::Lighting::MatchJobs
{
	enum class EGoal : uint8
	{
		/** Light the level the way the reference is lit. */
		Lighting,
		/** Build the level toward the reference: layout, shapes and materials, then light. */
		Level
	};

	/** Look mode against concept art or a photo, which no render reaches pixel for pixel. */
	constexpr float LightingTarget = 78.f;
	constexpr float LevelTarget = 72.f;
	constexpr int32 MaxFollowUps = 6;

	/** What Match it offers on the lighting and image match sources. */
	TArray<FHyperAIStudioPreviewPanel::FAgentAction> Actions();
	/** A new match session, a chat on the Lighting or Level Design route with the reference attached, and the job that watches it. */
	bool Start(EGoal Goal, const FString& ReferenceImagePath, FString& OutError);

	/** The camera a reference made from the level is taken through, and every attempt at matching it. */
	inline const TCHAR* const MatchCameraLabel = TEXT("HyperAI_MatchCamera");
	/**
	 * The view the Match jobs work on, as a picture for a reference to be made from, such as the level relit: HyperAI_MatchCamera
	 * at Size, placed where the level viewport looks when the level has none (one undo step). OutImagePath is project-relative.
	 */
	bool CaptureMatchView(const FIntPoint& Size, FString& OutImagePath, FString& OutError);

	// Pure pieces, exposed for tests.
	/** How this level's Blueprint sky is lit, for the prompt; empty without one. */
	FString SkyNote(UWorld& World);
	/**
	 * When an image model made the reference from a capture of this level, as Relight this view does, how to capture that same
	 * view for every attempt, from the capture.json beside the picture it changed; empty otherwise.
	 */
	FString SameViewNote(const FString& ReferenceImagePath);
	/** Lessons are what earlier jobs' studies teach this one (Studies::Lessons), empty without any. */
	FString BuildPrompt(EGoal Goal, const FString& ReferenceProjectPath, const FString& SessionId, float Target, const FString& SkyNote,
		const FString& SameView = FString(), const FString& Lessons = FString());

	struct FRoundState
	{
		float BestBefore = -1.f;
		int32 AttemptsBefore = 0;
		/** Rounds in a row that gained less than a point. */
		int32 FlatRounds = 0;
		/** Rounds in a row in which no attempt was scored. */
		int32 EmptyRounds = 0;
	};
	/**
	 * The verdict after a turn from the session's history. Weakest and Suggestions describe its best attempt. NextRound
	 * numbers the round the message would start, which also says what raised and what lowered the score so far.
	 */
	FHyperAIStudioAgentJobVerdict Judge(FRoundState& State, EGoal Goal, const FString& SessionId, float Target, int32 NextRound,
		const TArray<FHyperAIImageIteration>& History, const TArray<FString>& Weakest, const TArray<FString>& Suggestions);
	/**
	 * The study of an ended job, from its session's history: how it ended, what raised and lowered the score, where the
	 * best attempt still differed and what was suggested next. Unset when no attempt was scored.
	 */
	TOptional<Studies::FStudy> StudyOf(EGoal Goal, const FString& SessionId, const FString& ReferenceProjectPath, const FString& Level, float Target,
		const FHyperAIStudioAgentJobInfo& Job, const TArray<FHyperAIImageIteration>& History, const TArray<FString>& Weakest, const TArray<FString>& Suggestions);
}
