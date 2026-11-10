// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_RECAST

#include "NavMeshToolset.h"

#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMeshToolsetResolver.h"

FNavMeshToolsetTileInfo UNavMeshToolset::GetTileInfo(
	const FVector Location, ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	FNavMeshToolsetTileInfo Info;

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

	int32 TileX = 0;
	int32 TileY = 0;
	if (!Navmesh->GetNavMeshTileXY(Location, TileX, TileY))
	{
		// bValid stays false: (0,0) is a real tile, so the defaults must not read as a report.
		return Info;
	}

	Info.bValid = true;
	Info.TileX = TileX;
	Info.TileY = TileY;
	const FIntPoint Coord(TileX, TileY);
#if 0 // UE 5.8: no GetNavMeshTileBoundsFromCoord or tile build state grid; bounds come from the tile's first layer.
	Info.CoordBounds = Navmesh->GetNavMeshTileBoundsFromCoord(Coord);
	Info.BuildState = NavMeshToolset::ConvertTileBuildState(Navmesh->GetTileBuildState(Coord));
#else
	Info.BuildState = ENavMeshToolsetTileBuildState::Unknown;
#endif
	Info.bInActiveSet = Navmesh->GetActiveTileSet().Contains(Coord);

	TArray<FNavTileRef> TileRefs;
	Navmesh->GetNavMeshTilesAt(TileX, TileY, TileRefs);
	if (!TileRefs.IsEmpty())
	{
		Info.CoordBounds = Navmesh->GetNavMeshTileBounds(TileRefs[0]); // UE 5.8: see above
	}
	for (const FNavTileRef& TileRef : TileRefs)
	{
		FNavMeshToolsetTileLayerInfo& Layer = Info.Layers.AddDefaulted_GetRef();

		int32 RefX = 0;
		int32 RefY = 0;
		int32 RefLayer = 0;
		if (Navmesh->GetNavMeshTileXY(TileRef, RefX, RefY, RefLayer))
		{
			Layer.Layer = RefLayer;
		}

		TArray<FNavPoly> Polys;
		if (Navmesh->GetPolysInTile(TileRef, Polys))
		{
			Layer.PolyCount = Polys.Num();
		}

		Layer.Bounds = Navmesh->GetNavMeshTileBounds(TileRef);

		ENavigationDataResolution Resolution = ENavigationDataResolution::Default;
		if (Navmesh->GetNavmeshTileResolution(TileRef, Resolution))
		{
			Layer.Resolution = Resolution;
		}
	}

	return Info;
}

#endif
