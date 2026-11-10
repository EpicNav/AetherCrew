// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_RECAST

#include "AI/Navigation/NavQueryFilter.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
// RecastNavMesh.h stays out of the widely-included tool and types headers (heavy); only this
// resolver surface consumes ENavMeshTileBuildState directly, so it lives here.
#include "NavMesh/RecastNavMesh.h"
#include "NavMeshToolsetTypes.h"
#include "Templates/SubclassOf.h"
#include "UObject/NameTypes.h"

class ANavLinkProxy;
class ANavModifierVolume;
class UNavAreaBase;
class UNavigationSystemV1;
class UNavLinkCustomComponent;
class UNavModifierComponent;
class UWorld;
struct FNavAgentSelector;
struct FNavigationLink;

namespace NavMeshToolset
{
	/**
	 * Reports a tool failure on BOTH channels: an unconditional LogNavMeshToolset Error line and
	 * a script error for the MCP/script path. RaiseScriptError alone is silent when no script
	 * frame exists (console commands, direct C++ calls) and compiles out entirely in cooked Test,
	 * so every tool error must route through here.
	 */
	void RaiseToolError(const FString& Message);

	/**
	 * Resolves the world a tool call targets. A non-null input is validated (Editor/PIE/Game
	 * only); null resolves by priority: authority PIE/Game world, any PIE world, any Game world,
	 * then the editor world. Raises a script error and returns null when nothing matches.
	 */
	UWorld* ResolveWorld(UWorld* World);

	/**
	 * Resolves the world, then its UNavigationSystemV1. Raises a script error and returns null
	 * when the world resolves but has no navigation system.
	 */
	UNavigationSystemV1* ResolveNavSystem(UWorld* World);

	/**
	 * Resolves the navigation data a tool call targets. An explicit NavData wins (validated to
	 * live in the nav system's world); otherwise AgentName resolves through the supported-agent
	 * registry; otherwise the main navigation data. Raises a script error and returns null on
	 * unknown agents (listing the valid names) or when no navigation data exists.
	 */
	ANavigationData* ResolveNavData(UNavigationSystemV1& NavSys, ANavigationData* NavData, FName AgentName);

	/**
	 * ResolveNavData narrowed to ARecastNavMesh. Raises a script error naming the actual class
	 * and pointing at the generic tools when the resolved navdata is not a Recast navmesh.
	 */
	ARecastNavMesh* ResolveRecastNavMesh(UNavigationSystemV1& NavSys, ANavigationData* NavData, FName AgentName);

	/**
	 * Builds the query filter for one call: the navdata's default filter when no overrides are
	 * given, else a copy with per-area travel costs and exclusions applied. Raises a script error
	 * and returns an invalid filter on any failure (null or unregistered area class, missing
	 * default filter); the unregistered-area error lists the navdata's supported areas.
	 */
	FSharedConstNavQueryFilter BuildQueryFilter(
		const ANavigationData& NavData,
		const TArray<FNavMeshToolsetAreaCostOverride>& CostOverrides,
		const TArray<TSubclassOf<UNavAreaBase>>& ExcludedAreas);

	/**
	 * Captures reflected properties whose value differs from the class default object, exported
	 * as text and capped in length. Deprecated properties are skipped and fixed-size array
	 * properties are compared by their first element only. Surfaces game-derived tuning without
	 * game dependencies. Results are name-sorted.
	 */
	void DumpNonDefaultProperties(const UObject& Object, TArray<FNavMeshToolsetReflectedProperty>& OutProperties);

	/** Returns the agent config name a navdata serves (from its FNavDataConfig). */
	FName GetAgentNameForNavData(const ANavigationData& NavData);

	/** Expands an agent selector bitmask into the matching supported-agent names. */
	TArray<FName> GetAgentNamesFromSelector(const UNavigationSystemV1& NavSys, const FNavAgentSelector& Selector);

	/** Builds the shared navdata summary (identity, generation mode, tile counts, live state). */
	FNavMeshToolsetNavDataSummary BuildNavDataSummary(const UNavigationSystemV1& NavSys, ANavigationData& NavData);

	/** Fills one modifier record from a nav modifier component; Bounds is its navigation bounds. */
	void FillModifierEntry(UNavModifierComponent& Component, const FBox& Bounds, bool bIncludeNonDefaultProperties, FNavMeshToolsetModifierInfo& OutEntry);

	/** Fills one modifier record from a nav modifier volume actor; Bounds is its component bounding box. */
	void FillModifierEntry(ANavModifierVolume& Volume, const FBox& Bounds, bool bIncludeNonDefaultProperties, FNavMeshToolsetModifierInfo& OutEntry);

	/** Fills one link record from a custom nav link component. */
	void FillCustomLinkEntry(UNavLinkCustomComponent& LinkComponent, FNavMeshToolsetNavLinkInfo& OutEntry);

	/** Fills one link record from a nav link proxy point link; Start/End are its transformed endpoints. */
	void FillProxyPointLinkEntry(ANavLinkProxy& Proxy, const FNavigationLink& PointLink, const FVector& Start, const FVector& End, FNavMeshToolsetNavLinkInfo& OutEntry);

	/** Copies a navmesh's progressive-build statistics; bAvailable false when the feature is off. */
	void FillAdaptiveStats(const ARecastNavMesh& Navmesh, FNavMeshToolsetAdaptiveBuildStats& OutStats);

	/** Maps the engine per-tile build state onto the toolset mirror enum. */
	ENavMeshToolsetTileBuildState ConvertTileBuildState(ENavMeshTileBuildState EngineState);

	/**
	 * Reads a protected enum/byte/int UPROPERTY by name; for values with no public accessor.
	 * A missing or unsupported-type property (engine drift) returns the fallback AND records the
	 * name in OutUnresolvedProperties when provided, so consumers can tell "unknown" from a value.
	 */
	int64 ReadReflectedEnumValue(const UObject& Object, FName PropertyName, int64 FallbackValue, TArray<FName>* OutUnresolvedProperties = nullptr);

	/** Reads a protected bool UPROPERTY (including bitfields) by name; misses report like ReadReflectedEnumValue. */
	bool ReadReflectedBoolValue(const UObject& Object, FName PropertyName, bool bFallbackValue, TArray<FName>* OutUnresolvedProperties = nullptr);

	/** Reads the element count of a protected array UPROPERTY by name; misses report like ReadReflectedEnumValue. */
	int32 ReadReflectedArrayNum(const UObject& Object, FName PropertyName, TArray<FName>* OutUnresolvedProperties = nullptr);

	/** Formats a poly ref (or any 64-bit handle) as 0x-prefixed hex for JSON-safe transport. */
	FString PolyRefToHex(uint64 PolyRef);
}

#endif
