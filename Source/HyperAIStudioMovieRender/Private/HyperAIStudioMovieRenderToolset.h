// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioMovieRenderToolset.generated.h"

/** A setting by name and Unreal text form: output.OutputResolution (X=1920,Y=1080), GlobalOutputSettings.OutputDirectory. */
USTRUCT(BlueprintType)
struct FHyperAIMovieRenderSetting
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Value;
};

USTRUCT(BlueprintType)
struct FHyperAIMovieRenderOp
{
	GENERATED_BODY()

	/** config.create, config.set, graph.create, graph.set, render. */
	UPROPERTY() FString Kind;
	/**
	 * config.*: a Movie Render Queue config asset (/Game/Cinematics/MRQ_Final); graph.*: a Movie Render Graph
	 * (/Game/Cinematics/MRG_Final); render: the config or graph to render with, or empty to render with Preset, Format
	 * and Resolution alone.
	 */
	UPROPERTY() FString Asset;
	/** draft (one sample, no warm-up: fast) or final (8 temporal samples, warm-up frames, cinematic quality). */
	UPROPERTY() FString Preset;
	/** png, exr, jpg, bmp or mp4. */
	UPROPERTY() FString Format;
	UPROPERTY() FIntPoint Resolution = FIntPoint::ZeroValue;
	/** Frames per second to render at; the sequence's own when 0. */
	UPROPERTY() float FrameRate = 0.f;
	/** Where the frames go: an absolute folder, or Saved/MovieRenders/<sequence>_<time> when empty. */
	UPROPERTY() FString Directory;
	/**
	 * config.*: by setting and property: output.FileNameFormat, anti_aliasing.TemporalSampleCount, camera.ShutterTiming,
	 * high_res.TileCount, game_overrides.bCinematicQualitySettings. graph.*: by node and property:
	 * GlobalOutputSettings.OutputFrameRate. Unreal names and text form; a mistyped one is refused with the choices.
	 */
	UPROPERTY() TArray<FHyperAIMovieRenderSetting> Settings;
	/** config.*: console variables set for the render: r.MotionBlurQuality 4, r.DepthOfFieldQuality 4. */
	UPROPERTY() TArray<FHyperAIMovieRenderSetting> ConsoleVariables;
	/** render: the level sequence, and the level to render it in (the open level when empty). */
	UPROPERTY() FString Sequence;
	UPROPERTY() FString Map;
	/**
	 * render: local (the default) renders here in the editor, off screen, the level as it is now, for up to ten
	 * minutes; remote renders in a new process from the saved level and assets, and runs on past the wait.
	 */
	UPROPERTY() FString Executor;
};

USTRUCT(BlueprintType)
struct FHyperAIMovieRenderPlanRequest
{
	GENERATED_BODY()

	/** At most one render, and it goes last. */
	UPROPERTY() TArray<FHyperAIMovieRenderOp> Operations;
	/** Only check the plan: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Names the edit and the render, for hyper_operation_wait; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIMovieRenderPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, rendering (wait with hyper_operation_wait), checked, awaiting_user_approval, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() int32 Applied = 0;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
	/** Where the render writes its frames. */
	UPROPERTY() FString Output;
};

USTRUCT(BlueprintType)
struct FHyperAIMovieRenderInspectRequest
{
	GENERATED_BODY()

	/** A config or graph to read; every config and graph under /Game is listed when empty. */
	UPROPERTY() FString Asset;
};

USTRUCT(BlueprintType)
struct FHyperAIMovieRenderInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FString> Configs;
	UPROPERTY() TArray<FString> Graphs;
	/** A config's settings, or a graph's nodes with the properties they set. */
	UPROPERTY() TArray<FString> Settings;
	/** Renders running now: operation, sequence, progress. */
	UPROPERTY() TArray<FString> Renders;
};

/**
 * Movie Render Queue and Movie Render Graph: render configs and graphs made and tuned (format, resolution, samples,
 * warm-up, console variables), and level sequences rendered to frames or video, here or in a new process.
 */
UCLASS()
class HYPERAISTUDIOMOVIERENDER_API UHyperAIStudioMovieRenderToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Render configs and graphs under /Game and renders running now; or one config's settings, or one graph's nodes. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Cinematics")
	static FHyperAIMovieRenderInspectReport hyper_movie_render_inspect(const FHyperAIMovieRenderInspectRequest& Request);

	/**
	 * Renders level sequences: make or tune a Movie Render Queue config or a Movie Render Graph (draft or final quality,
	 * png, exr, jpg, bmp or mp4, resolution, frame rate, samples, console variables), then render a sequence with one or
	 * with a preset alone. A render returns rendering: wait on its operation_id with hyper_operation_wait.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Cinematics")
	static FHyperAIMovieRenderPlanReport hyper_movie_render_apply_plan(const FHyperAIMovieRenderPlanRequest& Request);
};

class FHyperAIStudioMovieRenderContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("cinematics");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiomovierendertoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_movie_render_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_movie_render_apply_plan");
	static constexpr int32 MaxOperations = 32;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIMovieRenderInspectReport Inspect(const FHyperAIMovieRenderInspectRequest& Request);
	static FHyperAIMovieRenderPlanReport ApplyPlan(const FHyperAIMovieRenderPlanRequest& Request);
};

class FHyperAIStudioMovieRenderRegistration final
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
