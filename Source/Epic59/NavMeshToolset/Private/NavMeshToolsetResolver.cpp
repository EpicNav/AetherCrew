// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_RECAST

#include "NavMeshToolsetResolver.h"

#include "AI/Navigation/NavAreaBase.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Navigation/NavLinkProxy.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavLinkCustomComponent.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavMeshToolsetModule.h"
#include "NavMeshToolsetTypes.h"
#include "NavModifierComponent.h"
#include "NavModifierVolume.h"
#include "UObject/UnrealType.h"

namespace NavMeshToolset
{
	namespace Private
	{
		/** Longest property value text kept by DumpNonDefaultProperties before truncation. */
		constexpr int32 MaxExportedValueLength = 512;

		FString DescribeSupportedAgents(const UNavigationSystemV1& NavSys)
		{
			TArray<FString> Names;
			for (const FNavDataConfig& AgentConfig : NavSys.GetSupportedAgents())
			{
				Names.Add(AgentConfig.Name.ToString());
			}

			return FString::Join(Names, TEXT(", "));
		}

		FString DescribeSupportedAreas(const ANavigationData& NavData)
		{
			TArray<FSupportedAreaData> Areas;
			NavData.GetSupportedAreas(Areas);

			TArray<FString> Names;
			for (const FSupportedAreaData& Area : Areas)
			{
				Names.Add(Area.AreaClassName);
			}

			return FString::Join(Names, TEXT(", "));
		}
	}

	void RaiseToolError(const FString& Message)
	{
		UE_LOGF(LogNavMeshToolset, Error, "%ls", *Message);
		UKismetSystemLibrary::RaiseScriptError(Message);
	}

	UWorld* ResolveWorld(UWorld* const World)
	{
		if (World)
		{
			const EWorldType::Type WorldType = World->WorldType;
			if (WorldType != EWorldType::Editor && WorldType != EWorldType::PIE && WorldType != EWorldType::Game)
			{
				RaiseToolError(FString::Printf(
					TEXT("World '%s' is not an Editor, PIE, or Game world and cannot be queried."), *World->GetName()));
				return nullptr;
			}

			return World;
		}

		if (!GEngine)
		{
			RaiseToolError(TEXT("No engine instance is available."));
			return nullptr;
		}

		UWorld* AuthorityPlayWorld = nullptr;
		UWorld* AnyPieWorld = nullptr;
		UWorld* AnyGameWorld = nullptr;
		UWorld* EditorWorld = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* const ContextWorld = Context.World();
			if (!ContextWorld)
			{
				continue;
			}

			switch (Context.WorldType)
			{
				case EWorldType::PIE:
					if (!AuthorityPlayWorld && ContextWorld->GetNetMode() != NM_Client)
					{
						AuthorityPlayWorld = ContextWorld;
					}

					if (!AnyPieWorld)
					{
						AnyPieWorld = ContextWorld;
					}
					break;

				case EWorldType::Game:
					if (!AuthorityPlayWorld && ContextWorld->GetNetMode() != NM_Client)
					{
						AuthorityPlayWorld = ContextWorld;
					}

					if (!AnyGameWorld)
					{
						AnyGameWorld = ContextWorld;
					}
					break;

				case EWorldType::Editor:
					if (!EditorWorld)
					{
						EditorWorld = ContextWorld;
					}
					break;

				default:
					break;
			}
		}

		UWorld* const Resolved = AuthorityPlayWorld ? AuthorityPlayWorld :
			AnyPieWorld ? AnyPieWorld :
			AnyGameWorld ? AnyGameWorld : EditorWorld;
		if (!Resolved)
		{
			RaiseToolError(
				TEXT("No Editor, PIE, or Game world is available. Run ListWorlds to see what exists."));
		}

		return Resolved;
	}

	UNavigationSystemV1* ResolveNavSystem(UWorld* const World)
	{
		UWorld* const ResolvedWorld = ResolveWorld(World);
		if (!ResolvedWorld)
		{
			return nullptr;
		}

		UNavigationSystemV1* const NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(ResolvedWorld);
		if (!NavSys)
		{
			RaiseToolError(FString::Printf(
				TEXT("World '%s' has no navigation system. Navigation may be disabled for this world type."),
				*ResolvedWorld->GetName()));
		}

		return NavSys;
	}

	ANavigationData* ResolveNavData(UNavigationSystemV1& NavSys, ANavigationData* const NavData, const FName AgentName)
	{
		if (NavData)
		{
			const UWorld* const NavSysWorld = NavSys.GetWorld();
			const UWorld* const NavDataWorld = NavData->GetWorld();
			if (NavDataWorld != NavSysWorld)
			{
				RaiseToolError(FString::Printf(
					TEXT("NavData '%s' belongs to world '%s', not the resolved world '%s'. Pass the matching World."),
					*NavData->GetName(),
					NavDataWorld ? *NavDataWorld->GetName() : TEXT("<none>"),
					NavSysWorld ? *NavSysWorld->GetName() : TEXT("<none>")));
				return nullptr;
			}

			return NavData;
		}

		if (!AgentName.IsNone())
		{
			ANavigationData* const AgentNavData = NavSys.GetNavDataForAgentName(AgentName);
			if (!AgentNavData)
			{
				RaiseToolError(FString::Printf(
					TEXT("No navigation data exists for agent '%s'. Supported agents: %s."),
					*AgentName.ToString(), *Private::DescribeSupportedAgents(NavSys)));
			}

			return AgentNavData;
		}

		ANavigationData* const MainNavData = NavSys.GetDefaultNavDataInstance();
		if (!MainNavData)
		{
			RaiseToolError(
				TEXT("The world has no navigation data. Add a NavMeshBoundsVolume or check ListNavData."));
		}

		return MainNavData;
	}

	ARecastNavMesh* ResolveRecastNavMesh(UNavigationSystemV1& NavSys, ANavigationData* const NavData, const FName AgentName)
	{
		ANavigationData* const Resolved = ResolveNavData(NavSys, NavData, AgentName);
		if (!Resolved)
		{
			return nullptr;
		}

		ARecastNavMesh* const RecastNavmesh = Cast<ARecastNavMesh>(Resolved);
		if (!RecastNavmesh)
		{
			RaiseToolError(FString::Printf(
				TEXT("Navigation data '%s' is a %s, not a Recast navmesh; tile and poly tools do not apply. ")
				TEXT("The generic tools (FindPath, ProjectPointToNavMesh, RaycastNavMesh) still work on it."),
				*Resolved->GetName(), *Resolved->GetClass()->GetName()));
		}

		return RecastNavmesh;
	}

	FSharedConstNavQueryFilter BuildQueryFilter(
		const ANavigationData& NavData,
		const TArray<FNavMeshToolsetAreaCostOverride>& CostOverrides,
		const TArray<TSubclassOf<UNavAreaBase>>& ExcludedAreas)
	{
		const FSharedConstNavQueryFilter DefaultFilter = NavData.GetDefaultQueryFilter();
		if (!DefaultFilter.IsValid())
		{
			RaiseToolError(FString::Printf(
				TEXT("Navigation data '%s' has no default query filter to derive from."), *NavData.GetName()));
			return nullptr;
		}

		if (CostOverrides.IsEmpty() && ExcludedAreas.IsEmpty())
		{
			return DefaultFilter;
		}

		const FSharedNavQueryFilter Filter = DefaultFilter->GetCopy();
		for (const FNavMeshToolsetAreaCostOverride& Override : CostOverrides)
		{
			const UClass* const AreaClass = Override.AreaClass.Get();
			if (!AreaClass)
			{
				RaiseToolError(TEXT("AreaCostOverrides contains a null AreaClass entry."));
				return nullptr;
			}

			const int32 AreaID = NavData.GetAreaID(AreaClass);
			if (AreaID == INDEX_NONE)
			{
				RaiseToolError(FString::Printf(
					TEXT("Area '%s' is not registered on navigation data '%s'. Supported areas: %s."),
					*AreaClass->GetName(), *NavData.GetName(), *Private::DescribeSupportedAreas(NavData)));
				return nullptr;
			}

			// dtQueryFilter's cost arrays are sized RECAST_MAX_AREAS and setAreaCost writes
			// unchecked, so anything past that bound is an out-of-bounds write, not a narrowing.
			if (AreaID < 0 || AreaID >= RECAST_MAX_AREAS)
			{
				RaiseToolError(FString::Printf(
					TEXT("Area '%s' has ID %d outside the filter range [0, %d) on navigation data '%s'."),
					*AreaClass->GetName(), AreaID, RECAST_MAX_AREAS, *NavData.GetName()));
				return nullptr;
			}

			Filter->SetAreaCost(static_cast<uint8>(AreaID), Override.Cost);
		}

		for (const TSubclassOf<UNavAreaBase>& ExcludedArea : ExcludedAreas)
		{
			const UClass* const AreaClass = ExcludedArea.Get();
			if (!AreaClass)
			{
				RaiseToolError(TEXT("ExcludedAreaClasses contains a null entry."));
				return nullptr;
			}

			const int32 AreaID = NavData.GetAreaID(AreaClass);
			if (AreaID == INDEX_NONE)
			{
				RaiseToolError(FString::Printf(
					TEXT("Area '%s' is not registered on navigation data '%s'. Supported areas: %s."),
					*AreaClass->GetName(), *NavData.GetName(), *Private::DescribeSupportedAreas(NavData)));
				return nullptr;
			}

			if (AreaID < 0 || AreaID >= RECAST_MAX_AREAS)
			{
				RaiseToolError(FString::Printf(
					TEXT("Area '%s' has ID %d outside the filter range [0, %d) on navigation data '%s'."),
					*AreaClass->GetName(), AreaID, RECAST_MAX_AREAS, *NavData.GetName()));
				return nullptr;
			}

			Filter->SetExcludedArea(static_cast<uint8>(AreaID));
		}

		return Filter;
	}

	void DumpNonDefaultProperties(const UObject& Object, TArray<FNavMeshToolsetReflectedProperty>& OutProperties)
	{
		const UClass* const Class = Object.GetClass();
		const UObject* const Defaults = Class->GetDefaultObject();
		for (TFieldIterator<FProperty> PropertyIterator(Class); PropertyIterator; ++PropertyIterator)
		{
			const FProperty* const Property = *PropertyIterator;
			if (Property->HasAnyPropertyFlags(CPF_Deprecated))
			{
				continue;
			}

			if (Property->Identical_InContainer(&Object, Defaults))
			{
				continue;
			}

			FString ValueText;
			Property->ExportTextItem_Direct(
				ValueText,
				Property->ContainerPtrToValuePtr<void>(&Object),
				Property->ContainerPtrToValuePtr<void>(Defaults),
				nullptr,
				PPF_None);
			if (ValueText.Len() > Private::MaxExportedValueLength)
			{
				ValueText.LeftInline(Private::MaxExportedValueLength);
				ValueText += TEXT("...");
			}

			FNavMeshToolsetReflectedProperty& Entry = OutProperties.AddDefaulted_GetRef();
			Entry.PropertyName = Property->GetFName();
			Entry.Value = MoveTemp(ValueText);
			Entry.DeclaringClass = Property->GetOwnerClass();
		}

		OutProperties.Sort([](const FNavMeshToolsetReflectedProperty& Left, const FNavMeshToolsetReflectedProperty& Right)
		{
			return Left.PropertyName.LexicalLess(Right.PropertyName);
		});
	}

	FName GetAgentNameForNavData(const ANavigationData& NavData)
	{
		return NavData.GetConfig().Name;
	}

	TArray<FName> GetAgentNamesFromSelector(const UNavigationSystemV1& NavSys, const FNavAgentSelector& Selector)
	{
		TArray<FName> Names;
		const TArray<FNavDataConfig>& SupportedAgents = NavSys.GetSupportedAgents();
		for (int32 AgentIndex = 0; AgentIndex < SupportedAgents.Num(); ++AgentIndex)
		{
			if (Selector.Contains(AgentIndex))
			{
				Names.Add(SupportedAgents[AgentIndex].Name);
			}
		}

		return Names;
	}

	FNavMeshToolsetNavDataSummary BuildNavDataSummary(const UNavigationSystemV1& NavSys, ANavigationData& NavData)
	{
		FNavMeshToolsetNavDataSummary Summary;
		Summary.NavData = &NavData;
		Summary.NavDataName = NavData.GetName();
		Summary.NavDataClass = NavData.GetClass();
		Summary.AgentName = GetAgentNameForNavData(NavData);
		Summary.RuntimeGeneration = NavData.GetRuntimeGenerationMode();
		Summary.bDrawingEnabled = NavData.IsDrawingEnabled();
		Summary.bRebuildingSuspended = NavData.IsRebuildingSuspended();
		Summary.NumSuspendedDirtyAreas = NavData.GetNumSuspendedDirtyAreas();
		Summary.bIsMainNavData = (NavSys.GetDefaultNavDataInstance() == &NavData);

		const ARecastNavMesh* const RecastNavmesh = Cast<ARecastNavMesh>(&NavData);
		if (RecastNavmesh)
		{
			Summary.bIsRecastNavmesh = true;
			Summary.TileCount = RecastNavmesh->GetNavMeshTilesCount();
			Summary.ActiveTileCount = RecastNavmesh->GetNumActiveTiles();
			Summary.NavmeshBounds = RecastNavmesh->GetNavMeshBounds();
		}

		return Summary;
	}

	void FillModifierEntry(UNavModifierComponent& Component, const FBox& Bounds, const bool bIncludeNonDefaultProperties, FNavMeshToolsetModifierInfo& OutEntry)
	{
		OutEntry.OwnerActor = Component.GetOwner();
		OutEntry.Component = &Component;
		OutEntry.bIsVolume = false;
		OutEntry.bNavigationRelevant = Component.IsNavigationRelevant();
		OutEntry.AreaClass = Component.AreaClass;
		OutEntry.AreaClassToReplace = Component.AreaClassToReplace;
		OutEntry.Bounds = Bounds;
		if (bIncludeNonDefaultProperties)
		{
			DumpNonDefaultProperties(Component, OutEntry.NonDefaultProperties);
		}
	}

	void FillModifierEntry(ANavModifierVolume& Volume, const FBox& Bounds, const bool bIncludeNonDefaultProperties, FNavMeshToolsetModifierInfo& OutEntry)
	{
		OutEntry.OwnerActor = &Volume;
		OutEntry.bIsVolume = true;
		OutEntry.bNavigationRelevant = Volume.IsNavigationRelevant();
		OutEntry.AreaClass = Volume.GetAreaClass();
		OutEntry.AreaClassToReplace = Volume.GetAreaClassToReplace();
		OutEntry.Bounds = Bounds;
		if (bIncludeNonDefaultProperties)
		{
			DumpNonDefaultProperties(Volume, OutEntry.NonDefaultProperties);
		}
	}

	void FillCustomLinkEntry(UNavLinkCustomComponent& LinkComponent, FNavMeshToolsetNavLinkInfo& OutEntry)
	{
		// UE59: 5.9 has no segment links on UNavLinkCustomComponent (UE6 59576255), so read the point link data
		FVector RelativeStart = FVector::ZeroVector;
		FVector RelativeEnd = FVector::ZeroVector;
		ENavLinkDirection::Type Direction = ENavLinkDirection::BothWays;
		LinkComponent.GetLinkData(RelativeStart, RelativeEnd, Direction);

		OutEntry.Kind = ENavMeshToolsetLinkKind::CustomLinkComponent;
		OutEntry.OwnerActor = LinkComponent.GetOwner();
		OutEntry.Component = &LinkComponent;
		OutEntry.Start = LinkComponent.GetStartPoint();
		OutEntry.End = LinkComponent.GetEndPoint();
		OutEntry.Direction = Direction;
		OutEntry.AreaClass = LinkComponent.GetEnabledArea();
		OutEntry.DisabledAreaClass = LinkComponent.GetDisabledArea();
		OutEntry.bEnabled = LinkComponent.IsEnabled();
		OutEntry.LinkId = PolyRefToHex(LinkComponent.GetId().GetId());
	}

	void FillProxyPointLinkEntry(ANavLinkProxy& Proxy, const FNavigationLink& PointLink, const FVector& Start, const FVector& End, FNavMeshToolsetNavLinkInfo& OutEntry)
	{
		OutEntry.Kind = ENavMeshToolsetLinkKind::ProxySimpleLink;
		OutEntry.OwnerActor = &Proxy;
		OutEntry.Start = Start;
		OutEntry.End = End;
		OutEntry.Direction = PointLink.Direction;
		OutEntry.AreaClass = PointLink.GetAreaClass();
		OutEntry.LinkId = PolyRefToHex(PointLink.NavLinkId.GetId());
	}

	void FillAdaptiveStats(const ARecastNavMesh& Navmesh, FNavMeshToolsetAdaptiveBuildStats& OutStats)
	{
		FAdaptiveNavMeshBuildStats EngineStats;
		if (!Navmesh.GetAdaptiveBuildStats(EngineStats))
		{
			OutStats.bAvailable = false;
			return;
		}

		OutStats.bAvailable = true;
		OutStats.GridTileCount = EngineStats.GridTileCount;
		OutStats.BuiltCount = EngineStats.BuiltCount;
		OutStats.NotBuiltCount = EngineStats.NotBuiltCount;
		OutStats.PendingCount = EngineStats.PendingCount;
		OutStats.BuildingCount = EngineStats.BuildingCount;
		OutStats.ActiveCount = EngineStats.ActiveCount;
		OutStats.PassiveCount = EngineStats.FrozenCount; // UE59: 5.9 predates the Frozen->Passive rename (UE6 55795088)
		OutStats.DeferredDirtyCount = EngineStats.DeferredDirtyCount;
		OutStats.bPreserveTiles = EngineStats.bPreserveTiles;
	}

	ENavMeshToolsetTileBuildState ConvertTileBuildState(const ENavMeshTileBuildState EngineState)
	{
		switch (EngineState)
		{
			case ENavMeshTileBuildState::NotBuilt:
				return ENavMeshToolsetTileBuildState::NotBuilt;

			case ENavMeshTileBuildState::Built:
				return ENavMeshToolsetTileBuildState::Built;

			case ENavMeshTileBuildState::Disabled:
				return ENavMeshToolsetTileBuildState::Disabled;

			case ENavMeshTileBuildState::OutOfExtent:
				return ENavMeshToolsetTileBuildState::OutOfExtent;

			default:
				return ENavMeshToolsetTileBuildState::Unknown;
		}
	}

	namespace
	{
		void ReportUnresolvedProperty(
			const UObject& Object, const FName PropertyName, TArray<FName>* const OutUnresolvedProperties)
		{
			if (OutUnresolvedProperties)
			{
				OutUnresolvedProperties->Add(PropertyName);
			}

			UE_LOGF(LogNavMeshToolset, Warning,
				"Reflected property '%ls' was not found (or has an unsupported type) on %ls; the fallback "
				"value is being reported.",
				*PropertyName.ToString(), *Object.GetClass()->GetName());
		}
	}

	int64 ReadReflectedEnumValue(
		const UObject& Object, const FName PropertyName, const int64 FallbackValue,
		TArray<FName>* const OutUnresolvedProperties)
	{
		const UClass* const Class = Object.GetClass();
		if (const FEnumProperty* const EnumProperty = FindFProperty<FEnumProperty>(Class, PropertyName))
		{
			const void* const ValuePtr = EnumProperty->ContainerPtrToValuePtr<void>(&Object);

			return EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
		}

		if (const FByteProperty* const ByteProperty = FindFProperty<FByteProperty>(Class, PropertyName))
		{
			return ByteProperty->GetPropertyValue_InContainer(&Object);
		}

		if (const FIntProperty* const IntProperty = FindFProperty<FIntProperty>(Class, PropertyName))
		{
			return IntProperty->GetPropertyValue_InContainer(&Object);
		}

		// A miss is engine drift, not data: surface it so "unknown" never masquerades as the fallback.
		ReportUnresolvedProperty(Object, PropertyName, OutUnresolvedProperties);

		return FallbackValue;
	}

	bool ReadReflectedBoolValue(
		const UObject& Object, const FName PropertyName, const bool bFallbackValue,
		TArray<FName>* const OutUnresolvedProperties)
	{
		const FBoolProperty* const BoolProperty = FindFProperty<FBoolProperty>(Object.GetClass(), PropertyName);
		if (!BoolProperty)
		{
			// A miss is engine drift, not data: surface it so "unknown" never masquerades as the fallback.
			ReportUnresolvedProperty(Object, PropertyName, OutUnresolvedProperties);

			return bFallbackValue;
		}

		return BoolProperty->GetPropertyValue_InContainer(&Object);
	}

	int32 ReadReflectedArrayNum(
		const UObject& Object, const FName PropertyName, TArray<FName>* const OutUnresolvedProperties)
	{
		const FArrayProperty* const ArrayProperty = FindFProperty<FArrayProperty>(Object.GetClass(), PropertyName);
		if (!ArrayProperty)
		{
			// A miss is engine drift, not data: surface it so "unknown" never masquerades as zero.
			ReportUnresolvedProperty(Object, PropertyName, OutUnresolvedProperties);

			return 0;
		}

		FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(&Object));

		return ArrayHelper.Num();
	}

	FString PolyRefToHex(const uint64 PolyRef)
	{
		return FString::Printf(TEXT("0x%016llX"), PolyRef);
	}
}

#endif
