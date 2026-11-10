// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioMotionWarpingToolset.generated.h"

/**
 * One op of a motion warping plan, in order. Window ops take a montage or an animation sequence, component ops a
 * character Blueprint. Times are seconds from the start of the asset. Params are "Name=Value; Name=Value", the settings
 * of the window's modifier or of the component; hyper_motion_warping_inspect lists them with their types and defaults.
 */
USTRUCT(BlueprintType)
struct FHyperAIMotionWarpingOp
{
	GENERATED_BODY()

	/**
	 * window.add (a Motion Warping notify state from StartSeconds to EndSeconds), window.set (change a window's times,
	 * modifier or settings), window.remove, component.add (a Motion Warping component on a character Blueprint),
	 * component.set (that component's settings).
	 */
	UPROPERTY() FString Kind;
	/** window.set and window.remove: the window by the Index inspect gave it before the plan; -1 picks it by WarpTargetName. */
	UPROPERTY() int32 Index = -1;
	/**
	 * window.add: the warp target the window warps to, the name game code sets with AddOrUpdateWarpTarget...; window.set
	 * and window.remove with Index -1: the window to change, by its warp target name. To rename one, give
	 * Params "WarpTargetName=NewName".
	 */
	UPROPERTY() FString WarpTargetName;
	/** window.add: where the window starts, seconds; window.set: its new start, -1 to keep it. */
	UPROPERTY() double StartSeconds = -1.0;
	/** window.add: where the window ends, seconds; window.set: its new end, -1 to keep it. */
	UPROPERTY() double EndSeconds = -1.0;
	/**
	 * window.add: how the window bends root motion: skew_warp (the default: translation and rotation bent toward the warp
	 * target), scale (translation scaled by Params Scale=x,y,z; no warp target), adjustment_blend (experimental),
	 * precomputed_warp (experimental, for targets that do not move). window.set: a new modifier for the window, its
	 * warp target kept; empty keeps the one it has.
	 */
	UPROPERTY() FString Modifier;
	/** window.add: the notify track by name, made when missing; empty puts it on the first track. */
	UPROPERTY() FString Track;
	/** component.add: the component's variable name, MotionWarping when empty. */
	UPROPERTY() FString ComponentName;
	/** "Name=Value; Name=Value": the modifier's settings (window ops) or the component's (component ops). */
	UPROPERTY() FString Params;
};

USTRUCT(BlueprintType)
struct FHyperAIMotionWarpingPlanRequest
{
	GENERATED_BODY()

	/** The montage, animation sequence or character Blueprint the plan changes, under /Game: /Game/Anims/AM_Vault. */
	UPROPERTY() FString Asset;
	UPROPERTY() TArray<FHyperAIMotionWarpingOp> Operations;
	/** Check the plan and report what would change; nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Save the asset. Off, nothing is saved and one undo takes the whole plan back. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

/** One Motion Warping window: a notify state on the asset's notify tracks. */
USTRUCT(BlueprintType)
struct FHyperAIMotionWarpingWindow
{
	GENERATED_BODY()

	/** Its place among the asset's windows, by start time; window.set and window.remove pick it by this. */
	UPROPERTY() int32 Index = 0;
	UPROPERTY() FString Track;
	UPROPERTY() float StartSeconds = 0.f;
	UPROPERTY() float EndSeconds = 0.f;
	/** The warp target game code must set before the montage reaches the window; empty for scale. */
	UPROPERTY() FString WarpTargetName;
	/** skew_warp, scale, adjustment_blend, precomputed_warp, or the class name of another modifier; none when it has none. */
	UPROPERTY() FString Modifier;
	UPROPERTY() FString ModifierClass;
	/** The modifier's settings, "Name=Value" each (bWarpTranslation, bIgnoreZAxis, bWarpRotation, RotationType...). */
	UPROPERTY() TArray<FString> Settings;
};

/** A kind a plan takes, or a window modifier, with its settings. */
USTRUCT(BlueprintType)
struct FHyperAIMotionWarpingKind
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString Summary;
	/** "Name: type, default value" for each setting Params takes. */
	UPROPERTY() TArray<FString> Settings;
};

USTRUCT(BlueprintType)
struct FHyperAIMotionWarpingPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, approval_not_queued, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Asset;
	/** montage, sequence or character_blueprint. */
	UPROPERTY() FString AssetKind;
	/** A montage's or sequence's length, seconds. */
	UPROPERTY() float LengthSeconds = 0.f;
	/** The windows after the plan (a dry run: as they are now), as inspect lists them. */
	UPROPERTY() TArray<FHyperAIMotionWarpingWindow> Windows;
	/** The warp target names the windows use: the names game code sets. */
	UPROPERTY() TArray<FString> WarpTargets;
	/** A character Blueprint: whether it has a Motion Warping component, its name and settings. */
	UPROPERTY() bool bHasComponent = false;
	UPROPERTY() FString ComponentName;
	UPROPERTY() TArray<FString> ComponentSettings;
	/** What each op did (a dry run: would do). */
	UPROPERTY() TArray<FString> Notes;
	/** What would stop warping in play: no root motion, a window past the end, a window without a warp target... */
	UPROPERTY() TArray<FString> Warnings;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIMotionWarpingInspectRequest
{
	GENERATED_BODY()

	/** A montage, animation sequence or character Blueprint by path. Empty: the op kinds, the modifiers and their settings, and the montages under /Game with warp windows. */
	UPROPERTY() FString Asset;
};

USTRUCT(BlueprintType)
struct FHyperAIMotionWarpingInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Asset;
	/** montage, sequence or character_blueprint. */
	UPROPERTY() FString AssetKind;
	UPROPERTY() float LengthSeconds = 0.f;
	/** Whether the montage's sequences (or the sequence) have root motion on; warping bends root motion, so it needs it. */
	UPROPERTY() bool bRootMotion = false;
	UPROPERTY() TArray<FHyperAIMotionWarpingWindow> Windows;
	UPROPERTY() TArray<FString> WarpTargets;
	/** A character Blueprint: whether it has a Motion Warping component (its own or inherited), its name, where it comes from and its settings. */
	UPROPERTY() bool bHasComponent = false;
	UPROPERTY() FString ComponentName;
	/** "this Blueprint", the parent Blueprint's path, or the native class it comes from. */
	UPROPERTY() FString ComponentSource;
	UPROPERTY() TArray<FString> ComponentSettings;
	UPROPERTY() TArray<FString> Warnings;
	/** Without Asset: the op kinds hyper_motion_warping_apply_plan takes, the window modifiers with their settings, and the montages under /Game with warp windows. */
	UPROPERTY() TArray<FHyperAIMotionWarpingKind> Kinds;
	UPROPERTY() TArray<FHyperAIMotionWarpingKind> Modifiers;
	UPROPERTY() TArray<FString> Assets;
};

/**
 * Motion Warping: warp windows (Motion Warping notify states with a skew warp, scale or other root motion modifier) on
 * montages and sequences, and the Motion Warping component on character Blueprints, in one undo step, so a vault, a
 * mantle or an attack lunge bends its root motion to a target the game sets at run time.
 */
UCLASS()
class HYPERAISTUDIOMOTIONWARPING_API UHyperAIStudioMotionWarpingToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Read only. A montage's or sequence's Motion Warping windows: index, track, start and end seconds, warp target name,
	 * the modifier and its settings, whether root motion is on, and warnings; or a character Blueprint's Motion Warping
	 * component (there or not, its settings). Without Asset: the op kinds hyper_motion_warping_apply_plan takes, the
	 * window modifiers with their settings and defaults, and the montages under /Game with warp windows.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIMotionWarpingInspectReport hyper_motion_warping_inspect(const FHyperAIMotionWarpingInspectRequest& Request);

	/**
	 * Changes one asset's motion warping in one plan, one undo step. On a montage or sequence: window.add (a warp window
	 * from StartSeconds to EndSeconds to WarpTargetName, with a skew_warp, scale, adjustment_blend or precomputed_warp
	 * modifier and its Params such as bIgnoreZAxis=false; RotationType=Facing), window.set (times, modifier or Params of
	 * a window by Index or WarpTargetName), window.remove. On a character Blueprint: component.add (the Motion Warping
	 * component, compiled) and component.set. The reply lists the windows after the plan and warns about missing root
	 * motion, windows past the end and warp targets no window uses. bDryRun checks first.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIMotionWarpingPlanReport hyper_motion_warping_apply_plan(const FHyperAIMotionWarpingPlanRequest& Request);
};

class FHyperAIStudioMotionWarpingContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("animation_rigging");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiomotionwarpingtoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_motion_warping_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_motion_warping_apply_plan");
	static constexpr int32 MaxOperations = 32;
	/** Montages listed by inspect without an asset. */
	static constexpr int32 MaxListedAssets = 200;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIMotionWarpingInspectReport Inspect(const FHyperAIMotionWarpingInspectRequest& Request);
	static FHyperAIMotionWarpingPlanReport ApplyPlan(const FHyperAIMotionWarpingPlanRequest& Request);
};

class FHyperAIStudioMotionWarpingRegistration final
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
