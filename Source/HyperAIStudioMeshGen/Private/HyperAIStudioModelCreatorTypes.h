// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

#include "HyperAIStudioModelCreatorTypes.generated.h"

/**
 * What a model is for and how it should be made: what hyper_model_create classifies into a plan. Every field after
 * Use overrides one choice the classification would make; empty, zero or "auto" leaves the choice to it.
 */
USTRUCT(BlueprintType)
struct FHyperAIModelBrief
{
	GENERATED_BODY()

	/** What to make, up to 1024 characters: shape, material, style and scale words help Tripo most. */
	UPROPERTY() FString Prompt;
	/** What Tripo should avoid, up to 255 characters; a sensible default is added for game assets when empty. */
	UPROPERTY() FString NegativePrompt;
	/** A picture inside the project to make the model from (PNG or JPEG, up to 20 MB), and the reference the result is compared with. */
	UPROPERTY() FString ImagePath;
	/**
	 * Views of one object inside the project, in the order front, left, back, right ("" skips one); the front view is
	 * required and at least two are needed. Makes the model with multiview-to-model.
	 */
	UPROPERTY() TArray<FString> MultiviewImages;
	/** Tripo's task id of an earlier image-to-multiview or edit-multiview task, to make the model from its four views. */
	UPROPERTY() FString MultiviewTaskId;
	/**
	 * Edits to the views before the model is made, one to four, each "view: what to change" for a distinct view (front,
	 * left, back, right), such as "back: add a quiver of arrows". With ImagePath, Tripo first makes four views of the
	 * picture (image-to-multiview), then edits them (edit-multiview), then makes the model from them; with MultiviewTaskId
	 * it edits those views.
	 */
	UPROPERTY() TArray<FString> ViewEdits;
	/**
	 * What the model is for: hero_prop, prop, environment (organic: rocks, cliffs, trunks), kit (modular architecture),
	 * background, character, creature, vehicle, weapon, foliage, miniature. Read from the prompt when empty.
	 */
	UPROPERTY() FString Use;
	/** pc (the default), console, mobile or vr: mobile quarters the face budget and caps textures, vr halves it. */
	UPROPERTY() FString Platform;
	/** The real-world size in metres: the height of a character, creature or tree, otherwise the longest side. 0 uses the use's default. */
	UPROPERTY() float SizeMeters = 0.f;

	/**
	 * auto (the default), tripo or hi3d. Hi3D (Hitem3D's Sparc3D/Ultra3D models) makes the densest, most faithful shape
	 * from a picture or views; auto chooses it for a picture of a hero prop, an environment piece, a vehicle, a bust or
	 * portrait, or a brief that asks for photoreal, intricate, max detail or sculpted, and Tripo for words alone, a face
	 * budget under 100,000, quads, parts, a rig, view edits, or mobile and VR. Hi3D from words alone makes a reference
	 * picture first with HyperAI's image generation, priced in the same approval.
	 */
	UPROPERTY() FString Provider;
	/**
	 * Tripo: H3.1 (high detail), P2 (Smart Poly) or a full model id. Hi3D: hi3dv3.0 (the default), hitem3dv2.1,
	 * hitem3dv2.0, hitem3dv1.5, or a portrait model (scene-portraitv2.1, v2.0, v1.5). Empty lets the classification choose.
	 */
	UPROPERTY() FString Model;
	/**
	 * Hi3D only: the resolution its model takes (v3.0 2048quality or 2048master; v2.1 1536fast or 1536pro; v2.0 1536 or
	 * 1536pro; v1.5 512, 1024, 1536 or 1536pro). Empty chooses: 2048quality, or 2048master for an extreme or master brief.
	 */
	UPROPERTY() FString Resolution;
	/** The face budget: triangles, or quads when Topology is quads. 0 lets the classification choose. */
	UPROPERTY() int32 FaceLimit = 0;
	/** tris or quads; empty chooses. */
	UPROPERTY() FString Topology;
	/** none, fast, standard, detailed or extreme (8K); empty chooses. */
	UPROPERTY() FString TextureQuality;
	/** auto, on or off: metallic, roughness and normal maps beside the colour. */
	UPROPERTY() FString Pbr;
	/** auto, on or off: H3.1 only, editable parts (textured afterwards by a texture task, priced in the plan). */
	UPROPERTY() FString Parts;
	/** auto, on or off: H3.1 only, Tripo's hand-made-looking low poly (500-20,000 triangles or 500-10,000 quads). */
	UPROPERTY() FString SmartLowPoly;
	/** auto, standard or detailed: H3.1 only, finer geometry (needed above 1,500,000 triangles). */
	UPROPERTY() FString GeometryQuality;
	/** A seed for the geometry, to make the same shape again or (changed) a different one; 0 is random. */
	UPROPERTY() int32 Seed = 0;

	/**
	 * auto, on or off: make a reference picture first with Tripo's text-to-image and the model from it (image-to-model).
	 * auto: on for words alone except background and miniature pieces (always for P2); off when a picture or views are
	 * given. Hi3D from words alone always does.
	 */
	UPROPERTY() FString ReferenceImage;
	/**
	 * The picture's model, as Tripo names it or as people say it ("gpt-image-2.5-sunburn" is chat_image_2.5_sunburst):
	 * chat_image_2.5_flare (the default, quick), chat_image_2.5_sunburst (most faithful), chat_image_2, banana2,
	 * banana_pro, banana, seedream_v5, seedream_v4.
	 */
	UPROPERTY() FString ImageModel;
	/** low, medium, high (the default), and on the 2.5 models xhigh or max. Only chat_image models take it. */
	UPROPERTY() FString ImageQuality;
	/** chat_image: 1024x1024 (the default), 2048x2048, 1536x1024, auto or WxH; seedream: 2K, 3K or 4K. */
	UPROPERTY() FString ImageSize;
	/** auto, opaque or transparent (the default on the 2.5 models, sent as PNG). */
	UPROPERTY() FString ImageBackground;
	/** asset_extraction (props), t_pose (characters), character_completion, variants, figure, or none; chosen by the use when empty. */
	UPROPERTY() FString ImageTemplate;
	/** auto, on or off: approve and look at the picture before the 3D generation is asked for. auto: on for hero props, characters, vehicles and weapons. */
	UPROPERTY() FString ReviewReference;
};

/** The choices hyper_model_create made from a brief, with the reason for each, and what the generation costs. */
USTRUCT(BlueprintType)
struct FHyperAIModelPlan
{
	GENERATED_BODY()

	/** tripo or hi3d: who makes it. */
	UPROPERTY() FString Provider;
	/** H3.1 or P2 (Tripo), or Hi3D. */
	UPROPERTY() FString Family;
	/** The model id sent: Tripo's v3.1-20260211 or P2-20260801, or Hi3D's, such as hi3dv3.0. */
	UPROPERTY() FString Model;
	/** Hi3D's resolution, such as 2048quality; empty for Tripo. */
	UPROPERTY() FString Resolution;
	/**
	 * The faces asked of the provider. Tripo: the budget. Hi3D: 100,000 at least, so a smaller budget is generated at
	 * 100,000 and then reduced in the editor for free (bLocalReduce); a Nanite piece keeps Hi3D's dense mesh.
	 */
	UPROPERTY() int32 ProviderFaces = 0;
	UPROPERTY() bool bLocalReduce = false;
	/**
	 * A reference picture is made first and the model is made from it: Tripo's text-to-image (ReferenceSource tripo), or,
	 * for Hi3D without a Tripo key, HyperAI's own image generation (imagegen). Then the generation is image-to-model.
	 */
	UPROPERTY() bool bReferenceFirst = false;
	UPROPERTY() FString ReferenceSource;
	/** Tripo's image model (such as chat_image_2.5_flare), or HyperAI's image model id for imagegen. */
	UPROPERTY() FString ImageModel;
	UPROPERTY() FString ImageQuality;
	UPROPERTY() FString ImageSize;
	UPROPERTY() FString ImageBackground;
	UPROPERTY() FString ImageTemplate;
	/** The words sent for the picture, negatives after --no. */
	UPROPERTY() FString ImagePrompt;
	/** The picture is approved and paid for first; the job then holds (awaiting_reference_review) until the generation is approved. */
	UPROPERTY() bool bReviewReference = false;
	/** text, image or multiview. */
	UPROPERTY() FString Input;
	/** Where the generation goes: Tripo's v3 endpoint, such as /generation/text-to-model, or Hi3D's /open-api/v1/submit-task. */
	UPROPERTY() FString Endpoint;
	/** Views made from the picture first (image-to-multiview), and the edits made to them (edit-multiview), as "view: prompt". */
	UPROPERTY() bool bMakeViews = false;
	UPROPERTY() TArray<FString> ViewEdits;
	UPROPERTY() FString Use;
	UPROPERTY() FString Platform;
	UPROPERTY() int32 FaceLimit = 0;
	UPROPERTY() bool bQuad = false;
	/** The range the face budget must stay in, for this family and topology. */
	UPROPERTY() int32 MinFaces = 0;
	UPROPERTY() int32 MaxFaces = 0;
	/** none, fast, standard, detailed or extreme. */
	UPROPERTY() FString TextureQuality;
	UPROPERTY() bool bPbr = false;
	UPROPERTY() bool bParts = false;
	UPROPERTY() bool bSmartLowPoly = false;
	/** standard or detailed (H3.1 only). */
	UPROPERTY() FString GeometryQuality;
	/** The texture model pinned, v3.5-20260815: needed for fast textures and delighting. */
	UPROPERTY() FString TextureVersion;
	UPROPERTY() float SizeMeters = 0.f;
	/** Texture pixels a centimetre the result should reach, for the texel-density check. */
	UPROPERTY() float TexelDensity = 0.f;
	/** Whether the static mesh should render with Nanite. */
	UPROPERTY() bool bNanite = false;
	/** box, convex, complex or none. */
	UPROPERTY() FString Collision;
	/** The negative prompt sent. */
	UPROPERTY() FString NegativePrompt;
	/** One line a choice: what was chosen and why (from the use, the platform, the prompt's words, or an override). */
	UPROPERTY() TArray<FString> Reasons;
	/**
	 * The provider's credits the generation costs: Tripo's at $0.01 (add-ons and a parts model's texture task included),
	 * or Hi3D's at $0.02. A reference picture is priced in USD beside them, in PriceLines and EstimatedCost.
	 */
	UPROPERTY() int32 Credits = 0;
	/** The same in USD, from MeshTaskPrices. */
	UPROPERTY() float EstimatedCost = 0.f;
	/** Each priced part of the generation, as "what: credits". */
	UPROPERTY() TArray<FString> PriceLines;
	/** The generation's request as it will be sent (Tripo's pictures show as file tokens to come; Hi3D's form as its fields). */
	UPROPERTY() FString RequestJson;
};

/** One check of a mesh: what was measured, what it should be, and the cheapest fix that would make it pass. */
USTRUCT(BlueprintType)
struct FHyperAIModelCheck
{
	GENERATED_BODY()

	/** faces, non_manifold, open_edges, holes, flipped, degenerate, floating, scale, pivot, orientation, uv_present, uv_overlap, uv_range, texel_density, textures, pbr, normal_map, roughness_metal, nanite, collision, lightmap_uv, visual. */
	UPROPERTY() FString Id;
	/** pass, warn or fail; skip when it could not be measured. */
	UPROPERTY() FString Result;
	UPROPERTY() FString Value;
	UPROPERTY() FString Expected;
	UPROPERTY() FString Detail;
	/** The free fix in the editor that addresses it, if any: normals, orient, fill_holes, remove_bits, pivot, scale, auto_uv, collision, lightmap_uv, nanite, materials. */
	UPROPERTY() FString LocalFix;
	/** The paid Tripo fix that addresses it, if any: retopo, smart_uv, texture, complete, quick_cap, convert, regenerate. */
	UPROPERTY() FString PaidFix;
};

/** Everything measured on a mesh. */
USTRUCT(BlueprintType)
struct FHyperAIModelCheckReport
{
	GENERATED_BODY()

	UPROPERTY() FString AssetPath;
	/** True when nothing failed (warnings allowed). */
	UPROPERTY() bool bPassed = false;
	UPROPERTY() int32 Failed = 0;
	UPROPERTY() int32 Warnings = 0;
	UPROPERTY() int32 Triangles = 0;
	UPROPERTY() int32 Quads = 0;
	UPROPERTY() int32 Vertices = 0;
	UPROPERTY() int32 Components = 0;
	UPROPERTY() int32 MaterialSlots = 0;
	/** Size in centimetres, X Y Z. */
	UPROPERTY() FVector Size = FVector::ZeroVector;
	UPROPERTY() TArray<FHyperAIModelCheck> Checks;
	/** The images the visual check used: Tripo's render, the in-engine capture, the reference. */
	UPROPERTY() TArray<FString> Images;
	/** The visual score against the reference (0-100), or -1 when there was nothing to compare. */
	UPROPERTY() float VisualScore = -1.f;
	UPROPERTY() FString Summary;
};

/** One step a creator job took or offered, with what it cost. */
USTRUCT(BlueprintType)
struct FHyperAIModelStep
{
	GENERATED_BODY()

	UPROPERTY() int32 Round = 0;
	/** classify, reference, generate, check, local_fix, paid_fix, materials, rig, stop. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString What;
	/** done, failed, skipped, declined, awaiting_user_approval, running. */
	UPROPERTY() FString Status;
	/** The Tripo endpoint and task it ran as, when paid. */
	UPROPERTY() FString Endpoint;
	UPROPERTY() FString TaskId;
	/** The approval and job it ran under, when paid. */
	UPROPERTY() FString OperationId;
	/** The price shown before it ran, in credits and USD; and what Tripo reported it consumed, when it said. */
	UPROPERTY() int32 Credits = 0;
	UPROPERTY() float PriceUSD = 0.f;
	UPROPERTY() float CreditsConsumed = -1.f;
	UPROPERTY() FString Detail;
};

USTRUCT(BlueprintType)
struct FHyperAIModelCreateRequest
{
	GENERATED_BODY()

	UPROPERTY() FHyperAIModelBrief Brief;
	/** The static mesh's name, such as SM_Crate; made from the prompt when empty. */
	UPROPERTY() FString Name;
	/** Where it is imported, under /Game; Mesh Import Folder in the settings when empty. */
	UPROPERTY() FString Folder;
	/** What the paid fixes after the generation may spend in all, in USD; the setting's per-job budget when 0. */
	UPROPERTY() float FixBudgetUSD = 0.f;
	/** Fix rounds at most (each round: check, free fixes, at most one paid fix); the setting's cap when 0. */
	UPROPERTY() int32 MaxRounds = 0;
	/** Free fixes in the editor only: never offer a paid Tripo fix. */
	UPROPERTY() bool bLocalFixesOnly = false;
	/** Rig it with Tripo (spec unreal, the UE Mannequin's bone names) once it passes; priced and approved on its own. */
	UPROPERTY() bool bRig = false;
	/** A skeletal mesh to retarget the rig onto, such as the project's Mannequin, through hyper_retarget_apply_plan. */
	UPROPERTY() FString RetargetTo;
	/** Picks up a job journaled earlier (this session or one before the editor restarted) where it stopped. */
	UPROPERTY() FString ResumeOperationId;
	/** Names the job; made when empty. */
	UPROPERTY() FString OperationId;
	/** Only classify and price: nothing is spent. */
	UPROPERTY() bool bDryRun = true;
};

USTRUCT(BlueprintType)
struct FHyperAIModelCreateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked (a dry run), awaiting_user_approval, queued, resumed, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FHyperAIModelPlan Plan;
	UPROPERTY() FString Destination;
	UPROPERTY() float FixBudgetUSD = 0.f;
	UPROPERTY() int32 MaxRounds = 0;
};

/** A creator job, or a check-and-fix job, as it stands. */
USTRUCT(BlueprintType)
struct FHyperAIModelJob
{
	GENERATED_BODY()

	UPROPERTY() FString OperationId;
	/** create or check_and_fix. */
	UPROPERTY() FString Kind;
	/** Who made the model: tripo or hi3d. A check-and-fix job's paid fixes are Tripo's. */
	UPROPERTY() FString Provider;
	/**
	 * awaiting_user_approval, making_reference, awaiting_reference_review (the picture is in; the 3D waits for its own
	 * approval), generating, checking, fixing, awaiting_fix_approval, materials, rigging, completed, stopped (checks still
	 * failing when the budget or the rounds ran out, or a fix or the reviewed generation was declined), failed, cancelled.
	 */
	UPROPERTY() FString State;
	/** The operation to follow now with hyper_operation_wait: a provider step's job, or a fix waiting for approval. */
	UPROPERTY() FString CurrentOperationId;
	UPROPERTY() FHyperAIModelPlan Plan;
	UPROPERTY() FString AssetPath;
	UPROPERTY() TArray<FString> Materials;
	UPROPERTY() TArray<FString> Textures;
	UPROPERTY() FString SkeletalMesh;
	UPROPERTY() TArray<FString> PreviewImages;
	UPROPERTY() FHyperAIModelCheckReport FirstCheck;
	UPROPERTY() FHyperAIModelCheckReport LastCheck;
	UPROPERTY() TArray<FHyperAIModelStep> Steps;
	UPROPERTY() int32 Round = 0;
	UPROPERTY() int32 MaxRounds = 0;
	UPROPERTY() float FixBudgetUSD = 0.f;
	/** Paid in USD, the generation included; and the Tripo credits it consumed where Tripo said. */
	UPROPERTY() float SpentUSD = 0.f;
	UPROPERTY() float CreditsConsumed = 0.f;
	/** checks_passed, budget_used, rounds_used, fix_declined, no_paid_fix, local_only, or empty while it runs. */
	UPROPERTY() FString StopReason;
	/** The journal, relative to the project: every step and decision, to audit or resume. */
	UPROPERTY() FString Journal;
	UPROPERTY() FString Diagnostic;
};

USTRUCT(BlueprintType)
struct FHyperAIModelStatusRequest
{
	GENERATED_BODY()

	/** One job; empty lists this session's creator and check-and-fix jobs. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIModelStatusReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAIModelJob> Jobs;
};

USTRUCT(BlueprintType)
struct FHyperAIModelCheckFixRequest
{
	GENERATED_BODY()

	/** A static or skeletal mesh in the project, such as one Tripo's DCC Bridge sent from Tripo Studio. */
	UPROPERTY() FString AssetPath;
	/** What it is for (as in a brief); read from its name when empty. Sets the budgets the checks use. */
	UPROPERTY() FString Use;
	UPROPERTY() FString Platform;
	/** Real-world size in metres for the scale check; 0 uses the use's default, or skips the scale check when the use is unknown. */
	UPROPERTY() float SizeMeters = 0.f;
	/** The face budget and topology the face check holds it to; 0 and empty use the use's defaults. */
	UPROPERTY() int32 FaceLimit = 0;
	UPROPERTY() FString Topology;
	/** A picture inside the project the in-engine capture is compared with. */
	UPROPERTY() FString ReferenceImagePath;
	/** Only check, change nothing. When false, the free fixes are asked for (one approval) and run. */
	UPROPERTY() bool bCheckOnly = true;
	/** Also offer paid Tripo fixes (each priced and approved on its own) while checks still fail; needs the Tripo key. */
	UPROPERTY() bool bAllowPaidFixes = false;
	UPROPERTY() float FixBudgetUSD = 0.f;
	UPROPERTY() int32 MaxRounds = 0;
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIModelCheckFixReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, awaiting_user_approval, queued, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FHyperAIModelCheckReport Check;
	/** The free fixes the run would make, and the paid ones it could offer, cheapest first. */
	UPROPERTY() TArray<FString> LocalFixes;
	UPROPERTY() TArray<FString> PaidFixes;
};

USTRUCT(BlueprintType)
struct FHyperAIModelAccountRequest
{
	GENERATED_BODY()

	/** Also read the per-task usage list. */
	UPROPERTY() bool bUsage = true;
	/** At most this many usage rows; 20 when 0. */
	UPROPERTY() int32 Limit = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIModelUsage
{
	GENERATED_BODY()

	UPROPERTY() FString TaskId;
	UPROPERTY() FString Type;
	UPROPERTY() float Credits = 0.f;
	UPROPERTY() FString CreatedAt;
};

USTRUCT(BlueprintType)
struct FHyperAIModelAccountReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** ok, pending (the answer is on its way: call again), missing_key, or the error. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	/** API credits only: Tripo Studio's subscription credits are a separate balance the API cannot read or spend. */
	UPROPERTY() float Balance = 0.f;
	UPROPERTY() float Frozen = 0.f;
	UPROPERTY() TArray<FHyperAIModelUsage> Usage;
	/** When this was read, UTC. */
	UPROPERTY() FString ReadAt;
	/** Hi3D's balance, read with its key (client_id:client_secret, traded for a token): ok, pending, missing_key, or the error. */
	UPROPERTY() FString Hi3DStatus;
	/** Hi3D credits, at $0.02 each. */
	UPROPERTY() float Hi3DBalance = 0.f;
	UPROPERTY() FString Hi3DDiagnostic;
};
