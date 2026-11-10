// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Math/Vector.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "Templates/SubclassOf.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class FAutomationTestBase;
class UNavArea;
class UWorld;

/**
 * Editor-only test world with one built navmesh, replicating the NavigationTestSuite recipe
 * (NavMeshTestWorld.cpp): new map, nav bounds volume with a cube brush, a BlockAll floor box,
 * build locks removed, and a polled NavSys build (bounded at 2000 build ticks; wall time scales
 * with tile generation, well past 20s on a slow build; can time out).
 */
class FNavMeshToolsetTestEnvironment
{
public:
	/** Builds the world and navmesh; false (with test errors recorded) when anything failed. */
	bool Setup(FAutomationTestBase& Test);

	/** Spawns a static BlockAll box (walkable roof, walls, obstacles); null when the world is
	 *  gone or the spawn failed. */
	AActor* SpawnBlockingBox(const FVector& Center, const FVector& Extent);

	/** Spawns a nav modifier volume with a cube brush covering Center plus/minus Extent; null
	 *  when the world is gone or the spawn failed. */
	AActor* SpawnAreaModifier(const FVector& Center, const FVector& Extent, TSubclassOf<UNavArea> AreaClass);

	/** Spawns an actor carrying a UNavigationInvokerComponent (exercises the reflected reads);
	 *  null when the world is gone or the spawn failed. */
	AActor* SpawnNavigationInvokerActor(const FVector& Center);

	/** Ticks the navigation system until the build finishes; false when the nav system is gone
	 *  or the build did not finish within the iteration budget. */
	bool RebuildNavigation();

	// Weak: this is a plain class with no UPROPERTY reachability, so if anything collects
	// garbage or tears the map down mid-test, these must read as null rather than dangle.
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UNavigationSystemV1> NavSys;
	TWeakObjectPtr<ARecastNavMesh> Navmesh;

	/** A point projected onto the built navmesh, safe to path from. */
	FVector WalkableOrigin = FVector::ZeroVector;

	/** Half-extents of the floor box Setup spawns centered at Z = -100. */
	FVector FloorExtent = FVector(2000.f, 2000.f, 50.f);
};
