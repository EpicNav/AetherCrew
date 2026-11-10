// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_RECAST

#include "NavMeshToolset.h"

#include "Engine/World.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMeshToolsetResolver.h"

namespace
{
	/** Per-navdata projection extent: positive values win, zero or negative falls back to the
	 *  navdata default. */
	FVector MakeProjectionExtent(const ANavigationData& NavData, const float HorizontalExtent, const float VerticalExtent)
	{
		const FVector DefaultExtent = NavData.GetConfig().DefaultQueryExtent;

		return FVector(
			HorizontalExtent > 0.f ? HorizontalExtent : DefaultExtent.X,
			HorizontalExtent > 0.f ? HorizontalExtent : DefaultExtent.Y,
			VerticalExtent > 0.f ? VerticalExtent : DefaultExtent.Z);
	}

	FNavMeshToolsetProjectionResult ProjectOntoNavData(
		UNavigationSystemV1& NavSys, ANavigationData& NavData, const FVector& Point,
		const float HorizontalExtent, const float VerticalExtent)
	{
		FNavMeshToolsetProjectionResult Result;
		Result.InputPoint = Point;
		Result.NavData = &NavData;
		Result.AgentName = NavMeshToolset::GetAgentNameForNavData(NavData);

		const FVector Extent = MakeProjectionExtent(NavData, HorizontalExtent, VerticalExtent);
		FNavLocation NavLocation;
		Result.bProjected = NavSys.ProjectPointToNavigation(Point, NavLocation, Extent, &NavData);
		if (Result.bProjected)
		{
			Result.ProjectedPoint = NavLocation.Location;
			if (NavLocation.HasNodeRef())
			{
				Result.PolyRef = NavMeshToolset::PolyRefToHex(NavLocation.NodeRef);
			}
		}

		return Result;
	}
}

TArray<FNavMeshToolsetProjectionResult> UNavMeshToolset::ProjectPointToNavMesh(
	const FVector Point, const float HorizontalExtent, const float VerticalExtent, const bool bAllAgents,
	ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	TArray<FNavMeshToolsetProjectionResult> Results;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Results;
	}

	if (bAllAgents)
	{
		for (const FNavDataConfig& AgentConfig : NavSys->GetSupportedAgents())
		{
			// OverrideSupportedAgents invalidates filtered-out entries in place; skip them.
			if (!AgentConfig.IsValid())
			{
				continue;
			}

			ANavigationData* const AgentNavData = NavSys->GetNavDataForAgentName(AgentConfig.Name);
			if (!AgentNavData)
			{
				continue;
			}

			Results.Add(ProjectOntoNavData(*NavSys, *AgentNavData, Point, HorizontalExtent, VerticalExtent));
		}

		if (Results.IsEmpty())
		{
			NavMeshToolset::RaiseToolError(TEXT("No agent has live navigation data in this world."));
		}

		return Results;
	}

	ANavigationData* const Resolved = NavMeshToolset::ResolveNavData(*NavSys, NavData, AgentName);
	if (!Resolved)
	{
		return Results;
	}

	Results.Add(ProjectOntoNavData(*NavSys, *Resolved, Point, HorizontalExtent, VerticalExtent));

	return Results;
}

FNavMeshToolsetPolyInfo UNavMeshToolset::DescribePolyAtLocation(
	const FVector Point, ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	FNavMeshToolsetPolyInfo Info;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Info;
	}

	ARecastNavMesh* const Navmesh = NavMeshToolset::ResolveRecastNavMesh(*NavSys, NavData, AgentName);
	if (!Navmesh)
	{
		return Info;
	}

	const FVector Extent = MakeProjectionExtent(*Navmesh, 0.f, 0.f);
	const NavNodeRef PolyRef = Navmesh->FindNearestPoly(Point, Extent);
	if (PolyRef == INVALID_NAVNODEREF)
	{
		return Info;
	}

	Info.bValid = true;
	Info.PolyRef = NavMeshToolset::PolyRefToHex(PolyRef);
	Info.AreaID = static_cast<int32>(Navmesh->GetPolyAreaID(PolyRef));
	Info.AreaClass = const_cast<UClass*>(Navmesh->GetAreaClass(Info.AreaID));
	Info.AreaColor = Navmesh->GetAreaIDColor(static_cast<uint8>(Info.AreaID));

	uint16 PolyFlags = 0;
	uint16 AreaFlags = 0;
	if (Navmesh->GetPolyFlags(PolyRef, PolyFlags, AreaFlags))
	{
		Info.PolyFlags = PolyFlags;
		Info.bIsNavLink = (PolyFlags & ARecastNavMesh::GetNavLinkFlag()) != 0;
	}

	Navmesh->GetPolyVerts(PolyRef, Info.Verts);
	Info.SurfaceArea = static_cast<float>(Navmesh->GetPolySurfaceArea(PolyRef));

	// Derive the tile from the found poly, not the query point: FindNearestPoly searches an
	// extent, so an off-mesh point can resolve to a poly owned by a neighboring tile. When
	// either read fails, bTileResolved stays false (tile coords are signed, so no sentinel).
	FVector PolyCenter = FVector::ZeroVector;
	int32 TileX = 0;
	int32 TileY = 0;
	if (Navmesh->GetPolyCenter(PolyRef, PolyCenter) && Navmesh->GetNavMeshTileXY(PolyCenter, TileX, TileY))
	{
		Info.bTileResolved = true;
		Info.TileX = TileX;
		Info.TileY = TileY;

		// Resolve the layer by finding which tile at this coordinate actually contains the poly.
		TArray<FNavTileRef> TileRefs;
		Navmesh->GetNavMeshTilesAt(TileX, TileY, TileRefs);
		for (const FNavTileRef& TileRef : TileRefs)
		{
			TArray<FNavPoly> TilePolys;
			if (!Navmesh->GetPolysInTile(TileRef, TilePolys))
			{
				continue;
			}

			const bool bContainsPoly = TilePolys.ContainsByPredicate([PolyRef](const FNavPoly& Poly)
			{
				return Poly.Ref == PolyRef;
			});
			if (!bContainsPoly)
			{
				continue;
			}

			int32 RefX = 0;
			int32 RefY = 0;
			int32 RefLayer = 0;
			if (Navmesh->GetNavMeshTileXY(TileRef, RefX, RefY, RefLayer))
			{
				Info.TileLayer = RefLayer;
			}
			break;
		}
	}

	TArray<NavNodeRef> Neighbors;
	if (Navmesh->GetPolyNeighbors(PolyRef, Neighbors))
	{
		for (const NavNodeRef Neighbor : Neighbors)
		{
			Info.NeighborPolyRefs.Add(NavMeshToolset::PolyRefToHex(Neighbor));
		}
	}

	return Info;
}

FNavMeshToolsetWallDistanceResult UNavMeshToolset::FindDistanceToWall(
	const FVector Point, const float MaxDistance, ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	FNavMeshToolsetWallDistanceResult Result;

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

	FVector ClosestPointOnWall = FVector::ZeroVector;
	const FVector::FReal Distance = Navmesh->FindDistanceToWall(Point, nullptr, MaxDistance, &ClosestPointOnWall);

	// The engine returns 0 both for failure and for a point exactly on a wall; the two are not
	// distinguishable here, so an exact-zero distance reports bValid false.
	if (Distance > 0.)
	{
		Result.bValid = true;
		Result.Distance = static_cast<float>(Distance);
		Result.ClosestWallPoint = ClosestPointOnWall;
	}

	return Result;
}

#endif
