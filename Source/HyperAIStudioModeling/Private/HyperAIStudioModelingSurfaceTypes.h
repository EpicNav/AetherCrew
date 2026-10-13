// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

#include "HyperAIStudioModelingSurfaceTypes.generated.h"

/** One UV channel of one LOD, measured: what a texture laid out in it would get. */
USTRUCT(BlueprintType)
struct FHyperAIModelUVMetrics
{
	GENERATED_BODY()

	UPROPERTY() int32 Channel = 0;
	/** Triangles measured; ones with no 3D area are left out. */
	UPROPERTY() int32 Triangles = 0;
	/** Pieces of the layout: triangles joined by edges whose UVs agree on both sides. */
	UPROPERTY() int32 Islands = 0;
	/** 3D length of the edges where the UVs split, cm. */
	UPROPERTY() float SeamLengthCm = 0.f;
	/** Share of the laid-out UV area covered more than once, percent. */
	UPROPERTY() float OverlapPercent = 0.f;
	/** Islands that overlap, largest overlap first: "3+7" (island numbers as the layout PNG draws them), "5 itself" for one folded over itself. */
	UPROPERTY() TArray<FString> OverlappingIslands;
	/** Triangles wound against the rest of their island: folded over in UV space. */
	UPROPERTY() int32 FlippedTriangles = 0;
	/** Triangles with 3D area but no UV area, or no UVs at all. */
	UPROPERTY() int32 DegenerateTriangles = 0;
	/** Triangles with a corner outside 0-1, percent. */
	UPROPERTY() float OutsideUnitPercent = 0.f;
	/** The UVs' bounds, "u,v". */
	UPROPERTY() FString UVMin;
	UPROPERTY() FString UVMax;
	/** Share of the 0-1 square the layout covers, percent: how tightly it is packed. */
	UPROPERTY() float CoveragePercent = 0.f;
	/** Texels per meter at TextureSize: the 1st, 50th and 99th area-weighted percentiles, so a sliver does not set them. */
	UPROPERTY() float TexelDensityMin = 0.f;
	UPROPERTY() float TexelDensityMedian = 0.f;
	UPROPERTY() float TexelDensityMax = 0.f;
	/** TexelDensityMax over TexelDensityMin: 1 is even; past 4, some parts look blurry beside others. */
	UPROPERTY() float TexelDensitySpread = 0.f;
};

/** The asset's lightmap channel, counted in texels at a lightmap resolution. */
USTRUCT(BlueprintType)
struct FHyperAIModelLightmapCheck
{
	GENERATED_BODY()

	/** The asset's lightmap coordinate index. */
	UPROPERTY() int32 Channel = 0;
	/** Texels per side the check counted at. */
	UPROPERTY() int32 Resolution = 0;
	/** The channel is made when the mesh builds (Generate Lightmap UVs), so the built mesh was read. */
	UPROPERTY() bool bGeneratedAtBuild = false;
	UPROPERTY() bool bPresent = false;
	/** Texels two or more islands share, percent of the texels the layout covers: light bleeds there. */
	UPROPERTY() float SharedTexelsPercent = 0.f;
	/** Island pairs closer than the padding asked for (sharing texels counts), "3+7". */
	UPROPERTY() TArray<FString> TooClose;
	UPROPERTY() int32 TooClosePairs = 0;
	/** Triangles with a corner outside 0-1, percent. */
	UPROPERTY() float OutsideUnitPercent = 0.f;
	/** Present, no shared texels, no pairs too close, nothing outside 0-1. */
	UPROPERTY() bool bSuitable = false;
	UPROPERTY() FString Summary;
};

USTRUCT(BlueprintType)
struct FHyperAIModelUVCheckRequest
{
	GENERATED_BODY()

	/** The static mesh (/Game/Models/SM_Rock). Any static mesh can be read, Engine content too. */
	UPROPERTY() FString Asset;
	/** Source LOD; -1 reads the hi-res (Nanite) source. */
	UPROPERTY() int32 LOD = 0;
	/** UV channel to measure. */
	UPROPERTY() int32 Channel = 0;
	/** Texture size texel density is counted at, 16 to 8192. */
	UPROPERTY() int32 TextureSize = 1024;
	/** Lightmap resolution the lightmap check counts texels at; 0 takes the asset's own. */
	UPROPERTY() int32 LightmapResolution = 0;
	/** Empty texels the lightmap check wants between islands; 0 only refuses shared texels. */
	UPROPERTY() int32 LightmapPadding = 0;
	/** Draw the layout as a PNG, problems coloured (LayoutPath). */
	UPROPERTY() bool bLayoutImage = true;
};

USTRUCT(BlueprintType)
struct FHyperAIModelUVCheckReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Asset;
	UPROPERTY() int32 LOD = 0;
	/** good, usable (warnings only), bad (errors: overlap, folds, no UV area) or missing (no such channel). */
	UPROPERTY() FString Verdict;
	/** What decided the verdict, errors first: "error overlap: ...", "warning outside_unit: ...". */
	UPROPERTY() TArray<FString> Problems;
	/** UV channels the LOD has. */
	UPROPERTY() int32 Channels = 0;
	UPROPERTY() FHyperAIModelUVMetrics Metrics;
	UPROPERTY() FHyperAIModelLightmapCheck Lightmap;
	/**
	 * The layout as a PNG (Saved/HyperAIStudio/Previews): the 0-1 square outlined, islands in muted colours with their
	 * edges light, overlaps red, flipped triangles magenta, degenerate ones yellow.
	 */
	UPROPERTY() FString LayoutPath;
};

USTRUCT(BlueprintType)
struct FHyperAIModelUVApplyRequest
{
	GENERATED_BODY()

	/** The static mesh to change, in the project. Engine content, and cooked or read-only assets, are refused. */
	UPROPERTY() FString Asset;
	/**
	 * xatlas (the default), patch (PatchBuilder), box, planar, cylinder, repack (the channel's own islands packed
	 * again), or lightmap (Unreal's lightmap packer, from SourceChannel into Channel when the mesh builds).
	 */
	UPROPERTY() FString Method;
	/** Source LODs to change; empty changes LOD 0, -1 is the hi-res (Nanite) source. */
	UPROPERTY() TArray<int32> LODs;
	/** The UV channel written: -1 is 0, or 1 for lightmap. One past the last channel adds a channel. */
	UPROPERTY() int32 Channel = -1;
	/** lightmap: the channel its charts come from. */
	UPROPERTY() int32 SourceChannel = 0;
	/** Texture size the packing leaves padding for; 0 is 1024, or 64 for lightmap (the lightmap resolution). */
	UPROPERTY() int32 Resolution = 0;
	/** Texels between islands at Resolution; -1 is 4 (lightmap: Unreal's own packer, which leaves one). */
	UPROPERTY() int32 Padding = -1;
	/** box, planar, cylinder: pack the projected islands into 0-1 for baking (true), or keep TileSize cm per tile for tiling materials. */
	UPROPERTY() bool bRepack = true;
	/** box, planar, cylinder without bRepack: cm per texture tile. */
	UPROPERTY() float TileSize = 100.f;
	/** Build the new layout in memory and report the UV check before and after; nothing is written. */
	UPROPERTY() bool bDryRun = false;
	/** Names the edit, for hyper_operation_wait; made when empty. */
	UPROPERTY() FString OperationId;
};

/** One LOD's channel before and after. */
USTRUCT(BlueprintType)
struct FHyperAIModelUVLodResult
{
	GENERATED_BODY()

	/** -1 for the hi-res source. */
	UPROPERTY() int32 LOD = 0;
	UPROPERTY() int32 Triangles = 0;
	UPROPERTY() FString VerdictBefore;
	UPROPERTY() FString VerdictAfter;
	UPROPERTY() FHyperAIModelUVMetrics Before;
	UPROPERTY() FHyperAIModelUVMetrics After;
	/** Read back from the written asset, the layout matches the one built (islands, overlap, flips). False on a dry run. */
	UPROPERTY() bool bVerified = false;
};

USTRUCT(BlueprintType)
struct FHyperAIModelUVApplyReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, queued, approval_not_queued, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Asset;
	UPROPERTY() FString Method;
	UPROPERTY() int32 Channel = 0;
	UPROPERTY() TArray<FHyperAIModelUVLodResult> LODs;
	/** On every LOD the check is no worse, and overlap, flipped and degenerate triangles and UVs outside 0-1 are no higher. */
	UPROPERTY() bool bImproved = false;
	/** The first LOD's new layout, problems coloured; drawn on a dry run too. */
	UPROPERTY() FString LayoutPath;
	/** A PNG of the asset as written; empty on a dry run, or when the editor cannot draw (Notes say). */
	UPROPERTY() FString PreviewPath;
	/** lightmap: the check of the channel as the mesh builds it. */
	UPROPERTY() FHyperAIModelLightmapCheck Lightmap;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIModelBakeRequest
{
	GENERATED_BODY()

	/** The static mesh the maps are laid out for, in its channel TargetChannel. Its slots change only with AssignSlots. */
	UPROPERTY() FString Target;
	UPROPERTY() int32 TargetLOD = 0;
	UPROPERTY() int32 TargetChannel = 0;
	/** The detailed mesh the maps come from (a high-poly, or the original model); empty bakes the target from itself. Its hi-res source is read when it has one. */
	UPROPERTY() FString Source;
	/**
	 * normal (tangent space), object_normal, bent_normal, ao, curvature, position, height, vertex_color, texture,
	 * multi_texture, or material: the source drawn with render captures (needs an editor that renders) into base colour,
	 * normal, and roughness and metal in the ORM pack.
	 */
	UPROPERTY() TArray<FString> Maps;
	/** Pixels per side: a power of two from 16 to 4096. */
	UPROPERTY() int32 Resolution = 1024;
	/** Samples per pixel: 1, 4, 16 or 64. */
	UPROPERTY() int32 Samples = 1;
	/** Pixels the maps are dilated past the islands' edges, so mips and filtering do not pull in the background. */
	UPROPERTY() int32 Padding = 4;
	/** ao, bent_normal: rays per sample, 1 to 256. */
	UPROPERTY() int32 OcclusionRays = 16;
	/** ao, bent_normal: how far the rays look, cm; 0 has no limit. */
	UPROPERTY() float OcclusionDistance = 0.f;
	/** How far from the target the source is searched, cm; 0 takes 3% of the target's size. height maps this distance either side to black and white. */
	UPROPERTY() float ProjectionDistance = 0.f;
	/** texture: the texture to carry across; empty takes the base colour texture of the source's slot 0 material. multi_texture takes each slot's. */
	UPROPERTY() FString SourceTexture;
	/** texture, multi_texture: the source's UV channel those textures are laid out in. */
	UPROPERTY() int32 SourceChannel = 0;
	/** Where the textures go; empty is the target's folder. Named T_<target>_<suffix>: N, ON, BN, AO, CV, P, H, VC, BC (texture or multi_texture), ORM. */
	UPROPERTY() FString Folder;
	/** Overwrite textures already at those paths, in the undo step; otherwise those paths are refused. */
	UPROPERTY() bool bReplace = false;
	/** With ao: also pack T_<target>_ORM, red occlusion, green Roughness, blue Metallic. */
	UPROPERTY() bool bPackORM = true;
	UPROPERTY() float Roughness = 0.8f;
	UPROPERTY() float Metallic = 0.f;
	/**
	 * new makes MI_<target>_Baked in Folder, an instance of HyperAI's baked-surface material (BaseColor, Normal and ORM
	 * texture parameters), using the maps; a path updates that material instance; empty makes none.
	 */
	UPROPERTY() FString Material;
	/** Put that material on the target's slots, in the undo step: all, or slot numbers "0,2"; empty leaves them. */
	UPROPERTY() FString AssignSlots;
	/** Check everything and name the textures; nothing is baked or written. With bPreviewDryRun, also bake at 128 px and draw the maps. */
	UPROPERTY() bool bDryRun = false;
	UPROPERTY() bool bPreviewDryRun = false;
	/** Names the edit, for hyper_operation_wait; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIModelBakedTexture
{
	GENERATED_BODY()

	UPROPERTY() FString Map;
	UPROPERTY() FString Path;
	UPROPERTY() int32 Size = 0;
	/** The texture's source format: BGRA8. */
	UPROPERTY() FString Format;
	/** TC_Normalmap, TC_Masks, TC_Grayscale, TC_VectorDisplacementmap or TC_Default. */
	UPROPERTY() FString Compression;
	UPROPERTY() bool bSRGB = false;
	/** It overwrote a texture (bReplace), which undo takes back. */
	UPROPERTY() bool bReplaced = false;
	/** Brightness over the whole image, 0-1 (written or previewed maps only): darkest, mean and brightest. */
	UPROPERTY() float Min = 0.f;
	UPROPERTY() float Mean = 0.f;
	UPROPERTY() float Max = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAIModelBakeReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, queued, approval_not_queued, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Target;
	/** The mesh the maps came from, and its LOD read ("hi-res source" or "LOD 0"). */
	UPROPERTY() FString Source;
	UPROPERTY() TArray<FHyperAIModelBakedTexture> Textures;
	/** The material instance made or updated, and the slots it was put on. */
	UPROPERTY() FString MaterialInstance;
	UPROPERTY() TArray<int32> AssignedSlots;
	/** The target channel's UV check: a bake wants islands that do not overlap and stay inside 0-1. */
	UPROPERTY() FString TargetUVs;
	/** The maps side by side, in Textures' order: written maps, or a dry run's 128 px preview. */
	UPROPERTY() FString PreviewPath;
	UPROPERTY() int32 BakeMs = 0;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIModelPrepareRequest
{
	GENERATED_BODY()

	/** The generated or imported static mesh, such as one hyper_mesh_generate imported. */
	UPROPERTY() FString Asset;
	/** Unwrap channel 0 again: auto (the default, when the UV check says bad or missing), always, or never. Its base colour and normal textures are carried to the new layout. */
	UPROPERTY() FString UnwrapUVs;
	/** xatlas (the default) or patch. */
	UPROPERTY() FString UVMethod;
	/** Write a lighter copy here (/Game/Models/SM_Rock_Low) and bake onto it from the original; empty writes none. */
	UPROPERTY() FString LowPoly;
	/** LowPoly's triangles; 0 is a tenth of the original's, at least 500. */
	UPROPERTY() int32 TargetTriangles = 0;
	/** With LowPoly: maps baked from the original onto it; empty is normal, ao and texture (its base colour, when it has one). */
	UPROPERTY() TArray<FString> Maps;
	/** Texture size for the unwrap's padding and the bakes. */
	UPROPERTY() int32 Resolution = 1024;
	UPROPERTY() int32 Samples = 1;
	UPROPERTY() int32 Padding = 4;
	/** Also have the mesh (and LowPoly) build lightmap UVs into channel 1. */
	UPROPERTY() bool bLightmapUVs = false;
	/** Where textures and the material instance go; empty is the asset's folder. */
	UPROPERTY() FString Folder;
	/** Check, unwrap and decimate in memory and report the steps; nothing is baked or written. */
	UPROPERTY() bool bDryRun = false;
	/** Names the edit, for hyper_operation_wait; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIModelPrepareReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, queued, approval_not_queued, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Asset;
	/** The steps, in order, as run (or as a dry run would run them). */
	UPROPERTY() TArray<FString> Steps;
	UPROPERTY() FString VerdictBefore;
	UPROPERTY() FString VerdictAfter;
	UPROPERTY() bool bUnwrapped = false;
	UPROPERTY() FString LowPoly;
	UPROPERTY() int32 LowPolyTriangles = 0;
	UPROPERTY() TArray<FHyperAIModelBakedTexture> Textures;
	UPROPERTY() FString MaterialInstance;
	/** Channel 0's layout after the steps, problems coloured. */
	UPROPERTY() FString LayoutPath;
	/** The baked maps side by side. */
	UPROPERTY() FString MapsPreviewPath;
	/** A PNG of the asset (or LowPoly) as written. */
	UPROPERTY() FString PreviewPath;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};
