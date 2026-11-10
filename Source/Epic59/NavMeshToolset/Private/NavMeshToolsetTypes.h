// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AI/Navigation/NavigationDataResolution.h"
#include "AI/Navigation/NavigationInvokerPriority.h"
#include "AI/Navigation/NavigationTypes.h"
#include "AI/Navigation/NavLinkDefinition.h"
#include "AI/NavigationModifier.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Math/Box.h"
#include "Math/Color.h"
#include "Math/IntPoint.h"
#include "Math/Vector.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "Templates/SubclassOf.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"
#include "UObject/SoftObjectPath.h"

#include "NavMeshToolsetTypes.generated.h"

class AActor;
class ANavigationData;
class UActorComponent;
class UClass;
class UNavArea;
class UNavAreaBase;
class UWorld;

namespace NavMeshToolsetBudgets
{
	/** Ceiling on the caller-tunable MaxResults of list tools. Report structs' bTruncated (or
	 * per-source flags) say when a cap bit; a full-length list with the flag unset is complete. */
	inline constexpr int32 MaxResultsCeiling = 256;

	/** Ceiling on the caller-tunable MaxPolyCenters cap of GetReachableArea's centers array. */
	inline constexpr int32 MaxReachablePolyCenters = 4096;

	/** Cap on the held-path points GetActorNavInfo returns. */
	inline constexpr int32 MaxActorPathPoints = 64;

	/**
	 * Tiles the ListNavLinks in-mesh sweep may visit per call. Each visited tile pays a full
	 * debug-geometry gather (the only API exposing link ValidEnds), so the sweep bounds work,
	 * not results; bInMeshTilesTruncated reports when it bit.
	 */
	inline constexpr int32 MaxLinkSweepTiles = 4096;

}

/** Per-tile build state, mirroring ENavMeshTileBuildState plus Unknown for meshes without a tile state grid. */
UENUM()
enum class ENavMeshToolsetTileBuildState : uint8
{
	/** No tile state grid on this mesh, or the state could not be read. */
	Unknown,

	/** The tile has not been processed, or was un-processed (e.g. a data chunk detached). */
	NotBuilt,

	/** The tile has been PROCESSED; it may legitimately hold zero geometry ("Built == processed",
	 *  never "walkable navmesh exists here"). */
	Built,

	/** The progressive-build FEATURE is disabled on this mesh; says nothing about this tile. */
	Disabled,

	/** The tile coordinate lies outside the mesh's configured extent. */
	OutOfExtent,
};

/** Where a listed nav link record comes from. */
UENUM()
enum class ENavMeshToolsetLinkKind : uint8
{
	ProxySimpleLink,
	ProxySegmentLink,
	CustomLinkComponent,
	InMeshLink,
};

/** Which link sources ListNavLinks gathers; single sources spend the whole result cap. */
UENUM()
enum class ENavMeshToolsetLinkSource : uint8
{
	All,
	ProxyActors,
	CustomLinkComponents,
	InMeshLinks,
};

/** One reflected property captured from a concrete subclass, value exported as text. */
USTRUCT()
struct FNavMeshToolsetReflectedProperty
{
	GENERATED_BODY()

	/** Property name as declared. */
	UPROPERTY()
	FName PropertyName;

	/** Property value exported with ExportText (string by nature, there is no engine type for arbitrary values). */
	UPROPERTY()
	FString Value;

	/** Class in the hierarchy that declares this property. */
	UPROPERTY()
	TObjectPtr<UClass> DeclaringClass;
};

/** One world an MCP client can target; pass World back into other tools' World parameter. */
USTRUCT()
struct FNavMeshToolsetWorldInfo
{
	GENERATED_BODY()

	/** The world object; use as the World parameter of every other tool. */
	UPROPERTY()
	TObjectPtr<UWorld> World;

	/** Short readable world name. */
	UPROPERTY()
	FString WorldName;

	/** Editor, PIE, or Game (other world types are not listed). */
	UPROPERTY()
	TEnumAsByte<EWorldType::Type> WorldType = EWorldType::None;

	/** Net mode; navmesh truth lives on authority worlds. */
	UPROPERTY()
	TEnumAsByte<ENetMode> NetMode = NM_Standalone;

	/** True when this world has authority over game state (server or standalone play world);
	 *  always false for non-play editor worlds, which have no game state. */
	UPROPERTY()
	bool bIsAuthority = false;

	/** True when a navigation system exists on this world. */
	UPROPERTY()
	bool bHasNavigationSystem = false;

	/** Number of navigation data actors registered on this world. */
	UPROPERTY()
	int32 NavDataCount = 0;

	/** Number of player controllers in this world. */
	UPROPERTY()
	int32 PlayerCount = 0;
};

/** Snapshot of the navigation system level state for one world. */
USTRUCT()
struct FNavMeshToolsetNavSystemInfo
{
	GENERATED_BODY()

	/** The world this report describes. */
	UPROPERTY()
	TObjectPtr<UWorld> World;

	/** Concrete navigation system class (e.g. a game-derived system). */
	UPROPERTY()
	TObjectPtr<UClass> NavSystemClass;

	/** The main (default agent) navigation data, if any. */
	UPROPERTY()
	TObjectPtr<ANavigationData> MainNavData;

	/** Names of every supported agent config; use as AgentName in other tools. */
	UPROPERTY()
	TArray<FName> AgentNames;

	/** True while any navigation build lock is active (map changes are discarded while locked). */
	UPROPERTY()
	bool bBuildingLocked = false;

	/** Active build lock reasons, empty when unlocked. */
	UPROPERTY()
	TArray<TEnumAsByte<ENavigationBuildLock::Type>> BuildLockReasons;

	/** True while the navigation octree is locked (map changes are not stored). */
	UPROPERTY()
	bool bOctreeLocked = false;

	/** True when any navigation data needs rebuilding. */
	UPROPERTY()
	bool bNavigationDirty = false;

	/** Number of dirty areas currently pending. */
	UPROPERTY()
	int32 NumDirtyAreas = 0;

	/** Tile build tasks currently running. */
	UPROPERTY()
	int32 NumRunningBuildTasks = 0;

	/** Tile build tasks still queued. */
	UPROPERTY()
	int32 NumRemainingBuildTasks = 0;

	/** True when tiles are generated only around navigation invokers. */
	UPROPERTY()
	bool bGenerateOnlyAroundInvokers = false;

	/** True when tiles outside invoker radii are preserved instead of removed (hybrid invoker mode). */
	UPROPERTY()
	bool bPreserveTilesOutsideInvokers = false;

	/** How nav-relevant geometry is gathered; Invalid when the read missed. */
	UPROPERTY()
	ENavDataGatheringModeConfig DataGatheringMode = ENavDataGatheringModeConfig::Invalid;

	/** Reflected reads that missed (engine drift); the matching fields above hold fallback values. */
	UPROPERTY()
	TArray<FName> UnresolvedProperties;

	/** Non-default properties of the concrete (game-derived) system class, when requested. */
	UPROPERTY()
	TArray<FNavMeshToolsetReflectedProperty> NonDefaultProperties;
};

/** One supported agent config and its resolved live navigation data. */
USTRUCT()
struct FNavMeshToolsetAgentInfo
{
	GENERATED_BODY()

	/** Agent config name; use as AgentName in other tools. */
	UPROPERTY()
	FName AgentName;

	/** Debug color assigned to this agent. */
	UPROPERTY()
	FColor Color = FColor::Black;

	/** Agent capsule radius, in cm. */
	UPROPERTY()
	float AgentRadius = 0.f;

	/** Agent capsule height, in cm. */
	UPROPERTY()
	float AgentHeight = 0.f;

	/** Max step height, in cm; -1 means the navdata default applies. */
	UPROPERTY()
	float AgentStepHeight = -1.f;

	/** True when the agent can walk. */
	UPROPERTY()
	bool bCanWalk = false;

	/** True when the agent can jump. */
	UPROPERTY()
	bool bCanJump = false;

	/** True when the agent can swim. */
	UPROPERTY()
	bool bCanSwim = false;

	/** True when the agent can fly. */
	UPROPERTY()
	bool bCanFly = false;

	/** Navigation data class configured for this agent. */
	UPROPERTY()
	FSoftClassPath NavDataClass;

	/** Default projection extent for queries against this agent's navdata. */
	UPROPERTY()
	FVector DefaultQueryExtent = FVector::ZeroVector;

	/** Live navigation data instance for this agent, null when not created in this world. */
	UPROPERTY()
	TObjectPtr<ANavigationData> ResolvedNavData;
};

/** Summary of one navigation data actor. */
USTRUCT()
struct FNavMeshToolsetNavDataSummary
{
	GENERATED_BODY()

	/** The navigation data actor; use as the NavData parameter of other tools. */
	UPROPERTY()
	TObjectPtr<ANavigationData> NavData;

	/** Readable actor name. */
	UPROPERTY()
	FString NavDataName;

	/** Concrete class (e.g. a game-derived navmesh). */
	UPROPERTY()
	TObjectPtr<UClass> NavDataClass;

	/** Agent config this navdata serves. */
	UPROPERTY()
	FName AgentName;

	/** Runtime generation mode (Static, DynamicModifiersOnly, Dynamic). */
	UPROPERTY()
	ERuntimeGenerationType RuntimeGeneration = ERuntimeGenerationType::Static;

	/** True when this is an ARecastNavMesh (tile and poly tools apply). */
	UPROPERTY()
	bool bIsRecastNavmesh = false;

	/** Total tiles (pool capacity for fixed pools); Recast only. */
	UPROPERTY()
	int32 TileCount = 0;

	/** Tiles with actual data; Recast only. */
	UPROPERTY()
	int32 ActiveTileCount = 0;

	/** World-space bounds of the built navmesh; Recast only. */
	UPROPERTY()
	FBox NavmeshBounds = FBox(ForceInit);

	/** True when in-world debug drawing is enabled for this navdata. */
	UPROPERTY()
	bool bDrawingEnabled = false;

	/** True while rebuilding is suspended (dirty areas queue up). */
	UPROPERTY()
	bool bRebuildingSuspended = false;

	/** Dirty areas queued while suspended. */
	UPROPERTY()
	int32 NumSuspendedDirtyAreas = 0;

	/** True when this is the world's main navigation data. */
	UPROPERTY()
	bool bIsMainNavData = false;
};

/** Per-resolution generation parameters (Low, Default, High). */
USTRUCT()
struct FNavMeshToolsetResolutionParams
{
	GENERATED_BODY()

	/** Which resolution bucket these values belong to. */
	UPROPERTY()
	ENavigationDataResolution Resolution = ENavigationDataResolution::Default;

	/** Voxel cell size (XY), in cm. */
	UPROPERTY()
	float CellSize = 0.f;

	/** Voxel cell height (Z), in cm. */
	UPROPERTY()
	float CellHeight = 0.f;

	/** Max step height at this resolution, in cm. */
	UPROPERTY()
	float AgentMaxStepHeight = 0.f;
};

/** One nav area registered on a navdata, with its runtime costs. */
USTRUCT()
struct FNavMeshToolsetAreaCostEntry
{
	GENERATED_BODY()

	/** The area class. */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> AreaClass;

	/** Area ID assigned by this navdata (0-63 for Recast). */
	UPROPERTY()
	int32 AreaID = -1;

	/** Travel cost multiplier from the navdata's default query filter; 1 when the filter or the
	 *  area ID is unavailable. */
	UPROPERTY()
	float TravelCost = 1.f;

	/** Fixed cost applied when entering the area, from the default query filter; 0 when the
	 *  filter or the area ID is unavailable. */
	UPROPERTY()
	float EnteringCost = 0.f;
};

/** Full generation and query configuration of one navmesh. */
USTRUCT()
struct FNavMeshToolsetNavMeshDetails
{
	GENERATED_BODY()

	/** Identity and live-state summary. */
	UPROPERTY()
	FNavMeshToolsetNavDataSummary Summary;

	/** Tile side length, in cm. */
	UPROPERTY()
	float TileSizeUU = 0.f;

	/** Agent radius used for generation, in cm. */
	UPROPERTY()
	float AgentRadius = 0.f;

	/** Agent height used for generation, in cm. */
	UPROPERTY()
	float AgentHeight = 0.f;

	/** Max walkable slope, in degrees. */
	UPROPERTY()
	float AgentMaxSlope = 0.f;

	/** Per-resolution cell parameters. */
	UPROPERTY()
	TArray<FNavMeshToolsetResolutionParams> Resolutions;

	/** Max contour simplification error, in voxels. */
	UPROPERTY()
	float MaxSimplificationError = 0.f;

	/** Minimum region area in voxels; smaller regions are discarded. */
	UPROPERTY()
	float MinRegionArea = 0.f;

	/** Regions below this size get merged, in voxels. */
	UPROPERTY()
	float MergeRegionSize = 0.f;

	/** Hard cap on tile count (drives poly ref bit layout). */
	UPROPERTY()
	int32 TileNumberHardLimit = 0;

	/** True when the tile pool is fixed-size. */
	UPROPERTY()
	bool bFixedTilePoolSize = false;

	/** Tile pool size when fixed. */
	UPROPERTY()
	int32 TilePoolSize = 0;

	/** True when this navmesh is world-partitioned. */
	UPROPERTY()
	bool bIsWorldPartitioned = false;

	/** True when generated nav links (jump links) are enabled. */
	UPROPERTY()
	bool bGenerateNavLinks = false;

	/** Number of configured generated-jump-link configs. */
	UPROPERTY()
	int32 NumGeneratedJumpLinkConfigs = 0;

	/** Reflected reads that missed (engine drift); the matching fields above hold fallback values. */
	UPROPERTY()
	TArray<FName> UnresolvedProperties;

	/** A* heuristic scale for this navmesh's queries. */
	UPROPERTY()
	float HeuristicScale = 0.f;

	/** Default max A* search nodes per query. */
	UPROPERTY()
	float DefaultMaxSearchNodes = 0.f;

	/** Every registered area with its ID and default filter costs. */
	UPROPERTY()
	TArray<FNavMeshToolsetAreaCostEntry> SupportedAreas;

	/** Non-default properties of the concrete subclass, when requested. */
	UPROPERTY()
	TArray<FNavMeshToolsetReflectedProperty> NonDefaultProperties;
};

/** One dirty area pending navmesh rebuild processing. */
USTRUCT()
struct FNavMeshToolsetDirtyAreaInfo
{
	GENERATED_BODY()

	/** World-space bounds of the dirty region. */
	UPROPERTY()
	FBox Bounds = FBox(ForceInit);

	/** Raw ENavigationDirtyFlag bits. */
	UPROPERTY()
	int32 RawFlags = 0;

	/** What made the area dirty, when the source element is known. */
	UPROPERTY()
	FString SourceDescription;
};

/** Progressive-build (adaptive coverage) statistics for one Recast navmesh. */
USTRUCT()
struct FNavMeshToolsetAdaptiveBuildStats
{
	GENERATED_BODY()

	/** True when progressive build is active and the stats below are meaningful. */
	UPROPERTY()
	bool bAvailable = false;

	/** Total tiles tracked by the tile state grid. */
	UPROPERTY()
	int32 GridTileCount = 0;

	/** Tiles already built. */
	UPROPERTY()
	int32 BuiltCount = 0;

	/** Tiles not built yet. */
	UPROPERTY()
	int32 NotBuiltCount = 0;

	/** Tiles waiting on the generator queue. */
	UPROPERTY()
	int32 PendingCount = 0;

	/** Tiles with submitted build tasks. */
	UPROPERTY()
	int32 BuildingCount = 0;

	/** Active tiles (inside invoker radii). */
	UPROPERTY()
	int32 ActiveCount = 0;

	/** Passive preserved tiles (outside invoker radii, preserve-tiles mode). */
	UPROPERTY()
	int32 PassiveCount = 0;

	/** Dirty tiles deferred because they are passive. */
	UPROPERTY()
	int32 DeferredDirtyCount = 0;

	/** True when preserve-tiles (hybrid invoker) mode is on. */
	UPROPERTY()
	bool bPreserveTiles = false;
};

/** Build status for one navigation data actor. */
USTRUCT()
struct FNavMeshToolsetNavDataBuildStatus
{
	GENERATED_BODY()

	/** The navigation data this status describes. */
	UPROPERTY()
	TObjectPtr<ANavigationData> NavData;

	/** Agent config this navdata serves. */
	UPROPERTY()
	FName AgentName;

	/** True while rebuilding is suspended. */
	UPROPERTY()
	bool bRebuildingSuspended = false;

	/** Dirty areas queued while suspended. */
	UPROPERTY()
	int32 NumSuspendedDirtyAreas = 0;

	/** Lifetime total of tile updates on this navmesh (churn indicator). */
	UPROPERTY()
	int64 TotalUpdatedTilesCount = 0;

	/** Progressive-build statistics, when the feature is active. */
	UPROPERTY()
	FNavMeshToolsetAdaptiveBuildStats AdaptiveStats;

	/** True when the tile-detail fields below were filled (the call addressed this navmesh). */
	UPROPERTY()
	bool bTileDetailIncluded = false;

	/** Total tiles (pool capacity for fixed pools); tile detail only. */
	UPROPERTY()
	int32 TileCount = 0;

	/** Tiles with actual data; tile detail only. */
	UPROPERTY()
	int32 ActiveTileCount = 0;

	/** True when the tile pool is fixed-size; tile detail only. */
	UPROPERTY()
	bool bFixedTilePoolSize = false;

	/** Tile pool size when fixed; tile detail only. */
	UPROPERTY()
	int32 TilePoolSize = 0;

	/** World-space bounds of the built navmesh; tile detail only. */
	UPROPERTY()
	FBox NavmeshBounds = FBox(ForceInit);

	/** True when a tile state grid exists (progressive build); tile detail only. */
	UPROPERTY()
	bool bHasTileStateGrid = false;

	/** Tile grid extent minimum coordinate; tile detail only. */
	UPROPERTY()
	FIntPoint GridMin = FIntPoint::ZeroValue;

	/** Tile grid extent maximum coordinate; tile detail only. */
	UPROPERTY()
	FIntPoint GridMax = FIntPoint::ZeroValue;

	/** Reflected reads that missed (engine drift); the matching fields above hold fallback values. */
	UPROPERTY()
	TArray<FName> UnresolvedProperties;
};

/** World-level navmesh build health report. */
USTRUCT()
struct FNavMeshToolsetBuildStatusReport
{
	GENERATED_BODY()

	/** True while any tile build is in progress. */
	UPROPERTY()
	bool bBuildInProgress = false;

	/** Dirty areas currently pending (drained every navigation tick). */
	UPROPERTY()
	int32 NumDirtyAreas = 0;

	/** Tile build tasks currently running. */
	UPROPERTY()
	int32 NumRunningBuildTasks = 0;

	/** Tile build tasks still queued. */
	UPROPERTY()
	int32 NumRemainingBuildTasks = 0;

	/** The pending dirty areas, capped; the list drains every tick unless rebuilding is suspended or locked. */
	UPROPERTY()
	TArray<FNavMeshToolsetDirtyAreaInfo> PendingDirtyAreas;

	/** True when PendingDirtyAreas hit its cap. */
	UPROPERTY()
	bool bPendingDirtyAreasTruncated = false;

	/** Per-navdata build status. */
	UPROPERTY()
	TArray<FNavMeshToolsetNavDataBuildStatus> PerNavData;
};

/** One registered navigation bounds region. */
USTRUCT()
struct FNavMeshToolsetNavBoundsInfo
{
	GENERATED_BODY()

	/** World-space bounds. */
	UPROPERTY()
	FBox Bounds = FBox(ForceInit);

	/** The bounds volume actor, when the bounds came from one. */
	UPROPERTY()
	TObjectPtr<AActor> VolumeActor;

	/** Agent configs these bounds apply to. */
	UPROPERTY()
	TArray<FName> SupportedAgentNames;

	/** Package name of the level the bounds belong to. */
	UPROPERTY()
	FName LevelName;
};

/** One area modifier carried by a modifier volume, component, or octree element. */
USTRUCT()
struct FNavMeshToolsetAreaModifierInfo
{
	GENERATED_BODY()

	/** Area class the modifier applies. */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> AreaClass;

	/** Area class being replaced, for Replace-mode modifiers. */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> AreaClassToReplace;

	/** Modifier shape. */
	UPROPERTY()
	TEnumAsByte<ENavigationShapeType::Type> Shape = ENavigationShapeType::Unknown;

	/** How the area applies. */
	UPROPERTY()
	TEnumAsByte<ENavigationAreaMode::Type> ApplyMode = ENavigationAreaMode::Apply;

	/** World-space bounds of the modifier shape. */
	UPROPERTY()
	FBox Bounds = FBox(ForceInit);
};

/** One nav modifier source (volume actor or component). */
USTRUCT()
struct FNavMeshToolsetModifierInfo
{
	GENERATED_BODY()

	/** Owning actor. */
	UPROPERTY()
	TObjectPtr<AActor> OwnerActor;

	/** The modifier component, null when the source is a modifier volume actor. */
	UPROPERTY()
	TObjectPtr<UActorComponent> Component;

	/** True for ANavModifierVolume sources. */
	UPROPERTY()
	bool bIsVolume = false;

	/** True when currently relevant to navigation (dynamic modifiers can toggle this). */
	UPROPERTY()
	bool bNavigationRelevant = false;

	/** Area class applied by this source. */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> AreaClass;

	/** Area class being replaced, when set. */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> AreaClassToReplace;

	/** World-space bounds of the modifier. */
	UPROPERTY()
	FBox Bounds = FBox(ForceInit);

	/** Non-default properties of the concrete subclass (surfaces game-derived tuning). */
	UPROPERTY()
	TArray<FNavMeshToolsetReflectedProperty> NonDefaultProperties;
};

/** ListNavModifiers result. */
USTRUCT()
struct FNavMeshToolsetModifierListResult
{
	GENERATED_BODY()

	/** Modifier records; the first MaxResults by owner name when truncated. */
	UPROPERTY()
	TArray<FNavMeshToolsetModifierInfo> Modifiers;

	/** True when more modifiers exist than MaxResults returned. */
	UPROPERTY()
	bool bTruncated = false;
};

/** One navigation octree element contributing geometry or modifiers at a location. */
USTRUCT()
struct FNavMeshToolsetOctreeElementInfo
{
	GENERATED_BODY()

	/** Source object (actor or component), when still resolvable. */
	UPROPERTY()
	TObjectPtr<UObject> SourceObject;

	/** Readable description of the source element. */
	UPROPERTY()
	FString SourceDescription;

	/** Element bounds. */
	UPROPERTY()
	FBox Bounds = FBox(ForceInit);

	/** True when the element carries collision geometry for the navmesh. */
	UPROPERTY()
	bool bHasGeometry = false;

	/** True when the element carries area modifiers. */
	UPROPERTY()
	bool bHasModifiers = false;

	/** True when the element's modifiers are dynamic (re-evaluated at runtime). */
	UPROPERTY()
	bool bDynamicModifiers = false;

	/** The element's area modifiers. */
	UPROPERTY()
	TArray<FNavMeshToolsetAreaModifierInfo> AreaModifiers;
};

/** QueryNavOctree result. */
USTRUCT()
struct FNavMeshToolsetOctreeQueryResult
{
	GENERATED_BODY()

	/** Octree elements overlapping the query box. */
	UPROPERTY()
	TArray<FNavMeshToolsetOctreeElementInfo> Elements;

	/** True when more elements overlapped than MaxResults returned. */
	UPROPERTY()
	bool bTruncated = false;
};

/** Area ID assignment of one area class on one navdata. */
USTRUCT()
struct FNavMeshToolsetAreaIdOnNavData
{
	GENERATED_BODY()

	/** The navdata assigning the ID. */
	UPROPERTY()
	TObjectPtr<ANavigationData> NavData;

	/** Assigned area ID, -1 when the navdata does not register this area. */
	UPROPERTY()
	int32 AreaID = -1;
};

/** One nav area class (UNavAreaBase subclass) known to the process. Cost, color, flag, and
 *  agent fields hold their defaults for classes that do not derive UNavArea. */
USTRUCT()
struct FNavMeshToolsetNavAreaInfo
{
	GENERATED_BODY()

	/** The area class. */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> AreaClass;

	/** Default travel cost multiplier. */
	UPROPERTY()
	float DefaultCost = 1.f;

	/** Fixed cost applied on entering the area. */
	UPROPERTY()
	float FixedEnteringCost = 0.f;

	/** Debug draw color. */
	UPROPERTY()
	FColor DrawColor = FColor::Black;

	/** Agent configs that support this area. */
	UPROPERTY()
	TArray<FName> SupportedAgentNames;

	/** Raw area flags. */
	UPROPERTY()
	int32 AreaFlags = 0;

	/** True for meta areas that pick a concrete area per agent at generation time. */
	UPROPERTY()
	bool bIsMetaArea = false;

	/** Area ID assignments per navdata in the target world. */
	UPROPERTY()
	TArray<FNavMeshToolsetAreaIdOnNavData> AreaIds;
};

/** One navigation invoker (tile generation driver). */
USTRUCT()
struct FNavMeshToolsetInvokerInfo
{
	GENERATED_BODY()

	/** Invoker owner actor, when the invoker is actor-based and alive. */
	UPROPERTY()
	TObjectPtr<AActor> Actor;

	/** Readable invoker name (covers interface-based invokers with no actor). */
	UPROPERTY()
	FString InvokerName;

	/** Current invoker location. */
	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** True when Location is valid this frame. */
	UPROPERTY()
	bool bLocationValid = false;

	/** Radius within which tiles are generated, in cm. */
	UPROPERTY()
	float GenerationRadius = 0.f;

	/** Radius beyond which tiles are removed, in cm. */
	UPROPERTY()
	float RemovalRadius = 0.f;

	/** Agent configs this invoker generates tiles for. */
	UPROPERTY()
	TArray<FName> SupportedAgentNames;

	/** Invoker priority. */
	UPROPERTY()
	ENavigationInvokerPriority Priority = ENavigationInvokerPriority::Default;
};

/** Active-tile count of one navdata (invoker-driven generation). */
USTRUCT()
struct FNavMeshToolsetActiveTilesOnNavData
{
	GENERATED_BODY()

	/** The navdata. */
	UPROPERTY()
	TObjectPtr<ANavigationData> NavData;

	/** Agent config this navdata serves. */
	UPROPERTY()
	FName AgentName;

	/** Tiles with actual data (same counter the navdata summaries report). */
	UPROPERTY()
	int32 ActiveTileCount = 0;

	/** Tile side length, in cm. */
	UPROPERTY()
	float TileSizeUU = 0.f;
};

/** World-level invoker report. */
USTRUCT()
struct FNavMeshToolsetInvokersReport
{
	GENERATED_BODY()

	/** True when tiles are generated only around invokers. */
	UPROPERTY()
	bool bInvokerOnlyGeneration = false;

	/** Every registered invoker. */
	UPROPERTY()
	TArray<FNavMeshToolsetInvokerInfo> Invokers;

	/** True when Invokers hit the result cap. */
	UPROPERTY()
	bool bTruncated = false;

	/** Seed bounds constraining invoker-driven generation, when configured. */
	UPROPERTY()
	TArray<FBox> SeedBounds;

	/** Active tile counts per navdata. */
	UPROPERTY()
	TArray<FNavMeshToolsetActiveTilesOnNavData> ActiveTiles;
};

/** One nav link record (proxy actor, custom component, or in-mesh link). */
USTRUCT()
struct FNavMeshToolsetNavLinkInfo
{
	GENERATED_BODY()

	/** Where this record comes from. */
	UPROPERTY()
	ENavMeshToolsetLinkKind Kind = ENavMeshToolsetLinkKind::InMeshLink;

	/** Owning actor, when known. */
	UPROPERTY()
	TObjectPtr<AActor> OwnerActor;

	/** Owning component for component-based links. */
	UPROPERTY()
	TObjectPtr<UActorComponent> Component;

	/** Link start, world space. */
	UPROPERTY()
	FVector Start = FVector::ZeroVector;

	/** Link end, world space. */
	UPROPERTY()
	FVector End = FVector::ZeroVector;

	/** Traversal direction. */
	UPROPERTY()
	TEnumAsByte<ENavLinkDirection::Type> Direction = ENavLinkDirection::BothWays;

	/** Area class stamped by this link (enabled-state area for custom links). */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> AreaClass;

	/** Area class used while a custom link is disabled. */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> DisabledAreaClass;

	/** True when the link is currently enabled (custom links). */
	UPROPERTY()
	bool bEnabled = true;

	/** Link user id as hex (uint64 does not survive JSON doubles). Empty when none. */
	UPROPERTY()
	FString LinkId;

	/** In-mesh links: true when both ends landed on navmesh; false means the link is broken. */
	UPROPERTY()
	bool bEndsValid = true;
};

/**
 * ListNavLinks result. Each gathered source caps and reports truncation independently, so the
 * in-mesh links (the only source not visible in the editor outliner, and the only one carrying
 * bEndsValid) can never be starved by authored proxies and components filling a shared cap.
 */
USTRUCT()
struct FNavMeshToolsetNavLinkListResult
{
	GENERATED_BODY()

	/** Link records from every gathered source. */
	UPROPERTY()
	TArray<FNavMeshToolsetNavLinkInfo> Links;

	/** True when more proxy actor links exist than the cap returned; the returned subset is the
	 *  nearest N to the query Center (the world origin when none was given). */
	UPROPERTY()
	bool bProxyLinksTruncated = false;

	/** True when more custom link components exist than the cap returned; the returned subset is
	 *  the nearest N to the query Center (the world origin when none was given). */
	UPROPERTY()
	bool bCustomLinksTruncated = false;

	/** True when more in-mesh links exist than the cap returned; the returned subset is arbitrary
	 *  (tile sweep order; gathering all to sort would defeat the tile budget). */
	UPROPERTY()
	bool bInMeshLinksTruncated = false;

	/** True when the in-mesh tile sweep hit its tile budget before visiting every tile. */
	UPROPERTY()
	bool bInMeshTilesTruncated = false;

	/** True when the in-mesh sweep actually RAN. False means zero in-mesh links proves nothing;
	 *  InMeshSkipReason says why. */
	UPROPERTY()
	bool bInMeshLinksGathered = false;

	/** Why the in-mesh source was not gathered, when bInMeshLinksGathered is false. */
	UPROPERTY()
	FString InMeshSkipReason;
};

/** Result of projecting one point onto navigation data. */
USTRUCT()
struct FNavMeshToolsetProjectionResult
{
	GENERATED_BODY()

	/** True when the point projected onto the navmesh. */
	UPROPERTY()
	bool bProjected = false;

	/** The input point. */
	UPROPERTY()
	FVector InputPoint = FVector::ZeroVector;

	/** The projected location on the navmesh. */
	UPROPERTY()
	FVector ProjectedPoint = FVector::ZeroVector;

	/** Containing poly ref as hex (uint64 does not survive JSON doubles); empty when the
	 *  projected location carries no poly ref. */
	UPROPERTY()
	FString PolyRef;

	/** Navdata the point projected onto. */
	UPROPERTY()
	TObjectPtr<ANavigationData> NavData;

	/** Agent config used. */
	UPROPERTY()
	FName AgentName;
};

/** Everything known about one navmesh polygon. */
USTRUCT()
struct FNavMeshToolsetPolyInfo
{
	GENERATED_BODY()

	/** True when a polygon was found at the queried location. */
	UPROPERTY()
	bool bValid = false;

	/** Poly ref as hex (uint64 does not survive JSON doubles). */
	UPROPERTY()
	FString PolyRef;

	/** Area class of the polygon. */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> AreaClass;

	/** Recast area ID; -1 until bValid. */
	UPROPERTY()
	int32 AreaID = -1;

	/** Debug color of the area. */
	UPROPERTY()
	FColor AreaColor = FColor::Black;

	/** Raw poly flags. */
	UPROPERTY()
	int32 PolyFlags = 0;

	/** Polygon vertices, world space. */
	UPROPERTY()
	TArray<FVector> Verts;

	/** Polygon surface area, in cm2. */
	UPROPERTY()
	float SurfaceArea = 0.f;

	/** True when the owning tile fields below were resolved; tile coordinates are signed, so no
	 *  sentinel value can mark failure. */
	UPROPERTY()
	bool bTileResolved = false;

	/** Owning tile X coordinate; only meaningful when bTileResolved. */
	UPROPERTY()
	int32 TileX = 0;

	/** Owning tile Y coordinate; only meaningful when bTileResolved. */
	UPROPERTY()
	int32 TileY = 0;

	/** Owning tile layer; -1 when the layer could not be resolved (layers are never negative). */
	UPROPERTY()
	int32 TileLayer = INDEX_NONE;

	/** True when the polygon is a nav link. */
	UPROPERTY()
	bool bIsNavLink = false;

	/** Neighbor poly refs as hex. */
	UPROPERTY()
	TArray<FString> NeighborPolyRefs;
};

/** Distance from a point to the nearest navmesh wall. */
USTRUCT()
struct FNavMeshToolsetWallDistanceResult
{
	GENERATED_BODY()

	/** True when a positive wall distance was found; false when the point is off-mesh or the
	 *  distance is exactly zero (indistinguishable from failure). */
	UPROPERTY()
	bool bValid = false;

	/** Distance to the nearest wall, in cm. */
	UPROPERTY()
	float Distance = 0.f;

	/** Closest point on the wall. */
	UPROPERTY()
	FVector ClosestWallPoint = FVector::ZeroVector;
};

/** Per-area travel cost override for counterfactual queries. */
USTRUCT()
struct FNavMeshToolsetAreaCostOverride
{
	GENERATED_BODY()

	/** Area class to override. */
	UPROPERTY()
	TSubclassOf<UNavAreaBase> AreaClass;

	/** Travel cost multiplier to use for this query. */
	UPROPERTY()
	float Cost = 1.f;
};

/** One point along a found path. */
USTRUCT()
struct FNavMeshToolsetPathPoint
{
	GENERATED_BODY()

	/** Point location, world space. */
	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** Poly ref at this point as hex, empty when unavailable. */
	UPROPERTY()
	FString PolyRef;

	/** Custom link id as hex when this point traverses one, empty otherwise. */
	UPROPERTY()
	FString CustomLinkId;
};

/** Result of one pathfinding query. */
USTRUCT()
struct FNavMeshToolsetPathResult
{
	GENERATED_BODY()

	/** Query outcome. */
	UPROPERTY()
	TEnumAsByte<ENavigationQueryResult::Type> Result = ENavigationQueryResult::Invalid;

	/** True when the path is partial (did not reach the requested end). */
	UPROPERTY()
	bool bIsPartial = false;

	/** Path length, in cm. */
	UPROPERTY()
	float PathLength = 0.f;

	/** Path cost under the query filter. */
	UPROPERTY()
	float PathCost = 0.f;

	/** String-pulled path points. */
	UPROPERTY()
	TArray<FNavMeshToolsetPathPoint> PathPoints;

	/** True when PathPoints hit the result cap. */
	UPROPERTY()
	bool bPointsTruncated = false;

	/** Number of polys in the path corridor. */
	UPROPERTY()
	int32 CorridorPolyCount = 0;

	/** The requested start. */
	UPROPERTY()
	FVector RequestedStart = FVector::ZeroVector;

	/** The requested end. */
	UPROPERTY()
	FVector RequestedEnd = FVector::ZeroVector;

	/** Where the path actually ends (differs from RequestedEnd for partial paths). */
	UPROPERTY()
	FVector EffectiveEnd = FVector::ZeroVector;

	/** Navdata used. */
	UPROPERTY()
	TObjectPtr<ANavigationData> NavData;

	/** Agent config used. */
	UPROPERTY()
	FName AgentName;
};

/** Result of a navmesh raycast. */
USTRUCT()
struct FNavMeshToolsetRaycastResult
{
	GENERATED_BODY()

	/** True when the ray hit a navmesh wall before reaching the end. */
	UPROPERTY()
	bool bHit = false;

	/** Hit location (equals the ray end when nothing was hit). */
	UPROPERTY()
	FVector HitLocation = FVector::ZeroVector;

	/** Only meaningful when bHit is false: true when the ray end's projection lies at the end of
	 *  the explored corridor; false when the end failed to project onto the navmesh or landed on
	 *  a node outside the corridor (e.g. a different height). Mirrors the engine's
	 *  FNavigationRaycastAdditionalResults contract. */
	UPROPERTY()
	bool bIsRayEndInCorridor = false;
};

/** Aggregate description of the area reachable from an origin within a cost budget. */
USTRUCT()
struct FNavMeshToolsetReachableAreaResult
{
	GENERATED_BODY()

	/** True when the flood query ran. */
	UPROPERTY()
	bool bValid = false;

	/** Exact number of polys reachable within the cost budget (never capped). */
	UPROPERTY()
	int32 PolyCount = 0;

	/** Exact total reachable surface area over every reachable poly, in cm2. */
	UPROPERTY()
	double TotalSurfaceArea = 0.0;

	/** Exact bounds of the full reachable poly set. */
	UPROPERTY()
	FBox ReachableBounds = FBox(ForceInit);

	/** Poly centers, capped at MaxPolyCenters; the counts and bounds above stay exact. */
	UPROPERTY()
	TArray<FVector> PolyCenters;

	/** True when the reachable set holds more polys than the centers cap; PolyCenters can also
	 *  fall short of the cap when a poly's center read fails. */
	UPROPERTY()
	bool bPolyCentersTruncated = false;
};

/** One layer of one navmesh tile. Fields keep their defaults when the underlying tile read
 *  fails; there is no per-field validity flag. */
USTRUCT()
struct FNavMeshToolsetTileLayerInfo
{
	GENERATED_BODY()

	/** Layer index at this tile coordinate. */
	UPROPERTY()
	int32 Layer = 0;

	/** Polys in this layer. */
	UPROPERTY()
	int32 PolyCount = 0;

	/** Tile bounds for this layer. */
	UPROPERTY()
	FBox Bounds = FBox(ForceInit);

	/** Tile resolution bucket. */
	UPROPERTY()
	ENavigationDataResolution Resolution = ENavigationDataResolution::Default;
};

/** Everything known about one tile coordinate. */
USTRUCT()
struct FNavMeshToolsetTileInfo
{
	GENERATED_BODY()

	/** True when the location resolved to a tile coordinate; every other field is meaningless
	 *  when false ((0,0) is a real tile on most meshes, so do not read the defaults as one). */
	UPROPERTY()
	bool bValid = false;

	/** Tile X coordinate. */
	UPROPERTY()
	int32 TileX = 0;

	/** Tile Y coordinate. */
	UPROPERTY()
	int32 TileY = 0;

	/** Tile coordinate bounds (from the navmesh layout, valid even when unbuilt). */
	UPROPERTY()
	FBox CoordBounds = FBox(ForceInit);

	/** Build state (progressive-build meshes; Unknown otherwise). */
	UPROPERTY()
	ENavMeshToolsetTileBuildState BuildState = ENavMeshToolsetTileBuildState::Unknown;

	/** True when this coordinate is in the active tile set (invoker-driven meshes). */
	UPROPERTY()
	bool bInActiveSet = false;

	/** Built layers at this coordinate (empty when unbuilt). */
	UPROPERTY()
	TArray<FNavMeshToolsetTileLayerInfo> Layers;
};

/** One nav-relevant agent actor in the world. */
USTRUCT()
struct FNavMeshToolsetAgentActorInfo
{
	GENERATED_BODY()

	/** The actor. */
	UPROPERTY()
	TObjectPtr<AActor> Actor;

	/** Concrete actor class. */
	UPROPERTY()
	TObjectPtr<UClass> ActorClass;

	/** Actor nav-agent location. */
	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	/** Agent radius from the actor's nav agent properties, in cm. */
	UPROPERTY()
	float AgentRadius = 0.f;

	/** Agent height from the actor's nav agent properties, in cm. */
	UPROPERTY()
	float AgentHeight = 0.f;

	/** Navdata resolved for this actor's agent properties. */
	UPROPERTY()
	TObjectPtr<ANavigationData> NavData;
};

/** ListNavAgentActors result. */
USTRUCT()
struct FNavMeshToolsetAgentActorListResult
{
	GENERATED_BODY()

	/** Pawn records; the first MaxResults by name when truncated. */
	UPROPERTY()
	TArray<FNavMeshToolsetAgentActorInfo> Pawns;

	/** True when more matching pawns exist than MaxResults returned. */
	UPROPERTY()
	bool bTruncated = false;
};

/** Deep navigation report for one actor. */
USTRUCT()
struct FNavMeshToolsetActorNavInfo
{
	GENERATED_BODY()

	/** Agent identity and resolved navdata. */
	UPROPERTY()
	FNavMeshToolsetAgentActorInfo AgentInfo;

	/** Nav modifier components on the actor. */
	UPROPERTY()
	TArray<FNavMeshToolsetModifierInfo> ModifierComponents;

	/** Navigation invoker components on the actor. */
	UPROPERTY()
	TArray<FNavMeshToolsetInvokerInfo> InvokerComponents;

	/** Nav link components and proxy point links on the actor. */
	UPROPERTY()
	TArray<FNavMeshToolsetNavLinkInfo> Links;

	/** True when a path following component was found on the actor or its pawn's controller. */
	UPROPERTY()
	bool bHasPathFollowing = false;

	/** Path following status, when present. */
	UPROPERTY()
	TEnumAsByte<EPathFollowingStatus::Type> PathFollowingStatus = EPathFollowingStatus::Idle;

	/** First MaxActorPathPoints (64) points of the path the component currently holds; a
	 *  retained (not live) path when PathFollowingStatus is not Moving. */
	UPROPERTY()
	TArray<FVector> CurrentPathPoints;

	/** True when the path holds more points than CurrentPathPoints returned; MoveGoal is still
	 *  the true destination beyond the last returned point. */
	UPROPERTY()
	bool bCurrentPathPointsTruncated = false;

	/** Destination of the held path; a movement in progress only while PathFollowingStatus is
	 *  Moving. */
	UPROPERTY()
	FVector MoveGoal = FVector::ZeroVector;

	/** Reflected reads that failed to resolve (engine drift); their fields hold defaults, not data. */
	UPROPERTY()
	TArray<FName> UnresolvedProperties;
};
