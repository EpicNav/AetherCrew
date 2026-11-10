// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioSequencerToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAISequenceKey
{
	GENERATED_BODY()

	/** Seconds from the sequence's start. */
	UPROPERTY() float Time = 0.f;
	/** keys.transform: where the actor stands (cm) and how it is turned. */
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;
	/** keys.property, fade.add: the value (a fade runs from 0, clear, to 1, black). */
	UPROPERTY() float Value = 0.f;
	/** auto (smooth, the default), linear, or constant (holds until the next key). */
	UPROPERTY() FString Interp;
};

USTRUCT(BlueprintType)
struct FHyperAISequenceOp
{
	GENERATED_BODY()

	/**
	 * sequence.create, sequence.set, actors.bind, camera.add, keys.transform, keys.property, cut.add, fade.add, audio.add,
	 * shot.frame, shot.add.
	 */
	UPROPERTY() FString Kind;
	/** sequence.create, sequence.set: frames per second (24 by default; 23.976, 25, 29.97, 30, 48, 50, 59.94, 60) and length in seconds. */
	UPROPERTY() float FrameRate = 0.f;
	/** sequence.create, sequence.set: the sequence's length; shot.frame: the shot's (3 by default); shot.add: the shot's, its own length when 0. Seconds. */
	UPROPERTY() float Duration = 0.f;
	/** actors.bind: loaded actors by label, for the sequence to animate. */
	UPROPERTY() TArray<FString> Actors;
	/** camera.add, cut.add, shot.frame: a cine camera by label; camera.add and shot.frame place a new one when none has it. */
	UPROPERTY() FString Camera;
	/** keys.transform, keys.property: the actor by label, bound to the sequence when it is not yet. */
	UPROPERTY() FString Actor;
	/**
	 * keys.property: a float, double or bool property of the actor, or of one of its components by name or class:
	 * CineCameraComponent.CurrentFocalLength, CineCameraComponent.FocusSettings.ManualFocusDistance, LightComponent.Intensity.
	 */
	UPROPERTY() FString Property;
	UPROPERTY() TArray<FHyperAISequenceKey> Keys;
	/** camera.add: where it stands (cm) and turns; LookAt aims it at an actor instead of Rotation. */
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;
	UPROPERTY() FString LookAt;
	/**
	 * camera.add, shot.frame: the lens in mm (shot.frame picks one for its framing), the f-stop, a filmback preset
	 * (16:9 Digital Film, Super 35mm, IMAX 70mm...), or an AspectRatio such as 2.39 for a letterboxed widescreen frame.
	 */
	UPROPERTY() float FocalLength = 0.f;
	UPROPERTY() float Aperture = 0.f;
	UPROPERTY() FString Filmback;
	UPROPERTY() float AspectRatio = 0.f;
	/** camera.add, shot.frame: keeps focus on this actor; shot.frame focuses on its first subject by default. */
	UPROPERTY() FString FocusOn;
	/** cut.add, audio.add, shot.frame, shot.add: when it starts, seconds. */
	UPROPERTY() float Time = 0.f;
	/**
	 * shot.frame: Subjects (one, or two for a two-shot) framed as Framing (extreme_wide, wide, full, medium_wide, medium,
	 * medium_close, close, extreme_close), from Angle (eye, high, low, overhead, dutch) and Side (front,
	 * three_quarter_left, three_quarter_right, profile_left, profile_right, back), or over the shoulder of Over.
	 */
	UPROPERTY() TArray<FString> Subjects;
	UPROPERTY() FString Framing;
	UPROPERTY() FString Angle;
	UPROPERTY() FString Side;
	UPROPERTY() FString Over;
	/**
	 * shot.frame: static, push_in, pull_out, truck_left, truck_right, pedestal_up, pedestal_down, orbit_left, orbit_right,
	 * crane_up, crane_down, or dolly_zoom; eased in and out. bHandheld adds a hand-held drift.
	 */
	UPROPERTY() FString Move;
	UPROPERTY() bool bHandheld = false;
	/** audio.add: the sound; shot.add: the shot's level sequence. */
	UPROPERTY() FString Asset;
};

USTRUCT(BlueprintType)
struct FHyperAISequencePlanRequest
{
	GENERATED_BODY()

	/** The level sequence the plan edits (/Game/Cinematics/LS_Intro); sequence.create makes it. */
	UPROPERTY() FString Sequence;
	UPROPERTY() TArray<FHyperAISequenceOp> Operations;
	/** Only check the plan: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Save the level too (cameras the plan placed); the sequence is always saved. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAISequencePlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, checked, awaiting_user_approval, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() int32 Applied = 0;
	/** What each op did: a shot's camera, lens, distance and move. */
	UPROPERTY() TArray<FString> Notes;
	/** The edit as a film editor would review it: jump cuts, crossing the line, hidden subjects, gaps with no camera. */
	UPROPERTY() TArray<FString> Review;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAISequenceInspectRequest
{
	GENERATED_BODY()

	/** The level sequence to read; every level sequence under /Game is listed when empty. */
	UPROPERTY() FString Sequence;
};

USTRUCT(BlueprintType)
struct FHyperAISequenceBindingInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** The actor or component class. */
	UPROPERTY() FString Class;
	/** Each track: its kind, what it animates, its keys and when they fall. */
	UPROPERTY() TArray<FString> Tracks;
};

USTRUCT(BlueprintType)
struct FHyperAISequenceInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	/** With no Sequence asked for: every level sequence under /Game (the first 100). */
	UPROPERTY() TArray<FString> Sequences;
	UPROPERTY() float FrameRate = 0.f;
	/** Playback range, seconds. */
	UPROPERTY() float Start = 0.f;
	UPROPERTY() float Duration = 0.f;
	UPROPERTY() TArray<FHyperAISequenceBindingInfo> Bindings;
	/** The edit: when each camera cut starts and ends, and its camera. */
	UPROPERTY() TArray<FString> Cuts;
	/** A master sequence's shots: when each plays, and its sequence. */
	UPROPERTY() TArray<FString> Shots;
	/** Fades, audio and other tracks of the sequence itself. */
	UPROPERTY() TArray<FString> Tracks;
	/** The edit as a film editor would review it. */
	UPROPERTY() TArray<FString> Review;
};

/**
 * Sequencer: level sequences made and cut like a film. Cine cameras placed and framed by shot language (framing, angle,
 * side, lens, camera move), camera cuts, keyed actors and properties, fades, audio, and shots in a master sequence.
 * Each plan is one undo step.
 */
UCLASS()
class HYPERAISTUDIOSEQUENCER_API UHyperAIStudioSequencerToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * A level sequence: frame rate, length, bound actors and their tracks and keys, the camera cuts, a master
	 * sequence's shots, fades and audio, and a film editor's review of the cut. Lists the level sequences when none is named.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Cinematics")
	static FHyperAISequenceInspectReport hyper_sequence_inspect(const FHyperAISequenceInspectRequest& Request);

	/**
	 * Builds and cuts a level sequence in one plan, one undo step: shot.frame places a cine camera and frames its subject
	 * by shot language (close, three-quarter, low angle, 85mm, slow push in) with focus and a camera cut; plus cameras,
	 * transform and property keys, cuts, fades, audio, and shots in a master sequence. The reply reviews the cut.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Cinematics")
	static FHyperAISequencePlanReport hyper_sequence_apply_plan(const FHyperAISequencePlanRequest& Request);
};

class FHyperAIStudioSequencerContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("cinematics");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiosequencertoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_sequence_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_sequence_apply_plan");
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxKeysPerOp = 2000;
	/** Seconds: a sequence no longer than two hours. */
	static constexpr float MaxSeconds = 7200.f;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAISequenceInspectReport Inspect(const FHyperAISequenceInspectRequest& Request);
	static FHyperAISequencePlanReport ApplyPlan(const FHyperAISequencePlanRequest& Request);
};

class FHyperAIStudioSequencerRegistration final
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
