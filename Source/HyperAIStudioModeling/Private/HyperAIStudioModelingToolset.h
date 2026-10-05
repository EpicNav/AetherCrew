// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioModelingToolset.generated.h"

/**
 * One op of a model plan. Ops work on named in-memory meshes: an add.* op or a recipe makes a mesh, later ops change
 * it, and write_asset saves one as a static mesh. Vectors are "x,y,z" in cm, rotations "pitch,yaw,roll" in degrees.
 */
USTRUCT(BlueprintType)
struct FHyperAIModelOp
{
	GENERATED_BODY()

	/**
	 * add.box, add.sphere, add.cylinder, add.cone, add.capsule, add.torus, add.disc, add.stairs, add.curved_stairs,
	 * add.revolve, add.sweep, add.extrude_polygon, append, copy, array.linear, array.radial, scatter_on, boolean,
	 * self_union, plane_cut, mirror, extrude, inset, bevel, shell, noise, bend, twist, flare, smooth, tessellate, remesh,
	 * simplify, solidify, morphology, transform, pivot, normals, repair, uv, material, color, drop, recipe, write_asset.
	 * hyper_model_inspect lists each one's fields and Params.
	 */
	UPROPERTY() FString Kind;
	/** The working mesh the op makes or changes: 1 to 48 letters, digits, _ or -. */
	UPROPERTY() FString Mesh;
	/** append, copy, array.*, scatter_on, boolean: the other mesh, by name. */
	UPROPERTY() FString Source;
	/** add.*: where the shape's base center sits (its center with Origin=center); transform, append, copy: a move; plane_cut, mirror: a point on the plane. */
	UPROPERTY() FString Location;
	/** "pitch,yaw,roll" degrees. */
	UPROPERTY() FString Rotation;
	/** "x,y,z" or one number. */
	UPROPERTY() FString Scale;
	/** add.*: the shape's size "x,y,z" in cm. Cylinders, cones, capsules, discs: X and Y across, Z tall; a torus: outer X and Y, Z the tube's thickness; stairs: run X, width Y, rise Z. */
	UPROPERTY() FString Size;
	/** add.revolve: a profile "r,z; r,z" (radius, height) turned about Z; add.sweep, add.extrude_polygon: a cross-section "x,y; x,y", or circle:radius,sides or rect:width,depth. */
	UPROPERTY() FString Points;
	/** add.sweep: the spine "x,y,z; x,y,z" the cross-section follows. */
	UPROPERTY() FString Path;
	/** What part of the mesh: all, top, bottom, normal:nx,ny,nz,maxdeg, box:x0,y0,z0,x1,y1,z1, sphere:x,y,z,r, plane:ox,oy,oz,nx,ny,nz or material:id. Params Invert=true and Grow=n adjust it. */
	UPROPERTY() FString Select;
	/** "Name=Value; Name=Value", the names each kind takes (hyper_model_inspect lists them). */
	UPROPERTY() FString Params;
	/** write_asset: the static mesh to write, under /Game (/Game/HyperAI/Models/SM_Rock), from the mesh as it is at this op. */
	UPROPERTY() FString Asset;
};

USTRUCT(BlueprintType)
struct FHyperAIModelPlanRequest
{
	GENERATED_BODY()

	/** In order; recipes expand into ordinary ops, listed in the reply's ExpandedOps. */
	UPROPERTY() TArray<FHyperAIModelOp> Operations;
	/** Build the meshes in memory and report them; nothing is written. */
	UPROPERTY() bool bDryRun = false;
	/** Names the edit, for hyper_operation_wait; made when empty. */
	UPROPERTY() FString OperationId;
};

/** A working mesh as the plan left it. */
USTRUCT(BlueprintType)
struct FHyperAIModelMeshStats
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() int32 Triangles = 0;
	UPROPERTY() int32 Vertices = 0;
	/** Bounds, "x,y,z" cm. */
	UPROPERTY() FString Size;
	UPROPERTY() FString Min;
	UPROPERTY() FString Max;
	/** No open edges: a watertight surface. */
	UPROPERTY() bool bClosed = false;
	UPROPERTY() int32 OpenEdges = 0;
	/** Separate pieces. */
	UPROPERTY() int32 Components = 0;
	UPROPERTY() int32 UVSets = 0;
	/** Material slots its triangles use (the highest material id + 1). */
	UPROPERTY() int32 MaterialIds = 0;
};

/** A static mesh write_asset wrote, or would write on a dry run. */
USTRUCT(BlueprintType)
struct FHyperAIModelAssetResult
{
	GENERATED_BODY()

	UPROPERTY() FString Path;
	UPROPERTY() FString Mesh;
	UPROPERTY() int32 Triangles = 0;
	UPROPERTY() int32 Slots = 0;
	UPROPERTY() bool bNanite = false;
	/** The collision asked for, and its simple shapes. */
	UPROPERTY() FString Collision;
	UPROPERTY() int32 CollisionShapes = 0;
	/** It replaced an existing asset (Replace=true), which undo takes back. */
	UPROPERTY() bool bReplaced = false;
};

USTRUCT(BlueprintType)
struct FHyperAIModelPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, approval_not_queued, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	/** Hash of the ops after recipes expand: the same ops and seeds build the same meshes. Written assets keep it. */
	UPROPERTY() FString PlanHash;
	/** Every working mesh the plan ends with, built in memory, also on a dry run. */
	UPROPERTY() TArray<FHyperAIModelMeshStats> Meshes;
	UPROPERTY() TArray<FHyperAIModelAssetResult> Assets;
	/** The ops recipes expanded into, as run: numbered, one per line. */
	UPROPERTY() TArray<FString> ExpandedOps;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
	/** Milliseconds the meshes took to build. */
	UPROPERTY() int32 BuildMs = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIModelInspectRequest
{
	GENERATED_BODY()

	/** The static meshes under this folder are listed: /Game/HyperAI/Models when empty. */
	UPROPERTY() FString Folder;
	/** One static mesh to read in full: slots, collision, Nanite and the plan that made it. */
	UPROPERTY() FString Asset;
};

USTRUCT(BlueprintType)
struct FHyperAIModelInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	/** Each op kind: its fields and Params, and what it does. */
	UPROPERTY() TArray<FString> Kinds;
	/** Each recipe: what it makes and its default size. */
	UPROPERTY() TArray<FString> Recipes;
	UPROPERTY() TArray<FString> Limits;
	/** Static meshes in Folder: path, triangles, size, material slots, Nanite. */
	UPROPERTY() TArray<FString> Assets;
	/** Asset read in full. */
	UPROPERTY() TArray<FString> Details;
	/** With Asset: its geometry fingerprint (source positions, triangles, material slots, first UV set), which changes when the mesh is edited by hand or rewritten. */
	UPROPERTY() FString Fingerprint;
	UPROPERTY() bool bTruncated = false;
};

USTRUCT(BlueprintType)
struct FHyperAIModelValidateRequest
{
	GENERATED_BODY()

	/** Static meshes to check (/Game/HyperAI/Models/SM_Rock). */
	UPROPERTY() TArray<FString> Assets;
	/** placeholder (the default) or production, which also wants simple collision and fewer triangles without Nanite. */
	UPROPERTY() FString Policy;
};

USTRUCT(BlueprintType)
struct FHyperAIModelIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Asset;
	/** error or warning. */
	UPROPERTY() FString Severity;
	/** empty_mesh, no_render_data, too_many_triangles, too_small, too_large, no_collision, empty_material_slot, open_edges, pivot_not_at_base, no_uvs, nanite_off. */
	UPROPERTY() FString Code;
	UPROPERTY() FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIModelValidateReport
{
	GENERATED_BODY()

	/** No errors (warnings allowed). */
	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAIModelIssue> Issues;
	/** Each asset checked: triangles, size, slots, collision shapes, open edges. */
	UPROPERTY() TArray<FString> Assets;
};

USTRUCT(BlueprintType)
struct FHyperAIModelHandoffRequest
{
	GENERATED_BODY()

	/** The static mesh to hand over, under /Game (/Game/HyperAI/Models/SM_Rock). An actor already showing it is used; with none, one is put in the level. Give Asset or Actor. */
	UPROPERTY() FString Asset;
	/** Or an actor in the open level, by its label or full path, whose one static mesh is handed over where it stands. */
	UPROPERTY() FString Actor;
	/** "x,y,z" cm: put a new actor here. Empty: where the level viewport looks (what it hits within 50 m). */
	UPROPERTY() FString Location;
	/** The new actor's label, 1 to 64 letters, digits, _ or -; empty makes HyperAI_Edit_<asset name>. Location or Label always puts a new actor. */
	UPROPERTY() FString Label;
	/** A Modeling Mode tool to point the person at: PolyEdit, TriEdit, PolyGroups, Sculpt, Smooth, Remesh, Simplify or UVs. Not started; the reply names its palette and button. */
	UPROPERTY() FString Tool;
	/** Leave the editor's tabs as they are: the level editor is not brought in front of HyperAI Studio. */
	UPROPERTY() bool bLeaveTabsAlone = false;
	/** Check and say what would happen; nothing is put in the level, selected or switched. */
	UPROPERTY() bool bDryRun = false;
	/** Names the hand-off, for hyper_operation_wait; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIModelHandoffReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, approval_not_queued, invalid_request, unavailable, operation_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	/** The static mesh handed over. */
	UPROPERTY() FString Asset;
	UPROPERTY() FString ActorLabel;
	/** Empty for an actor not yet put in the level (a dry run, or one waiting for approval). */
	UPROPERTY() FString ActorPath;
	/** The actor was put in the level for the hand-off (on a dry run: would be); Edit > Undo takes it away. */
	UPROPERTY() bool bPlaced = false;
	/** Where the actor stands, "x,y,z" cm. */
	UPROPERTY() FString Location;
	/** It is the only actor selected. */
	UPROPERTY() bool bSelected = false;
	/** Modeling Mode is active in the level editor. */
	UPROPERTY() bool bModeActive = false;
	/** brought_to_front, already_in_front, left_alone or no_level_editor_tab. */
	UPROPERTY() FString Viewport;
	/** Level viewports that now frame the actor. */
	UPROPERTY() int32 FramedViewports = 0;
	/** With Tool: its palette and button, "Deform > Vertex Sculpt". */
	UPROPERTY() FString ToolHint;
	/** That button accepts the selection now, as Modeling Mode's palette judges it; nothing is started. */
	UPROPERTY() bool bToolReady = false;
	/** The asset's geometry as handed over; hyper_model_inspect's Fingerprint differs once the person's edit is accepted. */
	UPROPERTY() FString Fingerprint;
	UPROPERTY() int32 Triangles = 0;
	UPROPERTY() int32 Vertices = 0;
	/** Other actors in the level showing the asset: an accepted edit changes them too. */
	UPROPERTY() int32 SharedBy = 0;
	/** What the person does next, in order. */
	UPROPERTY() TArray<FString> NextSteps;
	UPROPERTY() TArray<FString> Notes;
};

/**
 * 3D modelling with Geometry Script: named meshes built in memory from primitives, sweeps and revolves, booleans,
 * cuts, selection modelling, warps, remeshing, UVs, material ids and vertex colours, with recipe macros for props,
 * then written as static meshes with collision. Modeling Mode's interactive tools are left to people:
 * hyper_model_handoff hands a mesh over and reads the edit back.
 */
UCLASS()
class HYPERAISTUDIOMODELING_API UHyperAIStudioModelingToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Lists the model plan's op kinds with their fields and Params, the recipes, the limits, and the static meshes in a
	 * folder (triangles, size, slots, Nanite); with Asset, one mesh in full: collision, slots, the plan that made it, and
	 * its Fingerprint, which changes when the mesh is edited.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Modeling")
	static FHyperAIModelInspectReport hyper_model_inspect(const FHyperAIModelInspectRequest& Request);

	/**
	 * Free 3D models in one undo step: primitives, revolve, sweep, booleans, plane cut, mirror, extrude/inset/bevel,
	 * noise/bend/twist/flare, remesh, simplify, solidify, UVs, material ids, vertex colour, and recipes (rock, arch,
	 * tower, tree, crystal_cluster...), then write_asset to a /Game static mesh with collision. Dry run builds in memory.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Modeling")
	static FHyperAIModelPlanReport hyper_model_apply_plan(const FHyperAIModelPlanRequest& Request);

	/**
	 * Checks static meshes as props: render data, triangles against the policy, size, open edges, pivot at the base,
	 * UVs, material slots, collision, and Nanite on heavy meshes. Policy placeholder or production.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Modeling")
	static FHyperAIModelValidateReport hyper_model_validate(const FHyperAIModelValidateRequest& Request);

	/**
	 * Hands a /Game static mesh to a person in Unreal's Modeling Mode to edit by hand: selects an actor showing it (one
	 * is put in the level when none is, in one undo step), frames it, brings the level viewport in front of the Studio
	 * and opens the mode. No tool is started. The person edits, Accepts and saves; inspect's Fingerprint then shows the change.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Modeling")
	static FHyperAIModelHandoffReport hyper_model_handoff(const FHyperAIModelHandoffRequest& Request);
};

class FHyperAIStudioModelingContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("mesh_modeling");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiomodelingtoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_model_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_model_apply_plan");
	static constexpr const TCHAR* ValidateToolName = TEXT("hyper_model_validate");
	static constexpr const TCHAR* HandoffToolName = TEXT("hyper_model_handoff");

	/** Ops a request may send, and ops after recipes expand. */
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxExpandedOperations = 160;
	/** Working meshes alive at once, recipes' parts included. */
	static constexpr int32 MaxMeshes = 32;
	static constexpr int32 MaxAssets = 8;
	static constexpr int32 MaxProfilePoints = 128;
	static constexpr int32 MaxPathPoints = 256;
	static constexpr int32 MaxArrayCount = 64;
	static constexpr int32 MaxScatterCount = 128;
	/** Triangles one working mesh may hold, one asset without and with Nanite, and all meshes of a plan at once. */
	static constexpr int32 MaxTrianglesPerMesh = 250000;
	static constexpr int32 MaxTrianglesPerAsset = 100000;
	static constexpr int32 MaxTrianglesPerNaniteAsset = 250000;
	static constexpr int32 MaxTrianglesPerPlan = 1500000;
	static constexpr int32 MaxTessellateLevel = 3;
	/** Voxel grid cells along the longest side, for solidify and morphology. */
	static constexpr int32 MaxGridResolution = 256;
	static constexpr int32 MaxRemeshTriangles = 200000;
	/** Milliseconds the editor may spend building one plan's meshes. */
	static constexpr int32 MaxBuildMs = 8000;
	/** Package metadata key holding the hash of the ops that made an asset. */
	static constexpr const TCHAR* PlanHashKey = TEXT("HyperAI.ModelPlanHash");
	static constexpr const TCHAR* DefaultFolder = TEXT("/Game/HyperAI/Models");
	/** The vertex-colour material placeholders get unless a plan names materials, made on first use. */
	static constexpr const TCHAR* VertexColorMaterial = TEXT("/Game/HyperAI/Modeling/M_HyperAIVertexColor");

	/**
	 * UModelingToolsEditorMode::EM_ModelingToolsEditorModeId (ModelingToolsEditorMode.cpp:164 in 5.8), by name: linking
	 * the Beta plugin would keep this module, and with it the other modeling tools, from loading without it.
	 */
	static constexpr const TCHAR* ModelingModeId = TEXT("EM_ModelingToolsEditorMode");
	static constexpr const TCHAR* ModelingModePlugin = TEXT("ModelingToolsEditorMode");
	/** The outliner folder, and the label prefix, of an actor a hand-off puts in the level. */
	static constexpr const TCHAR* HandoffFolder = TEXT("HyperAI/Modeling");
	static constexpr const TCHAR* HandoffLabelPrefix = TEXT("HyperAI_Edit_");
	/** How far from the origin a placed actor may stand on each axis, cm (20 km). */
	static constexpr double MaxHandoffCoordinate = 2000000.0;
	/** How far the hand-off looks along the viewport's view for somewhere to stand, cm. */
	static constexpr double HandoffTraceDistance = 5000.0;
	/** Above this, Modeling Mode's tools are slow; the reply suggests Simplify or Remesh first. */
	static constexpr int32 HeavyHandoffTriangles = 500000;
	/** Above this many source triangles a mesh is not fingerprinted: hashing it would hold the editor too long. */
	static constexpr int32 MaxFingerprintTriangles = 4000000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIModelInspectReport Inspect(const FHyperAIModelInspectRequest& Request);
	static FHyperAIModelPlanReport ApplyPlan(const FHyperAIModelPlanRequest& Request);
	static FHyperAIModelValidateReport Validate(const FHyperAIModelValidateRequest& Request);
	static FHyperAIModelHandoffReport Handoff(const FHyperAIModelHandoffRequest& Request);
#if WITH_DEV_AUTOMATION_TESTS
	/** Test runs are unattended, which a hand-off refuses: this lets them through, and can name a mode id that does not exist. */
	static void SetHandoffTestHooks(bool bAllowUnattended, FName ModeId = NAME_None);
#endif
};

class FHyperAIStudioModelingRegistration final
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
