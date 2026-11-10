// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioFoliageToolset.generated.h"

/** One foliage type setting by its Unreal property name, in Unreal's text form: Density 20, ScaleX (Min=0.8,Max=1.2). */
USTRUCT(BlueprintType)
struct FHyperAIFoliageSetting
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Value;
};

/** A circle foliage.scatter keeps clear: a path, a camp, a doorway. */
USTRUCT(BlueprintType)
struct FHyperAIFoliageClearing
{
	GENERATED_BODY()

	/** Center in cm; Z is ignored. */
	UPROPERTY() FVector Center = FVector::ZeroVector;
	/** Meters. */
	UPROPERTY() float Radius = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAIFoliageOp
{
	GENERATED_BODY()

	/** foliage_type.create, foliage_type.set, foliage_type.remove, foliage.scatter, foliage.place, foliage.erase. */
	UPROPERTY() FString Kind;
	/**
	 * Foliage type assets: /Game/Foliage/FT_Pine, or FT_Pine when one asset has that name. foliage_type.create and
	 * foliage_type.set take one; foliage.erase erases every type when empty.
	 */
	UPROPERTY() TArray<FString> Types;
	/** foliage_type.create: the static mesh the type places; foliage_type.set: a new mesh, or empty to keep it. */
	UPROPERTY() FString Mesh;
	/**
	 * foliage_type.create, foliage_type.set: settings by Unreal name, in Unreal's units: Density (instances per 100 m2),
	 * Radius (cm between instances of the type), Scaling (Uniform, Free, LockXY), ScaleX (Min=0.8,Max=1.2), AlignToNormal,
	 * AlignMaxAngle, RandomYaw, RandomPitchAngle, GroundSlopeAngle (Min=0,Max=30), Height (Min=0,Max=5000) cm, ZOffset,
	 * LandscapeLayers ("Grass"), MinimumLayerWeight, ExclusionLandscapeLayers, CollisionWithWorld,
	 * CullDistance (Min=0,Max=8000), CastShadow. A mistyped name is refused with the full list.
	 */
	UPROPERTY() TArray<FHyperAIFoliageSetting> Settings;
	/**
	 * foliage.scatter, foliage.erase: the region, a circle Radius meters around Center (cm), a rectangle Size meters
	 * across centered on Center, or all of a Landscape (by label).
	 */
	UPROPERTY() FVector Center = FVector::ZeroVector;
	UPROPERTY() float Radius = 0.f;
	UPROPERTY() FVector2D Size = FVector2D::ZeroVector;
	UPROPERTY() FString Landscape;
	/**
	 * foliage.scatter: instances per 100 m2, or 0 for each type's own Density. Instances already in the region count
	 * toward it, so scattering twice does not double up.
	 */
	UPROPERTY() float Density = 0.f;
	/** foliage.scatter, foliage.erase: the same seed and region choose the same spots. */
	UPROPERTY() int32 Seed = 1;
	/**
	 * foliage.scatter, foliage.place: what instances stand on: landscape, static_mesh, bsp, foliage; translucent also
	 * allows see-through surfaces. Landscape only when empty.
	 */
	UPROPERTY() TArray<FString> Surfaces;
	/**
	 * foliage.scatter, foliage.place: also on ground with something above it (a roof, a bridge, a road mesh on the
	 * terrain). Off by default.
	 */
	UPROPERTY() bool bUnderCover = false;
	/** foliage.scatter: circles kept clear. */
	UPROPERTY() TArray<FHyperAIFoliageClearing> Clearings;
	/** foliage.place: one instance at each point (cm), dropped onto the surface below. */
	UPROPERTY() TArray<FVector> Points;
	/** foliage.erase: the share of the region's instances removed, chosen at random: 1 clears it, 0.5 thins it by half. */
	UPROPERTY() float Fraction = 1.f;
};

USTRUCT(BlueprintType)
struct FHyperAIFoliagePlanRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FHyperAIFoliageOp> Operations;
	/** Only check the plan: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Save the level (and its foliage actors) afterwards; foliage type assets are always saved. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIFoliagePlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, checked, awaiting_user_approval, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() int32 Applied = 0;
	/** Instances the plan placed and removed. */
	UPROPERTY() int32 Placed = 0;
	UPROPERTY() int32 Removed = 0;
	/** What each op did: a type's settings, or how many spots a scatter tried and why the rest were turned away. */
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIFoliageInspectRequest
{
	GENERATED_BODY()

	/** Foliage types to report; every type in the level when empty. */
	UPROPERTY() TArray<FString> Types;
	/** Where to look for foliage type assets not yet in the level; /Game when empty. */
	UPROPERTY() FString Folder;
	/** Each type's every setting as well as the summary. */
	UPROPERTY() bool bSettings = false;
};

USTRUCT(BlueprintType)
struct FHyperAIFoliageTypeInfo
{
	GENERATED_BODY()

	/** The asset, or the mesh's name for a type local to its foliage actor. */
	UPROPERTY() FString Type;
	UPROPERTY() FString Mesh;
	/** Instances in the loaded level, and the foliage actors (World Partition cells) holding them. */
	UPROPERTY() int32 Instances = 0;
	UPROPERTY() int32 FoliageActors = 0;
	/** Where its instances stand, cm. */
	UPROPERTY() FVector Min = FVector::ZeroVector;
	UPROPERTY() FVector Max = FVector::ZeroVector;
	/** Density, spacing, scale, alignment, slope and height limits, landscape layers, culling, collision. */
	UPROPERTY() FString Summary;
	UPROPERTY() TArray<FString> Settings;
};

USTRUCT(BlueprintType)
struct FHyperAIFoliageInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() bool bWorldPartition = false;
	UPROPERTY() int32 FoliageActors = 0;
	UPROPERTY() int32 Instances = 0;
	UPROPERTY() TArray<FHyperAIFoliageTypeInfo> Types;
	/** Foliage type assets under Folder that the level does not use yet. */
	UPROPERTY() TArray<FString> Assets;
};

/**
 * Foliage: foliage type assets made and tuned, instances scattered over regions by density and each type's placement
 * rules (slope, height, landscape layers, spacing), placed at points, thinned or erased. Each plan is one undo step.
 */
UCLASS()
class HYPERAISTUDIOFOLIAGE_API UHyperAIStudioFoliageToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Foliage in the open level: each type's instance count, where they stand and its placement settings, the foliage
	 * actors holding them, and foliage type assets not used yet.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|World")
	static FHyperAIFoliageInspectReport hyper_foliage_inspect(const FHyperAIFoliageInspectRequest& Request);

	/**
	 * Dresses a level with foliage in one plan, one undo step: make or tune foliage types (mesh, density, spacing, scale,
	 * slope and height limits, landscape layers, culling); scatter them over a circle, rectangle or landscape, off steep
	 * ground, out from under roofs, clear of paths; place them at points; thin or erase them; take types out.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|World")
	static FHyperAIFoliagePlanReport hyper_foliage_apply_plan(const FHyperAIFoliagePlanRequest& Request);
};

class FHyperAIStudioFoliageContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("worldbuilding_navigation");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiofoliagetoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_foliage_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_foliage_apply_plan");
	static constexpr int32 MaxOperations = 64;
	/** Spots a plan may try across its scatters and placements; each is a trace or two. */
	static constexpr int32 MaxSpots = 300000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIFoliageInspectReport Inspect(const FHyperAIFoliageInspectRequest& Request);
	static FHyperAIFoliagePlanReport ApplyPlan(const FHyperAIFoliagePlanRequest& Request);
};

class FHyperAIStudioFoliageRegistration final
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
