// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioDestructionToolset.generated.h"

/**
 * One op of a destruction plan, in order. Values are in cm; vectors "x,y,z" in the collection's own space (its pivot
 * at 0,0,0). Params are "Name=Value; Name=Value", the names each kind takes; hyper_destruction_inspect lists them with
 * their ranges and defaults.
 */
USTRUCT(BlueprintType)
struct FHyperAIDestructionOp
{
	GENERATED_BODY()

	/**
	 * make, fracture.uniform, fracture.clustered, fracture.radial, fracture.planar, fracture.slice, fracture.brick,
	 * cluster, cleanup, damage, collision, mass, state, anchor, material.internal, rendering.
	 */
	UPROPERTY() FString Kind;
	/**
	 * make: what the collection is made of, static meshes by path (/Game/Props/SM_Wall) and level actors by label, in one
	 * list; rendering: the proxy static meshes drawn while the collection is whole.
	 */
	UPROPERTY() TArray<FString> Sources;
	/**
	 * make: replace (level actors only, the default for them: the geometry collection actor takes their place and they
	 * are deleted), beside (placed next to them; they stay), at (placed at Location) or none (the asset only, the default
	 * for meshes).
	 */
	UPROPERTY() FString Place;
	/** make Place=at: where in the level; fracture.radial: the centre, from the middle of the pieces it cuts; fracture.brick: the brick grid's offset. */
	UPROPERTY() FString Location;
	/** "Name=Value; Name=Value", the names each kind takes (hyper_destruction_inspect lists them). */
	UPROPERTY() FString Params;
	/** material.internal: the material for the faces fracturing cut inside; mass: a physical material. */
	UPROPERTY() FString Material;
};

USTRUCT(BlueprintType)
struct FHyperAIDestructionPlanRequest
{
	GENERATED_BODY()

	/**
	 * The geometry collection the plan makes (with a make op) or changes, under /Game: /Game/Destruction/GC_Wall. A
	 * plan without make changes an existing one: its pieces with fracture, cluster and cleanup ops, its settings with the rest.
	 */
	UPROPERTY() FString Asset;
	UPROPERTY() TArray<FHyperAIDestructionOp> Operations;
	/** Build the pieces in memory and report them; nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Draw an exploded view of the pieces as a PNG, PreviewPath; a dry run draws it from memory. */
	UPROPERTY() bool bPreview = true;
	/** Save the geometry collection, and the level when actors changed. Off, nothing is saved and one undo takes the whole plan back. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

/** One level of the hierarchy: 0 is the whole object, 1 the first pieces it breaks into. */
USTRUCT(BlueprintType)
struct FHyperAIDestructionLevel
{
	GENERATED_BODY()

	UPROPERTY() int32 Level = 0;
	/** Bodies at this level: Clusters break further, Leaves do not. */
	UPROPERTY() int32 Pieces = 0;
	UPROPERTY() int32 Clusters = 0;
	UPROPERTY() int32 Leaves = 0;
	/** The strain a cluster at this level takes before it breaks; 0 when it is a level of leaves only. */
	UPROPERTY() float DamageThreshold = 0.f;
	/** Its pieces' size, the cube root of their volume, cm. */
	UPROPERTY() float SmallestCm = 0.f;
	UPROPERTY() float LargestCm = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAIDestructionPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, approval_not_queued, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Asset;
	/** Geometry collection actors the plan placed, by label. */
	UPROPERTY() TArray<FString> Actors;
	/** Every transform of the hierarchy (bones), the pieces with geometry, the leaves that never break further, and the clusters. */
	UPROPERTY() int32 Bones = 0;
	UPROPERTY() int32 Geometry = 0;
	UPROPERTY() int32 Leaves = 0;
	UPROPERTY() int32 Clusters = 0;
	UPROPERTY() TArray<FHyperAIDestructionLevel> Levels;
	UPROPERTY() int32 Triangles = 0;
	/** Mass of the whole, kg: density times the volume of the pieces. */
	UPROPERTY() float MassKg = 0.f;
	/** Bounds of the pieces in the collection's space, "x,y,z" cm. */
	UPROPERTY() FString Size;
	UPROPERTY() FString Min;
	UPROPERTY() FString Max;
	/** Leaves anchored in place (they never fall, even broken off). */
	UPROPERTY() int32 Anchored = 0;
	/** The collision shape, damage model and initial state the asset holds. */
	UPROPERTY() FString Collision;
	UPROPERTY() FString InitialState;
	/** What could go wrong in play: pieces too small to simulate well, levels without a threshold, nothing anchored... */
	UPROPERTY() TArray<FString> Warnings;
	/** What each op did. */
	UPROPERTY() TArray<FString> Notes;
	/** An exploded view of the pieces as a PNG (Saved/HyperAIStudio/Previews) to open with an image reader; Notes say why when none. */
	UPROPERTY() FString PreviewPath;
	/**
	 * A hyper_verify_run request that proves the placed collection breaks in play: damage at its middle, then
	 * AssertBroken on the pieces broken off. Empty when no actor was placed.
	 */
	UPROPERTY() FString VerifyCase;
	UPROPERTY() TArray<FString> Saved;
	/** Milliseconds the pieces took to build. */
	UPROPERTY() int32 BuildMs = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIDestructionInspectRequest
{
	GENERATED_BODY()

	/** A geometry collection by path; or give Actor. Neither: the op kinds and their Params, and the collections under /Game. */
	UPROPERTY() FString Asset;
	/** A geometry collection actor in the open level, by label. */
	UPROPERTY() FString Actor;
	/** Also list up to this many pieces, each with its level, parent, size and anchor; 0 lists none, 500 at most. */
	UPROPERTY() int32 MaxPieces = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIDestructionPiece
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = 0;
	UPROPERTY() FString Name;
	UPROPERTY() int32 Level = 0;
	UPROPERTY() int32 Parent = -1;
	UPROPERTY() int32 Children = 0;
	/** The cube root of its volume, cm; 0 for a cluster. */
	UPROPERTY() float SizeCm = 0.f;
	/** Its centre in the collection's space, "x,y,z" cm. */
	UPROPERTY() FString Center;
	UPROPERTY() bool bAnchored = false;
};

/** A material slot and the faces that use it. */
USTRUCT(BlueprintType)
struct FHyperAIDestructionMaterialSlot
{
	GENERATED_BODY()

	UPROPERTY() int32 Slot = 0;
	UPROPERTY() FString Material;
	/** Faces of the original surface, and faces fracturing cut inside. */
	UPROPERTY() int32 OuterFaces = 0;
	UPROPERTY() int32 InnerFaces = 0;
};

/** One size-specific collision entry: pieces up to MaxSize cm. */
USTRUCT(BlueprintType)
struct FHyperAIDestructionCollision
{
	GENERATED_BODY()

	UPROPERTY() float MaxSize = 0.f;
	/** volumetric or surface. */
	UPROPERTY() FString Type;
	/** convex, box, sphere, capsule, levelset or none. */
	UPROPERTY() FString Implicit;
	UPROPERTY() int32 DamageThreshold = 0;
};

/** An op kind a plan takes, with its Params. */
USTRUCT(BlueprintType)
struct FHyperAIDestructionKind
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString Summary;
	/** "Name: type, range, default - what it does", one per param. */
	UPROPERTY() TArray<FString> Params;
};

USTRUCT(BlueprintType)
struct FHyperAIDestructionInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Asset;
	UPROPERTY() int32 Bones = 0;
	UPROPERTY() int32 Geometry = 0;
	UPROPERTY() int32 Leaves = 0;
	UPROPERTY() int32 Clusters = 0;
	UPROPERTY() TArray<FHyperAIDestructionLevel> Levels;
	UPROPERTY() int32 Triangles = 0;
	UPROPERTY() float MassKg = 0.f;
	UPROPERTY() FString Size;
	/** The damage model and thresholds per level, as the asset holds them. */
	UPROPERTY() FString DamageModel;
	UPROPERTY() TArray<float> DamageThresholds;
	UPROPERTY() bool bSizeSpecificDamage = false;
	UPROPERTY() TArray<FHyperAIDestructionCollision> Collision;
	UPROPERTY() TArray<FHyperAIDestructionMaterialSlot> Materials;
	UPROPERTY() int32 Anchored = 0;
	/** The root's initial state: dynamic, kinematic, static, sleeping, or default (the component's own). */
	UPROPERTY() FString InitialState;
	UPROPERTY() bool bNanite = false;
	/** default, or root_proxy with the proxy meshes drawn while it is whole. */
	UPROPERTY() FString Renderer;
	UPROPERTY() TArray<FString> ProxyMeshes;
	/** Actors in the open level that use it, with their component's initial state. */
	UPROPERTY() TArray<FString> Actors;
	UPROPERTY() TArray<FHyperAIDestructionPiece> Pieces;
	UPROPERTY() TArray<FString> Warnings;
	/** Without Asset or Actor: the op kinds hyper_destruction_apply_plan takes, and the geometry collections under /Game. */
	UPROPERTY() TArray<FHyperAIDestructionKind> Kinds;
	UPROPERTY() TArray<FString> Assets;
};

/**
 * Destruction: Chaos geometry collections made from static meshes or level actors, fractured (Voronoi, clustered,
 * radial, planar, slice, brick, noise on the cuts), clustered into levels, given damage thresholds, collision, mass,
 * initial state, anchors, an inside material and Nanite or proxy rendering, in one undo step; then proved in play with
 * the playtest bot's Damage, Fire and AssertBroken steps.
 */
UCLASS()
class HYPERAISTUDIODESTRUCTION_API UHyperAIStudioDestructionToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * A geometry collection (by Asset or Actor), read only: its levels with piece counts and damage thresholds, its
	 * pieces, collision per size, materials (outer and inside faces), mass, anchors, initial state, Nanite and proxies,
	 * and the actors using it. Without either: the op kinds hyper_destruction_apply_plan takes, with their Params.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Physics")
	static FHyperAIDestructionInspectReport hyper_destruction_inspect(const FHyperAIDestructionInspectRequest& Request);

	/**
	 * Makes or changes a Chaos geometry collection in one plan, one undo step: make it from static meshes or level actors
	 * (replacing them, beside them, or at a place); fracture it (fracture.uniform Voronoi with Sites and Seed,
	 * fracture.clustered, fracture.radial rings and steps, fracture.planar, fracture.slice, fracture.brick, with Noise on
	 * the cuts); cluster pieces into levels; cleanup pieces under a size (merge or remove); damage thresholds per level;
	 * collision shapes per size; mass; initial state (dynamic, kinematic, static); anchor the pieces on the ground;
	 * material.internal for the faces cut inside; Nanite and proxy rendering. The reply counts pieces per level, bones,
	 * mass and bounds, warns, draws an exploded PNG, and hands back a hyper_verify_run case that proves it breaks.
	 * bDryRun builds it in memory first.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Physics")
	static FHyperAIDestructionPlanReport hyper_destruction_apply_plan(const FHyperAIDestructionPlanRequest& Request);
};

class FHyperAIStudioDestructionContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("destruction");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiodestructiontoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_destruction_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_destruction_apply_plan");
	static constexpr int32 MaxOperations = 32;
	static constexpr int32 MaxSources = 32;
	/** Voronoi sites one op places; up to twice this is fitted down to it, more is refused. */
	static constexpr int32 MaxSites = 500;
	/** Leaves a collection may end the plan with, and bricks or slices one op may cut. */
	static constexpr int32 MaxPieces = 2000;
	/** Triangles of the sources together. */
	static constexpr int32 MaxSourceTriangles = 300000;
	static constexpr int32 MaxListedPieces = 500;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIDestructionInspectReport Inspect(const FHyperAIDestructionInspectRequest& Request);
	static FHyperAIDestructionPlanReport ApplyPlan(const FHyperAIDestructionPlanRequest& Request);
};

class FHyperAIStudioDestructionRegistration final
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
