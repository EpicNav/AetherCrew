// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioMeshGenToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIMeshGenerateRequest
{
	GENERATED_BODY()

	/** tripo, meshy or hi3d. Empty picks the first whose key is set, in that order. */
	UPROPERTY() FString Provider;
	/** What to make, for Tripo and Meshy: up to 800 characters. Hi3D makes models only from a picture. */
	UPROPERTY() FString Prompt;
	/** A picture inside the project to make the model from: PNG or JPEG (Hi3D takes WebP too), up to 20 MB. */
	UPROPERTY() FString ImagePath;
	/** The static mesh's name, such as SM_Crate; made from the prompt when empty. */
	UPROPERTY() FString Name;
	/** Where it is imported, under /Game; Mesh Import Folder in the settings when empty. */
	UPROPERTY() FString Folder;
	/** The provider's model version; its current best when empty. */
	UPROPERTY() FString Model;
	/** Names the job; made when empty. Follow it with hyper_operation_wait (bWaitThroughApproval) or hyper_mesh_status. */
	UPROPERTY() FString OperationId;
	/** Only report the price, the provider and where the model would land: nothing is spent. */
	UPROPERTY() bool bDryRun = true;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshGenerateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked (a dry run), awaiting_user_approval, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Provider;
	UPROPERTY() FString Model;
	/** words or picture. */
	UPROPERTY() FString Source;
	/** What the job costs at most, in USD, against this session's spend cap. */
	UPROPERTY() float EstimatedCost = 0.f;
	/** The folder the model, its materials and its textures are imported into, named after it. */
	UPROPERTY() FString Destination;
	/** Each provider, and whether its key's variable is set. */
	UPROPERTY() TArray<FString> Providers;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshJob
{
	GENERATED_BODY()

	UPROPERTY() FString OperationId;
	UPROPERTY() FString Provider;
	UPROPERTY() FString Model;
	/** awaiting_user_approval, rejected, submitting, generating, downloading, importing, completed or failed. */
	UPROPERTY() FString State;
	/** 0-100, as the provider reports it. */
	UPROPERTY() int32 Progress = 0;
	UPROPERTY() FString Prompt;
	UPROPERTY() FString ImagePath;
	/** The provider's task. */
	UPROPERTY() FString TaskId;
	/** The GLB the provider made, relative to the project. */
	UPROPERTY() FString ModelFile;
	/** The imported static mesh. */
	UPROPERTY() FString AssetPath;
	/** Everything the import made: the mesh, its materials and textures. */
	UPROPERTY() TArray<FString> ImportedAssets;
	UPROPERTY() float Cost = 0.f;
	UPROPERTY() FString Diagnostic;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshStatusRequest
{
	GENERATED_BODY()

	/** One job; empty lists this session's jobs, newest first. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshStatusReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAIMeshJob> Jobs;
};

/** Textured 3D models from Tripo, Meshy and Hi3D, imported into the project as static meshes. */
UCLASS()
class HYPERAISTUDIOMESHGEN_API UHyperAIStudioMeshGenToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Makes a textured 3D model with Tripo, Meshy or Hi3D, from words (Tripo, Meshy) or a picture in the project, and
	 * imports it as a static mesh with its materials. It is paid, so it always waits for the user's approval: dry-run
	 * first (the default) to read the price. Then follow operation_id with hyper_operation_wait (bWaitThroughApproval)
	 * or hyper_mesh_status; a model takes one to five minutes. Keys come from TRIPO_API_KEY, MESHY_API_KEY or
	 * HITEM3D_API_KEY (client_id:client_secret).
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIMeshGenerateReport hyper_mesh_generate(const FHyperAIMeshGenerateRequest& Request);

	/** This session's 3D models: each job's state and progress, the model file, and the asset it became. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIMeshStatusReport hyper_mesh_status(const FHyperAIMeshStatusRequest& Request);
};

class FHyperAIStudioMeshGenContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("mesh_generation");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiomeshgentoolset.v1");
	static constexpr const TCHAR* GenerateToolName = TEXT("hyper_mesh_generate");
	static constexpr const TCHAR* StatusToolName = TEXT("hyper_mesh_status");
	static constexpr int32 MaxPromptCharacters = 800;
	static constexpr int32 MaxImageBytes = 20 * 1024 * 1024;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIMeshGenerateReport Generate(const FHyperAIMeshGenerateRequest& Request);
	static FHyperAIMeshStatusReport Status(const FString& OperationId);
#if WITH_DEV_AUTOMATION_TESTS
	/** How often a running job asks its provider; tests shorten it. */
	static void SetPollSecondsForTests(double Seconds);
#endif
};

class FHyperAIStudioMeshGenRegistration final
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
