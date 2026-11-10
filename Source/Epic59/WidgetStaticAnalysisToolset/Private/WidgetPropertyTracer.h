// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Misc/Guid.h"
#include "UObject/NameTypes.h"

class UWidgetBlueprint;
class UEdGraph;

/**
 * One direct write to a widget's property or function found by graph tracing. ExportedValue is the
 * widget's archetype value, not the write's own expression; empty if no PropertyName or archetype.
 */
struct FTracedPropertyWrite
{
	FName WidgetName;
	FName PropertyName;
	FName FunctionName;
	/** Originating node's NodeGuid; part of the dedup key. */
	FGuid NodeGuid;
	/** True when this write is reachable from PreConstruct or Construct. */
	bool bLifecycleReachable = false;
	/** True when reachable from PreConstruct specifically. */
	bool bPreConstructReachable = false;
	FString ExportedValue;

	/** True when every path to this write passes a branch condition that can't be statically resolved. */
	bool bBranchGated = false;
};

/**
 * Statically traces Blueprint graphs for direct writes to widget variables, classifying each by
 * PreConstruct/Construct reachability and branch-gating.
 *
 * Blind spots: writes through a slot accessor's returned object, and widgets not exposed as variables.
 */
class FWidgetPropertyTracer
{
public:
	static TArray<FTracedPropertyWrite> TraceWrites(const UWidgetBlueprint* WidgetBlueprint);

	/** Collects every graph in WidgetBlueprint, including compiled duplicate graphs GetAllGraphs misses. */
	static void CollectAllGraphs(const UWidgetBlueprint* WidgetBlueprint, TArray<UEdGraph*>& OutGraphs);
};
