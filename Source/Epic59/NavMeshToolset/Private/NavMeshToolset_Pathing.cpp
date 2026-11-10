// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_RECAST

#include "NavMeshToolset.h"

#include "AI/Navigation/NavAreaBase.h"
#include "Engine/World.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavigationSystemTypes.h"
#include "NavMesh/NavMeshPath.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMeshToolsetResolver.h"

namespace
{
	FPathFindingQuery MakeQuery(
		const ANavigationData& NavData, const FVector& Start, const FVector& End,
		const FSharedConstNavQueryFilter& Filter, const bool bAllowPartialPath, const float CostLimit)
	{
		FPathFindingQuery Query(nullptr, NavData, Start, End, Filter);
		Query.bAllowPartialPaths = bAllowPartialPath;
		if (CostLimit > 0.f)
		{
			Query.CostLimit = CostLimit;
		}

		return Query;
	}
}

FNavMeshToolsetPathResult UNavMeshToolset::FindPath(
	const FVector Start, const FVector End,
	const TArray<FNavMeshToolsetAreaCostOverride>& AreaCostOverrides,
	const TArray<TSubclassOf<UNavAreaBase>>& ExcludedAreaClasses,
	const bool bAllowPartialPath, const float CostLimit, const int32 MaxPathPoints,
	ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	FNavMeshToolsetPathResult Result;
	Result.RequestedStart = Start;
	Result.RequestedEnd = End;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Result;
	}

	ANavigationData* const Resolved = NavMeshToolset::ResolveNavData(*NavSys, NavData, AgentName);
	if (!Resolved)
	{
		return Result;
	}

	Result.NavData = Resolved;
	Result.AgentName = NavMeshToolset::GetAgentNameForNavData(*Resolved);

	const FSharedConstNavQueryFilter Filter = NavMeshToolset::BuildQueryFilter(*Resolved, AreaCostOverrides, ExcludedAreaClasses);
	if (!Filter.IsValid())
	{
		return Result;
	}

	const FPathFindingQuery Query = MakeQuery(*Resolved, Start, End, Filter, bAllowPartialPath, CostLimit);
	const FPathFindingResult PathResult = NavSys->FindPathSync(Query);
	Result.Result = PathResult.Result;

	const FNavPathSharedPtr Path = PathResult.Path;
	if (!Path.IsValid())
	{
		return Result;
	}

	Result.bIsPartial = Path->IsPartial();
	Result.PathLength = static_cast<float>(Path->GetLength());
	Result.PathCost = static_cast<float>(Path->GetCost());

	const FNavMeshPath* const MeshPath = Path->CastPath<FNavMeshPath>();
	if (MeshPath)
	{
		Result.CorridorPolyCount = MeshPath->PathCorridor.Num();
	}

	const TArray<FNavPathPoint>& PathPoints = Path->GetPathPoints();
	const int32 PointCap = FMath::Clamp(MaxPathPoints, 2, 4096);
	for (const FNavPathPoint& PathPoint : PathPoints)
	{
		if (Result.PathPoints.Num() >= PointCap)
		{
			Result.bPointsTruncated = true;
			break;
		}

		FNavMeshToolsetPathPoint& Entry = Result.PathPoints.AddDefaulted_GetRef();
		Entry.Location = PathPoint.Location;
		if (PathPoint.NodeRef != INVALID_NAVNODEREF)
		{
			Entry.PolyRef = NavMeshToolset::PolyRefToHex(PathPoint.NodeRef);
		}

		if (PathPoint.CustomNavLinkId != FNavLinkId::Invalid)
		{
			Entry.CustomLinkId = NavMeshToolset::PolyRefToHex(PathPoint.CustomNavLinkId.GetId());
		}
	}

	if (PathPoints.Num() > 0)
	{
		Result.EffectiveEnd = PathPoints.Last().Location;
	}

	return Result;
}

FNavMeshToolsetRaycastResult UNavMeshToolset::RaycastNavMesh(
	const FVector Start, const FVector End,
	ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	FNavMeshToolsetRaycastResult Result;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Result;
	}

	ANavigationData* const Resolved = NavMeshToolset::ResolveNavData(*NavSys, NavData, AgentName);
	if (!Resolved)
	{
		return Result;
	}

	const FSharedConstNavQueryFilter DefaultFilter = Resolved->GetDefaultQueryFilter();
	if (!DefaultFilter.IsValid())
	{
		NavMeshToolset::RaiseToolError(FString::Printf(
			TEXT("Navigation data '%s' has no default query filter."), *Resolved->GetName()));
		return Result;
	}

	FVector HitLocation = End;
	FNavigationRaycastAdditionalResults AdditionalResults;
	Result.bHit = Resolved->Raycast(Start, End, HitLocation, &AdditionalResults, DefaultFilter);
	Result.HitLocation = HitLocation;
	Result.bIsRayEndInCorridor = AdditionalResults.bIsRayEndInCorridor;

	return Result;
}

FNavMeshToolsetReachableAreaResult UNavMeshToolset::GetReachableArea(
	const FVector Origin, const float MaxCost, const int32 MaxPolyCenters,
	ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	FNavMeshToolsetReachableAreaResult Result;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Result;
	}

	ARecastNavMesh* const Navmesh = NavMeshToolset::ResolveRecastNavMesh(*NavSys, NavData, AgentName);
	if (!Navmesh)
	{
		return Result;
	}

	TArray<NavNodeRef> FoundPolys;
	if (!Navmesh->GetPolysWithinPathingDistance(Origin, MaxCost, FoundPolys))
	{
		return Result;
	}

	Result.bValid = true;

	// Counts, area, and bounds cover the WHOLE reachable set (MaxCost already bounded the
	// enumeration); only the returned centers array is capped.
	const int32 CenterCap = FMath::Clamp(MaxPolyCenters, 1, NavMeshToolsetBudgets::MaxReachablePolyCenters);
	Result.PolyCount = FoundPolys.Num();
	Result.bPolyCentersTruncated = FoundPolys.Num() > CenterCap;

	FBox ReachableBounds(ForceInit);
	double TotalSurfaceArea = 0.0;
	TArray<FVector> PolyVerts;
	for (const NavNodeRef PolyRef : FoundPolys)
	{
		TotalSurfaceArea += Navmesh->GetPolySurfaceArea(PolyRef);

		// Bounds accumulate the actual poly GEOMETRY: centers alone would come up short by half
		// a poly on every side, and merged open-floor polys are metres across.
		PolyVerts.Reset();
		if (Navmesh->GetPolyVerts(PolyRef, PolyVerts))
		{
			for (const FVector& PolyVert : PolyVerts)
			{
				ReachableBounds += PolyVert;
			}
		}

		FVector PolyCenter = FVector::ZeroVector;
		if (!Navmesh->GetPolyCenter(PolyRef, PolyCenter))
		{
			continue;
		}

		ReachableBounds += PolyCenter;
		if (Result.PolyCenters.Num() < CenterCap)
		{
			Result.PolyCenters.Add(PolyCenter);
		}
	}

	Result.TotalSurfaceArea = TotalSurfaceArea;
	Result.ReachableBounds = ReachableBounds;

	return Result;
}

#endif
