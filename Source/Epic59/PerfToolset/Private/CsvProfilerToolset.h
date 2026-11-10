// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "CsvProfilerToolset.generated.h"

/** Which reduced aggregate TopStats ranks stats by. */
UENUM(BlueprintType)
enum class ECsvProfilerStatRanking : uint8
{
	/** Mean of the per-frame values over the range. */
	Average,

	/** Largest per-frame value over the range. */
	Max,

	/** Sum of the per-frame values over the range. */
	Total,

	/** 50th-percentile (median) per-frame value over the range. */
	P50,

	/** 90th-percentile per-frame value over the range. */
	P90,

	/** 95th-percentile per-frame value over the range. */
	P95,

	/** 99th-percentile per-frame value over the range. */
	P99
};

/** Survey of a CSV profiler capture: its shape, event names, and metadata. */
USTRUCT(BlueprintType)
struct FCsvProfilerInfoResult
{
	GENERATED_BODY()

	/** Number of frames (rows) in the capture. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	int32 FrameCount = 0;

	/** Number of distinct stats (columns) in the capture. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	int32 StatCount = 0;

	/**
	 * Number of stats in each stat-name group, keyed by the group path at the requested GroupDepth
	 * (e.g. "Exclusive" at depth 1, "Exclusive/GameThread" at depth 2); ungrouped stats bucket under
	 * "(ungrouped)".
	 */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	TMap<FString, int32> StatGroups;

	/** Stat names matching the requested filter. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	TArray<FString> StatNames;

	/** Distinct event names recorded in the capture. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	TArray<FString> EventNames;

	/** Capture-level metadata (platform, build version, changelist, and similar). */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	TMap<FString, FString> Metadata;
};

/**
 * Reduced aggregates for one stat over a frame range. Percentiles are nearest-rank: each is an
 * actually observed per-frame value, not interpolated.
 */
USTRUCT(BlueprintType)
struct FCsvProfilerStatReduction
{
	GENERATED_BODY()

	/** The stat this reduction is for. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	FString StatName;

	/** Mean of the per-frame values over the range. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	double Average = 0.0;

	/** Smallest per-frame value over the range. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	float Min = 0.f;

	/** Largest per-frame value over the range. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	float Max = 0.f;

	/** Sum of the per-frame values over the range. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	double Total = 0.0;

	/** 50th percentile (median) of the per-frame values. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	float P50 = 0.f;

	/** 90th percentile of the per-frame values. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	float P90 = 0.f;

	/** 95th percentile of the per-frame values. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	float P95 = 0.f;

	/** 99th percentile of the per-frame values. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	float P99 = 0.f;
};

/** Reduced aggregates for a set of stats, keyed by stat name. */
USTRUCT(BlueprintType)
struct FCsvProfilerReduceResult
{
	GENERATED_BODY()

	/** One reduction per stat that was present in the capture, keyed by stat name. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	TMap<FString, FCsvProfilerStatReduction> Stats;
};

/** Per-frame values for a single stat over a frame range. */
USTRUCT(BlueprintType)
struct FCsvProfilerStatValues
{
	GENERATED_BODY()

	/** The stat these values are for. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	FString StatName;

	/** Inclusive first frame of the returned window. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	int32 FrameStart = 0;

	/** Exclusive last frame of the returned window. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	int32 FrameEnd = 0;

	/** Per-frame values over [FrameStart, FrameEnd). */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	TArray<float> Values;
};

/** One recorded event with the frame index at which it fired, and how many times it fired there. */
USTRUCT(BlueprintType)
struct FCsvProfilerEventEntry
{
	GENERATED_BODY()

	/** Event name. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	FString Name;

	/** Frame index the event was recorded at. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	int32 Frame = 0;

	/**
	 * Number of times this event name fired on this frame. Repeats of the same name within one frame
	 * are collapsed into a single entry carrying this count.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	int32 Count = 1;
};

/** Events recorded in a capture, in file order, each with its frame index. */
USTRUCT(BlueprintType)
struct FCsvProfilerEventList
{
	GENERATED_BODY()

	/** Recorded events with their frame indices, capped at the returned count. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	TArray<FCsvProfilerEventEntry> Events;

	/** How many distinct event entries matched the range before the cap; exceeds Events.Num() when the result was truncated. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	int32 ConsideredCount = 0;
};

/** The top-ranked stats in a capture, each with its reduced aggregates. */
USTRUCT(BlueprintType)
struct FCsvProfilerTopStatsResult
{
	GENERATED_BODY()

	/** The ranked stats, highest first. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	TArray<FCsvProfilerStatReduction> Stats;

	/** How many stats were ranked before the top-N cut. */
	UPROPERTY(BlueprintReadOnly, Category = "CsvProfilerToolset")
	int32 ConsideredCount = 0;
};

/**
 * Reads CSV profiler captures (text .csv or compressed .csv.bin) named by a virtual content path, and
 * reduces them to bounded summaries: stat and event names, per-stat aggregates over a frame range, a
 * per-frame value window, and the event list. Raw capture bytes are never returned, so large captures
 * can be inspected without their contents entering model context.
 */
UCLASS(MinimalAPI)
class UCsvProfilerToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Surveys a capture without returning per-frame values. StatGroups (the group histogram) is always
	 * populated; individual stat names only when StatNameFilter is set.
	 *
	 * GroupDepth groups the histogram by the first N segments of the '/'-delimited stat tree: depth 1
	 * buckets under "Exclusive", depth 2 under "Exclusive/GameThread".
	 *
	 * @param Path                Virtual content path to a .csv or .csv.bin capture (e.g. '/Saved/Profiling/CSV/Foo.csv').
	 * @param StatNameFilter      Wildcard ('*'/'?'); matching stat names are returned. Empty returns no names; "*" returns all.
	 * @param bIncludeAllMetadata Return every metadata field instead of the curated subset.
	 * @param GroupDepth          Stat-tree levels to group by (>= 1); values below 1 are treated as 1.
	 * @return Survey of the capture.
	 */
	UFUNCTION(meta = (AICallable), Category = "CsvProfilerToolset")
	static FCsvProfilerInfoResult GetCsvInfo(
		const FString& Path,
		const FString& StatNameFilter = TEXT(""),
		bool bIncludeAllMetadata = false,
		int32 GroupDepth = 8);

	/**
	 * Reduces each requested stat to average, min, max, total, and the P50/P90/P95/P99 percentiles over
	 * a frame-index range. FrameEnd -1 runs to the end; the defaults reduce over the whole run. Stats not
	 * present in the capture are omitted from the result.
	 *
	 * @param Path        Virtual content path to a .csv or .csv.bin capture (e.g. '/Saved/Profiling/CSV/Foo.csv').
	 * @param StatNames   Stat names to reduce.
	 * @param FrameStart  Inclusive first frame.
	 * @param FrameEnd    Exclusive last frame, or -1 to run to the end.
	 * @return One reduction per stat found, keyed by stat name.
	 */
	UFUNCTION(meta = (AICallable), Category = "CsvProfilerToolset")
	static FCsvProfilerReduceResult ReduceStat(
		const FString& Path,
		const TArray<FString>& StatNames,
		int32 FrameStart = 0,
		int32 FrameEnd = -1);

	/**
	 * Returns up to Count per-frame values of one stat, starting at FrameStart. An over-large Count
	 * clamps to the stat's own length rather than erroring; Count = -1 reads to the end of the column.
	 *
	 * @param Path        Virtual content path to a .csv or .csv.bin capture (e.g. '/Saved/Profiling/CSV/Foo.csv').
	 * @param StatName    Single stat to read.
	 * @param FrameStart  Inclusive first frame to read from.
	 * @param Count       Maximum frames to return from FrameStart; -1 reads to the end of the column.
	 * @return The value window.
	 */
	UFUNCTION(meta = (AICallable), Category = "CsvProfilerToolset")
	static FCsvProfilerStatValues ReadStatValues(
		const FString& Path,
		const FString& StatName,
		int32 FrameStart = 0,
		int32 Count = 256);

	/**
	 * Returns the capture's events with their frame indices. Repeats of the same event name on one
	 * frame collapse into a single entry carrying a Count. FrameEnd -1 runs to the end; the defaults
	 * return the whole run. At most Count events (the first in frame order) are returned.
	 *
	 * @param Path        Virtual content path to a .csv or .csv.bin capture (e.g. '/Saved/Profiling/CSV/Foo.csv').
	 * @param Count       Maximum number of events to return.
	 * @param FrameStart  Inclusive first frame.
	 * @param FrameEnd    Exclusive last frame, or -1 to run to the end.
	 * @return The event list.
	 */
	UFUNCTION(meta = (AICallable), Category = "CsvProfilerToolset")
	static FCsvProfilerEventList ListEvents(
		const FString& Path,
		int32 Count = 256,
		int32 FrameStart = 0,
		int32 FrameEnd = -1);

	/**
	 * Ranks the capture's stats by a reduced aggregate (average, max, total, or a P50/P90/P95/P99
	 * percentile) over a frame range and returns the top Count, each already reduced. A StatNameFilter
	 * wildcard restricts the ranking to a group (e.g. "GPU*"); empty ranks all stats. FrameEnd -1 runs
	 * to the end; the defaults rank the whole run.
	 *
	 * Ranking is by raw aggregate value with no unit normalisation, so an unfiltered ranking mixes unit
	 * domains (byte counters rank alongside millisecond timings).
	 *
	 * @param Path            Virtual content path to a .csv or .csv.bin capture (e.g. '/Saved/Profiling/CSV/Foo.csv').
	 * @param Count           Maximum number of stats to return.
	 * @param RankBy          Which aggregate to rank by: Average, Max, Total, or a P50/P90/P95/P99 percentile.
	 * @param StatNameFilter  Optional wildcard ('*'/'?') restricting which stats are ranked; empty ranks all.
	 * @param FrameStart      Inclusive first frame.
	 * @param FrameEnd        Exclusive last frame, or -1 to run to the end.
	 * @return The top stats by the chosen ranking.
	 */
	UFUNCTION(meta = (AICallable), Category = "CsvProfilerToolset")
	static FCsvProfilerTopStatsResult TopStats(
		const FString& Path,
		int32 Count = 10,
		ECsvProfilerStatRanking RankBy = ECsvProfilerStatRanking::Average,
		const FString& StatNameFilter = TEXT(""),
		int32 FrameStart = 0,
		int32 FrameEnd = -1);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface
};
