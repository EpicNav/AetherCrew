// Copyright Epic Games, Inc. All Rights Reserved.

// Stubs for targets built without Recast (bCompileRecast=false). Every implementation file
// compiles empty there, but the generated exec thunks still link against each UFUNCTION, so
// each tool gets a stub that raises a script error. The module never registers the toolset on
// such targets, so these only fire if something invokes the class directly.

#if !WITH_RECAST

#include "NavMeshToolset.h"

#include "Kismet/KismetSystemLibrary.h"
#include "NavMeshToolsetModule.h"

namespace
{
	void RaiseNoRecastError()
	{
		// Log unconditionally: RaiseScriptError is silent without a script frame and in Test builds.
		UE_LOGF(LogNavMeshToolset, Error,
			"NavMeshToolset is unavailable: this target was built without Recast (bCompileRecast=false).");
		UKismetSystemLibrary::RaiseScriptError(
			TEXT("NavMeshToolset is unavailable: this target was built without Recast (bCompileRecast=false)."));
	}
}

TArray<FNavMeshToolsetWorldInfo> UNavMeshToolset::ListWorlds()
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetNavSystemInfo UNavMeshToolset::GetNavigationSystemInfo(UWorld*, bool)
{
	RaiseNoRecastError();

	return {};
}

TArray<FNavMeshToolsetAgentInfo> UNavMeshToolset::ListSupportedAgents(UWorld*)
{
	RaiseNoRecastError();

	return {};
}

TArray<FNavMeshToolsetNavDataSummary> UNavMeshToolset::ListNavData(UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetNavMeshDetails UNavMeshToolset::DescribeNavMesh(bool, ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetBuildStatusReport UNavMeshToolset::GetNavMeshBuildStatus(ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

TArray<FNavMeshToolsetNavBoundsInfo> UNavMeshToolset::ListNavBoundsVolumes(UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetModifierListResult UNavMeshToolset::ListNavModifiers(FVector, float, int32, bool, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetOctreeQueryResult UNavMeshToolset::QueryNavOctree(FVector, float, int32, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

TArray<FNavMeshToolsetNavAreaInfo> UNavMeshToolset::ListNavAreas(UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetInvokersReport UNavMeshToolset::ListNavInvokers(UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetNavLinkListResult UNavMeshToolset::ListNavLinks(
	FVector, float, ENavMeshToolsetLinkSource, int32, ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	FNavMeshToolsetNavLinkListResult Result;
	Result.InMeshSkipReason = TEXT("Recast navigation is compiled out of this build.");

	return Result;
}

TArray<FNavMeshToolsetProjectionResult> UNavMeshToolset::ProjectPointToNavMesh(
	FVector, float, float, bool, ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetPolyInfo UNavMeshToolset::DescribePolyAtLocation(FVector, ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetWallDistanceResult UNavMeshToolset::FindDistanceToWall(
	FVector, float, ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetPathResult UNavMeshToolset::FindPath(
	FVector, FVector, const TArray<FNavMeshToolsetAreaCostOverride>&, const TArray<TSubclassOf<UNavAreaBase>>&,
	bool, float, int32, ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetRaycastResult UNavMeshToolset::RaycastNavMesh(FVector, FVector, ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetReachableAreaResult UNavMeshToolset::GetReachableArea(
	FVector, float, int32, ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetTileInfo UNavMeshToolset::GetTileInfo(FVector, ANavigationData*, FName, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetAgentActorListResult UNavMeshToolset::ListNavAgentActors(
	TSubclassOf<APawn>, FVector, float, int32, UWorld*)
{
	RaiseNoRecastError();

	return {};
}

FNavMeshToolsetActorNavInfo UNavMeshToolset::GetActorNavInfo(AActor*, bool)
{
	RaiseNoRecastError();

	return {};
}

#endif
