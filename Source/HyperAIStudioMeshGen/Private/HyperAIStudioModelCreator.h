// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HyperAIStudioModelCreatorTypes.h"

class UObject;
class UStaticMesh;
struct FHyperAIMeshCancelReport;
struct FHyperAIStudioImageModelSettings;

/**
 * The Tripo model creator: a brief classified into a plan (the provider, Tripo's H3.1 or P2 or Hi3D, the face budget,
 * quads or triangles, the texture tier and the add-ons, each with its reason), the provider's requests made from that
 * plan, a generated model checked in the editor, fixed the cheapest correct way first (free in the editor, then paid
 * steps, each priced and approved), its PBR maps made into a material instance, and every step journaled. Built on the
 * 3D jobs of HyperAIStudioMeshGenJob.h.
 */
namespace HyperAIStudio::MeshGen::Private
{
	struct FForm;
}

namespace UE::Geometry
{
	class FDynamicMesh3;
}

namespace HyperAIStudio::MeshGen::Creator
{
	/** Tripo's models, as its v3 docs list them (developers.tripo3d.ai/en/docs/models-and-versions, October 2026). */
	constexpr const TCHAR* H31Model = TEXT("v3.1-20260211");
	constexpr const TCHAR* P2Model = TEXT("P2-20260801");
	constexpr const TCHAR* TextureVersion = TEXT("v3.5-20260815");
	constexpr const TCHAR* RetopoModel = TEXT("v3.0");
	constexpr const TCHAR* SegmentModel = TEXT("v2.0-20260430");
	constexpr const TCHAR* RigModelV3 = TEXT("v3.0-20260909");

	/** The user's ranges: H3.1 500-2,000,000 triangles or 500-50,000 quads; P2 500-50,000 triangles or 500-25,000 quads. */
	constexpr int32 MinFaces = 500;
	constexpr int32 H31MaxTris = 2000000;
	constexpr int32 H31StandardMaxTris = 1500000;
	constexpr int32 H31MaxQuads = 50000;
	constexpr int32 P2MaxTris = 50000;
	constexpr int32 P2MaxQuads = 25000;
	constexpr int32 SmartLowPolyMaxTris = 20000;
	constexpr int32 SmartLowPolyMaxQuads = 10000;
	/** Retopology v3.0 and Smart UV limits (mesh-decimate, mesh-uv): retopo 500-20,000 triangles or 500-10,000 quads, UV 80,000 triangles. */
	constexpr int32 RetopoMaxTris = 20000;
	constexpr int32 RetopoMaxQuads = 10000;
	/**
	 * Decimation v1.0 (mesh-decimate's model v1.0): face_limit required, up to 2,000,000 triangles or 150,000 quads, no
	 * smart UV and no bake, 10 credits. What a budget above v3.0's 20,000 reduces with.
	 */
	constexpr const TCHAR* RetopoModelV1 = TEXT("v1.0");
	constexpr int32 RetopoV1MaxTris = 2000000;
	constexpr int32 RetopoV1MaxQuads = 150000;
	constexpr int32 SmartUvMaxTris = 80000;
	constexpr int32 MaxPromptCharacters = 1024;
	constexpr int32 MaxNegativeCharacters = 255;

	// ---------------------------------------------------------------- classification

	/**
	 * The brief made into a plan; false, with a reason, when an override cannot be honoured (P2 with parts, say). bRig:
	 * the job will rig the model, which only Tripo does, so auto keeps it with Tripo.
	 */
	bool Classify(const FHyperAIModelBrief& Brief, FHyperAIModelPlan& Out, FString& OutError, bool bRig = false);
	/** tripo or hi3d for a brief, with the reason; empty, with OutError, when the brief asks for something its provider cannot do. */
	FString ChooseProvider(const FHyperAIModelBrief& Brief, bool bRig, FString& OutReason, FString& OutError);
	/** The use a brief's words point to, or empty: "a sci-fi soldier" is a character, "a stone wall section" a kit piece. */
	FString UseFromWords(const FString& Words);

	// ---------------------------------------------------------------- Tripo v3 requests

	/** The generation's body; Inputs are the file tokens of the picture or the views (front, left, back, right, "" skipping one). */
	TSharedRef<FJsonObject> GenerationBody(const FHyperAIModelPlan& Plan, const FHyperAIModelBrief& Brief, const TArray<FString>& Inputs);
	/** v3.0 (smart UV and a bake, up to 20,000 triangles), or with bV1 decimation v1.0 (up to 2,000,000, neither). */
	TSharedRef<FJsonObject> RetopoBody(const FString& Input, int32 FaceLimit, bool bQuad, bool bSmartUv, bool bV1 = false);
	TSharedRef<FJsonObject> SmartUvBody(const FString& Input);
	/**
	 * Image, when set, goes as texture_prompt.image (the reference: Tripo recommends sending it again when the input is a
	 * task); Seed as texture_seed (0 leaves it out); Alignment as texture_alignment (original_image or geometry).
	 */
	TSharedRef<FJsonObject> TextureBody(const FString& Input, const FString& Quality, bool bPbr, const TArray<FString>& PartNames, const FString& Prompt,
		const FString& Image = FString(), int32 Seed = 0, const FString& Alignment = FString());
	struct FConvertOptions
	{
		FString Format = TEXT("FBX");
		bool bQuad = false;
		int32 FaceLimit = 0;
		bool bFlattenBottom = false;
		bool bPivotToCenterBottom = false;
		float ScaleFactor = 1.f;
		int32 TextureSize = 0;
		FString TextureFormat;
		bool bPackUv = false;
	};
	TSharedRef<FJsonObject> ConvertBody(const FString& Input, const FConvertOptions& Options);
	TSharedRef<FJsonObject> SegmentBody(const FString& Input);
	TSharedRef<FJsonObject> CompleteBody(const FString& SegmentTaskId, const TArray<FString>& PartNames, bool bQuickCap);
	TSharedRef<FJsonObject> ImageToMultiviewBody(const FString& Input);
	/** Edits are {view, prompt}: one to four, each a distinct view of front, left, back, right. */
	TSharedRef<FJsonObject> EditMultiviewBody(const FString& MultiviewTaskId, const TArray<TPair<FString, FString>>& Edits);
	/** Tripo's rig: RigModel v3.0-20260909 with spec unreal for a biped, v2.5-20260210 with the rig-check's rig_type (no spec) otherwise. */
	TSharedRef<FJsonObject> RigBody(const FString& Input, const FString& RigModel, const FString& RigType, const FString& Spec);
	/** The free rig check (/animations/rig-check): whether the model can be rigged, and as what. */
	TSharedRef<FJsonObject> RigCheckBody(const FString& Input);
	/** The rig model and spec for a rig-check's rig_type: biped is v3.0 with spec unreal, anything else v2.5 and no spec. */
	void RigChoice(const FString& RigType, FString& OutModel, FString& OutSpec);
	constexpr const TCHAR* RigModelV25 = TEXT("v2.5-20260210");

	// ---------------------------------------------------------------- the reference picture (Tripo text-to-image)

	/**
	 * An image model's id from what a person calls it: "gpt-image-2.5-sunburn" is chat_image_2.5_sunburst, "nano banana pro"
	 * banana_pro, "seedream 5" seedream_v5. False, with the ids it takes, for anything else or a retired model; OutNote
	 * says how a name was read, or warns of a model about to retire.
	 */
	bool ImageModelFromName(const FString& In, FString& OutId, FString& OutNote);
	/** Tripo's image models, newest of each family first. */
	TArray<FString> TripoImageModels();
	/** Whether a model takes quality (chat_image_2 low/medium/high; the 2.5 models also xhigh and max), background (2.5 only). */
	bool ImageModelTakesQuality(const FString& Model, const FString& Quality);
	bool ImageModelTakesBackground(const FString& Model);
	/** False, with a reason, when the model would refuse the size (presets, or custom sizes inside its rules). */
	bool ImageSizeValid(const FString& Model, const FString& Size, FString& OutError);
	/** 1k, 2k or 4k by the size's longest side; auto bills as 2k. */
	FString ImageSizeTier(const FString& Size);
	/** A picture's price at Tripo, in USD, from MeshTaskPrices (img_<model>[_<quality>]_<tier>). */
	float ImagePrice(const FString& Model, const FString& Quality, const FString& Size);
	/** /generation/text-to-image for the plan's reference: model, prompt (negatives after --no), size, quality, background, format, template. */
	TSharedRef<FJsonObject> TextToImageBody(const FHyperAIModelPlan& Plan);
	/** Hi3D from words with no Tripo key: the picture is HyperAI's image generation's instead, priced again, in the same approval. */
	void UseImageGenReference(FHyperAIModelPlan& Plan);

	// ---------------------------------------------------------------- Hi3D (HyperAIStudioModelCreatorHi3D.cpp)

	/**
	 * Hi3D's open API as docs.hi3d.ai documents it (read 31 October 2026): a token from client_id:client_secret, a
	 * multipart submit-task, query-task, and the balance. Its models: v3.0 (Ultra3D, the default), v2.1, v2.0 and v1.5
	 * (Sparc3D), and portrait models for heads and busts. Faces 100,000 to 2,000,000 (5,000,000 at 2048master).
	 */
	constexpr const TCHAR* Hi3DDefaultModel = TEXT("hi3dv3.0");
	constexpr const TCHAR* Hi3DDefaultPortrait = TEXT("scene-portraitv2.1");
	constexpr const TCHAR* Hi3DSubmitPath = TEXT("/open-api/v1/submit-task");
	constexpr int32 Hi3DMinFaces = 100000;
	constexpr int32 Hi3DMaxFaces = 2000000;
	constexpr int32 Hi3DMasterMaxFaces = 5000000;
	constexpr int64 Hi3DMaxFileBytes = 20 * 1024 * 1024;

	/** One Hi3D task: what submit-task is sent. */
	struct FHi3DRequest
	{
		/** 1 geometry only, 2 texture only on Mesh (a GLB), 3 both. */
		int32 RequestType = 3;
		FString Model = Hi3DDefaultModel;
		FString Resolution = TEXT("2048quality");
		/** Faces asked for; not sent for texture only. */
		int32 Faces = 2000000;
		bool bPbr = true;
		bool bRemoveBackground = true;
		/** De-shading strength, 0.0-1.0 in tenths (v2.0, v2.1 and v3.0). */
		float Shading = 0.5f;
		/** One picture (images), or views in Hi3D's order front, back, left, right ("" skips one) sent as multi_images. */
		TArray<FString> Images;
		bool bMultiview = false;
		/** The GLB textured by request type 2. */
		FString Mesh;
	};

	TArray<FString> Hi3DModels();
	/** The resolutions a model takes, as the docs list them; empty for a model Hi3D does not have. */
	TArray<FString> Hi3DResolutions(const FString& Model);
	/** The faces Hi3D recommends at a resolution: 500K at 512, 1M at 1024, 2M at 1536 and 2048quality, 5M at 2048master. */
	int32 Hi3DRecommendedFaces(const FString& Resolution);
	int32 Hi3DMaxFacesFor(const FString& Resolution);
	bool Hi3DIsModel(const FString& Model);
	bool Hi3DIsPortrait(const FString& Model);
	/** PBR and de-shading: v2.0, v2.1 and v3.0 (portrait v2.x too). */
	bool Hi3DSupportsPbr(const FString& Model);
	/** Texture only (request type 2): offered on v3.0 and v1.5 alone, since Hi3D's docs disagree about the others. */
	bool Hi3DCanTextureOnly(const FString& Model);
	/** The next resolution up for a model, for a regeneration; empty when it is at its best. */
	FString Hi3DResolutionUp(const FString& Model, const FString& Resolution);
	/** False, with a reason, when Hi3D would refuse the request (model, resolution, faces, PBR, images, mesh). */
	bool Hi3DValidate(const FHi3DRequest& Request, FString& OutError);
	/** The form's text fields in the order sent, and its files as (field, path); what the plan's request shows. */
	void Hi3DFields(const FHi3DRequest& Request, TArray<TPair<FString, FString>>& OutText, TArray<TPair<FString, FString>>& OutFiles);
	/** The multipart form, with each file read and checked (format, 20 MB); false, with a reason, before anything is sent. */
	bool Hi3DForm(const FHi3DRequest& Request, HyperAIStudio::MeshGen::Private::FForm& Out, FString& OutError);
	/** What one of Hi3D's error codes means, in words to act on; empty for a code it does not document. */
	FString Hi3DCodeMessage(const FString& Code);
	/** HTTP 401, or a token Hi3D no longer takes: get a new one and send again. */
	bool Hi3DIsAuthError(int32 HttpCode, const FString& Code);
	/** The Authorization header the token call sends: Basic base64(client_id:client_secret). Never logged. */
	FString Hi3DBasicAuth(const FString& Key);
	/** HyperAI's image model for a reference picture when there is no Tripo key: the first whose service has its key, else the first. */
	bool ReferenceImageModel(FHyperAIStudioImageModelSettings& Out);
	/** What that image model charges a picture, in USD. */
	float ReferenceImagePrice(const FString& ModelId);
	/** The creator's views (front, left, back, right) in Hi3D's order (front, back, left, right). */
	TArray<FString> Hi3DViewOrder(const TArray<FString>& FrontLeftBackRight);
	/** The plan's generation as a Hi3D task, from absolute pictures (one, or views in Hi3D's order). */
	FHi3DRequest Hi3DRequestFromPlan(const FHyperAIModelPlan& Plan, const TArray<FString>& Images, bool bMultiview);
	/** Hi3D's balance (data.totalBalance), with a token from the key; OnDone on the game thread: ok, missing_key, bad_key or error. */
	void Hi3DReadBalance(TFunction<void(const FString& Status, float Balance, const FString& Diagnostic)> OnDone);
#if WITH_DEV_AUTOMATION_TESTS
	/** Drops the cached Hi3D access token, so the next call asks for a new one; or sets one Hi3D will then refuse. */
	void ForgetHi3DTokenForTests();
	void SetHi3DTokenForTests(const FString& Token);
#endif

	/** Hi3D's price for a model and resolution in MeshTaskPrices, in USD: request type 3 with or without PBR, or 1. */
	float Hi3DPrice(const FString& Model, const FString& Resolution, int32 RequestType, bool bPbr);
	/** Texture only (request type 2): texture_only, or texture_only_pbr. */
	float Hi3DTexturePrice(bool bPbr);
	/** Credits for a price, at Hi3D's $0.02 a credit. */
	int32 Hi3DCredits(float USD);
	/** The provider's credits for a price: Tripo's at $0.01, Hi3D's at $0.02. */
	int32 ProviderCredits(const FString& Provider, float USD);

	// ---------------------------------------------------------------- prices

	/** A Tripo price in MeshTaskPrices, in USD; the built-in list when the user has not set it. */
	float Price(const TCHAR* Task);
	/** Credits for a price, at Tripo's $0.01 a credit. */
	int32 Credits(float USD);
	/** The generation's price lines and total, add-ons and a parts model's texture task included. */
	float GenerationPrice(const FHyperAIModelPlan& Plan, TArray<FString>* OutLines = nullptr);
	/** The texture task's price key for a tier: texture, texture_hd or texture_8k. */
	const TCHAR* TexturePriceKey(const FString& Quality);

	// ---------------------------------------------------------------- the fix ladder

	/** One paid fix: the endpoint, its body, its price, and the checks it is meant to clear. */
	struct FPaidFix
	{
		/** Tripo's retopo, smart_uv, texture, convert, quick_cap, complete, regenerate; Hi3D's hi3d_texture, hi3d_regenerate. */
		FString Id;
		FString What;
		/** tripo or hi3d. */
		FString Provider = TEXT("tripo");
		/** One or two Tripo tasks in order (segment then complete), or Hi3D's submit-task. */
		TArray<FString> Endpoints;
		TArray<TSharedPtr<FJsonObject>> Bodies;
		/** A Hi3D fix's task. */
		FHi3DRequest Hi3D;
		float PriceUSD = 0.f;
		TArray<FString> Clears;
	};

	/** What the ladder knows about the model it fixes. */
	struct FFixContext
	{
		/** The Tripo task the model is, while Tripo can take it as an input (its own generation, not changed by a paid fix since). */
		FString InputTaskId;
		/** The model's file token, when it was uploaded instead. */
		FString InputToken;
		bool bHasTextures = false;
		bool bSkeletal = false;
		/** The fixes already tried, and how often, so the ladder climbs instead of repeating itself. */
		TMap<FString, int32> Tried;
		/** USD left in the job's fix budget. */
		float RemainingUSD = 0.f;
		/** The generation's plan, for the budget, topology and texture tier; empty Family when the model did not come from the creator. */
		FHyperAIModelPlan Plan;
		/** Whether the creator can make the model again from its brief. */
		bool bCanRegenerate = false;
		/** Who made it: a Hi3D model gets Hi3D's texture and regeneration, and Tripo's fixes only with Tripo's key (bTripo). */
		FString Provider = TEXT("tripo");
		bool bTripo = true;
		/** Hi3D's pictures for a new texture or a new generation, in its order front, back, left, right, and whether they are views. */
		TArray<FString> Hi3DImages;
		bool bHi3DMultiview = false;
		/** The reference picture as Tripo takes it (its text-to-image task, or the picture's file token), for texture_prompt.image. */
		FString ReferenceInput;
	};

	/** The paid fixes that would clear something still failing, cheapest first; those over the remaining budget are left out. */
	TArray<FPaidFix> PaidLadder(const FHyperAIModelCheckReport& Report, const FFixContext& Context, TArray<FString>* OutOverBudget = nullptr);
	/** The free fixes a report calls for, in the order they run. */
	TArray<FString> LocalFixesFor(const FHyperAIModelCheckReport& Report);

	// ---------------------------------------------------------------- checks and free fixes (HyperAIStudioModelInspect.cpp)

	// ---------------------------------------------------------------- weapons (HyperAIStudioModelCreatorWeapon.cpp)

	/**
	 * How a weapon is finished in the editor, free: the long axis on Y with the head toward -Y and the handle toward +Y,
	 * the pivot in the grip, the real length, then thinned to real proportions, the flat of the blade across X, and reduced
	 * to the budget, Nanite off. HyperAI's defaults, or the project's convention, or a reference mesh's.
	 */
	struct FWeaponFinish
	{
		float LengthCm = 0.f;
		int32 TargetTris = 20000;
		float BladeRatio = 0.18f;
		float GripCm = 3.4f;
		float GuardDepthCm = 3.f;
		/** From the handle's end, 0-1; 0 puts the pivot in the middle of the grip. */
		float PivotFraction = 0.f;
		bool bNanite = false;
		int32 ColorSize = 2048;
		int32 NormalSize = 2048;
		int32 MaskSize = 1024;
		/** Where these came from, for the plan's reasons and the notes: defaults, the project's convention, or a reference. */
		FString Source;
	};

	/**
	 * A weapon measured where it lies. Sections along its longest axis: the widest is the guard (or an axe's head), the
	 * handle the narrower side of it; a weapon whose widest section is at one end (axe, hammer, mace) is headed.
	 */
	struct FWeaponShape
	{
		bool bValid = false;
		/** 0 X, 1 Y, 2 Z; and whether the handle's end is toward + on it. */
		int32 Axis = 1;
		bool bHandlePositive = true;
		bool bHeaded = false;
		float Length = 0.f;
		/** The axis across the blade's thickness (never Axis). */
		int32 ThicknessAxis = 2;
		/** The blade's (or head's) median width and thickness, and their ratio; 0 when there is none. */
		float BladeWidth = 0.f;
		float BladeThickness = 0.f;
		float BladeRatio = 0.f;
		float GripCm = 0.f;
		float GuardDepthCm = 0.f;
		float GuardSpanCm = 0.f;
		/** The origin along the length from the handle's end (0-1), and whether it lies within the grip. */
		float PivotFraction = 0.f;
		bool bPivotInGrip = false;
	};

	FWeaponShape MeasureWeapon(const TArray<FVector3d>& Points);
	/**
	 * Orients, places, scales and thins a weapon in place (no reduction): what the free weapon_finish fix does to the
	 * geometry before the simplifier. Notes say what changed. False when it does not read as a weapon.
	 */
	bool ShapeWeapon(UE::Geometry::FDynamicMesh3& Mesh, const FWeaponFinish& Finish, TArray<FString>& OutNotes);
	/** The finishing a plan's weapon gets: the project's convention when there is one, else HyperAI's defaults. */
	FWeaponFinish WeaponFinishFor(float SizeMeters, int32 Triangles);
	/**
	 * Measures a reference static mesh (match_asset) into a finishing and its reasons: triangles, Nanite, LODs, UV channels,
	 * slots, long axis and head end, pivot, flat axis, proportions, texture sizes and mask packing, collision. bKeep: kept
	 * as the project's weapon convention. False, with a reason, when it is not a static mesh.
	 */
	bool MatchReference(const FString& AssetPath, FWeaponFinish& Out, TArray<FString>& OutReasons, FString& OutError, bool bKeep);
	/** A weapon's textures capped at the finishing's sizes (their sources kept): run again after the material build. */
	void CapWeaponTextureSizes(UObject& Mesh, const FWeaponFinish& Finish, TArray<FString>& OutNotes);

	/** What a check measures against. */
	struct FCheckTargets
	{
		FString Use;
		int32 FaceLimit = 0;
		int32 MinFaces = 0;
		bool bQuad = false;
		/** 0 skips the scale check. */
		float SizeMeters = 0.f;
		float TexelDensity = 0.f;
		bool bPbr = true;
		bool bTextures = true;
		bool bNanite = false;
		FString Collision;
		/** A picture the in-engine capture is compared with, and Tripo's own render of the model. */
		FString ReferenceImage;
		FString ProviderRender;
		/** Where the capture is written; the visual check is skipped when empty. */
		FString CaptureFolder;
		/** The model came from a GLB, whose normal maps Unreal must flip to its own convention. */
		bool bFromGltf = true;
		/** Over the budget, a static mesh is reduced in the editor for free (simplify) before anything paid: Hi3D's 100,000-face floor. */
		bool bLocalReduce = false;
		/** A weapon: its proportions and convention checked, and the free weapon_finish fix offered. */
		bool bWeapon = false;
		FWeaponFinish Weapon;
	};

	/** The targets a use implies, for a mesh that did not come from the creator. */
	FCheckTargets TargetsForUse(const FString& Use, const FString& Platform, float SizeMeters, int32 FaceLimit, const FString& Topology);
	FCheckTargets TargetsFromPlan(const FHyperAIModelPlan& Plan);
	/** Measures a static or skeletal mesh; false, with a reason, when it is not one. */
	bool Inspect(UObject& Mesh, const FCheckTargets& Targets, FHyperAIModelCheckReport& Out, FString& OutError);

	/** The free fixes, run on a static mesh (geometry) or any mesh (materials). Notes say what each did. */
	bool ApplyLocalFixes(UObject& Mesh, const FCheckTargets& Targets, const TArray<FString>& Fixes, TArray<FString>& OutNotes, FString& OutError);

	/** The material build: PBR maps found on the mesh's materials set up as textures, an ORM packed, a material instance a slot. */
	struct FMaterialResult
	{
		TArray<FString> Materials;
		TArray<FString> Textures;
		TArray<FString> Notes;
	};
	bool BuildMaterials(UObject& Mesh, bool bPerPartSlots, FMaterialResult& Out, FString& OutError);
	/** HyperAI's master material for generated models: tiling, a detail normal, ORM; made on first use. */
	FString MasterMaterialPath();

	// ---------------------------------------------------------------- jobs (HyperAIStudioModelCreatorJob.cpp)

	FHyperAIModelCreateReport Create(const FHyperAIModelCreateRequest& Request);
	FHyperAIModelStatusReport Status(const FString& OperationId);
	FHyperAIModelCheckFixReport CheckAndFix(const FHyperAIModelCheckFixRequest& Request);
	/** A check-and-fix the user already approved (the Tripo Studio offer): no second approval. */
	bool StartApprovedCheckAndFix(const FHyperAIModelCheckFixRequest& Request, FString& OutError);
	/** hyper_mesh_cancel for a creator or check-and-fix job; false when OperationId names neither. */
	bool Cancel(const FString& OperationId, FHyperAIMeshCancelReport& OutReport);
	/** Advances every job one step; the ticker calls it, and tests may. */
	void TickJobs();
#if WITH_DEV_AUTOMATION_TESTS
	/** As if the editor had closed: the job is dropped from this session (its provider step stops being followed); its journal stays. */
	void ForgetForTests(const FString& OperationId);
#endif

	// ---------------------------------------------------------------- Tripo Studio's DCC Bridge (HyperAIStudioModelCreatorWatch.cpp)

	/**
	 * Whether a newly imported asset is a model Tripo's DCC Bridge sent: a static or skeletal mesh that lands under one of
	 * Folders, or whose name or source file carries one of Markers.
	 */
	bool IsTripoImport(const FString& ClassName, const FString& ObjectPath, const FString& SourceFile, const TArray<FString>& Folders, const TArray<FString>& Markers);
	/** How sure the watch is that the bridge sent an import. */
	enum class ETripoImportEvidence : uint8
	{
		None,
		/** Only its content folder or a name looks like Tripo's: anyone's import can. */
		Weak,
		/** Its file came from a Tripo folder outside the project, where the bridge downloads. */
		Strong
	};
	ETripoImportEvidence TripoImportEvidence(const FString& ClassName, const FString& ObjectPath, const FString& SourceFile, const TArray<FString>& Folders, const TArray<FString>& Markers);
	/**
	 * Offers a check and its free fixes for an imported mesh: an approval in Activity only a click approves (never
	 * auto-approval) and, when asked, a notification. Free fixes only. Returns its operation id; empty when the asset is
	 * already held by another job, or the offer could not be queued.
	 */
	FString OfferCheckAndFix(UObject& Mesh, bool bNotify, ETripoImportEvidence Evidence = ETripoImportEvidence::Weak);
	/** A creator or check-and-fix job that has not finished holds this asset (an object path); a second job on it is refused. */
	bool IsAssetBusy(const FString& AssetPath);
	/** The offers made this session, newest last. */
	TArray<FString> OfferedAssets();
	void StartWatch();
	void StopWatch();
	/** Our own imports are not the bridge's: the watch looks away while one runs. */
	struct FOwnImport
	{
		FOwnImport();
		~FOwnImport();
	};

	// ---------------------------------------------------------------- the account (HyperAIStudioModelCreatorWatch.cpp)

	/** Tripo's API balance and usage, and Hi3D's balance: the last reads, new ones started when stale. Never returns a key or token. */
	FHyperAIModelAccountReport Account(const FHyperAIModelAccountRequest& Request);
	/** Starts a fresh read now; OnDone runs on the game thread with what came back. */
	void RefreshAccount(TFunction<void(const FHyperAIModelAccountReport&)> OnDone = nullptr);
}
