// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FHyperAIPerfCompareReport;
struct FHyperAIPerfReport;
struct FHyperAIPerfSeries;

/**
 * What a play session's CSV profile says: pure functions over the file the CSV profiler wrote, so recorded captures test
 * them. Nothing here touches the engine.
 */
namespace HyperAIStudio::Profiling
{
	/** One CSV the CSV profiler wrote, as columns of numbers. */
	struct FCsvCapture
	{
		/** The final header row's names; a name written for several threads is one column, summed. */
		TArray<FString> Columns;
		/** Values[Column][Frame]; a frame written before a column existed reads 0. */
		TArray<TArray<double>> Values;
		/** Each frame's events, as written ("Category/Text"). */
		TArray<TArray<FString>> Events;
		TMap<FString, FString> Metadata;

		int32 NumFrames() const { return Events.Num(); }
		int32 Find(const FString& Name) const { return Columns.IndexOfByKey(Name); }
		/** The column's frames, or null. */
		const TArray<double>* Column(const FString& Name) const;
	};

	/**
	 * Reads a CSV profiler file: a header row (EVENTS, then a name per stat), one row per frame, a final header row that
	 * names every column, and a metadata row of [Key],Value pairs. Rows written before a stat first appeared are shorter.
	 */
	bool ParseCsv(const FString& Text, FCsvCapture& Out, FString& OutError);

	/** What the capture knew of the frame-rate limits while it ran. */
	struct FCaptureContext
	{
		/** The limit in effect for most frames, fps; 0 when none was found. */
		double CapFps = 0.0;
		/** t.MaxFPS 60, r.VSync, the editor's frame limit, background throttling. */
		FString CapSource;
		/** The RHI has no GPU (-nullrhi): GPU time is unavailable, not zero. */
		bool bNullRhi = false;
		/** Frames dropped from the start: the frame the capture began in carries its own setup. */
		int32 SkipFrames = 1;
	};

	struct FAnalysisOptions
	{
		double HitchThresholdMs = 50.0;
		/** A frame over twice the median is a hitch only when it is also this much over it, so a 2 ms frame doubling is not one. */
		double HitchMinOverMedianMs = 5.0;
		int32 TopN = 10;
		int32 MaxHitches = 10;
		/** A frame whose busiest thread or GPU is under this share of it spent the rest waiting: on a limit, vsync or something unmeasured. */
		double BusyShare = 0.8;
	};

	/** Nearest-rank percentile (0..100) of Values; 0 for none. */
	double Percentile(TArray<double> Values, double P);
	/** Name, average, p50, p95, p99, max and min of Values. */
	void Describe(const FString& Name, const TArray<double>& Values, FHyperAIPerfSeries& Out);

	/**
	 * Fills the report's frame-time and thread series, GPU availability, bound verdict, hitches, top CPU and GPU stats,
	 * waits, draw calls, primitives, memory and renderer figures from Csv. Leaves identity, files, hot functions, FX and
	 * scene cost alone.
	 */
	void Analyse(const FCsvCapture& Csv, const FCaptureContext& Context, const FAnalysisOptions& Options, FHyperAIPerfReport& Out);

	/** The level's cost report, as hyper_scene_cost_report answered it (its JSON), in the few figures advice uses. */
	struct FSceneCostSummary
	{
		bool bValid = false;
		int32 EstimatedDrawCalls = 0;
		int32 EstimatedShadowDrawCalls = 0;
		int32 NaniteMaterialBins = 0;
		int32 TranslucentMaterials = 0;
		int32 NanitePrimitives = 0;
		int32 NonNanitePrimitives = 0;
		int64 TrianglesNonNanite = 0;
		int32 ShadowCastingDynamicLights = 0;
		/** mesh (instances x triangles), heaviest first. */
		TArray<FString> HeaviestMeshes;
		TArray<FString> Findings;
	};
	/** Reads the cost report's JSON (the tool's return value) into Out. */
	bool ReadSceneCost(const FString& Json, FSceneCostSummary& Out);
	/** The cost report in a few lines for the profile. */
	TArray<FString> DescribeSceneCost(const FSceneCostSummary& Scene);

	/** Fills Advice from the verdict, hitches, top stats, hot functions, memory and, when given, the level's cost. */
	void Advise(FHyperAIPerfReport& Report, const FSceneCostSummary* Scene);

	/** Before and after, metric by metric, and the stats that moved most. */
	void Compare(const FHyperAIPerfReport& Before, const FHyperAIPerfReport& After, double ThresholdPercent, int32 TopN,
		FHyperAIPerfCompareReport& Out);
}
