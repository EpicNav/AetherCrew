// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioWorldPartitionToolset.generated.h"

/** A setting by its Unreal property name, in Unreal's text form and units (cm): CellSize 25600, LayerType MeshMerge. */
USTRUCT(BlueprintType)
struct FHyperAIWorldPartitionSetting
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Value;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldPartitionOp
{
	GENERATED_BODY()

	/**
	 * data_layer.create, data_layer.set, data_layer.delete, data_layer.add_actors, data_layer.remove_actors, actors.set,
	 * hlod_layer.create, hlod_layer.set, world_partition.set, world_partition.set_grid, region.load, region.unload.
	 */
	UPROPERTY() FString Kind;
	/** data_layer.*: the data layer by name. A new one is named after its asset. */
	UPROPERTY() FString DataLayer;
	/**
	 * data_layer.create: the data layer asset, made when missing (/Game/DataLayers/<DataLayer> by default).
	 * hlod_layer.create, hlod_layer.set: the HLOD layer asset.
	 */
	UPROPERTY() FString Asset;
	/** data_layer.create: Runtime (streams in and out in game; the default) or Editor (organizes the editor only). */
	UPROPERTY() FString Type;
	/** data_layer.create, data_layer.set: the parent data layer, of the same type, or none. */
	UPROPERTY() FString Parent;
	/** data_layer.create, data_layer.set: a runtime layer's state when play starts: Unloaded, Loaded or Activated. */
	UPROPERTY() FString InitialRuntimeState;
	/** data_layer.create, data_layer.set: true or false, or empty to leave it: shown (now and when the level opens), and loaded in the editor. */
	UPROPERTY() FString Visible;
	UPROPERTY() FString LoadedInEditor;
	/** data_layer.add_actors, data_layer.remove_actors, actors.set: loaded actors by label. */
	UPROPERTY() TArray<FString> Actors;
	/** actors.set: true streams the actors in by distance; false keeps them always loaded. Empty leaves it. */
	UPROPERTY() FString SpatiallyLoaded;
	/** actors.set: the runtime grid the actors stream with, or none for the default. */
	UPROPERTY() FString RuntimeGrid;
	/** actors.set: the HLOD layer standing in for the actors from afar; world_partition.set: the level's default. An asset path, or none. */
	UPROPERTY() FString HLODLayer;
	/**
	 * hlod_layer.create, hlod_layer.set: the HLOD layer's settings: LayerType (Instancing, MeshMerge, MeshSimplify,
	 * MeshApproximate), bIsSpatiallyLoaded, CellSize, LoadingRange, ParentLayer. world_partition.set_grid: the grid's or
	 * partition's: CellSize, LoadingRange, bBlockOnSlowStreaming, Priority. Unreal names, text form and cm.
	 */
	UPROPERTY() TArray<FHyperAIWorldPartitionSetting> Settings;
	/** hlod_layer.create, hlod_layer.set: settings of the builder its LayerType uses. */
	UPROPERTY() TArray<FHyperAIWorldPartitionSetting> BuilderSettings;
	/** world_partition.set_grid: the runtime grid or partition by name; the first when empty. */
	UPROPERTY() FString Grid;
	/**
	 * region.load: loads the actors of a rectangle Size meters across, centered on Center (cm), into the editor as the
	 * region Label. region.unload: unloads the region Label, or every region this tool loaded when empty.
	 */
	UPROPERTY() FVector Center = FVector::ZeroVector;
	UPROPERTY() FVector2D Size = FVector2D::ZeroVector;
	UPROPERTY() FString Label;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldPartitionPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FHyperAIWorldPartitionOp> Operations;
	/** Only check the plan: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Save the level and the actors changed afterwards; new data layer and HLOD layer assets are always saved. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldPartitionPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, checked, awaiting_user_approval, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() int32 Applied = 0;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldPartitionInspectRequest
{
	GENERATED_BODY()

	/** List the always-loaded actors (the first 50). */
	UPROPERTY() bool bActors = false;
	/** Each HLOD layer's every setting. */
	UPROPERTY() bool bSettings = false;
};

USTRUCT(BlueprintType)
struct FHyperAIDataLayerInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Asset;
	/** Runtime or Editor. */
	UPROPERTY() FString Type;
	UPROPERTY() FString InitialRuntimeState;
	UPROPERTY() bool bVisible = false;
	UPROPERTY() bool bLoadedInEditor = false;
	UPROPERTY() FString Parent;
	/** Its actors loaded in the editor now, and in the whole level. */
	UPROPERTY() int32 LoadedActors = 0;
	UPROPERTY() int32 Actors = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIHLODLayerInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Asset;
	UPROPERTY() FString LayerType;
	UPROPERTY() bool bSpatiallyLoaded = false;
	/** cm. */
	UPROPERTY() int32 CellSize = 0;
	UPROPERTY() double LoadingRange = 0.0;
	UPROPERTY() FString Parent;
	/** Actors it stands in for. */
	UPROPERTY() int32 Actors = 0;
	UPROPERTY() TArray<FString> Settings;
};

USTRUCT(BlueprintType)
struct FHyperAIWorldPartitionInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() bool bWorldPartition = false;
	UPROPERTY() bool bStreaming = false;
	/** Spatial hash (grids) or hash set (runtime partitions). */
	UPROPERTY() FString RuntimeHash;
	/** Each runtime grid or partition: name, cell size, loading range, HLOD layer. */
	UPROPERTY() TArray<FString> Grids;
	UPROPERTY() FString DefaultHLODLayer;
	UPROPERTY() TArray<FHyperAIDataLayerInfo> DataLayers;
	UPROPERTY() TArray<FHyperAIHLODLayerInfo> HLODLayers;
	/** Actors in the level, loaded in the editor now, and always loaded (not spatially loaded). */
	UPROPERTY() int32 Actors = 0;
	UPROPERTY() int32 LoadedActors = 0;
	UPROPERTY() int32 AlwaysLoaded = 0;
	UPROPERTY() TArray<FString> AlwaysLoadedActors;
	/** HLOD actors built for the level. */
	UPROPERTY() int32 HLODActors = 0;
	/** Regions loaded in the editor: label and bounds. */
	UPROPERTY() TArray<FString> Regions;
	/** How HLODs get built: an editor menu, or a commandlet to run. */
	UPROPERTY() FString BuildHLODs;
};

/**
 * World Partition: data layers made, set up and filled; actors' streaming (spatially loaded, runtime grid, HLOD layer);
 * HLOD layers made and tuned; runtime grids tuned; regions loaded into the editor. Each plan is one undo step.
 */
UCLASS()
class HYPERAISTUDIOWORLDPARTITION_API UHyperAIStudioWorldPartitionToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * The open level's World Partition: runtime grids, default HLOD layer, data layers with their states and actors,
	 * HLOD layers in use, actor counts (loaded, always loaded), built HLOD actors, loaded editor regions.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|World")
	static FHyperAIWorldPartitionInspectReport hyper_world_partition_inspect(const FHyperAIWorldPartitionInspectRequest& Request);

	/**
	 * Sets up World Partition in one plan, one undo step: data layers (make, parent, start state, show, load, delete,
	 * add or remove actors); actors streamed or always loaded, their grid and HLOD layer; HLOD layers made and tuned;
	 * the default HLOD layer; runtime grid cell size and loading range; editor regions loaded and unloaded.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|World")
	static FHyperAIWorldPartitionPlanReport hyper_world_partition_apply_plan(const FHyperAIWorldPartitionPlanRequest& Request);
};

class FHyperAIStudioWorldPartitionContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("worldbuilding_navigation");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioworldpartitiontoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_world_partition_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_world_partition_apply_plan");
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxActorsPerOp = 1000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIWorldPartitionInspectReport Inspect(const FHyperAIWorldPartitionInspectRequest& Request);
	static FHyperAIWorldPartitionPlanReport ApplyPlan(const FHyperAIWorldPartitionPlanRequest& Request);
};

class FHyperAIStudioWorldPartitionRegistration final
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
