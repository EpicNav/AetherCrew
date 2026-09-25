// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolCallAsyncResult.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioLightingToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAILightingProperty
{
	GENERATED_BODY()

	UPROPERTY() FString Key;
	UPROPERTY() FString Value;
};

/** One lighting actor in the level and the settings the apply ops can change. */
USTRUCT(BlueprintType)
struct FHyperAILightingActorRecord
{
	GENERATED_BODY()

	/** sun, sky_light, fog, sky_atmosphere or post_process. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Label;
	UPROPERTY() FString Path;
	UPROPERTY() TArray<FHyperAILightingProperty> Properties;
};

USTRUCT(BlueprintType)
struct FHyperAILightingInspectRequest
{
	GENERATED_BODY()

	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAILightingInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	/** The level open in the editor; apply_plan edits only this level. */
	UPROPERTY() FString LevelPath;
	/** Pass to apply_plan as expected_revision. */
	UPROPERTY() FString Revision;
	/** False means auto exposure is on, so screenshots cannot be compared fairly; lock it first. */
	UPROPERTY() bool bExposureLocked = false;
	UPROPERTY() TArray<FHyperAILightingActorRecord> Actors;
	UPROPERTY() TArray<FString> Capabilities;
};

/** Measurements of one image, in display-referred terms both a photo and a viewport capture share. */
USTRUCT(BlueprintType)
struct FHyperAILightingImageMetrics
{
	GENERATED_BODY()

	UPROPERTY() bool bValid = false;
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
	/** Linear luminance percentiles, 0-1. */
	UPROPERTY() float LuminanceP5 = 0.f;
	UPROPERTY() float LuminanceP50 = 0.f;
	UPROPERTY() float LuminanceP95 = 0.f;
	/** Brightest over darkest tones (p95 / p5): higher is harder, more contrasty light. */
	UPROPERTY() float ContrastRatio = 0.f;
	/** Correlated colour temperature of the average colour, in kelvin: lower is warmer. */
	UPROPERTY() float ColorTemperatureK = 0.f;
	UPROPERTY() float MeanSaturation = 0.f;
	/** Share of near-black pixels, and of clipped highlights. */
	UPROPERTY() float ShadowFraction = 0.f;
	UPROPERTY() float HighlightFraction = 0.f;
	/** 16 bins of perceptual brightness, summing to 1. */
	UPROPERTY() TArray<float> Histogram;
};

USTRUCT(BlueprintType)
struct FHyperAILightingCompareRequest
{
	GENERATED_BODY()

	/** PNG or JPG inside the project folder: the look to match. */
	UPROPERTY() FString ReferenceImagePath;
	/** Optional image to measure instead of the active level viewport. */
	UPROPERTY() FString CurrentImagePath;
	/** Brightness-distribution distance (0-1) at or below which the look counts as matched. */
	UPROPERTY() float MatchThreshold = 0.05f;
};

USTRUCT(BlueprintType)
struct FHyperAILightingCompareReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FHyperAILightingImageMetrics Reference;
	UPROPERTY() FHyperAILightingImageMetrics Current;
	/** Earth mover's distance between the two brightness histograms, 0 (same) to 1. */
	UPROPERTY() float HistogramDistance = 0.f;
	/** Exposure change that would bring the current median brightness to the reference's, in stops. */
	UPROPERTY() float ExposureDeltaEV = 0.f;
	/** Reference minus current colour temperature: negative means the reference is warmer. */
	UPROPERTY() float ColorTemperatureDeltaK = 0.f;
	UPROPERTY() float ContrastRatioDelta = 0.f;
	UPROPERTY() float SaturationDelta = 0.f;
	UPROPERTY() bool bMatched = false;
	/** False when auto exposure is on; lock_exposure first so comparisons measure the lighting, not the eye. */
	UPROPERTY() bool bExposureLocked = false;
	/** What to change next, strongest difference first. */
	UPROPERTY() TArray<FString> Suggestions;
	/** A PNG of what was measured, to view beside the reference. */
	UPROPERTY() FString CurrentImageWrittenPath;
	UPROPERTY() TArray<FString> Capabilities;
};

/**
 * One closed lighting edit. Kind and Property:
 *   set_sun: intensity (lux), temperature (K), color (r,g,b), pitch, yaw (degrees), source_angle
 *   set_sky_light: intensity, color
 *   set_fog: density, height_falloff, start_distance, inscattering_color
 *   set_sky_atmosphere: rayleigh_scale, mie_scale
 *   set_post_process: exposure_bias (EV), white_temp (K), saturation, contrast, gamma, and grading as r,g,b:
 *     shadows_gain, shadows_offset (-0.5 to 0.5), highlights_gain
 *   lock_exposure: value is the exposure bias in EV (default 0); unlock_exposure: no value
 *   set_actor_material: actor_label (required) names any placed actor; property is empty (slot 0),
 *     slot:<index or name>, or component:<name>/slot:<index or name>; value is the material's path
 *   set_sky_system: a Blueprint sky such as Ultra Dynamic Sky, which owns its sun, sky light and fog. Property is a
 *     variable by the name its Details panel shows (Time of Day, Cloud Coverage, Sun Light Intensity, ...), value is
 *     a number, true/false, an enum's display name or Unreal's text form; empty actor_label takes the first sky that
 *     has the variable. With such a sky, set_sun, set_sky_light, set_fog and set_sky_atmosphere are refused.
 * ActorLabel picks an actor; empty uses the first of its kind, adding one if the level has none.
 */
USTRUCT(BlueprintType)
struct FHyperAILightingOp
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString ActorLabel;
	UPROPERTY() FString Property;
	UPROPERTY() FString Value;
};

USTRUCT(BlueprintType)
struct FHyperAILightingApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	/** The revision hyper_lighting_inspect reported. */
	UPROPERTY() FString ExpectedRevision;
	/** Applied in order as one undo step. */
	UPROPERTY() TArray<FHyperAILightingOp> Ops;
	/** Saves the changed lighting actors. Off by default so iterations stay unsaved until the look is right. */
	UPROPERTY() bool bSave = false;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAILightingApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTrustedPrepared = false;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString LevelPath;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() TArray<FString> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAICaptureViewRequest
{
	GENERATED_BODY()

	/** camera (a camera actor by label), transform (Location, Rotation, FovDegrees), or viewport (the level viewport's view). */
	UPROPERTY() FString Source = TEXT("camera");
	UPROPERTY() FString CameraActorLabel;
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;
	UPROPERTY() float FovDegrees = 90.f;
	/** Up to 1920 x 1080. */
	UPROPERTY() int32 Width = 1280;
	UPROPERTY() int32 Height = 720;
	/** 1-4. With two or more, each frame is compared with the one before it. */
	UPROPERTY() int32 FrameCount = 1;
	/** world mode: seconds of editor world time between frames, 0.05-5. */
	UPROPERTY() float IntervalSeconds = 0.5f;
	/** world, or material_time: every frame renders with the material Time node forced to the matching MaterialTimes entry. */
	UPROPERTY() FString TimeMode = TEXT("world");
	UPROPERTY() TArray<float> MaterialTimes;
	/** Discarded captures before the first frame, 0-8, so temporal effects settle. */
	UPROPERTY() int32 WarmupFrames = 3;
	/**
	 * Then keep drawing until the level shows its latest change: sky captures, global illumination and fog take many
	 * frames to catch up, and the first of them can look unchanged. At least 1.5 seconds and 45 frames after the last
	 * edit to the level, then until two draws in a row match; 6 seconds at most. False takes exactly WarmupFrames.
	 */
	UPROPERTY() bool bSettle = true;
	/** Largest per-channel change (0-1) above which a pixel counts as changed between frames. */
	UPROPERTY() float DiffThreshold = 0.03f;
	/** Names the batch folder: letters, digits, - and _. */
	UPROPERTY() FString Label;
};

USTRUCT(BlueprintType)
struct FHyperAICaptureFrame
{
	GENERATED_BODY()

	/** Project-relative PNG. */
	UPROPERTY() FString ImagePath;
	UPROPERTY() float WorldTimeSeconds = 0.f;
	/** The forced material time, or -1 in world mode. */
	UPROPERTY() float MaterialTime = -1.f;
	UPROPERTY() FHyperAILightingImageMetrics Metrics;
};

USTRUCT(BlueprintType)
struct FHyperAICaptureDiff
{
	GENERATED_BODY()

	UPROPERTY() int32 FrameA = 0;
	UPROPERTY() int32 FrameB = 0;
	/** Mean absolute RGB change, 0-1. */
	UPROPERTY() float MeanAbsDiff = 0.f;
	/** Share of pixels whose largest channel change exceeds DiffThreshold. */
	UPROPERTY() float ChangedPixelFraction = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAICaptureViewReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString BatchId;
	/** Project-relative folder holding the frames. */
	UPROPERTY() FString Folder;
	UPROPERTY() FString Source;
	UPROPERTY() FVector ViewLocation = FVector::ZeroVector;
	UPROPERTY() FRotator ViewRotation = FRotator::ZeroRotator;
	UPROPERTY() float FovDegrees = 0.f;
	UPROPERTY() TArray<FHyperAICaptureFrame> Frames;
	UPROPERTY() TArray<FHyperAICaptureDiff> Diffs;
	/** world mode: false means the editor world did not tick between frames, so an unchanged result proves nothing. */
	UPROPERTY() bool bWorldTimeAdvanced = false;
	/** True when shaders were still compiling at capture time, so some surfaces may show default materials. */
	UPROPERTY() bool bShadersStillCompiling = false;
	/** Draws settling took beyond WarmupFrames. */
	UPROPERTY() int32 SettleFrames = 0;
	/** True when settling ran out of time: the picture kept changing (a moving sky, clouds or water) or the level kept being edited. */
	UPROPERTY() bool bSettleTimedOut = false;
	UPROPERTY() TArray<FString> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIImageCompareRequest
{
	GENERATED_BODY()

	/** The look to reach: an image inside the project, such as a generated reference, concept art, a photo or an earlier capture. */
	UPROPERTY() FString ReferenceImagePath;
	/** The attempt: an image inside the project, such as a hyper_capture_view frame. Empty captures the active level viewport. */
	UPROPERTY() FString CandidateImagePath;
	/**
	 * look: a concept or photo that will never line up pixel for pixel, so palette, tone, layout and detail count.
	 * aligned: both frame the same view (the same camera, a material preview against its target), so structure counts too.
	 */
	UPROPERTY() FString Mode = TEXT("look");
	/** Score (0-100) at or above which the candidate counts as matched. */
	UPROPERTY() float TargetScore = 80.f;
	/** Keeps iterations together: letters, digits, - and _. Each compare in a session records its candidate, score and note. */
	UPROPERTY() FString SessionId;
	/** What changed since the last iteration, in a few words, kept in the session's history. */
	UPROPERTY() FString Note;
	/** Cells a side for the region breakdown, 2-8. */
	UPROPERTY() int32 Grid = 4;
};

USTRUCT(BlueprintType)
struct FHyperAIImageRegion
{
	GENERATED_BODY()

	/** Where it is, such as "top left" or "center". */
	UPROPERTY() FString Name;
	UPROPERTY() int32 Row = 0;
	UPROPERTY() int32 Column = 0;
	/** 0-100 for this cell alone, after the overall differences are taken out. */
	UPROPERTY() float Score = 0.f;
	/** Brightness change that would bring the cell to the reference, in stops, beyond the overall one. */
	UPROPERTY() float BrightenEV = 0.f;
	/** Colour difference of the cell's averages, CIE delta E, beyond the overall one. */
	UPROPERTY() float ColorDeltaE = 0.f;
	/** Candidate detail over reference detail: below 1 is flatter, above 1 busier. */
	UPROPERTY() float DetailRatio = 1.f;
	/** The cell's average colour in each image, sRGB 0-1. */
	UPROPERTY() FLinearColor ReferenceColor = FLinearColor::Black;
	UPROPERTY() FLinearColor CandidateColor = FLinearColor::Black;
	/** In words, such as "darker by 0.8 EV, cooler, less detail". */
	UPROPERTY() FString Difference;
};

USTRUCT(BlueprintType)
struct FHyperAIImageIteration
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = 0;
	UPROPERTY() float Score = 0.f;
	UPROPERTY() FString Note;
	/** The candidate as it was, copied into the session so a later capture cannot overwrite it; project-relative. */
	UPROPERTY() FString CandidateImagePath;
	UPROPERTY() FString SideBySidePath;
	UPROPERTY() FString ComparedUtc;
};

USTRUCT(BlueprintType)
struct FHyperAIImageCompareReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** matched, differs, or why not */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Mode;
	/** 0-100: how close the candidate is to the reference, by the measures Mode weights. */
	UPROPERTY() float Score = 0.f;
	UPROPERTY() float TargetScore = 0.f;
	UPROPERTY() bool bMatched = false;
	/** Each measure, 0-100. Structure is weighed in aligned mode only; layout and palette ignore an overall shift. */
	UPROPERTY() float StructureScore = 0.f;
	UPROPERTY() float PaletteScore = 0.f;
	UPROPERTY() float ToneScore = 0.f;
	UPROPERTY() float LayoutScore = 0.f;
	UPROPERTY() float DetailScore = 0.f;
	/** Changes that would bring the candidate to the reference: stops (positive brightens), kelvin (negative warms), saturation, contrast ratio. */
	UPROPERTY() float ExposureDeltaEV = 0.f;
	UPROPERTY() float ColorTemperatureDeltaK = 0.f;
	UPROPERTY() float SaturationDelta = 0.f;
	UPROPERTY() float ContrastRatioDelta = 0.f;
	/** Each image's average colour, sRGB 0-1. */
	UPROPERTY() FLinearColor ReferenceMeanColor = FLinearColor::Black;
	UPROPERTY() FLinearColor CandidateMeanColor = FLinearColor::Black;
	/** Every cell of the grid, worst first. */
	UPROPERTY() TArray<FHyperAIImageRegion> Regions;
	/** What to change next, strongest difference first. */
	UPROPERTY() TArray<FString> Suggestions;
	/** Reference and candidate side by side with the grid and the worst cells outlined, and a heat map of where they differ. Open them. */
	UPROPERTY() FString SideBySidePath;
	UPROPERTY() FString DiffPath;
	/** The candidate as measured, when it was a viewport capture. */
	UPROPERTY() FString CandidateWrittenPath;
	UPROPERTY() FString SessionId;
	/** Every iteration of the session so far, this one last. */
	UPROPERTY() TArray<FHyperAIImageIteration> History;
	UPROPERTY() int32 BestIteration = -1;
	UPROPERTY() float BestScore = 0.f;
	/** Score change since the previous iteration. */
	UPROPERTY() float Improvement = 0.f;
	/** Three iterations without a point gained over the best before them: change approach, or stop. */
	UPROPERTY() bool bPlateau = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/** The pending result of hyper_capture_view; frames spread over editor ticks while the agent's call stays open. */
UCLASS()
class UHyperAICaptureViewResult final : public UToolCallAsyncResult
{
	GENERATED_BODY()

public:
	bool SetValue(const FHyperAICaptureViewReport& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(FHyperAICaptureViewReport(InValue), Value);
	}

	UPROPERTY()
	FHyperAICaptureViewReport Value;
};

/** Match a level's lighting to a reference image: inspect, lock exposure, compare, adjust, compare again. */
UCLASS()
class HYPERAISTUDIOLIGHTING_API UHyperAIStudioLightingToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Sun, sky light, fog, sky atmosphere, post process and any Blueprint sky (Ultra Dynamic Sky and Weather) in the open level, whether exposure is locked, and the revision apply_plan needs. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Lighting")
	static FHyperAILightingInspectReport hyper_lighting_inspect(const FHyperAILightingInspectRequest& Request);

	/**
	 * Closed lighting edits as one undo step: sun, sky light, fog, sky atmosphere, post process, exposure lock,
	 * set_actor_material for a placed mesh, and set_sky_system for a Blueprint sky such as Ultra Dynamic Sky, by its
	 * Details names. Submit with bDryRun false and an operation_id, call hyper_operation_wait, then compare. Unsaved
	 * unless bSave.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Lighting")
	static FHyperAILightingApplyPlanReport hyper_lighting_apply_plan(const FHyperAILightingApplyPlanRequest& Request);

	/**
	 * Measures the active level viewport (or current_image_path) against a reference image: brightness percentiles,
	 * contrast, colour temperature, saturation, histogram distance, and suggested next changes. Frame the view to
	 * match the reference first, and lock exposure, or brightness comparisons measure auto exposure, not lighting.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Lighting")
	static FHyperAILightingCompareReport hyper_lighting_compare(const FHyperAILightingCompareRequest& Request);

	/**
	 * Final-colour PNGs from a named camera, a transform, or the level viewport, with metrics and frame-to-frame
	 * differences, under Saved/HyperAIStudio/Captures. To prove an animation, capture two or more frames; TimeMode
	 * material_time samples a material's Time node at exact times. Changes nothing in the project.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Lighting")
	static UHyperAICaptureViewResult* hyper_capture_view(const FHyperAICaptureViewRequest& Request);

	/**
	 * How close an attempt is to a reference image, for scenes, materials, effects and UI: a 0-100 score from palette,
	 * tone, layout and detail (structure too when aligned), the cells that differ most, suggestions, and side-by-side and
	 * heat-map PNGs to open. Pass session_id and a note on each iteration of a match loop to keep its history. Changes nothing.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Lighting")
	static FHyperAIImageCompareReport hyper_image_compare(const FHyperAIImageCompareRequest& Request);
};

class FHyperAIStudioLightingInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAILightingInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingComparePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAILightingCompareRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingCapturePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAICaptureViewRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingCaptureResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAICaptureViewReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingEditOpsPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString LevelPath;
	FString BaseRevision;
	TArray<FHyperAILightingOp> Ops;
	bool bSave = false;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

class FHyperAIStudioLightingInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAILightingInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingCompareResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAILightingCompareReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingImageComparePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIImageCompareRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingImageCompareResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIImageCompareReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioLightingDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioLightingFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
{
public:
	virtual FString GetOwnerAdapterFingerprint() const override;
	virtual bool ResolveCanonicalEffectTarget(
		const IHyperAIStudioTypedArtifactPayload& Request,
		FString& OutCanonicalEffectTarget,
		FString& OutError) override;
	virtual bool VerifyFreshExact(
		const IHyperAIStudioTypedArtifactPayload& Request,
		const IHyperAIStudioDomainResultPayload& Result,
		FString& OutPostconditionHash,
		FString& OutError) override;
};

class FHyperAIStudioLightingContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("lighting_lookdev");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiolightingtoolset.v1");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.lighting_editor_world");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_lighting_inspect");
	static constexpr const TCHAR* MutationToolName = TEXT("hyper_lighting_apply_plan");
	static constexpr const TCHAR* CompareToolName = TEXT("hyper_lighting_compare");
	static constexpr const TCHAR* InspectVariantId = TEXT("lighting_actor_capture.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("lighting_edit_ops.v1");
	static constexpr const TCHAR* CompareVariantId = TEXT("image_metrics_compare.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.lighting.inspect.v1");
	static constexpr const TCHAR* EditOpsPayloadTypeId = TEXT("hyperai.payload.lighting.edit_ops.v1");
	static constexpr const TCHAR* ComparePayloadTypeId = TEXT("hyperai.payload.lighting.compare.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.lighting.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.lighting.edit_ops.v1");
	static constexpr const TCHAR* CompareResultTypeId = TEXT("hyperai.result.lighting.compare.v1");
	static constexpr const TCHAR* CaptureToolName = TEXT("hyper_capture_view");
	static constexpr const TCHAR* CaptureVariantId = TEXT("view_capture.v1");
	static constexpr const TCHAR* CapturePayloadTypeId = TEXT("hyperai.payload.lighting.capture.v1");
	static constexpr const TCHAR* CaptureResultTypeId = TEXT("hyperai.result.lighting.capture.v1");
	static constexpr const TCHAR* ImageCompareToolName = TEXT("hyper_image_compare");
	static constexpr const TCHAR* ImageCompareVariantId = TEXT("image_match_compare.v1");
	static constexpr const TCHAR* ImageComparePayloadTypeId = TEXT("hyperai.payload.lighting.image_compare.v1");
	static constexpr const TCHAR* ImageCompareResultTypeId = TEXT("hyperai.result.lighting.image_compare.v1");
	static constexpr int32 MaxOpsPerPlan = 32;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FString> GetCapabilities();
	static bool IsCanonicalSha256(const FString& Value);
	static FString InspectPayloadSchemaFingerprint();
	static FString EditOpsPayloadSchemaFingerprint();
	static FString ComparePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString CompareResultSchemaFingerprint();
	static FString CapturePayloadSchemaFingerprint();
	static FString CaptureResultSchemaFingerprint();
	static FString ImageComparePayloadSchemaFingerprint();
	static FString ImageCompareResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAILightingInspectReport Inspect(const FHyperAILightingInspectRequest& Request);
	static FHyperAILightingCompareReport Compare(const FHyperAILightingCompareRequest& Request);
	/** Captures without waiting between frames: one frame, or frames at forced material times. */
	static FHyperAICaptureViewReport CaptureNow(const FHyperAICaptureViewRequest& Request);
	static FHyperAIImageCompareReport CompareImages(const FHyperAIImageCompareRequest& Request);
	static FHyperAILightingApplyPlanReport BuildPlan(const FHyperAILightingApplyPlanRequest& Request);
	/** Order-sensitive: the same ops in a different order are a different plan. */
	static FString ComputeEditOpsSemanticFingerprint(const FHyperAIStudioLightingEditOpsPayload& Payload);
};

class FHyperAIStudioLightingRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;
	bool HasLiveOwnership() const;

private:
	void RegisterAfterEngineInit();
	void RollBackRegistration();

	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FHyperAIStudioLightingDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
