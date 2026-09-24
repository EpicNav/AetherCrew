// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioMeshTerrainToolset.generated.h"

/** One Mesh Terrain: a Mesh Partition actor and the base mesh sections it was created with. */
USTRUCT(BlueprintType)
struct FHyperAIMeshTerrain
{
	GENERATED_BODY()

	/** The actor label; ops name the terrain by it. */
	UPROPERTY() FString Name;
	UPROPERTY() FString ActorPath;
	UPROPERTY() FString Definition;
	/** "x,y,z" of the actor, and "x,y,z" of the area its sections cover, in centimetres. */
	UPROPERTY() FString Location;
	UPROPERTY() FString Size;
	UPROPERTY() int32 SectionCount = 0;
	UPROPERTY() int32 ModifierCount = 0;
	/** Preview sections are still rebuilding after a change. */
	UPROPERTY() bool bBuilding = false;
};

/** One modifier that shapes a terrain. */
USTRUCT(BlueprintType)
struct FHyperAIMeshTerrainModifier
{
	GENERATED_BODY()

	/** The modifier actor's label; ops name the modifier by it. */
	UPROPERTY() FString Name;
	/** noise, remesh, texture_patch or spline; other engine modifiers by class name. */
	UPROPERTY() FString Type;
	UPROPERTY() FString Terrain;
	UPROPERTY() FString Location;
	UPROPERTY() FString Rotation;
	UPROPERTY() FString Scale;
	UPROPERTY() bool bDisabled = false;
	/** Editable settings as Name=Value pairs, in the form set_modifier takes. */
	UPROPERTY() FString Settings;
	/** Spline points relative to the modifier, "x,y,z; x,y,z". */
	UPROPERTY() FString Points;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshTerrainInspectRequest
{
	GENERATED_BODY()

	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshTerrainInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString LevelPath;
	/** Pass to apply_plan as expected_revision. */
	UPROPERTY() FString Revision;
	UPROPERTY() TArray<FHyperAIMeshTerrain> Terrains;
	UPROPERTY() TArray<FHyperAIMeshTerrainModifier> Modifiers;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/**
 * One Mesh Terrain edit. Vectors are "x,y,z" in centimetres, rotations "pitch,yaw,roll" in degrees.
 *   create_terrain: Name, Location, Size ("width,depth", 1000-200000 cm each), Spacing (cm between vertices, 25-1000;
 *     0 means 100)
 *   add_modifier: Name, Terrain (may be empty when the level has one terrain), Type (noise, remesh, texture_patch,
 *     spline), Location, optional Rotation, Scale and Properties; a spline also needs Points
 *   set_modifier: Name, then any of Location, Rotation, Scale, Properties, Points
 *   remove_modifier: Name
 * Properties are Name=Value pairs split by ";" in Unreal text form, e.g. "Intensity=800;
 * NoiseFrequency=(X=0.0005,Y=0.0005)". One level of sub-object works, e.g. "HeightChannel.TextureAsset=/Game/T/H.H".
 * Points are "x,y,z; x,y,z" relative to Location, at least two.
 */
USTRUCT(BlueprintType)
struct FHyperAIMeshTerrainOp
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	/** 1-48 letters, digits, _ or -; unique among terrains and modifiers. */
	UPROPERTY() FString Name;
	UPROPERTY() FString Terrain;
	UPROPERTY() FString Type;
	UPROPERTY() FString Location;
	UPROPERTY() FString Rotation;
	UPROPERTY() FString Scale;
	UPROPERTY() FString Size;
	UPROPERTY() float Spacing = 0.f;
	UPROPERTY() FString Properties;
	UPROPERTY() FString Points;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshTerrainApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString ExpectedRevision;
	/** Applied in order as one undo step. */
	UPROPERTY() TArray<FHyperAIMeshTerrainOp> Ops;
	/** Saves the level afterwards. Off by default so terrain can be iterated before it is kept. */
	UPROPERTY() bool bSave = false;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshTerrainApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTrustedPrepared = false;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString LevelPath;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	/** What the plan will build, e.g. a terrain's vertex count. */
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshTerrainIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	/** error or warning. */
	UPROPERTY() FString Severity;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshTerrainValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshTerrainValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString LevelPath;
	UPROPERTY() int32 TerrainCount = 0;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAIMeshTerrainIssue> Issues;
	/** Some terrain is still rebuilding its preview after a change. */
	UPROPERTY() bool bBuilding = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/** Mesh Terrain (UE 5.8 Mesh Partition, experimental): create terrains and shape them with modifiers, then check them. */
UCLASS()
class HYPERAISTUDIOMESHTERRAIN_API UHyperAIStudioMeshTerrainToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Mesh Terrains in the open level with their modifiers (type, place, settings), and the revision apply_plan needs. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshTerrain")
	static FHyperAIMeshTerrainInspectReport hyper_mesh_terrain_inspect(const FHyperAIMeshTerrainInspectRequest& Request);

	/**
	 * Terrain edits as one undo step: create a flat terrain, then add, change or remove noise, remesh, texture_patch
	 * (heightmap stamp) and spline (roads, rivers, ridges) modifiers. Submit with bDryRun false (expected_plan_hash
	 * optional) and call hyper_operation_wait: it returns when the terrain has rebuilt. Unsaved unless bSave.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshTerrain")
	static FHyperAIMeshTerrainApplyPlanReport hyper_mesh_terrain_apply_plan(const FHyperAIMeshTerrainApplyPlanRequest& Request);

	/** Problems in the open level's terrains: no base mesh or definition, unbound or disabled modifiers, stamps without a texture, splines without points, and rebuilds still running. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshTerrain")
	static FHyperAIMeshTerrainValidateReport hyper_mesh_terrain_validate(const FHyperAIMeshTerrainValidateRequest& Request);
};

class FHyperAIStudioMeshTerrainInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIMeshTerrainInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMeshTerrainValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIMeshTerrainValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMeshTerrainEditOpsPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString LevelPath;
	FString BaseRevision;
	TArray<FHyperAIMeshTerrainOp> Ops;
	bool bSave = false;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

class FHyperAIStudioMeshTerrainInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIMeshTerrainInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMeshTerrainValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIMeshTerrainValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMeshTerrainMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioMeshTerrainDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioMeshTerrainFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
{
public:
	virtual FString GetOwnerAdapterFingerprint() const override;
	virtual bool ResolveCanonicalEffectTarget(
		const IHyperAIStudioTypedArtifactPayload& Request,
		FString& OutCanonicalEffectTarget,
		FString& OutError) override;
	virtual bool VerifyFreshExact(
		const IHyperAIStudioTypedArtifactPayload& Request,
		const IHyperAIStudioDomainResultPayload& Result,
		FString& OutPostconditionHash,
		FString& OutError) override;
};

class FHyperAIStudioMeshTerrainContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("mesh_terrain");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiomeshterraintoolset.v1");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.mesh_terrain");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_mesh_terrain_inspect");
	static constexpr const TCHAR* MutationToolName = TEXT("hyper_mesh_terrain_apply_plan");
	static constexpr const TCHAR* ValidateToolName = TEXT("hyper_mesh_terrain_validate");
	static constexpr const TCHAR* InspectVariantId = TEXT("terrain_modifier_capture.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("terrain_modifier_ops.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("terrain_checks.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.mesh_terrain.inspect.v1");
	static constexpr const TCHAR* EditOpsPayloadTypeId = TEXT("hyperai.payload.mesh_terrain.edit_ops.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.mesh_terrain.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.mesh_terrain.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.mesh_terrain.edit_ops.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.mesh_terrain.validate.v1");
	static constexpr int32 MaxOpsPerPlan = 16;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;
	/** How long the job after an edit waits for preview sections to rebuild. */
	static constexpr int32 RebuildDeadlineMs = 180 * 1000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FString> GetCapabilities();
	static bool IsCanonicalSha256(const FString& Value);
	static FString InspectPayloadSchemaFingerprint();
	static FString EditOpsPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAIMeshTerrainInspectReport Inspect(const FHyperAIMeshTerrainInspectRequest& Request);
	static FHyperAIMeshTerrainValidateReport Validate(const FHyperAIMeshTerrainValidateRequest& Request);
	static FHyperAIMeshTerrainApplyPlanReport BuildPlan(const FHyperAIMeshTerrainApplyPlanRequest& Request);
	static FString ComputeEditOpsSemanticFingerprint(const FHyperAIStudioMeshTerrainEditOpsPayload& Payload);
};

class FHyperAIStudioMeshTerrainRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;
	bool HasLiveOwnership() const;

private:
	void RegisterAfterEngineInit();
	void RollBackRegistration();

	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FHyperAIStudioMeshTerrainDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
