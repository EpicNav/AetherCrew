// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HyperAIStudioModelCreatorTypes.h"

class UObject;
class UStaticMesh;
struct FHyperAIMeshCancelReport;

/**
 * The Tripo model creator: a brief classified into a plan (H3.1 or P2, the face budget, quads or triangles, the texture
 * tier and the add-ons, each with its reason), Tripo's v3 requests made from that plan, a generated model checked in the
 * editor, fixed the cheapest correct way first (free in the editor, then paid Tripo steps, each priced and approved), its
 * PBR maps made into a material instance, and every step journaled. Built on the 3D jobs of HyperAIStudioMeshGenJob.h.
 */
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
	constexpr int32 SmartUvMaxTris = 80000;
	constexpr int32 MaxPromptCharacters = 1024;
	constexpr int32 MaxNegativeCharacters = 255;

	// ---------------------------------------------------------------- classification

	/** The brief made into a plan; false, with a reason, when an override cannot be honoured (P2 with parts, say). */
	bool Classify(const FHyperAIModelBrief& Brief, FHyperAIModelPlan& Out, FString& OutError);
	/** The use a brief's words point to, or empty: "a sci-fi soldier" is a character, "a stone wall section" a kit piece. */
	FString UseFromWords(const FString& Words);

	// ---------------------------------------------------------------- Tripo v3 requests

	/** The generation's body; Inputs are the file tokens of the picture or the views (front, left, back, right, "" skipping one). */
	TSharedRef<FJsonObject> GenerationBody(const FHyperAIModelPlan& Plan, const FHyperAIModelBrief& Brief, const TArray<FString>& Inputs);
	TSharedRef<FJsonObject> RetopoBody(const FString& Input, int32 FaceLimit, bool bQuad, bool bSmartUv);
	TSharedRef<FJsonObject> SmartUvBody(const FString& Input);
	TSharedRef<FJsonObject> TextureBody(const FString& Input, const FString& Quality, bool bPbr, const TArray<FString>& PartNames, const FString& Prompt);
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
	TSharedRef<FJsonObject> RigBody(const FString& Input, const FString& Spec);

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

	/** One paid Tripo fix: the endpoint, its body, its price, and the checks it is meant to clear. */
	struct FPaidFix
	{
		/** retopo, smart_uv, texture, convert, quick_cap, complete, regenerate. */
		FString Id;
		FString What;
		/** One or two Tripo tasks in order (segment then complete). */
		TArray<FString> Endpoints;
		TArray<TSharedPtr<FJsonObject>> Bodies;
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
	};

	/** The paid fixes that would clear something still failing, cheapest first; those over the remaining budget are left out. */
	TArray<FPaidFix> PaidLadder(const FHyperAIModelCheckReport& Report, const FFixContext& Context, TArray<FString>* OutOverBudget = nullptr);
	/** The free fixes a report calls for, in the order they run. */
	TArray<FString> LocalFixesFor(const FHyperAIModelCheckReport& Report);

	// ---------------------------------------------------------------- checks and free fixes (HyperAIStudioModelInspect.cpp)

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
	/** Offers a check and its free fixes for an imported mesh: an approval in Activity and, when asked, a notification. Returns its operation id. */
	FString OfferCheckAndFix(UObject& Mesh, bool bNotify);
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

	/** Tripo's API balance and usage: the last read, and a new read started when it is stale. Never returns the key. */
	FHyperAIModelAccountReport Account(const FHyperAIModelAccountRequest& Request);
	/** Starts a fresh read now; OnDone runs on the game thread with what came back. */
	void RefreshAccount(TFunction<void(const FHyperAIModelAccountReport&)> OnDone = nullptr);
}
