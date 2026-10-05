// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioLandscapeToolset.generated.h"

/** Fractal noise for landscape.create and the noise sculpt: rolling hills, or ridged mountains. */
USTRUCT(BlueprintType)
struct FHyperAILandscapeNoise
{
	GENERATED_BODY()

	UPROPERTY() int32 Seed = 1;
	/** Peak-to-trough height of the largest features, meters. */
	UPROPERTY() float Amplitude = 0.f;
	/** Size of the largest features, meters (a hill every 500 m, a ridge every 2000 m). */
	UPROPERTY() float Wavelength = 500.f;
	/** Layers of smaller detail, 1 to 8. */
	UPROPERTY() int32 Octaves = 5;
	/** Sharp crests instead of round hills: mountains, dunes. */
	UPROPERTY() bool bRidged = false;
};

/**
 * One paint rule: where a paint layer shows, by height, slope and distance from roads, in patches if asked. Rules later
 * in the list win where they overlap.
 */
USTRUCT(BlueprintType)
struct FHyperAILandscapePaintRule
{
	GENERATED_BODY()

	/** The paint layer, as the landscape material names it: Grass, Rock, Snow. */
	UPROPERTY() FString Layer;
	/**
	 * World height band in meters: both 0 means any height, and a HeightMax at or below HeightMin means no upper limit
	 * (HeightMin 300 alone: everything above 300 m). HeightFade softens the band's edges.
	 */
	UPROPERTY() float HeightMin = 0.f;
	UPROPERTY() float HeightMax = 0.f;
	UPROPERTY() float HeightFade = 10.f;
	/** Slope band in degrees (0 flat, 90 a wall), read the same way (SlopeMin 35 alone: steeper than 35). SlopeFade softens its edges. */
	UPROPERTY() float SlopeMin = 0.f;
	UPROPERTY() float SlopeMax = 0.f;
	UPROPERTY() float SlopeFade = 5.f;
	/**
	 * Distance band in meters from the landscape's roads and other splines, read the same way: DistanceMax 15 alone
	 * keeps the layer within 15 m of them (a verge, a riverbank), DistanceMin 30 alone at least 30 m away.
	 * DistanceFade softens its edges.
	 */
	UPROPERTY() float DistanceMin = 0.f;
	UPROPERTY() float DistanceMax = 0.f;
	UPROPERTY() float DistanceFade = 5.f;
	/**
	 * Patches: the layer takes Coverage (0.05 to 1) of the painted ground, in patches about Patch meters across, so
	 * biomes form a mosaic rather than bands. Patch 0 paints wherever the bands hold. Seed lays the patches out
	 * another way; 0 takes one from the layer's name.
	 */
	UPROPERTY() float Patch = 0.f;
	UPROPERTY() float Coverage = 1.f;
	UPROPERTY() int32 Seed = 0;
	/** 0 to 1: noise that breaks the edges up so bands do not read as lines. */
	UPROPERTY() float Breakup = 0.3f;
	/** 0 to 1: how fully the layer covers where the rule holds. */
	UPROPERTY() float Strength = 1.f;
};

USTRUCT(BlueprintType)
struct FHyperAILandscapeOp
{
	GENERATED_BODY()

	/**
	 * landscape.create, landscape.sculpt, landscape.create_material, landscape.set_material, landscape.add_paint_layer,
	 * landscape.paint, landscape.add_edit_layer, landscape.road.
	 */
	UPROPERTY() FString Kind;
	/**
	 * The landscape, by its label in the level; for landscape.create, the new one's label. landscape.create_material
	 * gives the new material to it when named.
	 */
	UPROPERTY() FString Landscape;
	/**
	 * landscape.create: centered on Location (cm), Size meters a side at Resolution cm between vertices (100 is Unreal's
	 * default; 50 for detail, 200 for vast terrain), with HeightRange meters of vertical room (512 by default).
	 */
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() float Size = 1008.f;
	UPROPERTY() float Resolution = 100.f;
	UPROPERTY() float HeightRange = 512.f;
	/**
	 * landscape.create: its heights from a heightmap texture (a 16-bit grayscale import, black the lowest and white the
	 * highest point of HeightRange), or from Noise; flat when neither.
	 */
	UPROPERTY() FString Heightmap;
	UPROPERTY() FHyperAILandscapeNoise Noise;
	/**
	 * landscape.create, landscape.set_material: the landscape material, one made earlier in the plan too.
	 * landscape.create_material: the new material's path under /Game (/Game/Landscape/M_Valley).
	 */
	UPROPERTY() FString Material;
	/**
	 * landscape.create, landscape.add_paint_layer: paint layers by the names the material's layer blend uses (the first
	 * covers everything at first); their layer info assets go in LayerInfoFolder (/Game/Landscape/LayerInfos by default).
	 * landscape.create_material: the layers its blend names, the Landscape's own when empty.
	 */
	UPROPERTY() TArray<FString> PaintLayers;
	UPROPERTY() FString LayerInfoFolder;
	/**
	 * landscape.create_material: each paint layer's flat color, in PaintLayers' order (R, G, B 0 to 1), so painted
	 * ground reads without textures; picked from the layer's name (grass green, rock grey, snow white) when missing.
	 */
	UPROPERTY() TArray<FLinearColor> LayerColors;
	/**
	 * landscape.sculpt: raise, lower, flatten, smooth, noise, terrace or ramp, in a circle at Center (cm) of Radius meters
	 * whose edge fades over Falloff meters. Amount: meters to raise or lower, the noise's size, or a terrace's step.
	 * Height: meters to flatten to. Smooth takes Strength 0 to 1. A ramp runs from Center to To, Width meters wide,
	 * graded evenly between the ground at each end.
	 */
	UPROPERTY() FString Mode;
	UPROPERTY() FVector Center = FVector::ZeroVector;
	UPROPERTY() FVector To = FVector::ZeroVector;
	UPROPERTY() float Radius = 50.f;
	UPROPERTY() float Falloff = 20.f;
	UPROPERTY() float Amount = 0.f;
	UPROPERTY() float Height = 0.f;
	UPROPERTY() float Width = 8.f;
	UPROPERTY() float Strength = 0.5f;
	/** landscape.paint: rules applied over the whole landscape, or within Radius of Center when Radius is above 0. */
	UPROPERTY() TArray<FHyperAILandscapePaintRule> Rules;
	/** landscape.add_edit_layer: the new edit layer; landscape.sculpt and landscape.paint: the edit layer to change (the first when empty). */
	UPROPERTY() FString EditLayer;
	/**
	 * landscape.road: a landscape spline through Points (cm; each set on the ground), Width meters wide, the ground
	 * leveled under it and blending back over Falloff meters, the paint layer Layer painted under it, the static Mesh
	 * laid along it, and back to the first point when bClosed. In a World Partition level it is a spline actor
	 * labelled Road.
	 */
	UPROPERTY() TArray<FVector> Points;
	UPROPERTY() FString Layer;
	UPROPERTY() FString Mesh;
	UPROPERTY() FString Road;
	UPROPERTY() bool bClosed = false;
};

USTRUCT(BlueprintType)
struct FHyperAILandscapePlanRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FHyperAILandscapeOp> Operations;
	/** Only check the plan: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Save the level (and its landscape actors) afterwards; layer info assets are always saved. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAILandscapePlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, checked, awaiting_user_approval, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() int32 Applied = 0;
	/** Each landscape the plan made or changed, read back. */
	UPROPERTY() TArray<FString> Made;
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAILandscapeInspectRequest
{
	GENERATED_BODY()

	/** Landscape labels; every landscape in the open level when empty. */
	UPROPERTY() TArray<FString> Landscapes;
};

USTRUCT(BlueprintType)
struct FHyperAILandscapeInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Label;
	/** Center and extent in cm, vertices a side, cm between vertices, components and sections. */
	UPROPERTY() FVector Center = FVector::ZeroVector;
	UPROPERTY() FVector Extent = FVector::ZeroVector;
	UPROPERTY() int32 VerticesX = 0;
	UPROPERTY() int32 VerticesY = 0;
	UPROPERTY() float Resolution = 0.f;
	UPROPERTY() int32 Components = 0;
	UPROPERTY() int32 QuadsPerSection = 0;
	UPROPERTY() int32 SectionsPerComponent = 0;
	/** Lowest and highest ground, meters, and the vertical room the Z scale allows. */
	UPROPERTY() float HeightMin = 0.f;
	UPROPERTY() float HeightMax = 0.f;
	UPROPERTY() float HeightRange = 0.f;
	UPROPERTY() FString Material;
	/** Paint layers as Name (layer info asset): the share of the landscape each covers. */
	UPROPERTY() TArray<FString> PaintLayers;
	UPROPERTY() TArray<FString> EditLayers;
	/** Streaming proxies in a World Partition level. */
	UPROPERTY() int32 StreamingProxies = 0;
};

USTRUCT(BlueprintType)
struct FHyperAILandscapeInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() bool bWorldPartition = false;
	UPROPERTY() TArray<FHyperAILandscapeInfo> Landscapes;
};

/**
 * Landscapes: made from a heightmap texture, fractal noise or flat; sculpted in regions (raise, lower, flatten, smooth,
 * noise, terraces, ramps); given a material and paint layers painted by height and slope rules; with edit layers. Each
 * plan is one undo step.
 */
UCLASS()
class HYPERAISTUDIOLANDSCAPE_API UHyperAIStudioLandscapeToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Landscapes in the open level: center, extent, vertices, cm per vertex, components, the lowest and highest ground,
	 * material, paint layers with the share of ground each covers, edit layers, and World Partition streaming proxies.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|World")
	static FHyperAILandscapeInspectReport hyper_landscape_inspect(const FHyperAILandscapeInspectRequest& Request);

	/**
	 * Builds and shapes landscapes in one undoable plan: create one from a 16-bit heightmap, fractal noise or flat; sculpt
	 * regions (raise, lower, flatten, smooth, noise, terrace, ramp); make a material showing each paint layer in a color,
	 * or set one; paint layers by height, slope, distance from roads and in patches; add edit layers; lay roads through
	 * points, leveling and painting under them.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|World")
	static FHyperAILandscapePlanReport hyper_landscape_apply_plan(const FHyperAILandscapePlanRequest& Request);
};

class FHyperAIStudioLandscapeContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("worldbuilding_navigation");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiolandscapetoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_landscape_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_landscape_apply_plan");
	static constexpr int32 MaxOperations = 64;
	/** Vertices a side: 8129 is Unreal's largest recommended single landscape. */
	static constexpr int32 MaxVertices = 8129;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAILandscapeInspectReport Inspect(const FHyperAILandscapeInspectRequest& Request);
	static FHyperAILandscapePlanReport ApplyPlan(const FHyperAILandscapePlanRequest& Request);
};

class FHyperAIStudioLandscapeRegistration final
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
