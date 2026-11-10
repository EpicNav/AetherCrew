// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_RECAST

#include "NavMeshToolset.h"

#include "AI/Navigation/NavigationDirtyArea.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "NavigationData.h"
#include "NavigationDirtyAreasController.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMeshToolsetResolver.h"
#include "UObject/UnrealType.h"

TArray<FNavMeshToolsetWorldInfo> UNavMeshToolset::ListWorlds()
{
	TArray<FNavMeshToolsetWorldInfo> Results;
	if (!GEngine)
	{
		return Results;
	}

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* const World = Context.World();
		if (!World)
		{
			continue;
		}

		const EWorldType::Type WorldType = Context.WorldType;
		if (WorldType != EWorldType::Editor && WorldType != EWorldType::PIE && WorldType != EWorldType::Game)
		{
			continue;
		}

		FNavMeshToolsetWorldInfo& Entry = Results.AddDefaulted_GetRef();
		Entry.World = World;
		Entry.WorldName = World->GetName();
		Entry.WorldType = WorldType;
		Entry.NetMode = World->GetNetMode();
		// A non-play editor world has no game state to have authority over.
		Entry.bIsAuthority = (WorldType != EWorldType::Editor) && (Entry.NetMode != NM_Client);
		Entry.PlayerCount = World->GetNumPlayerControllers();

		const UNavigationSystemV1* const NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		if (NavSys)
		{
			Entry.bHasNavigationSystem = true;

			// Count like ListNavData: null NavDataSet slots are skipped.
			for (const ANavigationData* const NavData : NavSys->NavDataSet)
			{
				if (NavData)
				{
					Entry.NavDataCount++;
				}
			}
		}
	}

	// Same priority ladder as the implicit World=null resolution: authority play world first,
	// then any PIE, any Game, the editor world LAST.
	auto WorldRank = [](const FNavMeshToolsetWorldInfo& Entry)
	{
		if (Entry.WorldType == EWorldType::Editor)
		{
			return 3;
		}

		if (Entry.bIsAuthority)
		{
			return 0;
		}

		return (Entry.WorldType == EWorldType::PIE) ? 1 : 2;
	};
	// StableSort keeps equal-rank entries in the gather (world-context) order, which is the
	// order the null-World resolution scans, so entry 0 is exactly what null resolves to.
	Results.StableSort([&WorldRank](const FNavMeshToolsetWorldInfo& Left, const FNavMeshToolsetWorldInfo& Right)
	{
		return WorldRank(Left) < WorldRank(Right);
	});

	return Results;
}

FNavMeshToolsetNavSystemInfo UNavMeshToolset::GetNavigationSystemInfo(UWorld* const World, const bool bIncludeNonDefaultProperties)
{
	FNavMeshToolsetNavSystemInfo Info;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Info;
	}

	Info.World = NavSys->GetWorld();
	Info.NavSystemClass = NavSys->GetClass();
	Info.MainNavData = NavSys->GetDefaultNavDataInstance();

	for (const FNavDataConfig& AgentConfig : NavSys->GetSupportedAgents())
	{
		// OverrideSupportedAgents invalidates filtered-out entries in place; skip them.
		if (AgentConfig.IsValid())
		{
			Info.AgentNames.Add(AgentConfig.Name);
		}
	}

	Info.bBuildingLocked = NavSys->IsNavigationBuildingLocked();
	for (const ENavigationBuildLock::Type LockFlag :
		{ENavigationBuildLock::InitialLock, ENavigationBuildLock::Custom, ENavigationBuildLock::NoUpdateInEditor,
			ENavigationBuildLock::NoUpdateInPIE, ENavigationBuildLock::AsyncLoadLock})
	{
		if (NavSys->IsNavigationBuildingLocked(LockFlag))
		{
			Info.BuildLockReasons.Add(LockFlag);
		}
	}

	Info.bOctreeLocked = NavSys->IsNavigationOctreeLocked();
	Info.bNavigationDirty = NavSys->IsNavigationDirty();
	Info.NumDirtyAreas = NavSys->GetNumDirtyAreas();
	Info.NumRunningBuildTasks = NavSys->GetNumRunningBuildTasks();
	Info.NumRemainingBuildTasks = NavSys->GetNumRemainingBuildTasks();
	Info.bGenerateOnlyAroundInvokers = NavSys->IsActiveTilesGenerationEnabled();
	Info.bPreserveTilesOutsideInvokers = NavMeshToolset::ReadReflectedBoolValue(
		*NavSys, TEXT("bPreserveTilesOutsideInvokerRadius"), false, &Info.UnresolvedProperties);
	// Fallback keeps the struct's Invalid sentinel: a reflection miss must not read as a real mode.
	Info.DataGatheringMode = static_cast<ENavDataGatheringModeConfig>(NavMeshToolset::ReadReflectedEnumValue(
		*NavSys, TEXT("DataGatheringMode"), static_cast<int64>(ENavDataGatheringModeConfig::Invalid),
		&Info.UnresolvedProperties));

	if (bIncludeNonDefaultProperties)
	{
		NavMeshToolset::DumpNonDefaultProperties(*NavSys, Info.NonDefaultProperties);
	}

	return Info;
}

TArray<FNavMeshToolsetAgentInfo> UNavMeshToolset::ListSupportedAgents(UWorld* const World)
{
	TArray<FNavMeshToolsetAgentInfo> Results;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Results;
	}

	for (const FNavDataConfig& AgentConfig : NavSys->GetSupportedAgents())
	{
		// OverrideSupportedAgents invalidates filtered-out entries in place; skip them.
		if (!AgentConfig.IsValid())
		{
			continue;
		}

		FNavMeshToolsetAgentInfo& Entry = Results.AddDefaulted_GetRef();
		Entry.AgentName = AgentConfig.Name;
		Entry.Color = AgentConfig.Color;
		Entry.AgentRadius = AgentConfig.AgentRadius;
		Entry.AgentHeight = AgentConfig.AgentHeight;
		Entry.AgentStepHeight = AgentConfig.AgentStepHeight;
		Entry.bCanWalk = AgentConfig.bCanWalk;
		Entry.bCanJump = AgentConfig.bCanJump;
		Entry.bCanSwim = AgentConfig.bCanSwim;
		Entry.bCanFly = AgentConfig.bCanFly;
		Entry.DefaultQueryExtent = AgentConfig.DefaultQueryExtent;

		const TSubclassOf<AActor> NavDataClass = AgentConfig.GetNavDataClass<AActor>();
		if (NavDataClass.Get())
		{
			Entry.NavDataClass = FSoftClassPath(NavDataClass.Get());
		}

		Entry.ResolvedNavData = NavSys->GetNavDataForAgentName(AgentConfig.Name);
	}

	return Results;
}

TArray<FNavMeshToolsetNavDataSummary> UNavMeshToolset::ListNavData(UWorld* const World)
{
	TArray<FNavMeshToolsetNavDataSummary> Results;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Results;
	}

	for (const TObjectPtr<ANavigationData>& NavData : NavSys->NavDataSet)
	{
		if (!NavData)
		{
			continue;
		}

		Results.Add(NavMeshToolset::BuildNavDataSummary(*NavSys, *NavData));
	}

	Results.Sort([](const FNavMeshToolsetNavDataSummary& Left, const FNavMeshToolsetNavDataSummary& Right)
	{
		return Left.NavDataName < Right.NavDataName;
	});

	return Results;
}

FNavMeshToolsetNavMeshDetails UNavMeshToolset::DescribeNavMesh(
	const bool bIncludeNonDefaultProperties, ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	FNavMeshToolsetNavMeshDetails Details;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Details;
	}

	ARecastNavMesh* const Navmesh = NavMeshToolset::ResolveRecastNavMesh(*NavSys, NavData, AgentName);
	if (!Navmesh)
	{
		return Details;
	}

	Details.Summary = NavMeshToolset::BuildNavDataSummary(*NavSys, *Navmesh);
	Details.TileSizeUU = Navmesh->GetTileSizeUU();
	Details.AgentRadius = Navmesh->AgentRadius;
	Details.AgentHeight = Navmesh->AgentHeight;
	Details.AgentMaxSlope = Navmesh->AgentMaxSlope;

	for (int32 ResolutionIndex = 0; ResolutionIndex < static_cast<int32>(ENavigationDataResolution::MAX); ++ResolutionIndex)
	{
		const ENavigationDataResolution Resolution = static_cast<ENavigationDataResolution>(ResolutionIndex);
		FNavMeshToolsetResolutionParams& Params = Details.Resolutions.AddDefaulted_GetRef();
		Params.Resolution = Resolution;
		Params.CellSize = Navmesh->GetCellSize(Resolution);
		Params.CellHeight = Navmesh->GetCellHeight(Resolution);
		Params.AgentMaxStepHeight = Navmesh->GetAgentMaxStepHeight(Resolution);
	}

	Details.MaxSimplificationError = Navmesh->MaxSimplificationError;
	Details.MinRegionArea = Navmesh->MinRegionArea;
	Details.MergeRegionSize = Navmesh->MergeRegionSize;
	Details.TileNumberHardLimit = Navmesh->GetTileNumberHardLimit();
	Details.bFixedTilePoolSize = NavMeshToolset::ReadReflectedBoolValue(
		*Navmesh, TEXT("bFixedTilePoolSize"), false, &Details.UnresolvedProperties);
	Details.TilePoolSize = static_cast<int32>(NavMeshToolset::ReadReflectedEnumValue(
		*Navmesh, TEXT("TilePoolSize"), 0, &Details.UnresolvedProperties));
	Details.bIsWorldPartitioned = Navmesh->bIsWorldPartitioned;
	Details.bGenerateNavLinks = Navmesh->bGenerateNavLinks;
	Details.NumGeneratedJumpLinkConfigs = NavMeshToolset::ReadReflectedArrayNum(
		*Navmesh, TEXT("NavLinkJumpConfigs"), &Details.UnresolvedProperties);
	Details.HeuristicScale = Navmesh->HeuristicScale;
	Details.DefaultMaxSearchNodes = Navmesh->DefaultMaxSearchNodes;

	TArray<FSupportedAreaData> SupportedAreas;
	Navmesh->GetSupportedAreas(SupportedAreas);

	// Neutral defaults (travel 1, entering 0): GetAllAreaCosts is a virtual on the filter
	// interface, so a custom filter could leave slots unwritten; those must not read as free.
	float TravelCosts[RECAST_MAX_AREAS];
	float EnteringCosts[RECAST_MAX_AREAS] = {};
	for (float& TravelCost : TravelCosts)
	{
		TravelCost = 1.f;
	}
	bool bHasCosts = false;
	const FSharedConstNavQueryFilter DefaultFilter = Navmesh->GetDefaultQueryFilter();
	if (DefaultFilter.IsValid())
	{
		DefaultFilter->GetAllAreaCosts(TravelCosts, EnteringCosts, RECAST_MAX_AREAS);
		bHasCosts = true;
	}

	for (const FSupportedAreaData& Area : SupportedAreas)
	{
		FNavMeshToolsetAreaCostEntry& Entry = Details.SupportedAreas.AddDefaulted_GetRef();
		Entry.AreaClass = const_cast<UClass*>(Area.AreaClass.Get());
		Entry.AreaID = Area.AreaID;
		if (bHasCosts && Area.AreaID >= 0 && Area.AreaID < RECAST_MAX_AREAS)
		{
			Entry.TravelCost = TravelCosts[Area.AreaID];
			Entry.EnteringCost = EnteringCosts[Area.AreaID];
		}
	}

	if (bIncludeNonDefaultProperties)
	{
		NavMeshToolset::DumpNonDefaultProperties(*Navmesh, Details.NonDefaultProperties);
	}

	return Details;
}

FNavMeshToolsetBuildStatusReport UNavMeshToolset::GetNavMeshBuildStatus(
	ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	FNavMeshToolsetBuildStatusReport Report;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Report;
	}

	// Addressing a navmesh narrows PerNavData to it and adds the tile-detail fields; the default
	// call stays the cheap whole-world sweep.
	const bool bSpecificTarget = (NavData != nullptr) || !AgentName.IsNone();
	ANavigationData* AddressedNavData = nullptr;
	if (bSpecificTarget)
	{
		AddressedNavData = NavMeshToolset::ResolveNavData(*NavSys, NavData, AgentName);
		if (!AddressedNavData)
		{
			return Report;
		}

		// ResolveNavData only validates the world. Navdata outside NavDataSet (e.g. the abstract
		// navdata) has no tracked build state and would otherwise read as an empty report.
		if (!NavSys->NavDataSet.Contains(AddressedNavData))
		{
			NavMeshToolset::RaiseToolError(FString::Printf(
				TEXT("Navigation data '%s' is not in the world's NavDataSet; it has no tracked build state."),
				*AddressedNavData->GetName()));
			return Report;
		}
	}

	Report.bBuildInProgress = NavSys->IsNavigationBuildInProgress();
	Report.NumDirtyAreas = NavSys->GetNumDirtyAreas();
	Report.NumRunningBuildTasks = NavSys->GetNumRunningBuildTasks();
	Report.NumRemainingBuildTasks = NavSys->GetNumRemainingBuildTasks();

	// UE 5.8: DefaultDirtyAreasController is protected with no getter; read it through a derived-class member pointer.
	struct FDirtyAreasAccess : UNavigationSystemV1 { using UNavigationSystemV1::DefaultDirtyAreasController; };
	const FNavigationDirtyAreasController& DirtyAreasController = NavSys->*(&FDirtyAreasAccess::DefaultDirtyAreasController);
	for (const FNavigationDirtyArea& DirtyArea : DirtyAreasController.DirtyAreas)
	{
		if (Report.PendingDirtyAreas.Num() >= NavMeshToolsetBudgets::MaxResultsCeiling)
		{
			Report.bPendingDirtyAreasTruncated = true;
			break;
		}

		FNavMeshToolsetDirtyAreaInfo& Entry = Report.PendingDirtyAreas.AddDefaulted_GetRef();
		Entry.Bounds = DirtyArea.Bounds;
		Entry.RawFlags = static_cast<int32>(DirtyArea.Flags);
		Entry.SourceDescription = DirtyArea.GetSourceDescription();
	}

	for (const TObjectPtr<ANavigationData>& WorldNavData : NavSys->NavDataSet)
	{
		if (!WorldNavData || (AddressedNavData && WorldNavData.Get() != AddressedNavData))
		{
			continue;
		}

		FNavMeshToolsetNavDataBuildStatus& Entry = Report.PerNavData.AddDefaulted_GetRef();
		Entry.NavData = WorldNavData;
		Entry.AgentName = NavMeshToolset::GetAgentNameForNavData(*WorldNavData);
		Entry.bRebuildingSuspended = WorldNavData->IsRebuildingSuspended();
		Entry.NumSuspendedDirtyAreas = WorldNavData->GetNumSuspendedDirtyAreas();

		const ARecastNavMesh* const RecastNavmesh = Cast<ARecastNavMesh>(WorldNavData.Get());
		if (!RecastNavmesh)
		{
			continue;
		}

#if 0 // UE 5.8: no GetTotalUpdatedTilesCount
		Entry.TotalUpdatedTilesCount = static_cast<int64>(RecastNavmesh->GetTotalUpdatedTilesCount());
#endif
		NavMeshToolset::FillAdaptiveStats(*RecastNavmesh, Entry.AdaptiveStats);

		if (bSpecificTarget)
		{
			Entry.bTileDetailIncluded = true;
			Entry.TileCount = RecastNavmesh->GetNavMeshTilesCount();
			Entry.ActiveTileCount = RecastNavmesh->GetNumActiveTiles();
			Entry.bFixedTilePoolSize = NavMeshToolset::ReadReflectedBoolValue(
				*RecastNavmesh, TEXT("bFixedTilePoolSize"), false, &Entry.UnresolvedProperties);
			Entry.TilePoolSize = static_cast<int32>(NavMeshToolset::ReadReflectedEnumValue(
				*RecastNavmesh, TEXT("TilePoolSize"), 0, &Entry.UnresolvedProperties));
			Entry.NavmeshBounds = RecastNavmesh->GetNavMeshBounds();

#if 0 // UE 5.8: no tile state grid (GetTileGridExtent); bHasTileStateGrid stays false.
			const FNavMeshTileGridExtent& GridExtent = RecastNavmesh->GetTileGridExtent();
			if (GridExtent.Num() > 0)
			{
				Entry.bHasTileStateGrid = true;
				Entry.GridMin = GridExtent.Origin;
				Entry.GridMax = GridExtent.Origin + FIntPoint(GridExtent.Width - 1, GridExtent.Height - 1);
			}
#endif
		}
	}

	return Report;
}

#endif
