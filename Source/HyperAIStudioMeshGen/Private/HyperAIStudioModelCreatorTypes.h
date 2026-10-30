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

	/** H3.1 (high detail), P2 (Smart Poly) or a full Tripo model id; empty lets the classification choose. */
	UPROPERTY() FString Model;
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
};

/** The choices hyper_model_create made from a brief, with the reason for each, and what the generation costs. */
USTRUCT(BlueprintType)
struct FHyperAIModelPlan
{
	GENERATED_BODY()

	/** H3.1 or P2. */
	UPROPERTY() FString Family;
	/** The Tripo model id sent: v3.1-20260211 or P2-20260801. */
	UPROPERTY() FString Model;
	/** text, image or multiview. */
	UPROPERTY() FString Input;
	/** The v3 endpoint the generation goes to, such as /generation/text-to-model. */
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
	/** Tripo credits the generation costs, add-ons included, and the follow-up texture task a parts model needs. */
	UPROPERTY() int32 Credits = 0;
	/** The same in USD, from MeshTaskPrices. */
	UPROPERTY() float EstimatedCost = 0.f;
	/** Each priced part of the generation, as "what: credits". */
	UPROPERTY() TArray<FString> PriceLines;
	/** The generation's request body as it will be sent (image inputs show as file tokens to come). */
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
	/** classify, generate, check, local_fix, paid_fix, materials, rig, stop. */
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
	/**
	 * awaiting_user_approval, generating, checking, fixing, awaiting_fix_approval, materials, rigging, completed,
	 * stopped (checks still failing when the budget or the rounds ran out, or a fix was declined), failed, cancelled.
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
};
