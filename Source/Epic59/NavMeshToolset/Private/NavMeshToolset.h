// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NavMeshToolsetTypes.h"
#include "Templates/SubclassOf.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "NavMeshToolset.generated.h"

class AActor;
class ANavigationData;
class APawn;
class UNavAreaBase;
class UWorld;

/**
 * Answers read-only questions about AI navigation meshes in a live session and the level
 * editor: which navmeshes and agents exist, whether they are built, what shaped or carved
 * them, and whether an agent can stand at or travel between points. Tools raise script
 * errors on invalid targets (unknown world, agent, or navigation data).
 */
UCLASS(MinimalAPI, BlueprintType, Hidden)
class UNavMeshToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------- //
	// Discovery
	// ---------------------------------------------------------------- //

	/**
	 * Lists the worlds a navmesh tool call can target (editor, PIE server, PIE clients, game).
	 * @return One entry per Editor/PIE/Game world, best target first; entry 0 is what a null
	 *         World parameter resolves to.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API TArray<FNavMeshToolsetWorldInfo> ListWorlds();

	/**
	 * Reports the state of a world's navigation system.
	 * @param World Target world; null resolves to the best play world.
	 * @param bIncludeNonDefaultProperties Also dump the system's non-default reflected properties.
	 * @return The navigation system report. Raises a script error when the world has no
	 *         navigation system.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetNavSystemInfo GetNavigationSystemInfo(UWorld* World = nullptr, bool bIncludeNonDefaultProperties = false);

	/**
	 * Lists the agent configs a world supports and the live navigation data each one resolves to.
	 * @param World Target world; null resolves to the best play world.
	 * @return One entry per supported agent, in config order.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API TArray<FNavMeshToolsetAgentInfo> ListSupportedAgents(UWorld* World = nullptr);

	/**
	 * Lists the navigation data actors registered with the world's navigation system. Non-Recast
	 * navdata are included and flagged.
	 * @param World Target world; null resolves to the best play world.
	 * @return One entry per navigation data actor, name-sorted.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API TArray<FNavMeshToolsetNavDataSummary> ListNavData(UWorld* World = nullptr);

	/**
	 * Dumps the full generation and query configuration of one navmesh.
	 * @param bIncludeNonDefaultProperties Also dump the navmesh's non-default reflected properties.
	 * @param NavData Navmesh to describe; wins over AgentName when both are set.
	 * @param AgentName Agent whose navmesh to describe; empty targets the main navmesh.
	 * @param World Target world; null resolves to the best play world.
	 * @return The configuration report. Raises a script error for non-Recast navdata.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetNavMeshDetails DescribeNavMesh(bool bIncludeNonDefaultProperties = true, ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	/**
	 * Reports whether navmesh building is up to date and what rebuild work is pending or
	 * suspended. Addressing a navmesh narrows the report to it and adds tile detail (Recast
	 * only); null NavData with empty AgentName sweeps every navdata without tile detail.
	 * @param NavData Navmesh to report in tile detail; wins over AgentName.
	 * @param AgentName Agent whose navmesh to report in tile detail.
	 * @param World Target world; null resolves to the best play world.
	 * @return The build status report.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetBuildStatusReport GetNavMeshBuildStatus(ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	// ---------------------------------------------------------------- //
	// Generation inputs
	// ---------------------------------------------------------------- //

	/**
	 * Lists every registered navigation bounds region and its supported agents. Navmesh only
	 * generates inside these bounds; a missing volume is the most common "no navmesh here".
	 * @param World Target world; null resolves to the best play world.
	 * @return One entry per registered bounds region.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API TArray<FNavMeshToolsetNavBoundsInfo> ListNavBoundsVolumes(UWorld* World = nullptr);

	/**
	 * Lists the nav modifier volumes and components (area carvers) shaping the navmesh.
	 * @param Center Optional world-space filter center; used when Radius is positive.
	 * @param Radius Filter radius in cm; 0 lists modifiers everywhere.
	 * @param MaxResults Result cap, clamped to 256; bTruncated reports when it bit.
	 * @param bIncludeNonDefaultProperties Also dump each modifier's non-default reflected
	 *        properties.
	 * @param World Target world; null resolves to the best play world.
	 * @return Modifier records name-sorted by owner, plus the truncation flag.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetModifierListResult ListNavModifiers(FVector Center = FVector(0.0, 0.0, 0.0), float Radius = 0.f, int32 MaxResults = 256, bool bIncludeNonDefaultProperties = false, UWorld* World = nullptr);

	/**
	 * Queries the navigation octree for the elements contributing geometry or modifiers around a
	 * location: the authoritative answer to "what carved or built the navmesh here".
	 * @param Center World-space query center.
	 * @param Radius Half-extent of the axis-aligned query box, in cm; floored at 1.
	 * @param MaxResults Result cap, clamped to 256; bTruncated reports when it bit.
	 * @param World Target world; null resolves to the best play world.
	 * @return Octree elements overlapping the query box, plus the truncation flag.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetOctreeQueryResult QueryNavOctree(FVector Center, float Radius = 500.f, int32 MaxResults = 64, UWorld* World = nullptr);

	/**
	 * Lists the concrete nav area classes in the process (engine and game); abstract, deprecated,
	 * and superseded classes are skipped. Meta areas (per-agent area pickers) are flagged.
	 * @param World Target world; null resolves to the best play world.
	 * @return One entry per listed class, name-sorted.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API TArray<FNavMeshToolsetNavAreaInfo> ListNavAreas(UWorld* World = nullptr);

	/**
	 * Reports the navigation invokers driving tile generation. On invoker-driven maps navmesh
	 * only exists near a registered invoker.
	 * @param World Target world; null resolves to the best play world.
	 * @return The invoker report; the invoker list caps at 256 with bTruncated set.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetInvokersReport ListNavInvokers(UWorld* World = nullptr);

	/**
	 * Lists nav links from three sources: nav link proxy actors, custom link components, and
	 * links baked into the navmesh itself, including whether an in-mesh link is broken. COST
	 * NOTE: the in-mesh source pays a full debug-geometry gather per visited tile, bounded by a
	 * tile budget; pass a Radius or Source=InMeshLinks to spend that budget where it matters.
	 * @param Center Truncation ranking origin, and filter center when Radius is positive; proxy
	 *        and custom truncation keeps the nearest links, in-mesh truncation is tile-sweep order.
	 * @param Radius Filter radius in cm; 0 lists links everywhere.
	 * @param Source Which sources to gather; a single source spends the whole cap on itself.
	 * @param MaxResults Per-source result cap, clamped to 256.
	 * @param NavData Navmesh whose in-mesh links to inspect; wins over AgentName.
	 * @param AgentName Agent whose navmesh to inspect; empty targets the main navmesh.
	 * @param World Target world; null resolves to the best play world.
	 * @return Link records plus per-source truncation flags.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetNavLinkListResult ListNavLinks(FVector Center = FVector(0.0, 0.0, 0.0), float Radius = 0.f, ENavMeshToolsetLinkSource Source = ENavMeshToolsetLinkSource::All, int32 MaxResults = 256, ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	// ---------------------------------------------------------------- //
	// Location queries
	// ---------------------------------------------------------------- //

	/**
	 * Projects a point onto navigation data: does walkable space exist here, and where exactly.
	 * @param Point World-space point to project.
	 * @param HorizontalExtent XY search extent in cm, applied to both axes; 0 or negative uses
	 *        the navdata default.
	 * @param VerticalExtent Z search extent in cm; 0 or negative uses the navdata default.
	 * @param bAllAgents Project against every supported agent instead of one; overrides NavData
	 *        and AgentName.
	 * @param NavData Navmesh to project onto; wins over AgentName.
	 * @param AgentName Agent whose navmesh to project onto; empty targets the main navmesh.
	 * @param World Target world; null resolves to the best play world.
	 * @return Projection results (one entry, or with bAllAgents one per agent that has live
	 *         navigation data).
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API TArray<FNavMeshToolsetProjectionResult> ProjectPointToNavMesh(FVector Point, float HorizontalExtent = 0.f, float VerticalExtent = 0.f, bool bAllAgents = false, ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	/**
	 * Describes the navmesh polygon nearest a location.
	 * @param Point World-space location.
	 * @param NavData Navmesh to query; wins over AgentName.
	 * @param AgentName Agent whose navmesh to query; empty targets the main navmesh.
	 * @param World Target world; null resolves to the best play world.
	 * @return The polygon description; bValid false when no polygon is near the point.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetPolyInfo DescribePolyAtLocation(FVector Point, ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	/**
	 * Measures the distance from a point to the nearest navmesh wall (walkable-space boundary).
	 * @param Point World-space point (projected onto the navmesh first).
	 * @param MaxDistance Search cap in cm.
	 * @param NavData Navmesh to query; wins over AgentName.
	 * @param AgentName Agent whose navmesh to query; empty targets the main navmesh.
	 * @param World Target world; null resolves to the best play world.
	 * @return Distance and closest wall point; bValid false when the point is off-mesh or the
	 *         distance is exactly zero (indistinguishable from failure).
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetWallDistanceResult FindDistanceToWall(FVector Point, float MaxDistance = 10000.f, ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	// ---------------------------------------------------------------- //
	// Pathing
	// ---------------------------------------------------------------- //

	/**
	 * Finds a path between two points. Area cost overrides and exclusions apply to this query
	 * only; no state is touched.
	 * @param Start Path start, world space.
	 * @param End Path end, world space.
	 * @param AreaCostOverrides Per-area travel cost overrides for this query; pass [] for none.
	 * @param ExcludedAreaClasses Areas excluded from this query; pass [] for none.
	 * @param bAllowPartialPath Accept a best-effort path when the end is unreachable.
	 * @param CostLimit Abort the search above this cost; 0 means no limit.
	 * @param MaxPathPoints Cap on returned path points, clamped to 2..4096; bPointsTruncated
	 *        reports when it bit.
	 * @param NavData Navmesh to path on; wins over AgentName.
	 * @param AgentName Agent whose navmesh to path on; empty targets the main navmesh.
	 * @param World Target world; null resolves to the best play world.
	 * @return The path result.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetPathResult FindPath(FVector Start, FVector End, const TArray<FNavMeshToolsetAreaCostOverride>& AreaCostOverrides, const TArray<TSubclassOf<UNavAreaBase>>& ExcludedAreaClasses, bool bAllowPartialPath = true, float CostLimit = 0.f, int32 MaxPathPoints = 256, ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	/**
	 * Casts a ray across the navmesh surface and reports where walkable space ends.
	 * @param Start Ray start, world space.
	 * @param End Ray end, world space.
	 * @param NavData Navmesh to cast on; wins over AgentName.
	 * @param AgentName Agent whose navmesh to cast on; empty targets the main navmesh.
	 * @param World Target world; null resolves to the best play world.
	 * @return The raycast result.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetRaycastResult RaycastNavMesh(FVector Start, FVector End, ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	/**
	 * Computes the navmesh region reachable from an origin within a path-cost budget,
	 * connectivity respected.
	 * @param Origin World-space origin.
	 * @param MaxCost Path cost budget (equals distance in cm at cost 1).
	 * @param MaxPolyCenters Cap on returned poly centers only (clamped to 4096); the counts and
	 *        bounds stay exact.
	 * @param NavData Navmesh to flood; wins over AgentName.
	 * @param AgentName Agent whose navmesh to flood; empty targets the main navmesh.
	 * @param World Target world; null resolves to the best play world.
	 * @return The reachable-area aggregate; bValid false when the flood query fails.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetReachableAreaResult GetReachableArea(FVector Origin, float MaxCost, int32 MaxPolyCenters = 512, ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	// ---------------------------------------------------------------- //
	// Tiles
	// ---------------------------------------------------------------- //

	/**
	 * Describes the navmesh tile containing a location.
	 * @param Location World-space location.
	 * @param NavData Navmesh to inspect; wins over AgentName.
	 * @param AgentName Agent whose navmesh to inspect; empty targets the main navmesh.
	 * @param World Target world; null resolves to the best play world.
	 * @return The tile report; bValid false when the location maps to no tile coordinate.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetTileInfo GetTileInfo(FVector Location, ANavigationData* NavData = nullptr, FName AgentName = NAME_None, UWorld* World = nullptr);

	// ---------------------------------------------------------------- //
	// Actors on the mesh
	// ---------------------------------------------------------------- //

	/**
	 * Lists pawns as navigation agents, each with the navmesh it resolves to.
	 * @param ClassFilter Only include pawns of this class; null includes all pawns.
	 * @param Center Optional world-space filter center; used when Radius is positive.
	 * @param Radius Filter radius in cm; 0 lists pawns everywhere.
	 * @param MaxResults Result cap, clamped to 256; bTruncated reports when it bit.
	 * @param World Target world; null resolves to the best play world.
	 * @return Agent actor records name-sorted, plus the truncation flag.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetAgentActorListResult ListNavAgentActors(TSubclassOf<APawn> ClassFilter = nullptr, FVector Center = FVector(0.0, 0.0, 0.0), float Radius = 0.f, int32 MaxResults = 256, UWorld* World = nullptr);

	/**
	 * Deep navigation report for one actor: everything navigation-relevant it carries and, when
	 * it follows paths, where it is going.
	 * @param Actor The actor to inspect.
	 * @param bIncludeNonDefaultProperties Also dump each nav modifier component's non-default
	 *        reflected properties.
	 * @return The actor navigation report. Raises a script error when Actor is null.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"))
	static NAVMESHTOOLSET_API FNavMeshToolsetActorNavInfo GetActorNavInfo(AActor* Actor, bool bIncludeNonDefaultProperties = true);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface
};
