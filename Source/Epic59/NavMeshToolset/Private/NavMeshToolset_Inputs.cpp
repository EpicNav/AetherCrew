// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_RECAST

#include "NavMeshToolset.h"

#include "AI/NavigationModifier.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "NavAreas/NavArea.h"
#include "NavAreas/NavAreaMeta.h"
#include "Navigation/NavLinkProxy.h"
#include "NavigationOctree.h"
#include "NavigationSystem.h"
#include "NavLinkCustomComponent.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavModifierComponent.h"
#include "NavModifierVolume.h"
#include "NavMeshToolsetResolver.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
	void FillAreaModifiers(const FCompositeNavModifier& Modifiers, TArray<FNavMeshToolsetAreaModifierInfo>& OutAreaModifiers)
	{
		for (const FAreaNavModifier& AreaModifier : Modifiers.GetAreas())
		{
			FNavMeshToolsetAreaModifierInfo& Entry = OutAreaModifiers.AddDefaulted_GetRef();
			Entry.AreaClass = AreaModifier.GetAreaClass();
			Entry.AreaClassToReplace = AreaModifier.GetAreaClassToReplace();
			Entry.Shape = AreaModifier.GetShapeType();
			Entry.ApplyMode = AreaModifier.GetApplyMode();
			Entry.Bounds = AreaModifier.GetBounds();
		}
	}

	bool PassesRadiusFilter(const FVector& Location, const FVector& Center, const float Radius)
	{
		if (Radius <= 0.f)
		{
			return true;
		}

		return FVector::DistSquared(Location, Center) <= FMath::Square(Radius);
	}
}

TArray<FNavMeshToolsetNavBoundsInfo> UNavMeshToolset::ListNavBoundsVolumes(UWorld* const World)
{
	TArray<FNavMeshToolsetNavBoundsInfo> Results;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Results;
	}

	for (const FNavigationBounds& RegisteredBounds : NavSys->GetNavigationBounds())
	{
		FNavMeshToolsetNavBoundsInfo& Entry = Results.AddDefaulted_GetRef();
		Entry.Bounds = RegisteredBounds.AreaBox;
		Entry.SupportedAgentNames = NavMeshToolset::GetAgentNamesFromSelector(*NavSys, RegisteredBounds.SupportedAgents);

		const ULevel* const Level = RegisteredBounds.Level.Get();
		if (Level)
		{
			Entry.LevelName = Level->GetOutermost()->GetFName();
		}
	}

	// Match bounds volumes to their registered boxes so callers get the actor reference too.
	UWorld* const TargetWorld = NavSys->GetWorld();
	for (TActorIterator<ANavMeshBoundsVolume> VolumeIterator(TargetWorld); VolumeIterator; ++VolumeIterator)
	{
		ANavMeshBoundsVolume* const Volume = *VolumeIterator;
		const FBox VolumeBounds = Volume->GetComponentsBoundingBox(true);
		for (FNavMeshToolsetNavBoundsInfo& Entry : Results)
		{
			if (!Entry.VolumeActor && Entry.Bounds.Equals(VolumeBounds, 1.f))
			{
				Entry.VolumeActor = Volume;
				break;
			}
		}
	}

	return Results;
}

FNavMeshToolsetModifierListResult UNavMeshToolset::ListNavModifiers(
	const FVector Center, const float Radius, const int32 MaxResults, const bool bIncludeNonDefaultProperties,
	UWorld* const World)
{
	FNavMeshToolsetModifierListResult Result;
	TArray<FNavMeshToolsetModifierInfo>& Results = Result.Modifiers;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Result;
	}

	UWorld* const TargetWorld = NavSys->GetWorld();
	const int32 ResultCap = FMath::Clamp(MaxResults, 1, NavMeshToolsetBudgets::MaxResultsCeiling);

	// Gather everything and cap after the sort, so a truncated result is the first N by name
	// (raising the cap yields supersets) instead of an arbitrary iteration-order subset.
	for (TActorIterator<ANavModifierVolume> VolumeIterator(TargetWorld); VolumeIterator; ++VolumeIterator)
	{
		ANavModifierVolume* const Volume = *VolumeIterator;
		const FBox Bounds = Volume->GetComponentsBoundingBox(true);
		if (!PassesRadiusFilter(Bounds.GetCenter(), Center, Radius))
		{
			continue;
		}

		NavMeshToolset::FillModifierEntry(*Volume, Bounds, bIncludeNonDefaultProperties, Results.AddDefaulted_GetRef());
	}

	for (TObjectIterator<UNavModifierComponent> ComponentIterator; ComponentIterator; ++ComponentIterator)
	{
		UNavModifierComponent* const Component = *ComponentIterator;
		if (Component->IsTemplate() || Component->GetWorld() != TargetWorld)
		{
			continue;
		}

		AActor* const Owner = Component->GetOwner();
		if (!Owner)
		{
			continue;
		}

		const FBox Bounds = Component->GetNavigationBounds();
		if (!PassesRadiusFilter(Bounds.GetCenter(), Center, Radius))
		{
			continue;
		}

		NavMeshToolset::FillModifierEntry(*Component, Bounds, bIncludeNonDefaultProperties, Results.AddDefaulted_GetRef());
	}

	// OwnerActor is never null here: volumes are their own owner and ownerless components were
	// skipped during the gather.
	Results.Sort([](const FNavMeshToolsetModifierInfo& Left, const FNavMeshToolsetModifierInfo& Right)
	{
		const FString LeftName = Left.OwnerActor->GetName();
		const FString RightName = Right.OwnerActor->GetName();
		if (LeftName != RightName)
		{
			return LeftName < RightName;
		}

		// Name ties (same-named owners in different sublevels, or several components on one
		// owner) break on the unique path so the truncation prefix stays stable and raising the
		// cap yields supersets.
		const FString LeftPath = Left.Component ? Left.Component->GetPathName() : Left.OwnerActor->GetPathName();
		const FString RightPath = Right.Component ? Right.Component->GetPathName() : Right.OwnerActor->GetPathName();

		return LeftPath < RightPath;
	});

	if (Results.Num() > ResultCap)
	{
		Result.bTruncated = true;
		Results.SetNum(ResultCap);
	}

	return Result;
}

FNavMeshToolsetOctreeQueryResult UNavMeshToolset::QueryNavOctree(
	const FVector Center, const float Radius, const int32 MaxResults, UWorld* const World)
{
	FNavMeshToolsetOctreeQueryResult Result;
	TArray<FNavMeshToolsetOctreeElementInfo>& Results = Result.Elements;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Result;
	}

	FNavigationOctreeFilter OctreeFilter;
	OctreeFilter.bIncludeGeometry = true;
	OctreeFilter.bIncludeOffmeshLinks = true;
	OctreeFilter.bIncludeAreas = true;
	OctreeFilter.bIncludeMetaAreas = true;

	const FVector QueryExtent(FMath::Max(Radius, 1.f));
	TArray<FNavigationOctreeElement> Elements;
	NavSys->FindElementsInNavOctree(FBox(Center - QueryExtent, Center + QueryExtent), OctreeFilter, Elements);

	// Gather everything and cap after the sort, so a truncated result is a stable prefix
	// (raising the cap yields supersets) instead of an octree-traversal-order subset that
	// shifts as unrelated elements insert and remove.
	const int32 ResultCap = FMath::Clamp(MaxResults, 1, NavMeshToolsetBudgets::MaxResultsCeiling);
	for (const FNavigationOctreeElement& Element : Elements)
	{
		FNavMeshToolsetOctreeElementInfo& Entry = Results.AddDefaulted_GetRef();
		Entry.Bounds = Element.Bounds.GetBox();

		const FNavigationRelevantData& Data = Element.Data.Get();
		Entry.bHasGeometry = Data.HasGeometry();
		Entry.bHasModifiers = Data.HasModifiers();
		Entry.bDynamicModifiers = Data.HasDynamicModifiers();
		FillAreaModifiers(Data.Modifiers, Entry.AreaModifiers);

		const TSharedRef<const FNavigationElement> SourceElement = Element.GetSourceElement();
		Entry.SourceDescription = SourceElement->GetName();
		// The element owner is only stored to serialize its object path; shedding const is safe.
		Entry.SourceObject = const_cast<UObject*>(SourceElement->GetWeakUObject().Get());
	}

	Results.Sort([](const FNavMeshToolsetOctreeElementInfo& Left, const FNavMeshToolsetOctreeElementInfo& Right)
	{
		if (Left.SourceDescription != Right.SourceDescription)
		{
			return Left.SourceDescription < Right.SourceDescription;
		}

		// Identically-named elements order deterministically by location.
		const FVector LeftMin = Left.Bounds.Min;
		const FVector RightMin = Right.Bounds.Min;

		return LeftMin.X != RightMin.X ? LeftMin.X < RightMin.X :
			(LeftMin.Y != RightMin.Y ? LeftMin.Y < RightMin.Y : LeftMin.Z < RightMin.Z);
	});

	if (Results.Num() > ResultCap)
	{
		Result.bTruncated = true;
		Results.SetNum(ResultCap);
	}

	return Result;
}

TArray<FNavMeshToolsetNavAreaInfo> UNavMeshToolset::ListNavAreas(UWorld* const World)
{
	TArray<FNavMeshToolsetNavAreaInfo> Results;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Results;
	}

	for (TObjectIterator<UClass> ClassIterator; ClassIterator; ++ClassIterator)
	{
		UClass* const Class = *ClassIterator;
		if (!Class->IsChildOf(UNavAreaBase::StaticClass()))
		{
			continue;
		}

		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		FNavMeshToolsetNavAreaInfo& Entry = Results.AddDefaulted_GetRef();
		Entry.AreaClass = Class;
		Entry.bIsMetaArea = Class->IsChildOf(UNavAreaMeta::StaticClass());

		// Non-const because GetFixedAreaEnteringCost is a non-const accessor.
		UNavArea* const AreaDefaults = Cast<UNavArea>(Class->GetDefaultObject());
		if (AreaDefaults)
		{
			Entry.DefaultCost = AreaDefaults->DefaultCost;
			Entry.FixedEnteringCost = AreaDefaults->GetFixedAreaEnteringCost();
			Entry.DrawColor = AreaDefaults->DrawColor;
			Entry.AreaFlags = AreaDefaults->GetAreaFlags();
			Entry.SupportedAgentNames = NavMeshToolset::GetAgentNamesFromSelector(*NavSys, AreaDefaults->SupportedAgents);
		}

		for (const TObjectPtr<ANavigationData>& NavData : NavSys->NavDataSet)
		{
			if (!NavData)
			{
				continue;
			}

			const int32 AreaID = NavData->GetAreaID(Class);
			if (AreaID == INDEX_NONE)
			{
				continue;
			}

			FNavMeshToolsetAreaIdOnNavData& IdEntry = Entry.AreaIds.AddDefaulted_GetRef();
			IdEntry.NavData = NavData;
			IdEntry.AreaID = AreaID;
		}
	}

	Results.Sort([](const FNavMeshToolsetNavAreaInfo& Left, const FNavMeshToolsetNavAreaInfo& Right)
	{
		const UClass* const LeftClass = Left.AreaClass.Get();
		const UClass* const RightClass = Right.AreaClass.Get();
		const FString LeftName = LeftClass ? LeftClass->GetName() : FString();
		const FString RightName = RightClass ? RightClass->GetName() : FString();

		return LeftName < RightName;
	});

	return Results;
}

FNavMeshToolsetInvokersReport UNavMeshToolset::ListNavInvokers(UWorld* const World)
{
	FNavMeshToolsetInvokersReport Report;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Report;
	}

	Report.bInvokerOnlyGeneration = NavSys->IsActiveTilesGenerationEnabled();
	Report.SeedBounds = NavSys->GetInvokersSeedBounds();

#if 0 // UE 5.8: no ForEachNavigationInvoker (the invoker map is private); the raw records below carry no actor or name.
	NavSys->ForEachNavigationInvoker([&Report, NavSys](const FNavigationInvoker& Invoker)
	{
		if (Report.Invokers.Num() >= NavMeshToolsetBudgets::MaxResultsCeiling)
		{
			Report.bTruncated = true;
			return;
		}

		FNavMeshToolsetInvokerInfo& Entry = Report.Invokers.AddDefaulted_GetRef();
		Entry.Actor = Invoker.Actor.Get();
		Entry.InvokerName = Invoker.GetName();
		Entry.GenerationRadius = Invoker.GenerationRadius;
		Entry.RemovalRadius = Invoker.RemovalRadius;
		Entry.SupportedAgentNames = NavMeshToolset::GetAgentNamesFromSelector(*NavSys, Invoker.SupportedAgents);
		Entry.Priority = Invoker.Priority;

		FVector InvokerLocation = FVector::ZeroVector;
		Entry.bLocationValid = Invoker.GetLocation(InvokerLocation);
		if (Entry.bLocationValid)
		{
			Entry.Location = InvokerLocation;
		}
	});
#else
	for (const FNavigationInvokerRaw& Invoker : NavSys->GetInvokerLocations())
	{
		if (Report.Invokers.Num() >= NavMeshToolsetBudgets::MaxResultsCeiling)
		{
			Report.bTruncated = true;
			break;
		}

		FNavMeshToolsetInvokerInfo& Entry = Report.Invokers.AddDefaulted_GetRef();
		Entry.GenerationRadius = Invoker.RadiusMin;
		Entry.RemovalRadius = Invoker.RadiusMax;
		Entry.SupportedAgentNames = NavMeshToolset::GetAgentNamesFromSelector(*NavSys, Invoker.SupportedAgents);
		Entry.Priority = Invoker.Priority;
		Entry.Location = Invoker.Location;
		Entry.bLocationValid = true;
	}
#endif

	for (const TObjectPtr<ANavigationData>& NavData : NavSys->NavDataSet)
	{
		const ARecastNavMesh* const RecastNavmesh = Cast<ARecastNavMesh>(NavData.Get());
		if (!RecastNavmesh)
		{
			continue;
		}

		FNavMeshToolsetActiveTilesOnNavData& Entry = Report.ActiveTiles.AddDefaulted_GetRef();
		Entry.NavData = NavData;
		Entry.AgentName = NavMeshToolset::GetAgentNameForNavData(*RecastNavmesh);
		Entry.ActiveTileCount = RecastNavmesh->GetNumActiveTiles();
		Entry.TileSizeUU = RecastNavmesh->GetTileSizeUU();
	}

	return Report;
}

FNavMeshToolsetNavLinkListResult UNavMeshToolset::ListNavLinks(
	const FVector Center, const float Radius, const ENavMeshToolsetLinkSource Source, const int32 MaxResults,
	ANavigationData* const NavData, const FName AgentName, UWorld* const World)
{
	FNavMeshToolsetNavLinkListResult Result;
	TArray<FNavMeshToolsetNavLinkInfo>& Results = Result.Links;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Result;
	}

	UWorld* const TargetWorld = NavSys->GetWorld();
	const int32 ResultCap = FMath::Clamp(MaxResults, 1, NavMeshToolsetBudgets::MaxResultsCeiling);
	const bool bGatherProxies =
		Source == ENavMeshToolsetLinkSource::All || Source == ENavMeshToolsetLinkSource::ProxyActors;
	const bool bGatherCustom =
		Source == ENavMeshToolsetLinkSource::All || Source == ENavMeshToolsetLinkSource::CustomLinkComponents;
	const bool bGatherInMesh =
		Source == ENavMeshToolsetLinkSource::All || Source == ENavMeshToolsetLinkSource::InMeshLinks;

	// Resident-memory sources gather everything, sort by distance to the query Center, then cap:
	// a truncated result is "the nearest N", and raising the cap yields supersets. (The in-mesh
	// source below cannot afford gather-all; its flag says its subset is arbitrary.)
	auto SortByDistanceAndCap = [&Center, ResultCap](TArray<FNavMeshToolsetNavLinkInfo>& Gathered, bool& bOutTruncated)
	{
		Gathered.Sort([&Center](const FNavMeshToolsetNavLinkInfo& Left, const FNavMeshToolsetNavLinkInfo& Right)
		{
			const double LeftDist =
				FMath::Min(FVector::DistSquared(Left.Start, Center), FVector::DistSquared(Left.End, Center));
			const double RightDist =
				FMath::Min(FVector::DistSquared(Right.Start, Center), FVector::DistSquared(Right.End, Center));
			if (LeftDist != RightDist)
			{
				return LeftDist < RightDist;
			}

			// LinkId tiebreak (every fill path sets it): equal distances stay deterministic under
			// the unstable sort, keeping the truncation prefix stable.
			return Left.LinkId < Right.LinkId;
		});

		if (Gathered.Num() > ResultCap)
		{
			bOutTruncated = true;
			Gathered.SetNum(ResultCap);
		}
	};

	// Source 1: nav link proxy actors (point and segment links; the smart link is a component below).
	// Each source caps independently so authored actors can never starve the in-mesh source.
	// The Source filter gates iterator CONSTRUCTION too: both iterators pay their setup walk in
	// the constructor, before any flag in the loop condition could skip it.
	TArray<FNavMeshToolsetNavLinkInfo> ProxyLinks;
	if (bGatherProxies)
	{
		for (TActorIterator<ANavLinkProxy> ProxyIterator(TargetWorld); ProxyIterator; ++ProxyIterator)
		{
			ANavLinkProxy* const Proxy = *ProxyIterator;
			const FTransform ProxyTransform = Proxy->GetActorTransform();
			for (const FNavigationLink& PointLink : Proxy->PointLinks)
			{
				const FVector Start = ProxyTransform.TransformPosition(PointLink.Left);
				const FVector End = ProxyTransform.TransformPosition(PointLink.Right);
				if (!PassesRadiusFilter(Start, Center, Radius) && !PassesRadiusFilter(End, Center, Radius))
				{
					continue;
				}

				NavMeshToolset::FillProxyPointLinkEntry(*Proxy, PointLink, Start, End, ProxyLinks.AddDefaulted_GetRef());
			}

			for (const FNavigationSegmentLink& SegmentLink : Proxy->SegmentLinks)
			{
				// Each side is a segment; represent it by its midpoint.
				const FVector Start = ProxyTransform.TransformPosition((SegmentLink.LeftStart + SegmentLink.LeftEnd) * 0.5);
				const FVector End = ProxyTransform.TransformPosition((SegmentLink.RightStart + SegmentLink.RightEnd) * 0.5);
				if (!PassesRadiusFilter(Start, Center, Radius) && !PassesRadiusFilter(End, Center, Radius))
				{
					continue;
				}

				FNavMeshToolsetNavLinkInfo& Entry = ProxyLinks.AddDefaulted_GetRef();
				Entry.Kind = ENavMeshToolsetLinkKind::ProxySegmentLink;
				Entry.OwnerActor = Proxy;
				Entry.Start = Start;
				Entry.End = End;
				Entry.Direction = SegmentLink.Direction;
				Entry.AreaClass = SegmentLink.GetAreaClass();
				Entry.LinkId = NavMeshToolset::PolyRefToHex(SegmentLink.NavLinkId.GetId());
			}
		}
	}

	SortByDistanceAndCap(ProxyLinks, Result.bProxyLinksTruncated);
	Results.Append(MoveTemp(ProxyLinks));

	// Source 2: custom (smart) link components anywhere in the world.
	TArray<FNavMeshToolsetNavLinkInfo> CustomLinks;
	if (bGatherCustom)
	{
		for (TObjectIterator<UNavLinkCustomComponent> LinkIterator; LinkIterator; ++LinkIterator)
		{
			UNavLinkCustomComponent* const LinkComponent = *LinkIterator;
			if (LinkComponent->IsTemplate() || LinkComponent->GetWorld() != TargetWorld)
			{
				continue;
			}

			const FVector Start = LinkComponent->GetStartPoint();
			const FVector End = LinkComponent->GetEndPoint();
			if (!PassesRadiusFilter(Start, Center, Radius) && !PassesRadiusFilter(End, Center, Radius))
			{
				continue;
			}

			NavMeshToolset::FillCustomLinkEntry(*LinkComponent, CustomLinks.AddDefaulted_GetRef());
		}
	}

	SortByDistanceAndCap(CustomLinks, Result.bCustomLinksTruncated);
	Results.Append(MoveTemp(CustomLinks));

	if (!bGatherInMesh)
	{
		Result.InMeshSkipReason = TEXT("The in-mesh source was not requested (Source filter).");
		return Result;
	}

	// Source 3 needs a navmesh. A world with link actors but no navdata at all is still a valid
	// query when no navmesh was explicitly requested, but zero in-mesh links must never read as
	// "no broken links here": the gathered flag plus reason say the source never ran.
	if (!NavData && AgentName.IsNone() && !NavSys->GetDefaultNavDataInstance())
	{
		Result.InMeshSkipReason = TEXT("The world has no navigation data; in-mesh links were not gathered.");
		return Result;
	}

	// Source 3: links baked into the navmesh (includes generated jump links); broken ends detectable.
	// The debug-geometry gather is the only API exposing ValidEnds, and it materializes the tile's
	// full triangle soup, so the sweep is bounded by a tile budget, not just by links found.
	ANavigationData* const Resolved = NavMeshToolset::ResolveNavData(*NavSys, NavData, AgentName);
	ARecastNavMesh* const Navmesh = Cast<ARecastNavMesh>(Resolved);
	if (!Resolved)
	{
		Result.InMeshSkipReason =
			TEXT("Navigation data did not resolve (see the raised error); in-mesh links were not gathered.");
	}
	else if (!Navmesh)
	{
		Result.InMeshSkipReason = FString::Printf(
			TEXT("Navigation data '%s' is a %s, not a Recast navmesh; in-mesh links are Recast-only."),
			*Resolved->GetName(), *Resolved->GetClass()->GetName());
	}

	if (Navmesh)
	{
		Result.bInMeshLinksGathered = true;
		TArray<FNavTileRef> TileRefs;
		Navmesh->GetAllNavMeshTiles(TileRefs);

		int32 InMeshLinkCount = 0;
		int32 SweptTileCount = 0;
		for (const FNavTileRef& TileRef : TileRefs)
		{
			if (Radius > 0.f)
			{
				const FBox TileBounds = Navmesh->GetNavMeshTileBounds(TileRef);
				const FBox QueryBounds(Center - FVector(Radius), Center + FVector(Radius));
				if (!TileBounds.Intersect(QueryBounds))
				{
					continue;
				}
			}

			// Budget check sits after the radius filter, and the full-cap sweep runs on until a
			// link is actually dropped, so neither flag can fire when nothing was left out.
			if (SweptTileCount >= NavMeshToolsetBudgets::MaxLinkSweepTiles)
			{
				Result.bInMeshTilesTruncated = true;
				break;
			}

			++SweptTileCount;
			FRecastDebugGeometry Geometry;
			Geometry.bGatherPolyEdges = false;
			Geometry.bGatherNavMeshEdges = false;
			// The bool is a timeslicing "done" flag (false for single-tile calls), not success.
			Navmesh->GetDebugGeometryForTile(Geometry, TileRef);

			for (const FRecastDebugGeometry::FOffMeshLink& Link : Geometry.OffMeshLinks)
			{
				if (!PassesRadiusFilter(Link.Left, Center, Radius) && !PassesRadiusFilter(Link.Right, Center, Radius))
				{
					continue;
				}

				if (InMeshLinkCount >= ResultCap)
				{
					Result.bInMeshLinksTruncated = true;
					break;
				}

				++InMeshLinkCount;
				FNavMeshToolsetNavLinkInfo& Entry = Results.AddDefaulted_GetRef();
				Entry.Kind = ENavMeshToolsetLinkKind::InMeshLink;
				Entry.Start = Link.Left;
				Entry.End = Link.Right;
				Entry.Direction = (Link.Direction != 0) ? ENavLinkDirection::BothWays : ENavLinkDirection::LeftToRight;
				Entry.AreaClass = const_cast<UClass*>(Navmesh->GetAreaClass(Link.AreaID));

				// Both end bits must be set for a healthy link; anything else means an end missed the navmesh.
				constexpr uint8 BothEndsValid = 3;
				Entry.bEndsValid = (Link.ValidEnds == BothEndsValid);
			}

			if (Result.bInMeshLinksTruncated)
			{
				break;
			}
		}
	}

	return Result;
}

#endif
