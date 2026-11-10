// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioRetargetToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIRetargetOp
{
	GENERATED_BODY()

	/**
	 * ik_rig.create, ik_rig.auto_chains, ik_rig.auto_full_body_ik, ik_rig.add_chain, ik_rig.set_chain, ik_rig.remove_chain,
	 * ik_rig.set_root; retargeter.create, retargeter.map_chain, retargeter.auto_map, retargeter.auto_align,
	 * retargeter.enable_op; retarget.animations.
	 */
	UPROPERTY() FString Kind;
	/** The IK Rig or IK Retargeter the op makes or edits, such as /Game/Retarget/IK_Hero; for retarget.animations, the retargeter. */
	UPROPERTY() FString Asset;
	/** ik_rig.create: the skeletal mesh it rigs. */
	UPROPERTY() FString Mesh;
	/** ik_rig.create: name the skeleton's chains (true unless set false), and set up full body IK (false unless true). */
	UPROPERTY() bool bAutoChains = true;
	UPROPERTY() bool bAutoFullBodyIK = false;
	/** ik_rig.add_chain, set_chain, remove_chain: the chain; its first and last bone, and an optional IK goal name. */
	UPROPERTY() FString Chain;
	UPROPERTY() FString StartBone;
	UPROPERTY() FString EndBone;
	UPROPERTY() FString Goal;
	/** ik_rig.set_root: the retarget root bone, usually the pelvis. */
	UPROPERTY() FString Bone;
	/** retargeter.create: the IK Rigs animation comes from and goes to; optional preview meshes (each rig's own when empty). */
	UPROPERTY() FString SourceRig;
	UPROPERTY() FString TargetRig;
	UPROPERTY() FString SourceMesh;
	UPROPERTY() FString TargetMesh;
	/** retargeter.map_chain: the target rig's chain and the source chain it copies (empty leaves it unmapped). */
	UPROPERTY() FString TargetChain;
	UPROPERTY() FString SourceChain;
	/** retargeter.auto_map: exact (the default: same names), fuzzy (closest names, for chains named differently), or clear. */
	UPROPERTY() FString Mode;
	/** retargeter.enable_op: the op, by name or type as hyper_retarget_inspect lists it (such as FIKRetargetRunIKRigOp), on or off. */
	UPROPERTY() FString OpName;
	UPROPERTY() bool bEnabled = true;
	/**
	 * retarget.animations: sequences, blend spaces, montages, composites and pose assets to copy onto the target; with
	 * SourceMesh and TargetMesh (the retargeter's preview meshes when empty), OutputFolder (/Game/...; beside each source
	 * when empty), and Prefix, Suffix, Search and Replace for the new names.
	 */
	UPROPERTY() TArray<FString> Animations;
	UPROPERTY() FString OutputFolder;
	UPROPERTY() FString Prefix;
	UPROPERTY() FString Suffix;
	UPROPERTY() FString Search;
	UPROPERTY() FString Replace;
	/**
	 * retarget.animations: also copy the animations a montage or blend space uses (true unless false); and replace assets
	 * that already have the new names, pointing their users at the new ones (false unless true).
	 */
	UPROPERTY() bool bIncludeReferenced = true;
	UPROPERTY() bool bOverwrite = false;
};

USTRUCT(BlueprintType)
struct FHyperAIRetargetPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FHyperAIRetargetOp> Operations;
	/** Only check the plan: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIRetargetPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, checked, awaiting_user_approval, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() int32 Applied = 0;
	/** Each IK Rig and retargeter the plan made or changed, read back: chains, root, goals, mapping. */
	UPROPERTY() TArray<FString> Made;
	/** The animations retarget.animations made. */
	UPROPERTY() TArray<FString> Retargeted;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIRetargetInspectRequest
{
	GENERATED_BODY()

	/** IK Rigs, IK Retargeters or skeletal meshes. */
	UPROPERTY() TArray<FString> Paths;
};

/** One IK Rig, retargeter or skeletal mesh. */
USTRUCT(BlueprintType)
struct FHyperAIRetargetAssetInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Path;
	/** ik_rig, retargeter or skeletal_mesh. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Mesh;
	UPROPERTY() FString Skeleton;
	UPROPERTY() int32 BoneCount = 0;
	/** An IK Rig's mesh as Epic's auto-characterizer sees it: the known skeleton it matches, which automatic chains follow. */
	UPROPERTY() FString Template;
	/** An IK Rig's retarget root, its chains as Name: start -> end (goal), its goals, and how many solvers it has. */
	UPROPERTY() FString RetargetRoot;
	UPROPERTY() TArray<FString> Chains;
	UPROPERTY() TArray<FString> Goals;
	UPROPERTY() int32 SolverCount = 0;
	/** A retargeter's rigs and preview meshes, its ops, and each target chain with the source chain it copies. */
	UPROPERTY() FString SourceRig;
	UPROPERTY() FString TargetRig;
	UPROPERTY() FString SourceMesh;
	UPROPERTY() FString TargetMesh;
	UPROPERTY() TArray<FString> Ops;
	UPROPERTY() TArray<FString> ChainMap;
};

USTRUCT(BlueprintType)
struct FHyperAIRetargetInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAIRetargetAssetInfo> Assets;
};

/**
 * Retargeting animation from one character to another: IK Rigs with their chains and root, IK Retargeters that map the chains
 * and align the poses, and batch retargeting of sequences, blend spaces and montages. Each plan is one undo step for the
 * rigs and retargeters, then saved; retargeted animations are new assets.
 */
UCLASS()
class HYPERAISTUDIORETARGET_API UHyperAIStudioRetargetToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * IK Rigs (skeletal mesh, retarget root, chains with their bones and goals, solvers), IK Retargeters (source and target
	 * rigs and meshes, ops, which source chain each target chain copies) and skeletal meshes (skeleton, bone count).
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIRetargetInspectReport hyper_retarget_inspect(const FHyperAIRetargetInspectRequest& Request);

	/**
	 * Sets up retargeting and runs it in one plan: IK Rigs for a skeletal mesh with their chains named automatically for
	 * known skeletons (UE5 and UE4 mannequins, MetaHumans, Mixamo...) or by hand, a retarget root and full body IK; an IK
	 * Retargeter between two rigs set up the way Epic's auto-retarget does it (default ops, chains mapped by name, target
	 * pose aligned with the feet kept flat, root motion and pinned bones configured, the IK pass off until the rigs' IK is
	 * tuned); then retarget.animations copies sequences, blend spaces and montages onto the target character. The report
	 * reads each rig and retargeter back and lists the new animations.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIRetargetPlanReport hyper_retarget_apply_plan(const FHyperAIRetargetPlanRequest& Request);
};

class FHyperAIStudioRetargetContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("animation_rigging");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioretargettoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_retarget_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_retarget_apply_plan");
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxAnimations = 512;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIRetargetInspectReport Inspect(const FHyperAIRetargetInspectRequest& Request);
	static FHyperAIRetargetPlanReport ApplyPlan(const FHyperAIRetargetPlanRequest& Request);
};

class FHyperAIStudioRetargetRegistration final
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
