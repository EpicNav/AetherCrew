// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioModelCreatorTypes.h"
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
	/** generate, rig, animate or retexture. */
	UPROPERTY() FString Kind;
	/** Each provider task the job started, in order, as "what: task id"; a rig's id is what hyper_mesh_animate takes. */
	UPROPERTY() TArray<FString> Tasks;
	/** rig, and animate when it rigged a mesh itself: the skeletal mesh, its skeleton and its physics asset. */
	UPROPERTY() FString SkeletalMesh;
	UPROPERTY() FString Skeleton;
	UPROPERTY() FString PhysicsAsset;
	/** The animation sequences imported onto the skeleton. */
	UPROPERTY() TArray<FString> Animations;
	/** retexture: the material instances and textures imported. */
	UPROPERTY() TArray<FString> Materials;
	UPROPERTY() TArray<FString> Textures;
	/**
	 * The retarget hand-off: the operation hyper_retarget_apply_plan runs it as (follow it with hyper_operation_wait), its
	 * status when handed off, and the plan as JSON, to send again by hand if it could not be handed off.
	 */
	UPROPERTY() FString RetargetOperationId;
	UPROPERTY() FString RetargetStatus;
	UPROPERTY() FString RetargetPlan;
};

/** What rigging, animating or retexturing would cost and make, or why it was refused. */
USTRUCT(BlueprintType)
struct FHyperAIMeshTaskReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked (a dry run), awaiting_user_approval, queued, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Provider;
	/** The provider tasks the job runs, in order, each with its price. */
	UPROPERTY() TArray<FString> Tasks;
	/** What the job costs at most, in USD, against this session's spend cap. */
	UPROPERTY() float EstimatedCost = 0.f;
	/** The folder everything is imported into, named after it. */
	UPROPERTY() FString Destination;
	/** Each provider, and whether its key's variable is set. */
	UPROPERTY() TArray<FString> Providers;
	/** The retarget plan the job hands to hyper_retarget_apply_plan once it has imported, as JSON; empty without RetargetTo. */
	UPROPERTY() FString RetargetPlan;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshRigRequest
{
	GENERATED_BODY()

	/** meshy or tripo. Empty: the provider that made SourceOperationId's model, else the first whose key is set (meshy, tripo). */
	UPROPERTY() FString Provider;
	/**
	 * The model to rig, one of: a hyper_mesh_generate or hyper_mesh_retexture job of this session; the provider's own
	 * task id; or a static or skeletal mesh in the project, exported as a GLB and sent to the provider.
	 */
	UPROPERTY() FString SourceOperationId;
	UPROPERTY() FString SourceTaskId;
	UPROPERTY() FString MeshPath;
	/** Meshy: the character's height in metres; 1.7 when 0. */
	UPROPERTY() float HeightMeters = 0.f;
	/**
	 * Tripo: mixamo (the default: Mixamo's bone names, which Epic's retargeting recognises) or tripo (its own); and its rig
	 * model, v1.0-20240301 (the default: humanoids, 101 preset animations) or v2.5-20260210 (other creatures too, 16 presets).
	 */
	UPROPERTY() FString BoneNaming;
	UPROPERTY() FString Model;
	/** The skeletal mesh's name, such as SK_Knight; made from the source when empty. */
	UPROPERTY() FString Name;
	/** Where it is imported, under /Game; Mesh Import Folder in the settings when empty. */
	UPROPERTY() FString Folder;
	/**
	 * A skeletal mesh in the project, such as the UE5 Mannequin: once the rig is imported, an IK Rig for each and a
	 * retargeter between them are set up through hyper_retarget_apply_plan, an edit the user approves on its own.
	 */
	UPROPERTY() FString RetargetTo;
	/** Names the job; made when empty. */
	UPROPERTY() FString OperationId;
	/** Only report the price, the tasks and where the rig would land: nothing is spent. */
	UPROPERTY() bool bDryRun = true;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshAnimateRequest
{
	GENERATED_BODY()

	/** meshy, tripo or viggle. Empty: the rig's provider for Actions; for Prompt, meshy with a Meshy rig, else viggle. */
	UPROPERTY() FString Provider;
	/**
	 * The rigged model: a hyper_mesh_rig job of this session (or a hyper_mesh_animate job that rigged one), or the
	 * provider's rig task id with RigMesh, the skeletal mesh that rig was imported as.
	 */
	UPROPERTY() FString RigOperationId;
	UPROPERTY() FString RigTaskId;
	UPROPERTY() FString RigMesh;
	/**
	 * Preset animations from the provider's library, by id or name (hyper_mesh_actions searches it): Meshy's action ids or
	 * names, up to 10; Tripo's presets, such as walk or preset:biped:wave_goodbye_01, up to 20.
	 */
	UPROPERTY() TArray<FString> Actions;
	/** Animation from words instead: what the character does, 400 characters at most. */
	UPROPERTY() FString Prompt;
	/** How long the clip from Prompt lasts: Meshy 2 to 10 seconds in half seconds, Viggle 3 to 60; 4 when 0. */
	UPROPERTY() float DurationSeconds = 0.f;
	/** Meshy: prime (the default, the best) or swift (quicker and cheaper). */
	UPROPERTY() FString Mode;
	/**
	 * A skeletal mesh in the project to animate from words, such as the UE5 Mannequin. Viggle's clip is imported straight
	 * onto its skeleton (a UE5 Mannequin, MetaHuman or Mixamo one). Meshy, given no rig, exports it as a GLB, rigs that,
	 * puts the clip on it, then retargets the clip back onto TargetMesh.
	 */
	UPROPERTY() FString TargetMesh;
	/** A skeletal mesh to batch-retarget the new animations onto, through hyper_retarget_apply_plan, approved on its own. */
	UPROPERTY() FString RetargetTo;
	/** Names the folder the animations land in, such as A_Knight_Locomotion; made from the rig and the actions when empty. */
	UPROPERTY() FString Name;
	UPROPERTY() FString Folder;
	UPROPERTY() FString OperationId;
	UPROPERTY() bool bDryRun = true;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshRetextureRequest
{
	GENERATED_BODY()

	/** meshy or tripo. Empty: the provider that made SourceOperationId's model, else the first whose key is set (meshy, tripo). */
	UPROPERTY() FString Provider;
	/** The model to texture: a job of this session, the provider's task id, or a mesh in the project (exported and sent). */
	UPROPERTY() FString SourceOperationId;
	UPROPERTY() FString SourceTaskId;
	UPROPERTY() FString MeshPath;
	/** What the surface should look like, 800 characters at most. */
	UPROPERTY() FString Prompt;
	/** Meshy: a picture in the project to take the look from instead (PNG, JPEG or WebP, up to 20 MB). */
	UPROPERTY() FString ImagePath;
	/** Meshy: 2k (the default), 4k or 8k. Tripo: standard (the default), detailed or 8k. */
	UPROPERTY() FString Resolution;
	/** Metallic, roughness and normal maps as well as colour. */
	UPROPERTY() bool bPBR = true;
	/** Meshy: keep the model's own UVs rather than make new ones. */
	UPROPERTY() bool bKeepUVs = false;
	/** The new model's name; made from the source when empty. */
	UPROPERTY() FString Name;
	UPROPERTY() FString Folder;
	UPROPERTY() FString OperationId;
	UPROPERTY() bool bDryRun = true;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshActionsRequest
{
	GENERATED_BODY()

	/** meshy or tripo; both when empty. */
	UPROPERTY() FString Provider;
	/** Words to find in an action's name or id, such as walk, attack or dance; every action when empty. */
	UPROPERTY() FString Search;
	/** Meshy's category (WalkAndRun, BodyMovements, DailyActions, Fighting, Dancing) or Tripo's rig model (v1.0, v2.5). */
	UPROPERTY() FString Category;
	/** At most this many; 50 when 0. */
	UPROPERTY() int32 Limit = 0;
};

/** One preset animation a provider can put on its rig. */
USTRUCT(BlueprintType)
struct FHyperAIMeshAction
{
	GENERATED_BODY()

	UPROPERTY() FString Provider;
	/** What hyper_mesh_animate takes in Actions. */
	UPROPERTY() FString Id;
	UPROPERTY() FString Name;
	UPROPERTY() FString Category;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshActionsReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAIMeshAction> Actions;
	/** How many matched, before Limit. */
	UPROPERTY() int32 Matched = 0;
	/** Whether Meshy's whole library is on this computer (kept from the last Meshy animation job), or only the built-in part. */
	UPROPERTY() bool bMeshyLibraryCached = false;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshCancelRequest
{
	GENERATED_BODY()

	/** The job to stop. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIMeshCancelReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** cancelled, or not_found or already_finished. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FHyperAIMeshJob Job;
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

/**
 * Textured 3D models from Tripo, Meshy and Hi3D, imported into the project as static meshes; rigged as skeletal meshes,
 * animated from presets or words (Meshy, Tripo, Viggle), retextured, and retargeted onto the project's own skeletons.
 */
UCLASS()
class HYPERAISTUDIOMESHGEN_API UHyperAIStudioMeshGenToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.2.0"); }

	/**
	 * Makes a textured 3D model with Tripo, Meshy or Hi3D, from words (Tripo, Meshy) or a picture in the project, and
	 * imports it as a static mesh with its materials. It is paid, so it always waits for the user's approval: dry-run
	 * first (the default) to read the price. Then follow operation_id with hyper_operation_wait (bWaitThroughApproval)
	 * or hyper_mesh_status; a model takes one to five minutes. Keys come from TRIPO_API_KEY, MESHY_API_KEY or
	 * HITEM3D_API_KEY (client_id:client_secret).
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIMeshGenerateReport hyper_mesh_generate(const FHyperAIMeshGenerateRequest& Request);

	/** This session's 3D jobs: each one's state and progress, its provider tasks, files, and the assets it became. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIMeshStatusReport hyper_mesh_status(const FHyperAIMeshStatusRequest& Request);

	/**
	 * Rigs a humanoid model with Meshy or Tripo and imports it as a skeletal mesh with its skeleton and physics asset (Meshy
	 * adds a walk and a run). The model is a hyper_mesh_generate job, the provider's task id, or a mesh in the project sent
	 * as a GLB. RetargetTo (such as the UE5 Mannequin) then sets up IK Rigs and a retargeter. Paid: dry-run first (the
	 * default) to read the price, then follow operation_id with hyper_operation_wait (bWaitThroughApproval) or hyper_mesh_status.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIMeshTaskReport hyper_mesh_rig(const FHyperAIMeshRigRequest& Request);

	/**
	 * Animates a rigged model: preset actions from Meshy's or Tripo's library (hyper_mesh_actions), or a clip made from words
	 * and a duration (Meshy on its rig, or Viggle straight onto a UE5 Mannequin, MetaHuman or Mixamo skeleton, such as the
	 * project's own Mannequin as TargetMesh). Imports AnimSequences on the skeleton; RetargetTo batch-retargets them. Paid:
	 * dry-run first, then hyper_operation_wait (bWaitThroughApproval) or hyper_mesh_status.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIMeshTaskReport hyper_mesh_animate(const FHyperAIMeshAnimateRequest& Request);

	/**
	 * Textures a model again from words or (Meshy) a picture, up to 8K (Meshy 2k/4k/8k, Tripo standard/detailed/8k), and
	 * imports it with its textures and material instances. The model is a job of this session, the provider's task id or a
	 * mesh in the project. Paid: dry-run first, then hyper_operation_wait (bWaitThroughApproval) or hyper_mesh_status.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIMeshTaskReport hyper_mesh_retexture(const FHyperAIMeshRetextureRequest& Request);

	/** Searches the preset animations Meshy and Tripo can put on their rigs, by words or category: the ids hyper_mesh_animate takes. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIMeshActionsReport hyper_mesh_actions(const FHyperAIMeshActionsRequest& Request);

	/** Stops a 3D job: one waiting for approval never runs; a running one stops being followed, and nothing more is booked. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIMeshCancelReport hyper_mesh_cancel(const FHyperAIMeshCancelRequest& Request);

	/**
	 * Makes a game-ready model with Tripo or Hi3D and fixes it in the editor as part of making it. The brief (prompt
	 * and/or a picture or views, its use, platform and size) is classified into a provider (auto: Hi3D for a picture of a
	 * hero prop, environment piece, vehicle or bust, or a max-fidelity brief; Tripo otherwise), Tripo's H3.1 (high
	 * detail) or P2 (Smart Poly) or Hi3D's model and resolution, a face budget, quads or triangles, a texture tier, PBR,
	 * parts and smart low poly, each with its reason; any choice can be overridden. Words alone get a reference picture
	 * first (Tripo text-to-image, chat_image_2.5 by default; reviewed before the 3D for hero props, characters, vehicles
	 * and weapons). Then: generate (paid, approved), import, check (budget, topology, holes, winding, stray bits, size,
	 * pivot, UVs, texel density, PBR maps, Nanite, collision, a visual compare), free fixes (a free reduction for Hi3D's
	 * 100,000-face floor), and paid fixes cheapest first (Tripo's retopology, Smart UV, texture, completion, a new
	 * generation; Hi3D's texture-only retexture and a resolution up), each priced and approved, within a fix budget and a
	 * round cap; PBR maps become an ORM material instance; optionally a rig (a free rig check first). Dry-run first (the
	 * default) to read the plan and price; follow operation_id with hyper_operation_wait, then hyper_model_status.
	 * resume_operation_id picks up a journaled job.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIModelCreateReport hyper_model_create(const FHyperAIModelCreateRequest& Request);

	/** Model creator and check-and-fix jobs: state, plan, every step with its price and credits, the checks, materials, and why it stopped. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIModelStatusReport hyper_model_status(const FHyperAIModelStatusRequest& Request);

	/**
	 * Checks any static or skeletal mesh as the model creator does (such as one Tripo's DCC Bridge sent from Tripo Studio)
	 * and lists the free and paid fixes. bCheckOnly false asks the user once for the free fixes (pivot, scale, normals,
	 * winding, stray bits, small holes, auto UVs, collision, lightmap UVs, Nanite, materials); bAllowPaidFixes also offers
	 * paid Tripo fixes, each priced and approved on its own, when the Tripo API key is set.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIModelCheckFixReport hyper_model_check_and_fix(const FHyperAIModelCheckFixRequest& Request);

	/**
	 * Tripo's API credit balance (available and held) and recent per-task usage, and Hi3D's balance (Hi3DStatus,
	 * Hi3DBalance), read with the stored keys, which are never returned.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|MeshGen")
	static FHyperAIModelAccountReport hyper_tripo_account(const FHyperAIModelAccountRequest& Request);
};

class FHyperAIStudioMeshGenContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("mesh_generation");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiomeshgentoolset.v1");
	static constexpr const TCHAR* GenerateToolName = TEXT("hyper_mesh_generate");
	static constexpr const TCHAR* StatusToolName = TEXT("hyper_mesh_status");
	static constexpr const TCHAR* RigToolName = TEXT("hyper_mesh_rig");
	static constexpr const TCHAR* AnimateToolName = TEXT("hyper_mesh_animate");
	static constexpr const TCHAR* RetextureToolName = TEXT("hyper_mesh_retexture");
	static constexpr const TCHAR* ActionsToolName = TEXT("hyper_mesh_actions");
	static constexpr const TCHAR* CancelToolName = TEXT("hyper_mesh_cancel");
	static constexpr const TCHAR* ModelCreateToolName = TEXT("hyper_model_create");
	static constexpr const TCHAR* ModelStatusToolName = TEXT("hyper_model_status");
	static constexpr const TCHAR* ModelCheckFixToolName = TEXT("hyper_model_check_and_fix");
	static constexpr const TCHAR* TripoAccountToolName = TEXT("hyper_tripo_account");
	static constexpr int32 MaxPromptCharacters = 800;
	static constexpr int32 MaxMotionPromptCharacters = 400;
	static constexpr int32 MaxImageBytes = 20 * 1024 * 1024;
	/** Meshy takes up to 10 actions in one task; Tripo up to 5 a task, and a job runs up to 4 of those. */
	static constexpr int32 MaxMeshyActions = 10;
	static constexpr int32 MaxTripoActions = 20;
	/** What a provider takes as a model sent from the project: Meshy 100 MB, Tripo 150 MB. */
	static constexpr int64 MaxUploadBytes = 100 * 1024 * 1024;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIMeshGenerateReport Generate(const FHyperAIMeshGenerateRequest& Request);
	static FHyperAIMeshStatusReport Status(const FString& OperationId);
	static FHyperAIMeshTaskReport Rig(const FHyperAIMeshRigRequest& Request);
	static FHyperAIMeshTaskReport Animate(const FHyperAIMeshAnimateRequest& Request);
	static FHyperAIMeshTaskReport Retexture(const FHyperAIMeshRetextureRequest& Request);
	static FHyperAIMeshActionsReport Actions(const FHyperAIMeshActionsRequest& Request);
	static FHyperAIMeshCancelReport Cancel(const FString& OperationId);

	// Pure parts, for tests.
	/** Meshy's resolution or Tripo's quality, as the provider takes it (2k, 4k, 8k; standard, detailed, extreme); empty when refused. */
	static FString TextureLevel(const FString& Provider, const FString& Resolution);
	/** The skeleton a Viggle clip goes onto unchanged, from the target's bone names: metahuman_body, mixamo, or empty. */
	static FString ViggleSkeletonFor(const TArray<FString>& BoneNames);
	/** A Tripo preset in full for the rig model (preset:walk, preset:biped:walk); empty when the rig model has no such preset. */
	static FString TripoPreset(const FString& RigModel, const FString& Action);
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
