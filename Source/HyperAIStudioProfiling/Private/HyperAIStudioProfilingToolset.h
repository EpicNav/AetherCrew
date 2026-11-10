// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioBot.h"
#include "ToolsetRegistry/ToolCallAsyncResult.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioProfilingToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIPerfSessionRequest
{
	GENERATED_BODY()

	/** auto (the default: attach when Play In Editor runs, else start it), attach (it must be running) or start (start it, and stop it after). */
	UPROPERTY() FString Mode;
	/** Seconds measured: 10 by default, 1 to 60. */
	UPROPERTY() float DurationSeconds = 10.f;
	/** Seconds of play before measuring starts, so loading and shader hitches stay out: 3 when the session starts play, 0 when it attaches; 0 to 30. */
	UPROPERTY() float WarmupSeconds = -1.f;
	/** A frame slower than this (ms), or over twice the median and 5 ms above it, is a hitch: 50 by default, 5 to 1000. */
	UPROPERTY() float HitchThresholdMs = 50.f;
	/** Stats, timers and hitches listed: 10 by default, 1 to 30. */
	UPROPERTY() int32 TopN = 10;
	/** Record an Unreal Insights trace (cpu, frame) and list the hottest CPU timers over the session and inside the worst frame. */
	UPROPERTY() bool bHotFunctions = true;
	/** Read the level's cost report (hyper_scene_cost_report, pie world) and tie its findings to the advice. */
	UPROPERTY() bool bSceneCost = true;
	/** Playtest bot steps played in order while the session is measured (at most 16): the same objects as hyper_playtest_run's bot field. */
	UPROPERTY() TArray<FHyperAIBotStep> Bot;
	/** Names the session and its profile; made when empty. */
	UPROPERTY() FString OperationId;
	/** A short note kept with the profile, such as "before the LOD change". */
	UPROPERTY() FString Label;
	/** Read back an earlier session's profile by its id instead of measuring. */
	UPROPERTY() FString ProfileId;
	/** Analyse a CSV the CSV profiler wrote (a path inside this project, such as Saved/Profiling/CSV/x.csv) instead of measuring. */
	UPROPERTY() FString CsvPath;
};

/** How one per-frame figure was spread over the session. */
USTRUCT(BlueprintType)
struct FHyperAIPerfSeries
{
	GENERATED_BODY()

	/** frame, game, render, rhi, gpu, draw_calls or primitives. */
	UPROPERTY() FString Name;
	UPROPERTY() double Average = 0.0;
	UPROPERTY() double P50 = 0.0;
	UPROPERTY() double P95 = 0.0;
	UPROPERTY() double P99 = 0.0;
	UPROPERTY() double Max = 0.0;
	UPROPERTY() double Min = 0.0;
};

/** What limited the frame rate, and why the profile says so. */
USTRUCT(BlueprintType)
struct FHyperAIPerfVerdict
{
	GENERATED_BODY()

	/** game_thread, render_thread, rhi_thread, gpu, capped (a frame limit, vsync or throttling held it back), or unknown. */
	UPROPERTY() FString Bound;
	/** Share of frames bound that way, percent. */
	UPROPERTY() double FramesPercent = 0.0;
	UPROPERTY() FString Summary;
	/** The figures behind it. */
	UPROPERTY() TArray<FString> Evidence;
	/** Every bound and its share of frames: "gpu 81%". */
	UPROPERTY() TArray<FString> Breakdown;
	/** The frame-rate limit in effect, when one was found; 0 when none. */
	UPROPERTY() double CapFps = 0.0;
	/** Where it came from: t.MaxFPS, r.VSync, the editor's frame limit or its background throttling. */
	UPROPERTY() FString CapSource;
};

/** One frame much slower than the rest. */
USTRUCT(BlueprintType)
struct FHyperAIPerfHitch
{
	GENERATED_BODY()

	/** The frame's index in the capture, from 0. */
	UPROPERTY() int32 Frame = 0;
	/** Seconds into the capture when it started. */
	UPROPERTY() double AtSeconds = 0.0;
	UPROPERTY() double FrameMs = 0.0;
	/** What took longest that frame: game, render, rhi, gpu, or wait (no thread was busy for most of it). */
	UPROPERTY() FString Bound;
	/** Stats that rose most above their median that frame: "Exclusive/GameThread/Physics 41.0 ms (median 1.2)". */
	UPROPERTY() TArray<FString> Spikes;
	/** CSV events that frame, such as a level streaming in. */
	UPROPERTY() TArray<FString> Events;
};

/** A CSV profiler stat's time per frame. */
USTRUCT(BlueprintType)
struct FHyperAIPerfStatLine
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** GameThread, RenderThread, RHIThread, GPU, or another thread the stat ran on. */
	UPROPERTY() FString Thread;
	UPROPERTY() double AverageMs = 0.0;
	UPROPERTY() double MaxMs = 0.0;
	/** Its share of its thread's average time per frame, percent; 0 when that is unknown. */
	UPROPERTY() double SharePercent = 0.0;
};

/** A CPU timer from the session's Unreal Insights trace. */
USTRUCT(BlueprintType)
struct FHyperAIPerfTimer
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Thread;
	/** Time inside it, its children left out, over the whole span (ms). */
	UPROPERTY() double ExclusiveMs = 0.0;
	/** Time inside it, children included (ms). */
	UPROPERTY() double InclusiveMs = 0.0;
	/** Exclusive time per game frame (ms). */
	UPROPERTY() double ExclusivePerFrameMs = 0.0;
	UPROPERTY() int64 Calls = 0;
};

/** A Niagara system's measured cost per frame. */
USTRUCT(BlueprintType)
struct FHyperAIPerfFxCost
{
	GENERATED_BODY()

	UPROPERTY() FString System;
	/** Instances it averaged per frame. */
	UPROPERTY() double Instances = 0.0;
	/** Game-thread work, concurrent ticks included, microseconds per frame. */
	UPROPERTY() double GameThreadUs = 0.0;
	UPROPERTY() double RenderThreadUs = 0.0;
	/** GPU simulation time, microseconds per frame; 0 without GPU timing. */
	UPROPERTY() double GpuUs = 0.0;
	UPROPERTY() double MemoryKB = 0.0;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/**
	 * completed, awaiting_user_approval, queued, invalid_request, no_pie_world, play_in_editor_running, busy, csv_unavailable,
	 * capture_failed, stopped, not_found.
	 */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	/** The id to read it back with (profile_id) and to compare it by. */
	UPROPERTY() FString ProfileId;
	UPROPERTY() FString Label;
	UPROPERTY() FString CreatedUtc;
	UPROPERTY() FString Level;
	/** session (measured now), csv (an existing CSV analysed) or stored (read back). */
	UPROPERTY() FString Source;
	/** attach or start. */
	UPROPERTY() FString Mode;
	UPROPERTY() double CapturedSeconds = 0.0;
	UPROPERTY() int32 Frames = 0;
	UPROPERTY() double AverageFps = 0.0;
	/** Frame time, ms. */
	UPROPERTY() FHyperAIPerfSeries FrameTime;
	/** game, render, rhi and gpu time per frame, ms; rhi only when the RHI runs its own thread. */
	UPROPERTY() TArray<FHyperAIPerfSeries> Threads;
	UPROPERTY() bool bGpuTimeAvailable = false;
	/** Why GPU time is missing, when it is: "GPU time unavailable (null RHI)". */
	UPROPERTY() FString GpuNote;
	UPROPERTY() FHyperAIPerfVerdict Verdict;
	UPROPERTY() double HitchThresholdMs = 0.0;
	UPROPERTY() int32 HitchCount = 0;
	/** The worst hitches, slowest first. */
	UPROPERTY() TArray<FHyperAIPerfHitch> Hitches;
	/** CPU stats by exclusive time per frame (the CSV profiler's Exclusive category), waits left out. */
	UPROPERTY() TArray<FHyperAIPerfStatLine> TopCpuStats;
	/** GPU passes by time per frame. */
	UPROPERTY() TArray<FHyperAIPerfStatLine> TopGpuStats;
	/** Time threads spent waiting per frame, such as the game thread waiting for the render thread. */
	UPROPERTY() TArray<FHyperAIPerfStatLine> Waits;
	/** The hottest CPU timers over the session on the game, render and RHI threads, by exclusive time. */
	UPROPERTY() TArray<FHyperAIPerfTimer> HotFunctions;
	/** The hottest game- and render-thread timers inside the trace's slowest game frame. */
	UPROPERTY() TArray<FHyperAIPerfTimer> HotFunctionsInWorstFrame;
	UPROPERTY() double WorstTraceFrameMs = 0.0;
	/** How the hot functions were found, or why there are none. */
	UPROPERTY() FString HotFunctionsNote;
	UPROPERTY() FHyperAIPerfSeries DrawCalls;
	UPROPERTY() FHyperAIPerfSeries Primitives;
	/** The editor process's physical memory at the start, end and peak of the capture (MB). */
	UPROPERTY() double MemoryStartMB = 0.0;
	UPROPERTY() double MemoryEndMB = 0.0;
	UPROPERTY() double MemoryPeakMB = 0.0;
	/** Renderer figures from the capture: Nanite streaming pool and visible set, virtual shadow map pages, render target pool. */
	UPROPERTY() TArray<FString> Renderer;
	/** Niagara systems by game-thread cost during the session. */
	UPROPERTY() TArray<FHyperAIPerfFxCost> FxSystems;
	/** The level's cost report headline (hyper_scene_cost_report on the play world). */
	UPROPERTY() TArray<FString> SceneCost;
	/** What the figures point at, and what to try. */
	UPROPERTY() TArray<FString> Advice;
	/** How the bot route went, when there was one. */
	UPROPERTY() FString Bot;
	UPROPERTY() FString CsvPath;
	UPROPERTY() FString TracePath;
	/** The stored profile (JSON). */
	UPROPERTY() FString ReportPath;
	UPROPERTY() TArray<FString> Notes;
};

/** Completes when the session is measured and analysed. */
UCLASS()
class UHyperAIPerfSessionResult final : public UToolCallAsyncResult
{
	GENERATED_BODY()

public:
	bool SetValue(const FHyperAIPerfReport& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(FHyperAIPerfReport(InValue), Value);
	}

	UPROPERTY()
	FHyperAIPerfReport Value;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfCompareRequest
{
	GENERATED_BODY()

	/** Before: a profile id, or a CSV path inside this project. */
	UPROPERTY() FString Baseline;
	/** After: a profile id, or a CSV path inside this project. */
	UPROPERTY() FString Candidate;
	/** A change smaller than this, percent, is the same: 5 by default, 1 to 50. */
	UPROPERTY() float ThresholdPercent = 5.f;
	/** Stats listed by how much they changed: 10 by default, 1 to 30. */
	UPROPERTY() int32 TopN = 10;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfDelta
{
	GENERATED_BODY()

	UPROPERTY() FString Metric;
	UPROPERTY() double Before = 0.0;
	UPROPERTY() double After = 0.0;
	UPROPERTY() double Change = 0.0;
	UPROPERTY() double ChangePercent = 0.0;
	/** better, worse or same (lower is better for every metric here, fps aside). */
	UPROPERTY() FString Verdict;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfCompareReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** compared, invalid_request, not_found. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Baseline;
	UPROPERTY() FString Candidate;
	UPROPERTY() FString BaselineLabel;
	UPROPERTY() FString CandidateLabel;
	UPROPERTY() FString BoundBefore;
	UPROPERTY() FString BoundAfter;
	/** Frame time, its percentiles, each thread, GPU, draw calls, primitives, hitches and memory. */
	UPROPERTY() TArray<FHyperAIPerfDelta> Metrics;
	/** The CPU and GPU stats that changed most. */
	UPROPERTY() TArray<FHyperAIPerfDelta> Stats;
	UPROPERTY() int32 Better = 0;
	UPROPERTY() int32 Worse = 0;
	UPROPERTY() FString Summary;
	UPROPERTY() TArray<FString> Notes;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfLiveRequest
{
	GENERATED_BODY()

	/** 0 (the default) reads what is there now; 1 to 10 also samples that long: Niagara cost per system, Nanite streaming, shadow pages. */
	UPROPERTY() float SampleSeconds = 0.f;
	/** Systems and meshes listed: 10 by default, 1 to 50. */
	UPROPERTY() int32 TopN = 10;
	UPROPERTY() bool bNanite = true;
	UPROPERTY() bool bNiagara = true;
	UPROPERTY() bool bRenderer = true;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfNaniteLive
{
	GENERATED_BODY()

	/** Nanite renders on this platform and project. */
	UPROPERTY() bool bEnabled = false;
	/** Any Nanite mesh is registered with the streaming manager. */
	UPROPERTY() bool bHasResources = false;
	/** The streaming pool (r.Nanite.Streaming.StreamingPoolSize), MB and pages. */
	UPROPERTY() double StreamingPoolMB = 0.0;
	UPROPERTY() int32 StreamingPoolPages = 0;
	UPROPERTY() int32 RootPagesLimit = 0;
	/** 1 while the visible set fits the pool; below 1 the streamer is lowering quality because it does not. */
	UPROPERTY() double QualityScale = 1.0;
	/** Static mesh components drawn with Nanite, and their instances. */
	UPROPERTY() int32 NanitePrimitives = 0;
	UPROPERTY() int64 NaniteInstances = 0;
	/** Components whose mesh has Nanite data but that draw its fallback mesh instead. */
	UPROPERTY() int32 FallbackPrimitives = 0;
	/** mesh on actor: why (disallowed on the component, Nanite off for this platform, or a material Nanite cannot draw). */
	UPROPERTY() TArray<FString> Fallbacks;
	/** Static mesh components with no Nanite data. */
	UPROPERTY() int32 NonNanitePrimitives = 0;
	/** Sampled: root page memory, the visible set's streaming data, page data requested and read from disk. */
	UPROPERTY() double RootAllocationMB = -1.0;
	UPROPERTY() double VisibleStreamingDataMB = -1.0;
	UPROPERTY() double RequestedMB = -1.0;
	UPROPERTY() int64 RequestedPages = -1;
	UPROPERTY() double IORequestMBps = -1.0;
	UPROPERTY() TArray<FString> Notes;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfNiagaraSystem
{
	GENERATED_BODY()

	UPROPERTY() FString System;
	UPROPERTY() FString EffectType;
	/** Active components playing it. */
	UPROPERTY() int32 Instances = 0;
	UPROPERTY() int32 ActiveEmitters = 0;
	UPROPERTY() int64 Particles = 0;
	UPROPERTY() int64 CpuParticles = 0;
	/** From the GPU's last count readback, which lags a frame or more. */
	UPROPERTY() int64 GpuParticles = 0;
	/** Sampled, microseconds per frame: game thread (concurrent ticks included), render thread, GPU. -1 when not sampled. */
	UPROPERTY() double GameThreadUs = -1.0;
	UPROPERTY() double RenderThreadUs = -1.0;
	UPROPERTY() double GpuUs = -1.0;
	UPROPERTY() double MemoryKB = -1.0;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfEffectType
{
	GENERATED_BODY()

	UPROPERTY() FString EffectType;
	/** Instances registered with it now, across the world. */
	UPROPERTY() int32 Instances = 0;
	/** Its cap at the current quality level; 0 when it has none. */
	UPROPERTY() int32 MaxInstances = 0;
	UPROPERTY() int32 MaxSystemInstances = 0;
	UPROPERTY() double MaxDistance = 0.0;
	UPROPERTY() FString CullReaction;
	/** It is at or past its instance cap, so new spawns of its systems are culled. */
	UPROPERTY() bool bAtCap = false;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfNiagaraLive
{
	GENERATED_BODY()

	UPROPERTY() int32 ActiveComponents = 0;
	/** Niagara components in the play world that are not playing. */
	UPROPERTY() int32 InactiveComponents = 0;
	UPROPERTY() int32 ActiveEmitters = 0;
	UPROPERTY() int64 Particles = 0;
	UPROPERTY() int64 CpuParticles = 0;
	UPROPERTY() int64 GpuParticles = 0;
	/** Busiest first: by game-thread cost when sampled, else by particles. */
	UPROPERTY() TArray<FHyperAIPerfNiagaraSystem> Systems;
	UPROPERTY() TArray<FHyperAIPerfEffectType> EffectTypes;
	/** Systems and effect types past a cap or the FX budget. */
	UPROPERTY() TArray<FString> OverBudget;
	/** The global FX budget (fx.Budget.Enabled), when on: "GT 1.2 of 2.0 ms". */
	UPROPERTY() FString FxBudget;
	UPROPERTY() TArray<FString> Notes;
};

USTRUCT(BlueprintType)
struct FHyperAIPerfLiveReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, invalid_request, no_pie_world, busy. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Level;
	UPROPERTY() double SampledSeconds = 0.0;
	UPROPERTY() int32 SampledFrames = 0;
	UPROPERTY() FHyperAIPerfNaniteLive Nanite;
	UPROPERTY() FHyperAIPerfNiagaraLive Niagara;
	/** Lumen, virtual shadow maps, MegaLights, screen percentage, the last frame's draw calls and thread times, and sampled pool figures. */
	UPROPERTY() TArray<FString> Renderer;
	UPROPERTY() TArray<FString> Notes;
};

/** Completes when the readout, and its sample window if any, is done. */
UCLASS()
class UHyperAIPerfLiveResult final : public UToolCallAsyncResult
{
	GENERATED_BODY()

public:
	bool SetValue(const FHyperAIPerfLiveReport& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(FHyperAIPerfLiveReport(InValue), Value);
	}

	UPROPERTY()
	FHyperAIPerfLiveReport Value;
};

/**
 * Play-session profiling: measure Play In Editor for a few seconds (optionally while a bot plays), then say whether it is
 * game-thread, render-thread, RHI or GPU bound, list its hitches, top stats and hottest CPU timers, compare two
 * profiles, and read Nanite, Niagara and renderer figures live during play.
 */
UCLASS()
class HYPERAISTUDIOPROFILING_API UHyperAIStudioProfilingToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Profile a play session and explain it. Attaches to Play In Editor (or starts it, and stops it after), waits
	 * WarmupSeconds, measures DurationSeconds with the CSV profiler and an Insights trace while an optional bot route
	 * plays, then answers: frame-time average and p50/p95/p99/max; the bound (game thread, render thread, RHI, GPU, or
	 * capped by vsync or a frame limit) with its evidence; hitches with their time, frame and the stats that spiked; top
	 * CPU and GPU stats; the hottest CPU timers; draw calls, memory, Niagara cost and the level's cost findings, with
	 * advice. GPU time is reported unavailable when the RHI has none. The profile is kept for hyper_profile_compare.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Profiling")
	static UHyperAIPerfSessionResult* hyper_profile_session(const FHyperAIPerfSessionRequest& Request);

	/**
	 * Compare two profiles, before and after a change: each by profile id or CSV path. Gives the change in frame time
	 * and its percentiles, each thread, GPU, draw calls, hitches and memory, the bound before and after, and the stats
	 * that moved most.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Profiling")
	static FHyperAIPerfCompareReport hyper_profile_compare(const FHyperAIPerfCompareRequest& Request);

	/**
	 * Read what the running Play In Editor session draws, read-only: Nanite (enabled, streaming pool and quality scale,
	 * Nanite and fallback meshes and why, sampled root and visible streaming data and pages requested), Niagara (active
	 * systems, emitters, CPU and GPU particles, effect types at their caps, FX budget, and per-system game, render and
	 * GPU cost over SampleSeconds), and renderer headlines (Lumen, virtual shadow maps, last frame's draw calls).
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Profiling")
	static UHyperAIPerfLiveResult* hyper_profile_live(const FHyperAIPerfLiveRequest& Request);
};

class FHyperAIStudioProfilingContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("automation_profiling_build");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioprofilingtoolset.v1");
	static constexpr const TCHAR* SessionToolName = TEXT("hyper_profile_session");
	static constexpr const TCHAR* CompareToolName = TEXT("hyper_profile_compare");
	static constexpr const TCHAR* LiveToolName = TEXT("hyper_profile_live");
	static constexpr int32 MaxBotSteps = 16;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	/** Where profiles are kept: Saved/HyperAIStudio/Profiles. */
	static FString GetProfilesDirectory();
};

class FHyperAIStudioProfilingRegistration final
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
