// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioLightingToolset.h"

class AActor;
class UMaterialInstanceConstant;
class UMaterialInterface;
class UMeshComponent;
class UPackage;
class UWorld;

/**
 * A level's own material instances, so a lighting plan can make a surface take light as the reference's does (glossier,
 * rougher, a truer colour, a brighter glow) without changing the project's materials or any other level.
 */
namespace HyperAIStudio::Lighting::LookDev
{
	/** The level's folder under the made-assets root; one actor's own instances go in its Actors folder. */
	FString FolderFor(const UWorld& World);

	/**
	 * set_material_parameter: the level's instance of the slot's material takes the value, and every mesh slot in the level
	 * that showed the material shows the instance. bActorOnly (set_actor_material_parameter): the slot alone gets its own
	 * instance. Value is a number, linear "r,g,b[,a]", a texture's path, or default to drop the change. Shows, when set, is
	 * the material an earlier op of the same plan puts in the slot, for checking a plan before it runs.
	 */
	bool SetParameter(UWorld& World, UMeshComponent& Mesh, int32 Slot, bool bActorOnly, const FString& Parameter, const FString& Value,
		UMaterialInterface* Shows, bool bWrite, TArray<AActor*>& OutTouched, FString& OutError);

	/** The instance such an op changed, making none: null when there is none yet. */
	UMaterialInstanceConstant* InstanceFor(UWorld& World, UMeshComponent& Mesh, int32 Slot, bool bActorOnly);

	/** Whether Instance holds what an op set: the value, or, after default, no value of its own. */
	bool Holds(const UMaterialInstanceConstant* Instance, const FString& Parameter, const FString& Value);

	/** Placed actors whose meshes show Instance, or an actor's own instance made from it, with changes not yet saved. */
	TArray<AActor*> UnsavedUsers(UWorld& World, const UMaterialInterface& Instance);

	/** Look-dev instances Actor's meshes show, and those they are made from, with changes not yet saved. */
	void AddUnsaved(const AActor& Actor, TArray<UPackage*>& OutPackages);

	/** What the level's look-dev instances hold, in a stable order: part of the lighting revision. */
	FString RevisionText(const UWorld& World);

	/**
	 * Materials on what a view shows, most of the frame first, each with its parameters and what keeps it from taking light
	 * as the real surface would. InView says whether a bounding sphere reaches into the view; Origin is where it looks from.
	 */
	TArray<FHyperAILightingMaterialRecord> ReadMaterials(UWorld& World, const FVector& Origin, TFunctionRef<bool(const FVector&, double)> InView,
		int32 Limit, bool bLumenReflections, float MaxRoughnessToTrace, int32& OutTotal);
}
