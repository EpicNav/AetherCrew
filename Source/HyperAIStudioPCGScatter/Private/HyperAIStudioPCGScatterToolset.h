// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioPCGScatterToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIPCGScatterMesh
{
	GENERATED_BODY()

	/** A static mesh asset. */
	UPROPERTY() FString Mesh;
	/** How often it is picked against the others. */
	UPROPERTY() int32 Weight = 1;
	/** Ecosystems: meters at which it stops drawing (fading from 80% of it); 0 takes the species' CullDistance. */
	UPROPERTY() float CullDistance = 0.f;
};

/**
 * One species of an ecosystem's biome: where in the biome it grows and how it is placed. Species are listed big to
 * small, and each keeps out of the room (footprint x Clearance) the species before it take.
 */
USTRUCT(BlueprintType)
struct FHyperAIPCGSpecies
{
	GENERATED_BODY()

	/** Unique in its biome; KeepOutOf names species by it. */
	UPROPERTY() FString Name;
	UPROPERTY() TArray<FHyperAIPCGScatterMesh> Meshes;
	/**
	 * Points tried per square meter before the masks (clumps, with Cluster): 0.005 to 0.05 for trees, 0.1 to 0.5 for
	 * bushes, 1 to 5 for ground cover. At most one per Footprint square (one per clump's square with Cluster).
	 */
	UPROPERTY() float Density = 0.f;
	/** Meters of ground one instance takes: two of a species stand at least this far apart (clump members a little closer). 1 by default. */
	UPROPERTY() float Footprint = 0.f;
	/** The steepest ground, in degrees, it grows on; 0 takes the biome's (any slope when that is 0 too). */
	UPROPERTY() float SlopeMax = 0.f;
	/**
	 * Stands and clearings: meters across one stand is (noise feature size), and the share of the biome the stands
	 * cover (0.05 to 1). Patch 0 spreads it evenly.
	 */
	UPROPERTY() float Patch = 0.f;
	UPROPERTY() float Coverage = 1.f;
	/** Clumps: instances per clump (2 to 36; 0 or 1 for single instances), spread over ClusterRadius meters. */
	UPROPERTY() int32 Cluster = 0;
	UPROPERTY() float ClusterRadius = 0.f;
	/** Uniform scale, Min to Max (X to Y); 0.8 to 1.2 when zero. */
	UPROPERTY() FVector2D Scale = FVector2D::ZeroVector;
	/** Centimeters pushed into the ground, so roots and rock bases do not float. */
	UPROPERTY() float Sink = 0.f;
	/** Degrees of random lean (pitch and roll). Turns about the up axis are always random. */
	UPROPERTY() float Tilt = 0.f;
	/** Turned to the ground's slope, for rocks and debris; trees and bushes stay upright when false. */
	UPROPERTY() bool bAlignToGround = false;
	/** Meters at which its meshes stop drawing; 0 never culls. */
	UPROPERTY() float CullDistance = 0.f;
	/** How much of its footprint later species keep out of: 1 is the footprint itself, 2 twice as wide. */
	UPROPERTY() float Clearance = 1.f;
	/** Earlier species of the biome it keeps out of; all of them when empty, none with ["none"]. */
	UPROPERTY() TArray<FString> KeepOutOf;
};

/** Where an ecosystem's biome grows: a landscape paint layer, a height band and a slope limit. */
USTRUCT(BlueprintType)
struct FHyperAIPCGBiome
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** The landscape paint layer it grows on (its weight at least LayerMin); everywhere when empty. */
	UPROPERTY() FString Layer;
	UPROPERTY() float LayerMin = 0.5f;
	/** Meters above zero it grows between, Min to Max (X to Y); any height when both are 0. */
	UPROPERTY() FVector2D Height = FVector2D::ZeroVector;
	/** The steepest ground, in degrees, for species that set none; 0 for any. */
	UPROPERTY() float SlopeMax = 0.f;
	/** Big to small: canopy, understory, rocks, debris, ground cover. */
	UPROPERTY() TArray<FHyperAIPCGSpecies> Species;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGScatterOp
{
	GENERATED_BODY()

	/** graph.create, graph.set, ecosystem.create, ecosystem.set, volume.create, volume.generate, volume.clear. */
	UPROPERTY() FString Kind;
	/** graph.*: the scatter graph (/Game/PCG/PCG_Forest); volume.create: the graph the new volume runs. */
	UPROPERTY() FString Graph;
	/** graph.*: the meshes scattered, and their weights. */
	UPROPERTY() TArray<FHyperAIPCGScatterMesh> Meshes;
	/** graph.*: points tried per square meter of landscape: 0.02 for sparse trees, 0.5 for rocks, 4 for grass. 0 leaves it. */
	UPROPERTY() float Density = 0.f;
	/** graph.*: meters kept between instances; 0 lets them crowd. Negative leaves it. */
	UPROPERTY() float Spacing = -1.f;
	/** graph.*: the steepest ground, in degrees, instances grow on; 0 for any. Negative leaves it. */
	UPROPERTY() float SlopeMax = -1.f;
	/** graph.*: the uniform scale range, Min to Max (X to Y); zero leaves it. */
	UPROPERTY() FVector2D Scale = FVector2D::ZeroVector;
	/** graph.*: true or false: each instance turned at random about its up axis. Empty leaves it (on for a new graph). */
	UPROPERTY() FString RandomYaw;
	/**
	 * graph.*: true or false: instances lean with the ground's slope (rocks, debris) instead of standing upright (trees).
	 * Empty leaves it (upright for a new graph).
	 */
	UPROPERTY() FString AlignToGround;
	/** graph.*: the same seed scatters the same way. 0 leaves it. */
	UPROPERTY() int32 Seed = 0;
	/**
	 * volume.*: the PCG volume by label. volume.create places it: centered on Center (cm), Size meters across, Height
	 * meters tall (500 by default), running Graph over the landscape inside it.
	 */
	UPROPERTY() FString Volume;
	UPROPERTY() FVector Center = FVector::ZeroVector;
	UPROPERTY() FVector2D Size = FVector2D::ZeroVector;
	UPROPERTY() float Height = 0.f;
	/** volume.create: centered on and sized to this landscape (by label) instead of Center and Size. */
	UPROPERTY() FString Cover;
	/**
	 * volume.create: true generates in a grid of World Partition cells, which stream with the world: for a World
	 * Partition level more than a few hundred meters across. Empty or false generates in one piece.
	 */
	UPROPERTY() FString Partitioned;
	/**
	 * ecosystem.create, ecosystem.set: the graph's biomes, each with its species big to small. ecosystem.set rebuilds
	 * an ecosystem graph this tool made from the biomes given.
	 */
	UPROPERTY() TArray<FHyperAIPCGBiome> Biomes;
	/** ecosystem.*: landscape paint layers nothing grows on (a road or riverbank layer), at weight 0.5 or more. */
	UPROPERTY() TArray<FString> AvoidLayers;
	/** ecosystem.*: actors with these tags (houses, rocks placed by hand) keep AvoidMargin meters clear around them. */
	UPROPERTY() TArray<FString> AvoidTags;
	UPROPERTY() float AvoidMargin = 2.f;
	/**
	 * ecosystem.*: meters every species keeps from the edges of the landscape's roads (its landscape splines), measured
	 * to each footprint; 0 leaves roads to AvoidLayers.
	 */
	UPROPERTY() float RoadMargin = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGScatterPlanRequest
{
	GENERATED_BODY()

	/** At most one volume.create or volume.generate, and it goes last. */
	UPROPERTY() TArray<FHyperAIPCGScatterOp> Operations;
	/** Only check the plan: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/**
	 * Save the level before generating: the volume, and any landscape kept whole rather than split into streaming
	 * proxies, which World Partition shows PCG only once saved. Graphs are always saved.
	 */
	UPROPERTY() bool bSave = false;
	/** Names the edit and the generation, for hyper_operation_wait; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGScatterPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, generating (wait with hyper_operation_wait), checked, awaiting_user_approval, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() int32 Applied = 0;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGScatterInspectRequest
{
	GENERATED_BODY()

	/**
	 * Any PCG graph to read (/Game/PCG/PCG_Forest, with or without .PCG_Forest); the PCG volumes in the level and the
	 * PCG graphs under /Game are listed when empty.
	 */
	UPROPERTY() FString Graph;
	/**
	 * How much of the graph: summary (each mesh spawner's recipe, the graph's parameters and node counts; the default),
	 * nodes (also every node with the settings that differ from the node's defaults), or full (also every edge).
	 */
	UPROPERTY() FString Detail;
	/** Only nodes, recipes and edges whose names, titles, kinds or meshes contain this text. */
	UPROPERTY() FString Filter;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGScatterInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	/** A scatter graph's density, spacing, slope, scale, yaw, seed and meshes. */
	UPROPERTY() TArray<FString> Settings;
	/**
	 * Each mesh spawner and the chain of nodes its points come through, upstream: what the node does and the settings
	 * that differ from its defaults, other inputs in brackets. One line is one species.
	 */
	UPROPERTY() TArray<FString> Recipes;
	/** Detail nodes and full: name | kind 'title' | settings that differ from the defaults. */
	UPROPERTY() TArray<FString> Nodes;
	/** Detail full: from node.pin -> to node.pin. */
	UPROPERTY() TArray<FString> Edges;
	/** The graph's parameters: name, type and value. */
	UPROPERTY() TArray<FString> Parameters;
	/**
	 * An ecosystem graph's op as ecosystem.set takes it (JSON): change what you need and send it back whole, since
	 * ecosystem.set rebuilds the graph from the biomes it is given.
	 */
	UPROPERTY() FString Ecosystem;
	/** Some lines were left out to keep the answer small; narrow it with Filter. */
	UPROPERTY() bool bTruncated = false;
	/** PCG volumes: label, graph, size, instances made, and whether it is generating. */
	UPROPERTY() TArray<FString> Volumes;
	UPROPERTY() TArray<FString> Graphs;
};

/**
 * PCG scatter: scatter graphs built in one call (landscape surface sampled at a density, kept off steep ground, spaced,
 * scaled and turned, spawning weighted meshes), ecosystem graphs (biomes on paint layers, layered species that keep out
 * of each other), placed in PCG volumes and generated, regenerated or cleared; and any PCG graph read back as recipes.
 */
UCLASS()
class HYPERAISTUDIOPCGSCATTER_API UHyperAIStudioPCGScatterToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Reads any PCG graph: each mesh spawner's recipe (the nodes its points pass through, with the settings that differ
	 * from defaults), its meshes and cull distances, and the graph's parameters; with Detail, every node and edge; for an
	 * ecosystem, the op that rebuilds it. With no Graph: the level's PCG volumes and the PCG graphs under /Game.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|PCG")
	static FHyperAIPCGScatterInspectReport hyper_pcg_scatter_inspect(const FHyperAIPCGScatterInspectRequest& Request);

	/**
	 * PCG in one plan: a one-layer scatter graph, or an ecosystem: biomes on landscape paint layers, each with species
	 * big to small (density, footprint, slope, stands, clumps, sink, lean, cull distance), each keeping out of the ones
	 * before. Then a PCG volume running it, sized to a landscape and partitioned if asked; generate (wait with
	 * hyper_operation_wait) or clear.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|PCG")
	static FHyperAIPCGScatterPlanReport hyper_pcg_scatter_apply_plan(const FHyperAIPCGScatterPlanRequest& Request);
};

class FHyperAIStudioPCGScatterContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("pcg");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiopcgscattertoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_pcg_scatter_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_pcg_scatter_apply_plan");
	static constexpr int32 MaxOperations = 32;
	/** Points a graph may try per square meter. */
	static constexpr float MaxDensity = 20.f;
	/** An ecosystem's size: at most about 20 nodes per species keeps even the largest graph near 800 nodes. */
	static constexpr int32 MaxBiomes = 8;
	static constexpr int32 MaxSpeciesPerBiome = 12;
	static constexpr int32 MaxSpecies = 40;
	/** Meters a species samples past its partition cell at most: half of World Partition's default 256 m PCG cell. */
	static constexpr int32 MaxHalo = 128;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIPCGScatterInspectReport Inspect(const FHyperAIPCGScatterInspectRequest& Request);
	static FHyperAIPCGScatterPlanReport ApplyPlan(const FHyperAIPCGScatterPlanRequest& Request);
};

class FHyperAIStudioPCGScatterRegistration final
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
