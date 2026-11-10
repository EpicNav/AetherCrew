// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "AsyncFunctionResult/AsyncFunctionResultJson.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "StatsToolset.generated.h"

/**
 * Async result for UStatsToolset::GetFrameTree - a captured stat frame as
 * a nested tree of {Name, InclusiveMs, ExclusiveMs, Children?} nodes.
 */
UCLASS(BlueprintType, MinimalAPI)
class UStatsFrameTreeResult : public UAsyncFunctionResultJson
{
	GENERATED_BODY()

public:
	virtual TSharedRef<FJsonObject> MakeJsonSchema() const override;
};

/**
 * Async result for UStatsToolset::GetStatCounters - a flat list of
 * {Name, Value, Unit} rows sampled from a single stat group.
 */
UCLASS(BlueprintType, MinimalAPI)
class UStatCountersResult : public UAsyncFunctionResultJson
{
	GENERATED_BODY()

public:
	virtual TSharedRef<FJsonObject> MakeJsonSchema() const override;
};

/**
 * Snapshot of per-frame timings - the same numbers `stat unit` displays.
 * Compare per-thread and GPU times against FrameMs to identify which
 * subsystem is gating frame rate.
 */
USTRUCT(BlueprintType)
struct FFrameTimings
{
	GENERATED_BODY()

	/** Total frame time in ms (wall-clock between frames). */
	UPROPERTY(BlueprintReadOnly, Category = "StatsToolset")
	float FrameMs = 0.f;

	/** Game thread ms — actor ticks, blueprint VM, gameplay code. */
	UPROPERTY(BlueprintReadOnly, Category = "StatsToolset")
	float GameThreadMs = 0.f;

	/** Render thread (draw thread) ms — culling, draw call submission. */
	UPROPERTY(BlueprintReadOnly, Category = "StatsToolset")
	float RenderThreadMs = 0.f;

	/** RHI thread ms. 0 with single-threaded RHI. */
	UPROPERTY(BlueprintReadOnly, Category = "StatsToolset")
	float RhiThreadMs = 0.f;

	/** GPU frame ms — time executing submitted work on the GPU. */
	UPROPERTY(BlueprintReadOnly, Category = "StatsToolset")
	float GpuMs = 0.f;
};

/**
 * CPU performance telemetry from Unreal's stats system. Provides high-level
 * frame timings (game/render/RHI/GPU thread costs), a captured per-scope
 * timing tree for the latest frame, and per-group counter/memory samples.
 */
UCLASS(BlueprintType, MinimalAPI)
class UStatsToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Reads the engine's per-frame timing globals once (no smoothing - usually
	 * the previous frame).
	 *
	 * @return Wall-clock frame time plus per-thread and GPU times for the most
	 *         recently completed frame.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"), Category = "StatsToolset")
	static FFrameTimings GetFrameTimings();

	/**
	 * Captures the latest stat frame as a nested tree. Siblings sorted
	 * most-expensive first. Top-level entries are typically thread roots
	 * (GameThread, RenderThread). On leaves, ExclusiveMs equals InclusiveMs.
	 *
	 * @param MinInclusiveMs  Drop nodes (and subtrees) below this. 0 = full tree.
	 * @param MaxDepth        Maximum tree depth to emit - value clamped to [1,120] and counts from the tree root down
	 * @param PathFilter      Case-insensitive substring. Only subtrees rooted at a matching node are emitted; ancestors are kept as context. Empty = no filter.
	 * @return Async result that completes with the captured tree.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"), Category = "StatsToolset")
	static UStatsFrameTreeResult* GetFrameTree(float MinInclusiveMs = 0.05f, int32 MaxDepth = 32, const FString& PathFilter = TEXT(""));

	/**
	 * Enumerates registered stat groups. Names are STATGROUP_-prefixed and
	 * match the argument accepted by GetStatCounters.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"), Category = "StatsToolset")
	static TArray<FString> ListStatGroups();

	/**
	 * Samples every non-cycle counter in a stat group for a single frame:
	 * integer/float counters and memory pool sizes (draw calls, primitive
	 * counts, LLM tags, etc).
	 *
	 * @param GroupName     STATGROUP_-prefixed group name, e.g. "STATGROUP_SceneRendering".
	 * @param bIncludeZero  Include counters whose sampled value is 0.
	 * @return Async result completing with the counter list.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"), Category = "StatsToolset")
	static UStatCountersResult* GetStatCounters(const FString& GroupName, bool bIncludeZero = false);
};
