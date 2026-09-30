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
};

USTRUCT(BlueprintType)
struct FHyperAIPCGScatterOp
{
	GENERATED_BODY()

	/** graph.create, graph.set, volume.create, volume.generate, volume.clear. */
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

	/** A scatter graph to read; the PCG volumes in the level and the PCG graphs under /Game are listed when empty. */
	UPROPERTY() FString Graph;
};

USTRUCT(BlueprintType)
struct FHyperAIPCGScatterInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	/** A scatter graph's density, spacing, slope, scale, yaw, seed and meshes. */
	UPROPERTY() TArray<FString> Settings;
	/** PCG volumes: label, graph, size, instances made, and whether it is generating. */
	UPROPERTY() TArray<FString> Volumes;
	UPROPERTY() TArray<FString> Graphs;
};

/**
 * PCG scatter: scatter graphs built in one call (landscape surface sampled at a density, kept off steep ground, spaced,
 * scaled and turned, spawning weighted meshes), placed in PCG volumes and generated, regenerated or cleared.
 */
UCLASS()
class HYPERAISTUDIOPCGSCATTER_API UHyperAIStudioPCGScatterToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** A scatter graph's settings, or the level's PCG volumes (graph, size, instances, generating) and the PCG graphs under /Game. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|PCG")
	static FHyperAIPCGScatterInspectReport hyper_pcg_scatter_inspect(const FHyperAIPCGScatterInspectRequest& Request);

	/**
	 * Procedural scatter in one plan: build a PCG graph that samples the landscape at a density, keeps off ground
	 * steeper than a slope, spaces, scales and turns the points and spawns weighted meshes; tune it; place a PCG volume
	 * running it and generate it (wait with hyper_operation_wait), regenerate or clear it.
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
