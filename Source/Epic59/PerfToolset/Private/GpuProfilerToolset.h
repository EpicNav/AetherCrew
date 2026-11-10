// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"

#include "AsyncFunctionResult/AsyncFunctionResultJson.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "GpuProfilerToolset.generated.h"

/**
 * Async result for UGpuProfilerToolset::TriggerGPUProfile — captured GPU
 * work for a rendered frame as a nested tree of {Name, InclusiveMs,
 * Children?} pass nodes.
 */
UCLASS(BlueprintType, MinimalAPI)
class UGpuProfileResult : public UAsyncFunctionResultJson
{
	GENERATED_BODY()

public:
	virtual TSharedRef<FJsonObject> MakeJsonSchema() const override;
};

/**
 * Per-pass GPU profiler. Captures the GPU work executed in a rendered frame
 * as a nested tree of render passes, so callers can locate which pass or
 * material is gating GPU cost.
 */
UCLASS(BlueprintType, MinimalAPI)
class UGpuProfilerToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Captures the next rendered frame's GPU work as a nested tree (2-3 frame
	 * latency). Top-level entries are GPU queues (Graphics, Async Compute);
	 * each contains its passes as a recursive subtree.
	 *
	 * @param MinInclusiveMs  Drop passes (and subtrees) below this. 0 = full tree.
	 * @return Async result that completes with the captured tree, or an empty
	 *         array if GPU profiling isn't compiled in.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"), Category = "GpuProfilerToolset")
	static UGpuProfileResult* TriggerGPUProfile(float MinInclusiveMs = 0.05f);

	/**
	 * Parse the JSON-array payload emitted by FProfile::AppendJsonEntries
	 * (RHI/Private/GPUProfiler.cpp) into a nested JSON tree.
	 *
	 * Source schema per entry:
	 *   { "EventName": "...", "FullPath": "...", "GPUDurationMs": float,
	 *     "Depth": int, "Queue": "..." }
	 * Source entries are pre-order DFS within each queue; multiple queues
	 * are concatenated in the source array.
	 *
	 * MinInclusiveMs culls during the walk: any Depth>=1 node below the
	 * floor is dropped along with its subtree. Queue roots (Depth==0) are
	 * preserved as anchors.
	 *
	 * Returns an FJsonValueArray of queue-root FJsonValueObjects. Never null;
	 * returns an empty array if the source can't be deserialized.
	 *
	 * Public (not UFUNCTION) solely so the unit spec can reach it; not part
	 * of the agent-facing toolset API.
	 */
	static TSharedPtr<FJsonValue> BuildGpuProfileJsonTree(const FString& SourceJson, float MinInclusiveMs);
};
