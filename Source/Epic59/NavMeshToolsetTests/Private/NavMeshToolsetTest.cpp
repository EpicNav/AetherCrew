// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbstractNavData.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "NavAreas/NavArea_Obstacle.h"
#include "NavigationSystem.h"
#include "NavMeshToolset.h"
#include "NavMeshToolsetTestEnvironment.h"
#include "AsyncFunctionResult/AsyncFunctionResultString.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_RECAST

BEGIN_DEFINE_SPEC(
	FNavMeshToolsetSpec,
	"AIAssistant.Toolsets.NavMeshToolset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	TUniquePtr<UE::ToolsetRegistry::FToolCallExceptionHandler> ExceptionHandler;
	TUniquePtr<FNavMeshToolsetTestEnvironment> Environment;

	// Ensure no blueprint script exceptions were caught and clear the handler.
	void ExpectNoException()
	{
		check(ExceptionHandler.IsValid());
		TestEqual(TEXT("Error"), *ExceptionHandler->GetException(), TEXT(""));
		ExceptionHandler.Reset();
	}

	// Ensure a blueprint script exception was caught containing the message, then clear the handler.
	void ExpectExceptionContains(const FString& ErrorMessage)
	{
		check(ExceptionHandler.IsValid());
		TestTrue(*FString::Printf(TEXT("Error contains '%s'"), *ErrorMessage),
			ExceptionHandler->GetException().Contains(ErrorMessage));
		ExceptionHandler.Reset();
	}

	void SetupEnvironment()
	{
		Environment = MakeUnique<FNavMeshToolsetTestEnvironment>();
		if (!Environment->Setup(*this))
		{
			Environment.Reset();
		}
	}

	// True when the environment is unavailable. Setup already recorded the failure as test
	// errors, so the test still FAILS; this guard only keeps the body from piling null-deref
	// noise on top of the real errors.
	bool SkipWithoutEnvironment()
	{
		if (Environment.IsValid())
		{
			return false;
		}

		AddInfo(TEXT("Environment setup failed (errors recorded above); skipping the test body."));
		ExceptionHandler.Reset();

		return true;
	}

END_DEFINE_SPEC(FNavMeshToolsetSpec)

void FNavMeshToolsetSpec::Define()
{
	BeforeEach([this]()
	{
		ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
	});

	AfterEach([this]()
	{
		if (ExceptionHandler.IsValid())
		{
			ExpectNoException();
		}

		Environment.Reset();
	});

	Describe(TEXT("Without a play world"), [this]()
	{
		It(TEXT("ListWorlds reports at least the editor world"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FNavMeshToolsetWorldInfo> Worlds = UNavMeshToolset::ListWorlds();
				TestTrue(TEXT("At least one world"), Worlds.Num() > 0);
			});
			ExpectNoException();
		});

		It(TEXT("GetActorNavInfo raises on a null actor"), [this]()
		{
			// RaiseToolError reports on BOTH channels, and under automation BOTH surface as error
			// events: the unconditional log line plus the script exception the framework records.
			// Exactly 2 (Plain: never regex-interpreted) pins each channel firing exactly once;
			// ExpectExceptionContains additionally pins that the script half reached the handler.
			AddExpectedErrorPlain(TEXT("Actor is null."), EAutomationExpectedMessageFlags::Contains, 2);
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				UNavMeshToolset::GetActorNavInfo(nullptr);
			});
			ExpectExceptionContains(TEXT("null"));
		});
	});

	Describe(TEXT("With a built navmesh"), [this]()
	{
		BeforeEach([this]()
		{
			SetupEnvironment();
		});

		It(TEXT("ListNavData reports the built recast navmesh"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FNavMeshToolsetNavDataSummary> NavData = UNavMeshToolset::ListNavData(Environment->World.Get());
				const bool bHasBuiltRecast = NavData.ContainsByPredicate([](const FNavMeshToolsetNavDataSummary& Summary)
				{
					return Summary.bIsRecastNavmesh && Summary.ActiveTileCount > 0;
				});
				TestTrue(TEXT("Has a built recast navmesh"), bHasBuiltRecast);
			});
			ExpectNoException();
		});

		It(TEXT("DescribeNavMesh dumps generation config"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FNavMeshToolsetNavMeshDetails Details =
					UNavMeshToolset::DescribeNavMesh(true, nullptr, NAME_None, Environment->World.Get());
				TestTrue(TEXT("TileSizeUU set"), Details.TileSizeUU > 0.f);
				TestEqual(TEXT("Three resolutions"), Details.Resolutions.Num(), 3);
				TestTrue(TEXT("Supported areas listed"), Details.SupportedAreas.Num() > 0);
				// Pins the string-keyed reflected reads: a rename otherwise stays green everywhere.
				TestEqual(TEXT("No unresolved properties"), Details.UnresolvedProperties.Num(), 0);
			});
			ExpectNoException();
		});

		It(TEXT("DescribeNavMesh raises on non-Recast navigation data"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			UNavigationSystemV1* const NavSys = Environment->NavSys.Get();
			if (!TestNotNull(TEXT("Nav system alive"), NavSys))
			{
				return;
			}

			AddExpectedErrorPlain(TEXT("not a Recast navmesh"), EAutomationExpectedMessageFlags::Contains, 2);
			ExceptionHandler->CaptureErrorsIn([this, NavSys]()
			{
				// The abstract navdata every navigation system carries is the always-available
				// non-Recast instance.
				UNavMeshToolset::DescribeNavMesh(
					false, NavSys->GetAbstractNavData(), NAME_None, Environment->World.Get());
			});
			ExpectExceptionContains(TEXT("not a Recast navmesh"));
		});

		It(TEXT("GetNavMeshBuildStatus raises on navigation data outside the NavDataSet"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			UNavigationSystemV1* const NavSys = Environment->NavSys.Get();
			if (!TestNotNull(TEXT("Nav system alive"), NavSys))
			{
				return;
			}

			AddExpectedErrorPlain(TEXT("no tracked build state"), EAutomationExpectedMessageFlags::Contains, 2);
			ExceptionHandler->CaptureErrorsIn([this, NavSys]()
			{
				// The abstract navdata lives in the world but is never added to NavDataSet.
				UNavMeshToolset::GetNavMeshBuildStatus(
					NavSys->GetAbstractNavData(), NAME_None, Environment->World.Get());
			});
			ExpectExceptionContains(TEXT("no tracked build state"));
		});

		It(TEXT("FindPath raises on a null area cost override class"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			AddExpectedErrorPlain(TEXT("null AreaClass"), EAutomationExpectedMessageFlags::Contains, 2);
			ExceptionHandler->CaptureErrorsIn([this]()
			{
				TArray<FNavMeshToolsetAreaCostOverride> Overrides;
				Overrides.AddDefaulted();
				UNavMeshToolset::FindPath(
					Environment->WalkableOrigin, Environment->WalkableOrigin + FVector(100.0, 0.0, 0.0),
					Overrides, {}, true, 0.f, 256, nullptr, NAME_None, Environment->World.Get());
			});
			ExpectExceptionContains(TEXT("null AreaClass"));
		});

		It(TEXT("ProjectPointToNavMesh projects the walkable origin"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FNavMeshToolsetProjectionResult> Projections = UNavMeshToolset::ProjectPointToNavMesh(
					Environment->WalkableOrigin, 0.f, 0.f, false, nullptr, NAME_None, Environment->World.Get());
				if (TestEqual(TEXT("One projection"), Projections.Num(), 1))
				{
					TestTrue(TEXT("Projected"), Projections[0].bProjected);
					TestFalse(TEXT("Has poly ref"), Projections[0].PolyRef.IsEmpty());
				}
			});
			ExpectNoException();
		});

		It(TEXT("FindPath succeeds across the floor"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FVector Start = Environment->WalkableOrigin + FVector(-800.f, -800.f, 0.f);
				const FVector End = Environment->WalkableOrigin + FVector(800.f, 800.f, 0.f);
				const FNavMeshToolsetPathResult Path = UNavMeshToolset::FindPath(
					Start, End, {}, {}, true, 0.f, 256, nullptr, NAME_None, Environment->World.Get());
				TestEqual(TEXT("Success"), Path.Result, TEnumAsByte<ENavigationQueryResult::Type>(ENavigationQueryResult::Success));
				TestTrue(TEXT("Has points"), Path.PathPoints.Num() >= 2);
				TestTrue(TEXT("Has length"), Path.PathLength > 0.f);
			});
			ExpectNoException();
		});

		It(TEXT("GetTileInfo reports layers at the origin"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FNavMeshToolsetTileInfo TileInfo = UNavMeshToolset::GetTileInfo(
					Environment->WalkableOrigin, nullptr, NAME_None, Environment->World.Get());
				TestTrue(TEXT("Tile is valid"), TileInfo.bValid);
				TestTrue(TEXT("Has layers"), TileInfo.Layers.Num() >= 1);
				if (TileInfo.Layers.Num() > 0)
				{
					TestTrue(TEXT("Layer has polys"), TileInfo.Layers[0].PolyCount > 0);
				}
			});
			ExpectNoException();
		});

		It(TEXT("QueryNavOctree finds the floor geometry"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FNavMeshToolsetOctreeQueryResult OctreeResult = UNavMeshToolset::QueryNavOctree(
					Environment->WalkableOrigin, 500.f, 64, Environment->World.Get());
				const bool bHasGeometry = OctreeResult.Elements.ContainsByPredicate(
					[](const FNavMeshToolsetOctreeElementInfo& Element)
				{
					return Element.bHasGeometry;
				});
				TestTrue(TEXT("Floor geometry registered"), bHasGeometry);
				TestFalse(TEXT("Not truncated"), OctreeResult.bTruncated);
			});
			ExpectNoException();
		});

		It(TEXT("ListNavBoundsVolumes reports the test volume"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const TArray<FNavMeshToolsetNavBoundsInfo> Bounds = UNavMeshToolset::ListNavBoundsVolumes(Environment->World.Get());
				TestTrue(TEXT("At least one bounds region"), Bounds.Num() >= 1);
			});
			ExpectNoException();
		});

		It(TEXT("Area cost overrides change the path cost"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			// An obstacle strip overhanging the floor on both sides forces the path to cross expensive area.
			Environment->SpawnAreaModifier(
				Environment->WalkableOrigin, FVector(150.f, 2600.f, 300.f), UNavArea_Obstacle::StaticClass());
			if (!TestTrue(TEXT("Rebuilt after modifier"), Environment->RebuildNavigation()))
			{
				ExceptionHandler.Reset();
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FVector Start = Environment->WalkableOrigin + FVector(-800.f, 0.f, 0.f);
				const FVector End = Environment->WalkableOrigin + FVector(800.f, 0.f, 0.f);

				const FNavMeshToolsetPathResult DefaultPath = UNavMeshToolset::FindPath(
					Start, End, {}, {}, true, 0.f, 256, nullptr, NAME_None, Environment->World.Get());

				TArray<FNavMeshToolsetAreaCostOverride> Overrides;
				FNavMeshToolsetAreaCostOverride& Override = Overrides.AddDefaulted_GetRef();
				Override.AreaClass = UNavArea_Obstacle::StaticClass();
				Override.Cost = 1.f;
				const FNavMeshToolsetPathResult CheapPath = UNavMeshToolset::FindPath(
					Start, End, Overrides, {}, true, 0.f, 256, nullptr, NAME_None, Environment->World.Get());

				TestEqual(TEXT("Default path succeeds"), DefaultPath.Result,
					TEnumAsByte<ENavigationQueryResult::Type>(ENavigationQueryResult::Success));
				TestEqual(TEXT("Overridden path succeeds"), CheapPath.Result,
					TEnumAsByte<ENavigationQueryResult::Type>(ENavigationQueryResult::Success));
				TestTrue(TEXT("Override lowers the cost substantially"), DefaultPath.PathCost > CheapPath.PathCost * 5.f);
			});
			ExpectNoException();
		});

		It(TEXT("ExecuteTool round-trips ListWorlds through the registry"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				// Toolsets register under "<ModuleName>.<ClassName>".
				UToolCallAsyncResultString* const Result = // UE 5.8: the 5.8 registry's result type
					UToolsetRegistry::ExecuteTool(TEXT("NavMeshToolset.NavMeshToolset"), TEXT("ListWorlds"), TEXT("{}"));
				if (TestNotNull(TEXT("Result object"), Result))
				{
					TestTrue(TEXT("Completed"), Result->bIsComplete);
					TestEqual(TEXT("No error"), Result->Error, FString());
					TestFalse(TEXT("Has value"), Result->Value.IsEmpty());
				}
			});
			ExpectNoException();
		});
	});

	Describe(TEXT("Deep query and truncation coverage"), [this]()
	{
		BeforeEach([this]()
		{
			SetupEnvironment();
		});

		It(TEXT("GetNavigationSystemInfo reports agents with no unresolved reads"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FNavMeshToolsetNavSystemInfo Info =
					UNavMeshToolset::GetNavigationSystemInfo(Environment->World.Get(), false);
				TestTrue(TEXT("Agents listed"), Info.AgentNames.Num() > 0);
				// UE 5.8: UNavigationSystemV1 has no bPreserveTilesOutsideInvokerRadius, so that read is unresolved.
				TArray<FName> Unresolved = Info.UnresolvedProperties;
				Unresolved.Remove(TEXT("bPreserveTilesOutsideInvokerRadius"));
				TestEqual(TEXT("No unresolved properties"), Unresolved.Num(), 0);
			});
			ExpectNoException();
		});

		It(TEXT("GetNavMeshBuildStatus adds tile detail when a navmesh is addressed"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				// The default call sweeps the world without tile detail.
				const FNavMeshToolsetBuildStatusReport Sweep =
					UNavMeshToolset::GetNavMeshBuildStatus(nullptr, NAME_None, Environment->World.Get());
				if (TestTrue(TEXT("Sweep reports navdata"), Sweep.PerNavData.Num() >= 1))
				{
					TestFalse(TEXT("Sweep omits tile detail"), Sweep.PerNavData[0].bTileDetailIncluded);
				}

				// Addressing the navmesh narrows the report to it and fills the tile detail.
				const FNavMeshToolsetBuildStatusReport Addressed = UNavMeshToolset::GetNavMeshBuildStatus(
					Environment->Navmesh.Get(), NAME_None, Environment->World.Get());
				if (TestEqual(TEXT("One addressed entry"), Addressed.PerNavData.Num(), 1))
				{
					const FNavMeshToolsetNavDataBuildStatus& Entry = Addressed.PerNavData[0];
					TestTrue(TEXT("Tile detail included"), Entry.bTileDetailIncluded);
					TestTrue(TEXT("Tiles exist"), Entry.ActiveTileCount > 0);
					TestTrue(TEXT("Bounds valid"), Entry.NavmeshBounds.IsValid != 0);
					// Pins the string-keyed reflected reads: a rename otherwise stays green everywhere.
					TestEqual(TEXT("No unresolved properties"), Entry.UnresolvedProperties.Num(), 0);
				}
			});
			ExpectNoException();
		});

		It(TEXT("DescribePolyAtLocation finds the floor poly"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FNavMeshToolsetPolyInfo Poly = UNavMeshToolset::DescribePolyAtLocation(
					Environment->WalkableOrigin, nullptr, NAME_None, Environment->World.Get());
				TestTrue(TEXT("Poly found"), Poly.bValid);
				TestTrue(TEXT("Area id assigned"), Poly.AreaID >= 0);
				TestFalse(TEXT("Has poly ref"), Poly.PolyRef.IsEmpty());
				TestTrue(TEXT("Owning tile resolved"), Poly.bTileResolved);
			});
			ExpectNoException();
		});

		It(TEXT("RaycastNavMesh stops at the floor edge"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				// The floor ends inside the nav bounds, so a ray across it must leave walkable space.
				const FNavMeshToolsetRaycastResult Raycast = UNavMeshToolset::RaycastNavMesh(
					Environment->WalkableOrigin,
					Environment->WalkableOrigin + FVector(Environment->FloorExtent.X * 2.f, 0.f, 0.f),
					nullptr, NAME_None, Environment->World.Get());
				TestTrue(TEXT("Ray hit the walkable edge"), Raycast.bHit);
				TestTrue(TEXT("Hit is before the requested end"),
					Raycast.HitLocation.X < Environment->WalkableOrigin.X + Environment->FloorExtent.X * 2.f);
			});
			ExpectNoException();
		});

		It(TEXT("GetReachableArea keeps counts exact and caps only centers"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				const FNavMeshToolsetReachableAreaResult Reachable = UNavMeshToolset::GetReachableArea(
					Environment->WalkableOrigin, 100000.f, 1, nullptr, NAME_None, Environment->World.Get());
				TestTrue(TEXT("More than one poly reachable"), Reachable.PolyCount > 1);
				TestTrue(TEXT("Surface area covers the whole set"), Reachable.TotalSurfaceArea > 0.f);
				TestEqual(TEXT("Centers capped at the requested one"), Reachable.PolyCenters.Num(), 1);
				TestTrue(TEXT("Center truncation reported"), Reachable.bPolyCentersTruncated);
			});
			ExpectNoException();
		});

		It(TEXT("ListNavModifiers reports truncation as a flag, never as a silent cap"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				Environment->SpawnAreaModifier(FVector(500.f, 500.f, 0.f), FVector(100.f, 100.f, 200.f), UNavArea_Obstacle::StaticClass());
				Environment->SpawnAreaModifier(FVector(-500.f, -500.f, 0.f), FVector(100.f, 100.f, 200.f), UNavArea_Obstacle::StaticClass());

				const FNavMeshToolsetModifierListResult Capped = UNavMeshToolset::ListNavModifiers(
					FVector(0.0, 0.0, 0.0), 0.f, 1, false, Environment->World.Get());
				TestEqual(TEXT("Cap respected"), Capped.Modifiers.Num(), 1);
				TestTrue(TEXT("Truncation reported"), Capped.bTruncated);

				const FNavMeshToolsetModifierListResult Full = UNavMeshToolset::ListNavModifiers(
					FVector(0.0, 0.0, 0.0), 0.f, 256, false, Environment->World.Get());
				TestTrue(TEXT("Both modifiers listed uncapped"), Full.Modifiers.Num() >= 2);
				TestFalse(TEXT("No truncation uncapped"), Full.bTruncated);
			});
			ExpectNoException();
		});

		// UE59: "ListNavInvokers lists raw records with the registered invokers" is not taken; 5.9 has no
		// UNavigationSystemV1::SetRawInvokers (//UE6/Main 59595023).

		It(TEXT("GetActorNavInfo reports a modifier volume actor as a volume"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				AActor* const Volume = Environment->SpawnAreaModifier(
					FVector(800.f, -800.f, 0.f), FVector(100.f, 100.f, 200.f), UNavArea_Obstacle::StaticClass());
				if (!TestNotNull(TEXT("Modifier volume spawned"), Volume))
				{
					return;
				}

				const FNavMeshToolsetActorNavInfo Info = UNavMeshToolset::GetActorNavInfo(Volume, false);
				if (TestEqual(TEXT("One modifier record"), Info.ModifierComponents.Num(), 1))
				{
					TestTrue(TEXT("Reported as a volume"), Info.ModifierComponents[0].bIsVolume);
				}

				TestEqual(TEXT("No unresolved properties"), Info.UnresolvedProperties.Num(), 0);
			});
			ExpectNoException();
		});

		It(TEXT("GetActorNavInfo resolves the invoker's reflected properties"), [this]()
		{
			if (SkipWithoutEnvironment())
			{
				return;
			}

			ExceptionHandler->CaptureErrorsIn([this]()
			{
				// A volume actor never enters the invoker branch, so only THIS actor exercises the
				// FStructProperty SupportedAgents read and the reflected Priority read; the
				// assertions below are the real reflection-drift tripwires for GetActorNavInfo.
				AActor* const InvokerActor = Environment->SpawnNavigationInvokerActor(Environment->WalkableOrigin);
				if (!TestNotNull(TEXT("Invoker actor spawned"), InvokerActor))
				{
					return;
				}

				const FNavMeshToolsetActorNavInfo Info = UNavMeshToolset::GetActorNavInfo(InvokerActor, false);
				if (TestEqual(TEXT("One invoker record"), Info.InvokerComponents.Num(), 1))
				{
					TestTrue(TEXT("Supported agents resolved and expanded"),
						Info.InvokerComponents[0].SupportedAgentNames.Num() > 0);
				}

				TestEqual(TEXT("No unresolved properties"), Info.UnresolvedProperties.Num(), 0);
			});
			ExpectNoException();
		});

	});

	Describe(TEXT("Cooked runtime subset"), [this]()
	{
		// AIRuntimeCallable is the only gate on which tools exist outside the editor, and a tool
		// silently losing the tag fails nothing else.
		It(TEXT("marks every tool runtime-callable"), [this]()
		{
			// Every query tool works against a cooked process; none is editor-only.
			const TSet<FName> ExpectedEditorOnly = {};

			int32 CallableToolCount = 0;
			TSet<FName> ObservedEditorOnly;
			// None: only tools declared on this class, not inherited UToolsetDefinition members.
			for (TFieldIterator<UFunction> FunctionIt(
					UNavMeshToolset::StaticClass(), EFieldIterationFlags::None); FunctionIt; ++FunctionIt)
			{
				UFunction* const Function = *FunctionIt;
				const TValueOrError<bool, FString> CallableResult =
					UToolsetDefinition::IsFunctionAICallable(Function);
				if (CallableResult.HasError() || !CallableResult.GetValue())
				{
					continue;
				}

				++CallableToolCount;

				if (!UE::ToolsetRegistry::IsFunctionAIRuntimeCallable(Function))
				{
					ObservedEditorOnly.Add(Function->GetFName());
				}
			}

			// Non-vacuity guard: with zero discovered tools every assertion below passes empty.
			TestTrue(TEXT("AI-callable tools discovered"), CallableToolCount > 0);

			TestEqual(TEXT("Editor-only tool count"), ObservedEditorOnly.Num(), ExpectedEditorOnly.Num());

			for (const FName& ToolName : ObservedEditorOnly.Difference(ExpectedEditorOnly))
			{
				AddError(FString::Printf(
					TEXT("Tool '%s' is not AIRuntimeCallable, so it will not exist outside the editor. ")
					TEXT("Add AIRuntimeCallable, or add it to ExpectedEditorOnly and document why in the class comment."),
					*ToolName.ToString()));
			}

			for (const FName& ToolName : ExpectedEditorOnly.Difference(ObservedEditorOnly))
			{
				AddError(FString::Printf(
					TEXT("Tool '%s' is expected to be editor-only but is tagged AIRuntimeCallable ")
					TEXT("(or no longer exists). Update ExpectedEditorOnly and the class comment together."),
					*ToolName.ToString()));
			}
		});
	});
}

#endif
