// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioPoseSearchToolset.generated.h"

/** One setting by the engine's property name, in Unreal's text form: SampleRate 60, Weight 2.5, Bone (BoneName="hand_r"). */
USTRUCT(BlueprintType)
struct FHyperAIPoseSearchSetting
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Value;
};

USTRUCT(BlueprintType)
struct FHyperAIPoseSearchOp
{
	GENERATED_BODY()

	/**
	 * schema.create, schema.set, schema.add_channel, schema.set_channel, schema.remove_channel; database.create, database.set,
	 * database.add_animations, database.set_animations, database.remove_animations, database.build.
	 */
	UPROPERTY() FString Kind;
	/** The schema or database the op makes or edits, such as /Game/MotionMatching/PSS_Hero or /Game/MotionMatching/PSD_Locomotion. */
	UPROPERTY() FString Asset;
	/** schema.create: the skeleton; an optional mirror data table; Epic's default channels (trajectory, and both feet) unless false. */
	UPROPERTY() FString Skeleton;
	UPROPERTY() FString MirrorTable;
	UPROPERTY() bool bDefaultChannels = true;
	/** database.create: the schema it indexes with, made earlier in the plan or on disk. */
	UPROPERTY() FString Schema;
	/**
	 * schema.add_channel: the channel type, such as Trajectory, Pose, Position, Velocity, Heading, Phase, Curve, Distance,
	 * TimeToEvent, SamplingTime, Padding or FilterCrashingLegs.
	 */
	UPROPERTY() FString Channel;
	/** schema.set_channel, schema.remove_channel: the channel's index, as hyper_pose_search_inspect lists them. */
	UPROPERTY() int32 ChannelIndex = -1;
	/**
	 * Settings by the engine's property names, as hyper_pose_search_inspect lists them: the schema's for schema.create and
	 * schema.set (SampleRate, DataPreprocessor...), the channel's for add_channel and set_channel (Weight, Bone, SampledBones,
	 * Samples...), the database's for database.create and database.set (Tags, PoseSearchMode, ContinuingPoseCostBias,
	 * PreviewMesh...), and each animation's for add_animations and set_animations (MirrorOption, SamplingRange, bEnabled,
	 * bDisableReselection...).
	 */
	UPROPERTY() TArray<FHyperAIPoseSearchSetting> Settings;
	/** database.add_animations, set_animations, remove_animations: sequences, blend spaces, montages or composites. */
	UPROPERTY() TArray<FString> Animations;
};

USTRUCT(BlueprintType)
struct FHyperAIPoseSearchPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FHyperAIPoseSearchOp> Operations;
	/** Only check the plan: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIPoseSearchPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, checked, awaiting_user_approval, invalid_request, operation_failed, build_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() int32 Applied = 0;
	/** Each schema and database the plan made or changed, read back. */
	UPROPERTY() TArray<FString> Made;
	/** Each database database.build indexed, with its pose count. */
	UPROPERTY() TArray<FString> Built;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIPoseSearchInspectRequest
{
	GENERATED_BODY()

	/** Pose Search schemas and databases. */
	UPROPERTY() TArray<FString> Paths;
};

/** One schema or database. */
USTRUCT(BlueprintType)
struct FHyperAIPoseSearchAssetInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Path;
	/** schema or database. */
	UPROPERTY() FString Kind;
	/** A schema's skeletons (with their mirror tables) and its channels, each with its settings. */
	UPROPERTY() TArray<FString> Skeletons;
	UPROPERTY() TArray<FString> Channels;
	/** A database's schema, and its animations, each with its settings. */
	UPROPERTY() FString Schema;
	UPROPERTY() TArray<FString> Animations;
	/** The asset's own settings as Name = Value, the names schema.set and database.set take. */
	UPROPERTY() TArray<FString> Settings;
	/** A database's search index: built with its pose count, building (as when the database is opened), or failed. */
	UPROPERTY() FString Index;
	/** Problems: bones the channels sample that the skeleton lacks, animations of another skeleton. */
	UPROPERTY() TArray<FString> Notes;
};

USTRUCT(BlueprintType)
struct FHyperAIPoseSearchInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAIPoseSearchAssetInfo> Assets;
};

/**
 * Motion matching data: Pose Search schemas (the skeleton, sample rate and the channels a pose is matched on) and databases
 * (the animations searched, with their mirroring and sampling ranges), built into a search index. Each plan is one undo step,
 * then saved.
 */
UCLASS()
class HYPERAISTUDIOPOSESEARCH_API UHyperAIStudioPoseSearchToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Pose Search schemas (skeletons, sample rate, preprocessing, each channel with its settings) and databases (schema, each
	 * animation with its mirroring, sampling range and flags, tags, search mode and cost biases, and its search index: built
	 * with its pose count, or building), with notes for bones a channel samples that the skeleton lacks.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIPoseSearchInspectReport hyper_pose_search_inspect(const FHyperAIPoseSearchInspectRequest& Request);

	/**
	 * Builds motion matching data in one plan: a schema for a skeleton with Epic's default channels (trajectory, foot
	 * positions and velocities) or channels added and tuned one by one; a database on that schema with animations added,
	 * tuned (mirroring, sampling range, enabled) or removed; database settings such as tags, search mode and cost biases; then
	 * database.build indexes it and reports its pose count. Settings use the engine's property names, as
	 * hyper_pose_search_inspect lists them.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIPoseSearchPlanReport hyper_pose_search_apply_plan(const FHyperAIPoseSearchPlanRequest& Request);
};

class FHyperAIStudioPoseSearchContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("animation_rigging");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioposesearchtoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_pose_search_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_pose_search_apply_plan");
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxAnimations = 512;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIPoseSearchInspectReport Inspect(const FHyperAIPoseSearchInspectRequest& Request);
	static FHyperAIPoseSearchPlanReport ApplyPlan(const FHyperAIPoseSearchPlanRequest& Request);
};

class FHyperAIStudioPoseSearchRegistration final
{
public:
	void Startup();
	void Shutdown();

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
