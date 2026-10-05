// Games by Hyper 2026.

#include "HyperAIStudioSpatialPlan.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Brush.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/WorldSettings.h"
#include "HyperAIStudioSceneSpatialToolset.h"
#include "HyperAIStudioSpatialQueries.h"
#include "InstancedFoliageActor.h"
#include "LandscapeProxy.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioSceneSpatial"

namespace HyperAIStudio::SceneSpatial::Plan
{
	using C = FHyperAIStudioSceneSpatialContracts;

	namespace
	{
		constexpr float DefaultTraceDistance = 50000.f;
		constexpr float MaxTraceDistance = 200000.f;
		/** How far a move computed again at apply time may drift from the checked one before the plan stops. */
		constexpr double LocationTolerance = 1.0;
		constexpr double RotationToleranceDegrees = 0.5;
		/** Seconds every clear-spot search of one check may take together; a plan of many crowded actors stops there. */
		constexpr double ClearSpotBudgetSeconds = 15.0;
		/** Said of a search that budget stopped, so a recheck can tell running out of time from a level that changed. */
		const TCHAR* const SearchTimeRanOut = TEXT("the plan's search time ran out");

#if WITH_DEV_AUTOMATION_TESTS
		int32 ChecksRun = 0;
		TOptional<double> BudgetForTests;
#endif

		double ClearSpotBudget()
		{
#if WITH_DEV_AUTOMATION_TESTS
			if (BudgetForTests.IsSet()) return BudgetForTests.GetValue();
#endif
			return ClearSpotBudgetSeconds;
		}

		UStaticMesh* LoadMesh(const FString& Path)
		{
			const FString Trimmed = Path.TrimStartAndEnd();
			if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("/"))) return nullptr;
			const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
			return LoadObject<UStaticMesh>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		bool IsLabelTaken(UWorld& World, const FString& Label)
		{
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (IsValid(*It) && It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)) return true;
			}
			return false;
		}

		/** The actor and everything attached under it, which moves with it and must not be its ground. */
		void IgnoreWithChildren(const AActor& Actor, FCollisionQueryParams& Params)
		{
			Params.AddIgnoredActor(&Actor);
			TArray<AActor*> Children;
			Actor.GetAttachedActors(Children, /*bResetArray=*/true, /*bRecursivelyIncludeAttachedActors=*/true);
			for (const AActor* Child : Children)
			{
				Params.AddIgnoredActor(Child);
			}
		}

		AStaticMeshActor* Spawn(UWorld& World, const FMove& Move)
		{
			UStaticMesh* Mesh = Move.Mesh.Get();
			if (!Mesh) return nullptr;
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Move.To, Params);
			if (!Actor) return nullptr;
			// A registered static component refuses a new mesh, so it is set the way the mesh actor factory does.
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->UnregisterComponent();
			Component->SetStaticMesh(Mesh);
			Component->RegisterComponent();
			Actor->SetActorLabel(Move.Name);
			if (!Move.Folder.IsEmpty()) Actor->SetFolderPath(FName(*Move.Folder));
			return Actor;
		}
	}

	FString FMove::Describe() const
	{
		if (Kind == TEXT("place"))
		{
			const UStaticMesh* Placed = Mesh.Get();
			return FString::Printf(TEXT("%s (new, %s): at %s, rot %s, scale %s"), *Name, Placed ? *Placed->GetPathName() : TEXT("no mesh"),
				*Spatial::FormatVector(To.GetLocation()), *Spatial::FormatRotator(To.Rotator()), *Spatial::FormatVector(To.GetScale3D()));
		}
		return FString::Printf(TEXT("%s: %s -> %s, rot %s -> %s"), *Name, *Spatial::FormatVector(From.GetLocation()), *Spatial::FormatVector(To.GetLocation()),
			*Spatial::FormatRotator(From.Rotator()), *Spatial::FormatRotator(To.Rotator()));
	}

	bool Check(UWorld& World, const FHyperAISpatialPlanRequest& Request, TArray<FMove>& OutMoves, FHyperAISpatialPlanReport& Report)
	{
#if WITH_DEV_AUTOMATION_TESTS
		++ChecksRun;
#endif
		OutMoves.Reset();
		Report.Moves.Reset();
		Report.Notes.Reset();
		auto Refuse = [&Report](const TCHAR* Status, const FString& Why)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Why;
			return false;
		};
		const int32 Count = Request.Operations.Num();
		if (Count < 1 || Count > C::MaxOperations)
		{
			return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations));
		}
		// Each name is looked for among every loaded actor, so the lists are bounded before any is read.
		int32 Named = 0;
		for (const FHyperAISpatialOp& Op : Request.Operations)
		{
			Named += Op.Actors.Num();
			if (Op.IgnoreActors.Num() > C::MaxScopeActors)
			{
				return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("IgnoreActors holds at most %d actors per operation."), C::MaxScopeActors));
			}
		}
		if (Named > C::MaxPlanActors)
		{
			return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("The plan names %d actors; at most %d per plan."), Named, C::MaxPlanActors));
		}

		// Every name is resolved and every field checked before any move is computed, so a refusal names all it can.
		TArray<TArray<AActor*>> OpActors;
		TArray<UStaticMesh*> OpMeshes;
		TArray<ECollisionChannel> OpChannels;
		TArray<TArray<const AActor*>> OpIgnores;
		TArray<AActor*> PlanActors;
		TSet<FString> NewLabels;
		TArray<FString> Problems;
		TArray<FString> Locked;
		TArray<FString> Attached;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FHyperAISpatialOp& Op = Request.Operations[Index];
			const FString Kind = Op.Kind.TrimStartAndEnd().ToLower();
			const FString Where = FString::Printf(TEXT("Operation %d (%s): "), Index, *Op.Kind);
			OpActors.AddDefaulted();
			OpMeshes.Add(nullptr);
			OpChannels.Add(ECC_Visibility);
			OpIgnores.AddDefaulted();
			if (Kind != TEXT("drop_to_ground") && Kind != TEXT("move_to_clear_spot") && Kind != TEXT("place"))
			{
				return Refuse(TEXT("invalid_request"), Where + TEXT("Kind is drop_to_ground, move_to_clear_spot or place."));
			}
			FString Error;
			if (!Spatial::ResolveChannel(Op.Channel, ECC_Visibility, OpChannels[Index], Error))
			{
				return Refuse(TEXT("invalid_request"), Where + Error);
			}
			const FString Samples = Op.Samples.TrimStartAndEnd().ToLower();
			if (!Samples.IsEmpty() && Samples != TEXT("center") && Samples != TEXT("corners"))
			{
				return Refuse(TEXT("invalid_request"), Where + TEXT("Samples is center or corners."));
			}
			for (const FString& Name : Op.IgnoreActors)
			{
				const AActor* Ignored = Spatial::FindActor(World, Name, Error);
				if (!Ignored) return Refuse(TEXT("invalid_request"), Where + TEXT("IgnoreActors: ") + Error);
				OpIgnores[Index].Add(Ignored);
			}
			if (Kind == TEXT("place"))
			{
				if (!Op.Actors.IsEmpty())
				{
					return Refuse(TEXT("invalid_request"), Where + TEXT("place makes a new actor from Mesh; Actors is for drop_to_ground and move_to_clear_spot."));
				}
				OpMeshes[Index] = LoadMesh(Op.Mesh);
				if (!OpMeshes[Index])
				{
					return Refuse(TEXT("invalid_request"), Where + FString::Printf(TEXT("%s is not a static mesh asset; give its path, like /Game/Props/SM_Rock."), *Op.Mesh));
				}
				const FString Label = Op.Label.TrimStartAndEnd();
				if (Label.IsEmpty() || Label.Len() > 128)
				{
					return Refuse(TEXT("invalid_request"), Where + TEXT("place needs Label: the new actor's name in the Outliner, 1 to 128 characters."));
				}
				if (NewLabels.Contains(Label.ToLower()) || IsLabelTaken(World, Label))
				{
					return Refuse(TEXT("invalid_request"), Where + FString::Printf(TEXT("An actor is already labelled %s; give the new one a label of its own."), *Label));
				}
				NewLabels.Add(Label.ToLower());
				const FVector Scale = Op.Scale.IsNearlyZero() ? FVector::OneVector : Op.Scale;
				if (FMath::IsNearlyZero(Scale.X) || FMath::IsNearlyZero(Scale.Y) || FMath::IsNearlyZero(Scale.Z))
				{
					return Refuse(TEXT("invalid_request"), Where + TEXT("Scale has a zero axis; leave it zero for 1, or give all three."));
				}
				continue;
			}
			if (Op.Actors.IsEmpty())
			{
				return Refuse(TEXT("invalid_request"), Where + TEXT("Actors is empty: give the actors' labels or paths."));
			}
			for (const FString& Name : Op.Actors)
			{
				AActor* Actor = Spatial::FindActor(World, Name, Error);
				if (!Actor)
				{
					Problems.Add(Error);
					continue;
				}
				const FString Label = Spatial::LabelOf(*Actor);
				if (PlanActors.Contains(Actor))
				{
					Problems.Add(FString::Printf(TEXT("%s is named more than once; each actor moves once per plan."), *Label));
					continue;
				}
				if (Actor->IsA<ABrush>() || Actor->IsA<ALandscapeProxy>() || Actor->IsA<AWorldSettings>() || Actor->IsA<AInstancedFoliageActor>())
				{
					Problems.Add(Actor->IsA<AInstancedFoliageActor>()
						? FString::Printf(TEXT("%s holds foliage instances, which hyper_foliage_apply_plan moves."), *Label)
						: FString::Printf(TEXT("%s is a brush, volume, landscape or level setting, which this plan does not move."), *Label));
					continue;
				}
				if (Actor->IsLockLocation()) Locked.Add(Label);
				if (const AActor* Parent = Actor->GetAttachParentActor())
				{
					Attached.Add(FString::Printf(TEXT("%s is attached to %s"), *Label, *Spatial::LabelOf(*Parent)));
				}
				PlanActors.Add(Actor);
				OpActors[Index].Add(Actor);
			}
		}
		if (!Problems.IsEmpty()) return Refuse(TEXT("invalid_request"), FString::Join(Problems, TEXT(" ")));
		if (!Locked.IsEmpty())
		{
			return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("Locked against moving (Lock Actor Movement): %s. Unlock them, or leave them out."), *FString::Join(Locked, TEXT(", "))));
		}
		if (!Attached.IsEmpty())
		{
			return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("%s; move the parent, so the children keep their places on it."), *FString::Join(Attached, TEXT("; "))));
		}

		// The plan's own actors are not ground for each other, so the check and the apply always agree.
		FCollisionQueryParams Base(SCENE_QUERY_STAT(HyperAISpatialPlan), /*bTraceComplex=*/false);
		if (Request.bIgnoreEachOther)
		{
			for (const AActor* Actor : PlanActors) IgnoreWithChildren(*Actor, Base);
		}
		TOptional<FCollisionQueryParams> WithoutFoliage;
		TArray<FString> NoGround;
		TArray<FString> NoSpot;
		// Where the plan puts things. Placements and drops are worked out first, so no clear spot is found where one lands.
		TArray<FBox> Claimed;
		TArray<int32> Order;
		for (const bool bSearch : {false, true})
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if ((Request.Operations[Index].Kind.TrimStartAndEnd().ToLower() == TEXT("move_to_clear_spot")) == bSearch) Order.Add(Index);
			}
		}
		TArray<TArray<FMove>> OpMoves;
		OpMoves.SetNum(Count);
		const double SearchDeadline = FPlatformTime::Seconds() + ClearSpotBudget();
		for (const int32 Index : Order)
		{
			const FHyperAISpatialOp& Op = Request.Operations[Index];
			const FString Kind = Op.Kind.TrimStartAndEnd().ToLower();
			// Foliage and PCG instances are not ground unless the op says so: a rock dropped by a tree lands under it.
			if (!Op.bIncludeFoliage && !WithoutFoliage.IsSet())
			{
				WithoutFoliage.Emplace(Base);
				if (!Spatial::IgnoreFoliage(World, WithoutFoliage.GetValue()))
				{
					Report.Notes.Add(FString::Printf(TEXT("More than %d foliage and PCG actors and components; traces pass through only the first."), Spatial::MaxIgnored));
				}
			}
			FCollisionQueryParams Ground = Op.bIncludeFoliage ? Base : WithoutFoliage.GetValue();
			for (const AActor* Ignored : OpIgnores[Index]) Ground.AddIgnoredActor(Ignored);

			Spatial::FDropSettings Drop;
			Drop.Channel = OpChannels[Index];
			Drop.TraceDistance = Op.TraceDistance > 0.f ? FMath::Min(Op.TraceDistance, MaxTraceDistance) : DefaultTraceDistance;
			Drop.bCorners = Op.Samples.TrimStartAndEnd().ToLower() == TEXT("corners");
			Drop.bAlign = Op.bAlignToNormal;
			Drop.MaxAlignDegrees = Op.MaxAlignDegrees > 0.f ? FMath::Min(Op.MaxAlignDegrees, 89.f) : 30.f;
			Drop.Sink = Op.Sink;

			if (Kind == TEXT("place"))
			{
				FMove& Move = OpMoves[Index].AddDefaulted_GetRef();
				Move.Operation = Index;
				Move.Kind = Kind;
				Move.Name = Op.Label.TrimStartAndEnd();
				Move.Mesh = OpMeshes[Index];
				Move.Folder = Op.Folder.TrimStartAndEnd();
				Move.From = FTransform(Op.Rotation, Op.Location, Op.Scale.IsNearlyZero() ? FVector::OneVector : Op.Scale);
				Move.To = Move.From;
				if (Op.bDropToGround)
				{
					Spatial::FDropSubject Subject;
					Subject.LocalBox = OpMeshes[Index]->GetBoundingBox();
					Subject.Transform = Move.From;
					Drop.Params = Ground;
					if (!Spatial::PlanDrop(World, Subject, Drop, Move.To)) NoGround.Add(Move.Name);
				}
				Claimed.Add(OpMeshes[Index]->GetBoundingBox().TransformBy(Move.To));
				continue;
			}
			for (AActor* Actor : OpActors[Index])
			{
				FMove& Move = OpMoves[Index].AddDefaulted_GetRef();
				Move.Operation = Index;
				Move.Kind = Kind;
				Move.Actor = Actor;
				Move.Name = Spatial::LabelOf(*Actor);
				Move.From = Actor->GetActorTransform();
				Move.To = Move.From;
				FCollisionQueryParams Own = Ground;
				IgnoreWithChildren(*Actor, Own);
				const FBox Local = Spatial::LocalBounds(*Actor);
				if (Kind == TEXT("drop_to_ground"))
				{
					Spatial::FDropSubject Subject;
					Subject.LocalBox = Local;
					Subject.Transform = Move.From;
					Drop.Params = Own;
					if (!Spatial::PlanDrop(World, Subject, Drop, Move.To)) NoGround.Add(Move.Name);
					else if (Local.IsValid) Claimed.Add(Local.TransformBy(Move.To));
					continue;
				}

				// move_to_clear_spot: the actor's own box, turned with it when it stands upright, fitted ring by ring.
				const FRotator Rotation = Move.From.Rotator();
				const bool bUpright = FMath::Abs(Rotation.Pitch) < 1.0 && FMath::Abs(Rotation.Roll) < 1.0 && Local.IsValid;
				const FBox Bounds = Spatial::WorldBounds(*Actor);
				const FVector Center = bUpright ? Move.From.TransformPosition(Local.GetCenter()) : (Bounds.IsValid ? Bounds.GetCenter() : Move.From.GetLocation());
				const FVector Extent = bUpright ? Local.GetExtent() * Move.From.GetScale3D().GetAbs() : (Bounds.IsValid ? Bounds.GetExtent() : FVector(50.0));
				const double Bottom = Center.Z - Extent.Z;
				Spatial::FClearSpotSettings Spot;
				Spot.Point = Op.Near.IsNearlyZero() ? FVector(Center.X, Center.Y, Bottom) : Op.Near;
				Spot.Extent = Extent;
				Spot.Yaw = bUpright ? Rotation.Yaw : 0.0;
				Spot.Clearance = FMath::Max(Op.Clearance, 0.f);
				Spot.Step = FMath::Max(Op.Step, 0.f);
				Spot.MaxRadius = Op.MaxRadius > 0.f ? FMath::Min(Op.MaxRadius, 50000.f) : 2000.f;
				Spot.MaxSlope = FMath::Clamp(Op.MaxSlope, 0.f, 89.f);
				Spot.bSnapToGround = Op.bSnapToGround;
				Spot.Channel = ECC_WorldDynamic;
				Spot.GroundChannel = OpChannels[Index];
				// Foliage still stands in the way of a spot; it only stops being ground.
				Spot.Params = FCollisionQueryParams(SCENE_QUERY_STAT(HyperAISpatialPlan), /*bTraceComplex=*/false);
				IgnoreWithChildren(*Actor, Spot.Params);
				Spot.GroundParams = Own;
				Spot.IgnoreBounds.Add(Actor);
				Spot.Claimed = Claimed;
				Spot.Deadline = SearchDeadline;
				const Spatial::FClearSpot Found = Spatial::FindClearSpot(World, Spot);
				if (!Found.bFound)
				{
					NoSpot.Add(FString::Printf(TEXT("%s (%d spots tried: %s%s)"), *Move.Name, Found.Candidates, *Found.Rejected(),
						Found.bDeadline ? *FString::Printf(TEXT("; %s"), SearchTimeRanOut) : TEXT("")));
					continue;
				}
				// The box's middle onto the spot, and its bottom onto the spot's floor; the pivot keeps its offset.
				FVector Location = Move.From.GetLocation();
				Location.X += Found.Location.X - Center.X;
				Location.Y += Found.Location.Y - Center.Y;
				Location.Z += Found.Location.Z - Bottom;
				Move.To.SetLocation(Location);
				Claimed.Add(Found.Box);
			}
		}
		// Reported in the plan's order, whatever order they were worked out in.
		for (TArray<FMove>& Moves : OpMoves)
		{
			OutMoves.Append(MoveTemp(Moves));
		}
		if (!NoGround.IsEmpty())
		{
			return Refuse(TEXT("no_ground"), FString::Printf(TEXT("No ground below %s within the trace distance; is the landscape under it loaded? Nothing was changed."),
				*FString::Join(NoGround, TEXT(", "))));
		}
		if (!NoSpot.IsEmpty())
		{
			return Refuse(TEXT("no_clear_spot"), FString::Printf(TEXT("No clear spot for %s; raise MaxRadius, lower Clearance, or give Near. Nothing was changed."),
				*FString::Join(NoSpot, TEXT("; "))));
		}
		if (Spatial::IsLandscapePending(World))
		{
			Report.Notes.Add(TEXT("A landscape is still merging its edit layers, so its collision, and the ground found on it, may lag behind its shape."));
		}
		for (const FMove& Move : OutMoves)
		{
			Report.Moves.Add(Move.Describe());
		}
		return true;
	}

	FHyperAISpatialPlanReport Execute(UWorld& World, const FHyperAISpatialPlanRequest& Request, const TArray<FMove>& Checked, const FString& OperationId,
		const bool bRecheck)
	{
		FHyperAISpatialPlanReport Report;
		Report.OperationId = OperationId;
		auto Fail = [&Report](const FString& Why)
		{
			Report.bOk = false;
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Why;
			return Report;
		};
		// Computed again: another agent or the user may have changed the level while the plan awaited approval.
		TArray<FMove> Now;
		if (bRecheck)
		{
			FHyperAISpatialPlanReport Recheck;
			if (!Check(World, Request, Now, Recheck))
			{
				// The search running out of time again says nothing about the level, which may be as it was.
				if (Recheck.Status == TEXT("no_clear_spot") && Recheck.Diagnostic.Contains(SearchTimeRanOut))
				{
					return Fail(TEXT("The clear-spot search ran out of time when the plan was computed again before applying; nothing was changed. Ask for approval again, or narrow the plan (fewer actors to move, a smaller MaxRadius, or Near): ")
						+ Recheck.Diagnostic);
				}
				return Fail(TEXT("The level changed after the plan was checked, and the plan no longer passes: ") + Recheck.Diagnostic);
			}
			if (Now.Num() != Checked.Num())
			{
				return Fail(TEXT("The level changed after the plan was checked; nothing was changed. Check the plan again."));
			}
			for (int32 Index = 0; Index < Now.Num(); ++Index)
			{
				const FMove& Was = Checked[Index];
				const FMove& Is = Now[Index];
				const bool bSame = Was.Actor == Is.Actor && Was.Name == Is.Name
					&& Was.To.GetLocation().Equals(Is.To.GetLocation(), LocationTolerance)
					&& FMath::RadiansToDegrees(Was.To.GetRotation().AngularDistance(Is.To.GetRotation())) <= RotationToleranceDegrees
					&& Was.To.GetScale3D().Equals(Is.To.GetScale3D(), 0.001);
				if (!bSame)
				{
					return Fail(FString::Printf(TEXT("%s would now go to %s rather than the checked %s: the level changed after the check. Nothing was changed; check the plan again."),
						*Is.Name, *Spatial::FormatVector(Is.To.GetLocation()), *Spatial::FormatVector(Was.To.GetLocation())));
				}
			}
			Report.Notes = Recheck.Notes;
		}
		int32 Moved = 0;
		int32 Placed = 0;
		{
			const FScopedTransaction Transaction(LOCTEXT("SpatialPlan", "HyperAI: Spatial Placement"));
			for (const FMove& Move : bRecheck ? Now : Checked)
			{
				if (Move.Kind == TEXT("place"))
				{
					if (!Spawn(World, Move))
					{
						return Fail(FString::Printf(TEXT("%s could not be placed.%s"), *Move.Name,
							Report.Applied > 0 ? TEXT(" The moves before it stay applied; Edit > Undo takes the plan back.") : TEXT("")));
					}
					++Placed;
				}
				else
				{
					AActor* Actor = Move.Actor.Get();
					Actor->Modify();
					Actor->SetActorLocationAndRotation(Move.To.GetLocation(), Move.To.GetRotation(), /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
					Actor->PostEditMove(/*bFinished=*/true);
					++Moved;
				}
				++Report.Applied;
				Report.Moves.Add(Move.Describe());
			}
		}
		if (Request.bSave)
		{
			if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/false))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = TEXT("The level could not be saved; the moves are applied in memory, and Edit > Undo takes them back.");
				return Report;
			}
			Report.Saved.Add(World.GetOutermost()->GetName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d actor(s) moved and %d placed in one undo step%s."), Moved, Placed,
			Request.bSave ? TEXT(", and the level saved") : TEXT("; the level is unsaved"));
		return Report;
	}

#if WITH_DEV_AUTOMATION_TESTS
	int32 GetCheckCountForTests()
	{
		return ChecksRun;
	}

	void SetClearSpotBudgetForTests(const TOptional<double> Seconds)
	{
		BudgetForTests = Seconds;
	}
#endif
}

#undef LOCTEXT_NAMESPACE
