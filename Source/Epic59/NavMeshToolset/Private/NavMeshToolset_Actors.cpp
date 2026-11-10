// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_RECAST

#include "NavMeshToolset.h"

#include "AI/Navigation/NavAgentInterface.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Navigation/NavLinkProxy.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationInvokerComponent.h"
#include "NavigationSystem.h"
#include "NavLinkCustomComponent.h"
#include "NavModifierComponent.h"
#include "NavModifierVolume.h"
#include "NavMeshToolsetResolver.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Resolves an actor's navdata through its nav agent interface (or defaults). */
	void FillAgentActorInfo(UNavigationSystemV1& NavSys, AActor& Actor, FNavMeshToolsetAgentActorInfo& OutInfo)
	{
		OutInfo.Actor = &Actor;
		OutInfo.ActorClass = Actor.GetClass();

		const INavAgentInterface* const AgentInterface = Cast<INavAgentInterface>(&Actor);
		const FNavAgentProperties& AgentProperties = AgentInterface ?
			AgentInterface->GetNavAgentPropertiesRef() : FNavAgentProperties::DefaultProperties;
		OutInfo.Location = AgentInterface ? AgentInterface->GetNavAgentLocation() : Actor.GetActorLocation();
		OutInfo.AgentRadius = AgentProperties.AgentRadius;
		OutInfo.AgentHeight = AgentProperties.AgentHeight;
		OutInfo.NavData = NavSys.GetNavDataForProps(AgentProperties, OutInfo.Location);
	}
}

FNavMeshToolsetAgentActorListResult UNavMeshToolset::ListNavAgentActors(
	const TSubclassOf<APawn> ClassFilter, const FVector Center, const float Radius, const int32 MaxResults, UWorld* const World)
{
	FNavMeshToolsetAgentActorListResult Result;
	TArray<FNavMeshToolsetAgentActorInfo>& Results = Result.Pawns;

	UNavigationSystemV1* const NavSys = NavMeshToolset::ResolveNavSystem(World);
	if (!NavSys)
	{
		return Result;
	}

	UWorld* const TargetWorld = NavSys->GetWorld();
	UClass* const FilterClass = ClassFilter.Get() ? ClassFilter.Get() : APawn::StaticClass();
	const int32 ResultCap = FMath::Clamp(MaxResults, 1, NavMeshToolsetBudgets::MaxResultsCeiling);

	// Gather everything and cap after the sort, so a truncated result is the first N by name
	// (raising the cap yields supersets) instead of an arbitrary iteration-order subset.
	for (TActorIterator<APawn> PawnIterator(TargetWorld, FilterClass); PawnIterator; ++PawnIterator)
	{
		APawn* const Pawn = *PawnIterator;
		if (Radius > 0.f && FVector::DistSquared(Pawn->GetActorLocation(), Center) > FMath::Square(Radius))
		{
			continue;
		}

		FNavMeshToolsetAgentActorInfo& Entry = Results.AddDefaulted_GetRef();
		FillAgentActorInfo(*NavSys, *Pawn, Entry);
	}

	// Actor is never null here: every record was just filled from a live TActorIterator pawn.
	Results.Sort([](const FNavMeshToolsetAgentActorInfo& Left, const FNavMeshToolsetAgentActorInfo& Right)
	{
		const FString LeftName = Left.Actor->GetName();
		const FString RightName = Right.Actor->GetName();
		if (LeftName != RightName)
		{
			return LeftName < RightName;
		}

		// Name ties (same-named actors in different sublevels) break on the unique path so the
		// truncation prefix stays stable and raising the cap yields supersets.
		return Left.Actor->GetPathName() < Right.Actor->GetPathName();
	});

	if (Results.Num() > ResultCap)
	{
		Result.bTruncated = true;
		Results.SetNum(ResultCap);
	}

	return Result;
}

FNavMeshToolsetActorNavInfo UNavMeshToolset::GetActorNavInfo(AActor* const Actor, const bool bIncludeNonDefaultProperties)
{
	FNavMeshToolsetActorNavInfo Info;

	if (!Actor)
	{
		NavMeshToolset::RaiseToolError(TEXT("Actor is null."));
		return Info;
	}

	// The actor's own world is the target, but it still has to be a queryable world type;
	// ResolveWorld raises for preview/inactive worlds like every other entry point.
	UWorld* const World = Actor->GetWorld();
	if (World && !NavMeshToolset::ResolveWorld(World))
	{
		return Info;
	}

	UNavigationSystemV1* const NavSys = World ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(World) : nullptr;
	if (!NavSys)
	{
		NavMeshToolset::RaiseToolError(FString::Printf(
			TEXT("Actor '%s' is not in a world with a navigation system."), *Actor->GetName()));
		return Info;
	}

	FillAgentActorInfo(*NavSys, *Actor, Info.AgentInfo);

	// Nav-relevant components on the actor.
	for (UActorComponent* const Component : Actor->GetComponents())
	{
		if (UNavModifierComponent* const ModifierComponent = Cast<UNavModifierComponent>(Component))
		{
			NavMeshToolset::FillModifierEntry(*ModifierComponent, ModifierComponent->GetNavigationBounds(),
				bIncludeNonDefaultProperties, Info.ModifierComponents.AddDefaulted_GetRef());
			continue;
		}

		if (UNavigationInvokerComponent* const InvokerComponent = Cast<UNavigationInvokerComponent>(Component))
		{
			FNavMeshToolsetInvokerInfo& Entry = Info.InvokerComponents.AddDefaulted_GetRef();
			Entry.Actor = Actor;
			Entry.InvokerName = Actor->GetName();
			// A live actor always yields a location, unlike registry records resolved via weak refs.
			Entry.Location = Actor->GetActorLocation();
			Entry.bLocationValid = true;
			Entry.GenerationRadius = InvokerComponent->GetGenerationRadius();
			Entry.RemovalRadius = InvokerComponent->GetRemovalRadius();

			// SupportedAgents and Priority are protected with no public accessors; reflected reads
			// report misses so a rename can never impersonate "unrestricted" or "Default".
			const FStructProperty* const SelectorProperty = CastField<FStructProperty>(
				UNavigationInvokerComponent::StaticClass()->FindPropertyByName(TEXT("SupportedAgents")));
			if (SelectorProperty && SelectorProperty->Struct == FNavAgentSelector::StaticStruct())
			{
				const FNavAgentSelector* const Selector =
					SelectorProperty->ContainerPtrToValuePtr<FNavAgentSelector>(InvokerComponent);
				Entry.SupportedAgentNames = NavMeshToolset::GetAgentNamesFromSelector(*NavSys, *Selector);
			}
			else
			{
				Info.UnresolvedProperties.Add(TEXT("SupportedAgents"));
			}

			Entry.Priority = static_cast<ENavigationInvokerPriority>(NavMeshToolset::ReadReflectedEnumValue(
				*InvokerComponent, TEXT("Priority"),
				static_cast<int64>(ENavigationInvokerPriority::Default), &Info.UnresolvedProperties));
			continue;
		}

		if (UNavLinkCustomComponent* const LinkComponent = Cast<UNavLinkCustomComponent>(Component))
		{
			NavMeshToolset::FillCustomLinkEntry(*LinkComponent, Info.Links.AddDefaulted_GetRef());
			continue;
		}
	}

	// An actor that IS a modifier volume carries no modifier component; report it directly.
	if (ANavModifierVolume* const ModifierVolume = Cast<ANavModifierVolume>(Actor))
	{
		NavMeshToolset::FillModifierEntry(*ModifierVolume, ModifierVolume->GetComponentsBoundingBox(true),
			bIncludeNonDefaultProperties, Info.ModifierComponents.AddDefaulted_GetRef());
	}

	if (ANavLinkProxy* const LinkProxy = Cast<ANavLinkProxy>(Actor))
	{
		const FTransform ProxyTransform = LinkProxy->GetActorTransform();
		for (const FNavigationLink& PointLink : LinkProxy->PointLinks)
		{
			NavMeshToolset::FillProxyPointLinkEntry(*LinkProxy, PointLink,
				ProxyTransform.TransformPosition(PointLink.Left), ProxyTransform.TransformPosition(PointLink.Right),
				Info.Links.AddDefaulted_GetRef());
		}
	}

	// Path following lives on the controller for pawns (AI controllers), or on the actor itself.
	UPathFollowingComponent* PathFollowing = Actor->FindComponentByClass<UPathFollowingComponent>();
	if (!PathFollowing)
	{
		const APawn* const Pawn = Cast<APawn>(Actor);
		if (Pawn)
		{
			const AController* const Controller = Pawn->GetController();
			if (Controller)
			{
				PathFollowing = Controller->FindComponentByClass<UPathFollowingComponent>();
			}
		}
	}

	if (PathFollowing)
	{
		Info.bHasPathFollowing = true;
		Info.PathFollowingStatus = PathFollowing->GetStatus();

		const FNavPathSharedPtr Path = PathFollowing->GetPath();
		if (Path.IsValid())
		{
			const TArray<FNavPathPoint>& PathPoints = Path->GetPathPoints();
			for (const FNavPathPoint& PathPoint : PathPoints)
			{
				if (Info.CurrentPathPoints.Num() >= NavMeshToolsetBudgets::MaxActorPathPoints)
				{
					Info.bCurrentPathPointsTruncated = true;
					break;
				}

				Info.CurrentPathPoints.Add(PathPoint.Location);
			}

			if (PathPoints.Num() > 0)
			{
				Info.MoveGoal = PathPoints.Last().Location;
			}
		}
	}

	return Info;
}

#endif
