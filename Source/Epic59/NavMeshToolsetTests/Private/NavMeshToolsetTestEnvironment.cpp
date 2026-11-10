// Copyright Epic Games, Inc. All Rights Reserved.

// Mirrors the specs' guard: FAutomationEditorCommonUtils only exists under automation tests.
#if WITH_DEV_AUTOMATION_TESTS && WITH_RECAST

#include "NavMeshToolsetTestEnvironment.h"

#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Components/BoxComponent.h"
#include "CoreGlobals.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "NavAreas/NavArea.h"
#include "NavigationInvokerComponent.h"
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavModifierVolume.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
	constexpr uint8 AllBuildLocks =
		ENavigationBuildLock::NoUpdateInEditor |
		ENavigationBuildLock::NoUpdateInPIE |
		ENavigationBuildLock::InitialLock |
		ENavigationBuildLock::Custom |
		ENavigationBuildLock::AsyncLoadLock;

	constexpr int32 MaxBuildIterations = 2000;
}

bool FNavMeshToolsetTestEnvironment::Setup(FAutomationTestBase& Test)
{
	// Locals for the pre-build phase (nothing here can free them); the weak members exist for
	// the entry points and for re-validation after the build wait.
	UWorld* const CreatedWorld = FAutomationEditorCommonUtils::CreateNewMap();
	World = CreatedWorld;
	if (!Test.TestNotNull(TEXT("Test map created"), CreatedWorld))
	{
		return false;
	}

	UNavigationSystemV1* const CreatedNavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(CreatedWorld);
	NavSys = CreatedNavSys;
	if (!Test.TestNotNull(TEXT("Navigation system exists"), CreatedNavSys))
	{
		return false;
	}

	const TArray<FNavDataConfig>& ProjectAgents = CreatedNavSys->GetSupportedAgents();
	if (!Test.TestTrue(TEXT("Project has at least one supported agent"), ProjectAgents.Num() > 0))
	{
		return false;
	}

	// One test agent keeps the build to a single navmesh even in projects with many agents.
	// OverrideSupportedAgents matches the requested configs against the CDO's agent array by
	// equivalence, so the override agent must come from the CDO: a live-instance config (or
	// the fallback agent injected when a project configures no agents) matches nothing and
	// the override would invalidate every agent. Projects with at most one configured agent
	// already build a single navmesh, so only multi-agent projects need the override.
	const UNavigationSystemV1* const NavSysCDO = CreatedNavSys->GetClass()->GetDefaultObject<UNavigationSystemV1>();
	const TArray<FNavDataConfig>& ConfiguredAgents = NavSysCDO->GetSupportedAgents();
	if (ConfiguredAgents.Num() > 1)
	{
		// The nav system is already world-initialized here, so the override logs one expected
		// warning; overriding through config instead would merge with game ini +SupportedAgents
		// lines. Register the warning so ElevateLogWarningsToErrors CI runs stay green AND the
		// framework notices if the engine ever stops (or doubles) warning. Plain, never regex:
		// the message text must match literally whatever characters it carries. Registered only
		// once the override is certain to run, or the unmatched expectation would itself fail
		// the test.
		Test.AddExpectedMessagePlain(
			TEXT("Trying to override NavigationSystem's SupportedAgents"),
			ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

		TArray<FNavDataConfig> SingleAgent;
		SingleAgent.Add(ConfiguredAgents[0]);
		CreatedNavSys->OverrideSupportedAgents(SingleAgent);
	}

	// Prove exactly one valid agent config survives setup instead of assuming, since everything
	// downstream expects exactly one navmesh. The override keeps the full agent array and
	// INVALIDATES the filtered-out configs, so count valid ones.
	int32 ValidAgentCount = 0;
	for (const FNavDataConfig& Agent : CreatedNavSys->GetSupportedAgents())
	{
		if (Agent.IsValid())
		{
			++ValidAgentCount;
		}
	}

	if (!Test.TestEqual(TEXT("Setup leaves exactly one valid agent config"), ValidAgentCount, 1))
	{
		return false;
	}

	// Nav bounds volume with a real cube brush (NavMeshTestWorld.cpp recipe).
	ANavMeshBoundsVolume* const BoundsVolume = CreatedWorld->SpawnActor<ANavMeshBoundsVolume>();
	if (!Test.TestNotNull(TEXT("Nav bounds volume spawned"), BoundsVolume))
	{
		return false;
	}

	UCubeBuilder* const CubeBuilder = NewObject<UCubeBuilder>();
	CubeBuilder->X = 5000.f;
	CubeBuilder->Y = 5000.f;
	CubeBuilder->Z = 2000.f;
	UActorFactory::CreateBrushForVolumeActor(BoundsVolume, CubeBuilder);

	if (!Test.TestNotNull(TEXT("Floor spawned"), SpawnBlockingBox(FVector(0.f, 0.f, -100.f), FloorExtent)))
	{
		return false;
	}

	CreatedNavSys->RemoveNavigationBuildLock(AllBuildLocks, UNavigationSystemV1::ELockRemovalRebuildAction::NoRebuild);

	if (!RebuildNavigation())
	{
		Test.AddError(TEXT("Navigation build did not complete."));
		return false;
	}

	// Re-resolve the weak members after the build wait.
	if (!Test.TestNotNull(TEXT("Test world alive after navigation build"), World.Get()))
	{
		return false;
	}

	UNavigationSystemV1* const LiveNavSys = NavSys.Get();
	if (!Test.TestNotNull(TEXT("Nav system alive after navigation build"), LiveNavSys))
	{
		return false;
	}

	// Resolve the navmesh through the nav system, not object-iteration order: the default navdata
	// is exactly the instance the toolset targets when a spec passes a null NavData.
	Navmesh = Cast<ARecastNavMesh>(LiveNavSys->GetDefaultNavDataInstance());

	if (!Test.TestNotNull(TEXT("Recast navmesh exists"), Navmesh.Get()))
	{
		return false;
	}

	if (!Test.TestTrue(TEXT("Navmesh has active tiles"), Navmesh->GetNumActiveTiles() > 0))
	{
		return false;
	}

	FNavLocation Projected;
	if (!Test.TestTrue(TEXT("Origin projects onto the navmesh"),
		Navmesh->ProjectPoint(FVector::ZeroVector, Projected, FVector(200.f, 200.f, 500.f))))
	{
		return false;
	}

	WalkableOrigin = Projected.Location;

	return true;
}

AActor* FNavMeshToolsetTestEnvironment::SpawnBlockingBox(const FVector& Center, const FVector& Extent)
{
	// The members are weak for a reason (see the header); every entry point re-validates.
	UWorld* const LocalWorld = World.Get();
	if (!LocalWorld)
	{
		return nullptr;
	}

	AActor* const Actor = LocalWorld->SpawnActor<AActor>();
	if (!Actor)
	{
		return nullptr;
	}

	UBoxComponent* const Box = NewObject<UBoxComponent>(Actor);
	Box->SetBoxExtent(Extent);
	Box->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	Box->SetMobility(EComponentMobility::Static);
	Actor->SetRootComponent(Box);
	Box->SetWorldLocation(Center);
	Box->RegisterComponent();

	return Actor;
}

AActor* FNavMeshToolsetTestEnvironment::SpawnAreaModifier(
	const FVector& Center, const FVector& Extent, const TSubclassOf<UNavArea> AreaClass)
{
	UWorld* const LocalWorld = World.Get();
	if (!LocalWorld)
	{
		return nullptr;
	}

	// UNavModifierComponent ignores collision-less primitives (its bounds fall back to a tiny
	// failsafe box), so use a real modifier volume with a cube brush, like the bounds volume.
	ANavModifierVolume* const Volume = LocalWorld->SpawnActor<ANavModifierVolume>(Center, FRotator::ZeroRotator);
	if (!Volume)
	{
		return nullptr;
	}

	UCubeBuilder* const CubeBuilder = NewObject<UCubeBuilder>();
	CubeBuilder->X = Extent.X * 2.f;
	CubeBuilder->Y = Extent.Y * 2.f;
	CubeBuilder->Z = Extent.Z * 2.f;
	UActorFactory::CreateBrushForVolumeActor(Volume, CubeBuilder);

	Volume->SetAreaClass(AreaClass);

	return Volume;
}

AActor* FNavMeshToolsetTestEnvironment::SpawnNavigationInvokerActor(const FVector& Center)
{
	UWorld* const LocalWorld = World.Get();
	if (!LocalWorld)
	{
		return nullptr;
	}

	AActor* const Actor = LocalWorld->SpawnActor<AActor>();
	if (!Actor)
	{
		return nullptr;
	}

	USceneComponent* const Root = NewObject<USceneComponent>(Actor);
	Actor->SetRootComponent(Root);
	Root->SetWorldLocation(Center);
	Root->RegisterComponent();

	UNavigationInvokerComponent* const Invoker = NewObject<UNavigationInvokerComponent>(Actor);
	Invoker->RegisterComponent();

	return Actor;
}

bool FNavMeshToolsetTestEnvironment::RebuildNavigation()
{
	UNavigationSystemV1* NavSysPtr = NavSys.Get();
	if (!NavSysPtr)
	{
		return false;
	}

	// GFrameCounter must advance per Tick: the generator's time-slicing keys "did work this
	// frame" off it, so without the increment the build loop below spins forever on one slice.
	++GFrameCounter;
	NavSysPtr->Tick(0.016f);
	NavSysPtr->Build();

	// Tick at least once between Build() and the first completion check, so a build whose tasks
	// are not yet submitted cannot read as already finished. The weak pointer re-resolves every
	// iteration so a mid-wait teardown reads as null. A distinct timeout flag keeps a build that
	// finishes on the final iteration from reporting as a timeout.
	int32 Iterations = 0;
	bool bTimedOut = false;
	while (true)
	{
		NavSysPtr = NavSys.Get();
		if (!NavSysPtr)
		{
			return false;
		}

		++GFrameCounter;
		NavSysPtr->Tick(0.016f);

		if (!NavSysPtr->IsNavigationBuildInProgress())
		{
			break;
		}

		if (Iterations >= MaxBuildIterations)
		{
			bTimedOut = true;
			break;
		}

		FPlatformProcess::Sleep(0.01f);
		++Iterations;
	}

	return !bTimedOut;
}

#endif
