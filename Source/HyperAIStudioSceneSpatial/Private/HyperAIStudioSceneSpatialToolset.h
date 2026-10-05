// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioSceneSpatialToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAISceneCostRequest
{
	GENERATED_BODY()

	/** editor (the default) or pie: the running Play In Editor world. */
	UPROPERTY() FString World;
	/** level (the default: every loaded actor), box (what touches BoxMin to BoxMax, cm) or actors (Actors). */
	UPROPERTY() FString Scope;
	UPROPERTY() FVector BoxMin = FVector::ZeroVector;
	UPROPERTY() FVector BoxMax = FVector::ZeroVector;
	/** Scope actors: labels or object paths, at most 64. */
	UPROPERTY() TArray<FString> Actors;
	/** Count actors hidden in the editor too. */
	UPROPERTY() bool bIncludeHidden = false;
	/** Heaviest actors and meshes listed: 15 by default, 1 to 50. */
	UPROPERTY() int32 TopN = 15;
	/** Actors read before the report stops early: 20000 by default, 100 to 100000. */
	UPROPERTY() int32 MaxActors = 20000;
	/** Milliseconds before the report stops early: 1500 by default, 100 to 5000. */
	UPROPERTY() int32 DeadlineMs = 1500;
	/** The answer's size: lists are cut from the tail to fit. 32768 by default, 4096 to 131072. */
	UPROPERTY() int32 MaxOutputBytes = 32768;
};

USTRUCT(BlueprintType)
struct FHyperAISceneCostEntry
{
	GENERATED_BODY()

	/** An actor's label, or a mesh's path. */
	UPROPERTY() FString Name;
	UPROPERTY() FString Class;
	/**
	 * Meshes: one instance's triangles (LOD0, or the Nanite source mesh). Actors: their non-Nanite triangles (static
	 * LOD0 times instances, skeletal LOD0, landscape).
	 */
	UPROPERTY() int64 Triangles = 0;
	UPROPERTY() int32 Instances = 0;
	/** Meshes: Triangles times Instances. Actors: Triangles plus their Nanite source triangles. */
	UPROPERTY() int64 TotalTriangles = 0;
	/** Estimated: a non-Nanite component draws once per mesh section, instances batched; Nanite adds none. */
	UPROPERTY() int32 DrawCalls = 0;
	UPROPERTY() int32 Sections = 0;
	UPROPERTY() bool bNanite = false;
	UPROPERTY() bool bCastsShadow = false;
	UPROPERTY() int32 Materials = 0;
	/** Actors: where they stand. Meshes: how many components use them. */
	UPROPERTY() FString Location;
};

USTRUCT(BlueprintType)
struct FHyperAISceneCostLights
{
	GENERATED_BODY()

	UPROPERTY() int32 Directional = 0;
	UPROPERTY() int32 Point = 0;
	UPROPERTY() int32 Spot = 0;
	UPROPERTY() int32 Rect = 0;
	UPROPERTY() int32 Sky = 0;
	UPROPERTY() int32 Movable = 0;
	UPROPERTY() int32 Stationary = 0;
	UPROPERTY() int32 Static = 0;
	/** Movable or stationary lights that cast dynamic shadows. */
	UPROPERTY() int32 ShadowCastingDynamic = 0;
	UPROPERTY() int32 MegaLights = 0;
};

USTRUCT(BlueprintType)
struct FHyperAISceneCostReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, invalid_request, no_editor_world, no_pie_world. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString LevelPath;
	UPROPERTY() FString World;
	UPROPERTY() FString Scope;
	/** The report stopped early or lists were cut: TruncatedReason says why (max_actors, deadline_reached, max_output_bytes). */
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString TruncatedReason;
	UPROPERTY() int32 ActorsScanned = 0;
	UPROPERTY() int32 ElapsedMs = 0;

	/** Actors in the scope; for a box, those with something drawn inside it. */
	UPROPERTY() int32 Actors = 0;
	UPROPERTY() int32 PrimitiveComponents = 0;
	/** Static mesh components that are not instanced. */
	UPROPERTY() int32 StaticMeshComponents = 0;
	/** Instanced and hierarchical instanced static mesh components, foliage's included. */
	UPROPERTY() int32 InstancedComponents = 0;
	/** Mesh instances drawn: one per static mesh component plus every instance of the instanced ones. */
	UPROPERTY() int32 Instances = 0;
	UPROPERTY() int32 SkeletalMeshComponents = 0;
	UPROPERTY() int32 LandscapeComponents = 0;
	UPROPERTY() int32 OtherPrimitives = 0;
	UPROPERTY() int32 UniqueMeshes = 0;
	UPROPERTY() int32 NanitePrimitives = 0;
	UPROPERTY() int32 NonNanitePrimitives = 0;

	/** Static mesh LOD0 triangles times instances, for meshes drawn without Nanite: the worst case, before LOD. */
	UPROPERTY() int64 TrianglesNonNanite = 0;
	/** Nanite source triangles times instances: what was imported, not what Nanite draws. */
	UPROPERTY() int64 TrianglesNaniteSource = 0;
	UPROPERTY() int64 TrianglesSkeletal = 0;
	UPROPERTY() int64 TrianglesLandscape = 0;
	/** Estimates: one draw per mesh section per non-Nanite component (one per landscape component), and the same again per dynamic shadow-casting light that reaches it. */
	UPROPERTY() int32 EstimatedDrawCalls = 0;
	UPROPERTY() int32 EstimatedShadowDrawCalls = 0;
	/** Distinct materials on Nanite meshes, each a shading bin. */
	UPROPERTY() int32 NaniteMaterialBins = 0;
	UPROPERTY() int32 ShadowCastingPrimitives = 0;

	UPROPERTY() FHyperAISceneCostLights Lights;

	UPROPERTY() int32 TranslucentMaterials = 0;
	UPROPERTY() int32 TranslucentPrimitives = 0;
	UPROPERTY() int32 MaskedNanitePrimitives = 0;
	UPROPERTY() bool bLandscapeNanite = false;
	UPROPERTY() int32 FoliageInstances = 0;
	UPROPERTY() int32 PCGInstances = 0;
	UPROPERTY() int32 PCGComponents = 0;

	/**
	 * What the last frame some viewport rendered actually drew (stat unit's figures), -1 when none rendered. It may
	 * be another view than this scope, or stale while the level viewport is hidden.
	 */
	UPROPERTY() int32 MeasuredLastFrameDrawCalls = -1;
	UPROPERTY() int32 MeasuredLastFramePrimitives = -1;

	/** World Partition: actors the level has, and how many are loaded (in the box, for box scope). */
	UPROPERTY() bool bWorldPartition = false;
	UPROPERTY() int32 WPActorDescs = 0;
	UPROPERTY() int32 WPLoadedActorDescs = 0;

	UPROPERTY() TArray<FHyperAISceneCostEntry> HeaviestActors;
	UPROPERTY() TArray<FHyperAISceneCostEntry> HeaviestMeshes;
	/** material | primitives using it. */
	UPROPERTY() TArray<FString> TranslucentList;
	/** Other primitive classes: class xN. */
	UPROPERTY() TArray<FString> OtherClasses;
	/** code: message. */
	UPROPERTY() TArray<FString> Findings;
};

USTRUCT(BlueprintType)
struct FHyperAISpatialQuery
{
	GENERATED_BODY()

	/** trace, sweep, overlap, ground, clear_spot, measure or nearest. */
	UPROPERTY() FString Kind;
	/** trace, sweep: from Start to End (cm). measure: End is the second point when ActorB is empty. */
	UPROPERTY() FVector Start = FVector::ZeroVector;
	UPROPERTY() FVector End = FVector::ZeroVector;
	/** overlap, ground, clear_spot, nearest: the point asked about. measure: the first point when ActorA is empty. */
	UPROPERTY() FVector Point = FVector::ZeroVector;
	/** sweep, overlap: box (the default), sphere or capsule. */
	UPROPERTY() FString Shape;
	/**
	 * box: half extents; sphere: radius in X; capsule: radius in X and half height in Z. clear_spot: the half extents
	 * of what must fit.
	 */
	UPROPERTY() FVector Extent = FVector::ZeroVector;
	UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;
	/** A collision channel by name: Visibility for trace, sweep and ground by default, WorldDynamic for overlap and clear_spot. */
	UPROPERTY() FString Channel;
	/** trace, sweep, overlap: object types (WorldStatic, WorldDynamic, Pawn, PhysicsBody...) to look for instead of a channel. */
	UPROPERTY() TArray<FString> ObjectTypes;
	UPROPERTY() bool bTraceComplex = false;
	/** trace, sweep: every hit along the way (up to 16), not only the first. */
	UPROPERTY() bool bMulti = false;
	/** Actors passed through, by label or path. */
	UPROPERTY() TArray<FString> IgnoreActors;
	/** Pass through foliage and PCG-generated instances. */
	UPROPERTY() bool bIgnoreFoliage = false;
	/** measure: two actors by label or path; the points Point and End stand in for them when empty. */
	UPROPERTY() FString ActorA;
	UPROPERTY() FString ActorB;
	/** nearest: how far from Point to look, cm; 5000 by default. */
	UPROPERTY() float Radius = 0.f;
	/** nearest: the actor's class (short name or path), a tag it has, text its label contains. */
	UPROPERTY() FString ClassFilter;
	UPROPERTY() FString TagFilter;
	UPROPERTY() FString LabelFilter;
	/** overlap, nearest: at most this many actors; 8 by default, 1 to 32. */
	UPROPERTY() int32 MaxResults = 8;
	/** clear_spot: cm kept free around Extent. */
	UPROPERTY() float Clearance = 10.f;
	/** clear_spot: cm between search rings; 0 sizes them to Extent. */
	UPROPERTY() float Step = 0.f;
	/** clear_spot: the farthest from Point a spot may be, cm. */
	UPROPERTY() float MaxRadius = 2000.f;
	/** clear_spot: the steepest ground a spot may be on, degrees. */
	UPROPERTY() float MaxSlope = 35.f;
	/** clear_spot: spots stand on the ground below (true), or at Point's height. */
	UPROPERTY() bool bSnapToGround = true;
	/** clear_spot: what counts as in the way: collision (the default: what blocks Channel), bounds (any actor's bounds) or both. */
	UPROPERTY() FString Mode;
};

USTRUCT(BlueprintType)
struct FHyperAISpatialHit
{
	GENERATED_BODY()

	/** The actor's label. */
	UPROPERTY() FString Actor;
	UPROPERTY() FString ActorPath;
	UPROPERTY() FString Component;
	/** An instanced mesh's instance index; -1 otherwise. */
	UPROPERTY() int32 Item = -1;
	UPROPERTY() FString PhysMaterial;
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FVector Normal = FVector::ZeroVector;
	UPROPERTY() float Distance = 0.f;
	UPROPERTY() float SlopeDegrees = 0.f;
	UPROPERTY() bool bStartPenetrating = false;
	/** overlap: it blocks the channel; false when it only overlaps, as a trigger does. */
	UPROPERTY() bool bBlocking = true;
};

USTRUCT(BlueprintType)
struct FHyperAISpatialActor
{
	GENERATED_BODY()

	UPROPERTY() FString Label;
	UPROPERTY() FString Path;
	UPROPERTY() FString Class;
	UPROPERTY() FVector Location = FVector::ZeroVector;
	/** From the query's point to the actor's bounds, cm. */
	UPROPERTY() float Distance = 0.f;
	/** The bounds' size, cm. */
	UPROPERTY() FVector Size = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct FHyperAISpatialResult
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	/** trace, sweep: something blocked; overlap: something blocks there; ground, clear_spot: found. */
	UPROPERTY() bool bHit = false;
	UPROPERTY() TArray<FHyperAISpatialHit> Hits;
	/**
	 * trace, sweep: where the line, or the shape's center, comes to rest (End when nothing blocks); ground: the surface
	 * point; clear_spot: the middle of the spot's floor, where what must fit stands.
	 */
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;
	UPROPERTY() float GroundZ = 0.f;
	/** ground: Point's height above the surface (negative when buried). */
	UPROPERTY() float HeightAboveGround = 0.f;
	UPROPERTY() FVector Normal = FVector::ZeroVector;
	UPROPERTY() float SlopeDegrees = 0.f;
	/** measure: between the two (cm), on the ground plane, and B's height above A. */
	UPROPERTY() float Distance = 0.f;
	UPROPERTY() float Horizontal = 0.f;
	UPROPERTY() float Vertical = 0.f;
	/** measure: the free space between the two bounds; 0 when they touch or overlap. */
	UPROPERTY() float Gap = 0.f;
	/** clear_spot: spots tried, and why the others were turned down (blocked:N, no_ground:N, too_steep:N). */
	UPROPERTY() int32 Candidates = 0;
	UPROPERTY() FString Rejected;
	UPROPERTY() TArray<FHyperAISpatialActor> Nearest;
	/** nearest: World Partition actors within Radius that are not loaded, and so were not looked at. */
	UPROPERTY() int32 UnloadedNearby = 0;
};

USTRUCT(BlueprintType)
struct FHyperAISpatialQueryRequest
{
	GENERATED_BODY()

	/** 1 to 32, answered in order. */
	UPROPERTY() TArray<FHyperAISpatialQuery> Queries;
	/** editor (the default) or pie. */
	UPROPERTY() FString World;
	/** Milliseconds for every query together: 1000 by default, 100 to 5000. */
	UPROPERTY() int32 DeadlineMs = 1000;
	/** 32768 by default, 4096 to 131072. */
	UPROPERTY() int32 MaxOutputBytes = 32768;
};

USTRUCT(BlueprintType)
struct FHyperAISpatialQueryReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, invalid_request, no_editor_world, no_pie_world. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString LevelPath;
	UPROPERTY() FString World;
	UPROPERTY() TArray<FHyperAISpatialResult> Results;
	/** Hits or actors were cut to fit MaxOutputBytes, or the deadline left queries unanswered. */
	UPROPERTY() bool bTruncated = false;
	/** A landscape is still merging its edit layers, so its collision, and the ground found, may lag behind. */
	UPROPERTY() bool bLandscapePending = false;
};

USTRUCT(BlueprintType)
struct FHyperAISpatialOp
{
	GENERATED_BODY()

	/** drop_to_ground, move_to_clear_spot or place. */
	UPROPERTY() FString Kind;
	/** drop_to_ground, move_to_clear_spot: the actors, by label or path; at most 256 across the plan. */
	UPROPERTY() TArray<FString> Actors;
	/** place: the static mesh asset (/Game/Props/SM_Rock). */
	UPROPERTY() FString Mesh;
	/** place: the new actor's label, unique in the level. */
	UPROPERTY() FString Label;
	/** place: its Outliner folder (Props/Rocks); none when empty. */
	UPROPERTY() FString Folder;
	/** place: where its pivot goes (cm), how it turns, and its scale (1 when zero). */
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FRotator Rotation = FRotator::ZeroRotator;
	UPROPERTY() FVector Scale = FVector::ZeroVector;
	/** place: dropped onto the ground below Location as drop_to_ground drops actors. */
	UPROPERTY() bool bDropToGround = false;
	/** drop: tilted to the ground's normal, at most MaxAlignDegrees; upright otherwise. */
	UPROPERTY() bool bAlignToNormal = false;
	UPROPERTY() float MaxAlignDegrees = 30.f;
	/** drop: cm pushed into the ground, so bases do not float. */
	UPROPERTY() float Sink = 0.f;
	/** drop: center (one trace under the middle; the default) or corners (also under the four corners; the highest wins). */
	UPROPERTY() FString Samples;
	/** drop: how far below the actor to look for ground, cm; 50000 by default. */
	UPROPERTY() float TraceDistance = 50000.f;
	/** The collision channel ground is found on; Visibility by default. */
	UPROPERTY() FString Channel;
	/** Actors the ground traces pass through, by label or path. */
	UPROPERTY() TArray<FString> IgnoreActors;
	/** Foliage and PCG-generated instances count as ground; by default traces pass through them, so a rock never lands on a tree. */
	UPROPERTY() bool bIncludeFoliage = false;
	/** move_to_clear_spot: search around this point (cm); around each actor's own place when zero. */
	UPROPERTY() FVector Near = FVector::ZeroVector;
	UPROPERTY() float MaxRadius = 2000.f;
	/** move_to_clear_spot: cm between search rings; 0 sizes them to the actor. */
	UPROPERTY() float Step = 0.f;
	/** move_to_clear_spot: cm kept free around the actor. */
	UPROPERTY() float Clearance = 10.f;
	UPROPERTY() bool bSnapToGround = true;
	UPROPERTY() float MaxSlope = 35.f;
};

USTRUCT(BlueprintType)
struct FHyperAISpatialPlanRequest
{
	GENERATED_BODY()

	/** 1 to 32, each computed against the level as it was before the plan. */
	UPROPERTY() TArray<FHyperAISpatialOp> Operations;
	/** Only check the plan and return the new transforms: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Save the level afterwards. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait; made when empty. */
	UPROPERTY() FString OperationId;
	/** Ground traces pass through the plan's other actors, so the plan never stacks them on each other. */
	UPROPERTY() bool bIgnoreEachOther = true;
};

USTRUCT(BlueprintType)
struct FHyperAISpatialPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/**
	 * checked, completed, awaiting_user_approval, approval_not_queued, invalid_request, no_ground, no_clear_spot,
	 * play_in_editor_running, operation_failed, save_failed.
	 */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() int32 Applied = 0;
	/** Label: (x, y, z) -> (x, y, z), rot pitch/yaw/roll -> pitch/yaw/roll; a placed actor says where it goes. */
	UPROPERTY() TArray<FString> Moves;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};

/**
 * Scene cost and spatial placement: what the loaded level costs to render, read-only spatial questions (traces,
 * sweeps, overlaps, ground, clear spots, distances, nearest actors), and actors dropped to the ground, moved to clear
 * spots or placed from meshes in one undo step.
 */
UCLASS()
class HYPERAISTUDIOSCENESPATIAL_API UHyperAIStudioSceneSpatialToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Rendering cost of the loaded level, a box or named actors: triangles (Nanite source, non-Nanite LOD0, skeletal,
	 * landscape), estimated draw and shadow draw calls, instances per mesh (ISM, foliage, PCG), shadow casters, lights
	 * by type and mobility, translucent materials, World Partition loaded share, heaviest actors and meshes, and the
	 * last frame's measured draw calls.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Level Design")
	static FHyperAISceneCostReport hyper_scene_cost_report(const FHyperAISceneCostRequest& Request);

	/**
	 * Up to 32 read-only spatial questions in one call, editor or PIE world: line traces and box, sphere or capsule
	 * sweeps (first or all hits, by channel or object type), overlap tests, ground height and slope under a point, a
	 * clear spot of a given size near a point, distance and gap between actors or points, nearest actors by class, tag
	 * or label.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Level Design")
	static FHyperAISpatialQueryReport hyper_spatial_query(const FHyperAISpatialQueryRequest& Request);

	/**
	 * Places and moves actors in one undo step: drop to the ground (bounds bottom on the surface, optionally tilted to
	 * its normal, sunk a few cm), move to the nearest clear spot, or place a static mesh as a new labelled actor.
	 * Foliage and PCG instances are not ground unless asked. bDryRun returns the new transforms; locked, attached or
	 * unloaded actors are refused by name.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Level Design")
	static FHyperAISpatialPlanReport hyper_spatial_apply_plan(const FHyperAISpatialPlanRequest& Request);
};

class FHyperAIStudioSceneSpatialContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("level_design");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioscenespatialtoolset.v1");
	static constexpr const TCHAR* CostToolName = TEXT("hyper_scene_cost_report");
	static constexpr const TCHAR* QueryToolName = TEXT("hyper_spatial_query");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_spatial_apply_plan");
	static constexpr int32 MaxQueries = 32;
	static constexpr int32 MaxOperations = 32;
	/** Actors one plan may move, across its operations. */
	static constexpr int32 MaxPlanActors = 256;
	static constexpr int32 MaxScopeActors = 64;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAISceneCostReport CostReport(const FHyperAISceneCostRequest& Request);
	static FHyperAISpatialQueryReport Query(const FHyperAISpatialQueryRequest& Request);
	static FHyperAISpatialPlanReport ApplyPlan(const FHyperAISpatialPlanRequest& Request);
};

class FHyperAIStudioSceneSpatialRegistration final
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
