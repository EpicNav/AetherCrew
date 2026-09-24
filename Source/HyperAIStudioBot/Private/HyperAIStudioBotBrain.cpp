// Games by Hyper 2026.

#include "HyperAIStudioBotBrain.h"

#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "InputAction.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"

namespace HyperAIStudio::Bot::Brain
{
	using namespace HyperAIStudio::Bot::Private;

	/** Explore counts the level in squares this big, in cm. */
	constexpr double CellCm = 400.0;
	/** Reach, Collect and Defeat give up on getting somewhere after this long without getting any closer. */
	constexpr double GiveUpSeconds = 8.0;
	constexpr double TrailSeconds = 0.2;
	constexpr int32 MaxTargets = 64;
	constexpr int32 MaxTrail = 3000;
	/** Everything the bot looks and steers by collides the way a pawn does. */
	constexpr ECollisionChannel Channel = ECC_Pawn;

	double Mix(const double A, const double B, const double T)
	{
		return A + (B - A) * T;
	}

	FIntPoint CellOf(const FVector& Point)
	{
		return FIntPoint(FMath::FloorToInt(Point.X / CellCm), FMath::FloorToInt(Point.Y / CellCm));
	}

	FString GoalName(const EHyperAIBotGoal Goal)
	{
		return StaticEnum<EHyperAIBotGoal>()->GetNameStringByValue(static_cast<int64>(Goal));
	}

	/** Where a person aims: the middle of what the actor collides with. */
	FVector AimPoint(const AActor& Actor)
	{
		FVector Origin;
		FVector Extent;
		Actor.GetActorBounds(/*bOnlyCollidingComponents=*/true, Origin, Extent);
		return Extent.IsNearlyZero() ? Actor.GetActorLocation() : Origin;
	}

	bool IsPartOf(const AActor* Hit, const AActor& Target)
	{
		return Hit && (Hit == &Target || Hit->IsAttachedTo(&Target) || Target.IsAttachedTo(Hit));
	}

	bool Alive(const AActor* Actor)
	{
		return IsValid(Actor) && !Actor->IsActorBeingDestroyed();
	}

	/** A button's press, a trigger pulled all the way, or a stick pushed forward. */
	FInputActionValue Pressed(const UInputAction& Action)
	{
		return InputValue(Action, FString());
	}

	FString Spots(const TArray<FVector>& Points)
	{
		TArray<FString> Parts;
		for (const FVector& Point : Points)
		{
			Parts.Add(Vec(Point));
		}
		return FString::Join(Parts, TEXT(", "));
	}
}

FHyperAIBotBrain::FHyperAIBotBrain(UWorld& InWorld, const FHyperAIBotStep& InStep, const FString& InMapPath)
	: World(&InWorld)
	, Step(InStep)
	, MapPath(InMapPath)
{
	Skill = FMath::Clamp(static_cast<double>(Step.Skill), 0.0, 1.0);
	// Each player plays its own way from the same seed.
	Random.Initialize(static_cast<int32>(static_cast<uint32>(Step.Seed) * 7919u + static_cast<uint32>(Step.PlayerIndex) * 104729u + 17u));
}

FHyperAIBotResult FHyperAIBotBrain::Begin()
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	if (!W)
	{
		return FResult::Failed(TEXT("world_unavailable"), TEXT("The game world has ended."));
	}
	APlayerController* Controller = PlayerController(*W, Step.PlayerIndex);
	APawn* Me = Controller ? Controller->GetPawn() : nullptr;
	if (!Me)
	{
		return FResult::Failed(TEXT("no_player"), Step.PlayerIndex == 0 ? FString(TEXT("There is no player pawn to play with."))
			: FString::Printf(TEXT("There is no Player%d with a pawn; AddPlayer adds a player."), Step.PlayerIndex));
	}
	Pawn = Me;
	Controls = ReadControls(*W, Step.PlayerIndex);
	Attack = Step.InputAction.IsEmpty() ? Controls.Get(ERole::Attack) : FindInputAction(Step.InputAction);
	if (!Step.InputAction.IsEmpty() && !Attack)
	{
		return FResult::Failed(TEXT("input_action_not_found"), FString::Printf(TEXT("No input action matches %s."), *Step.InputAction));
	}
	HealthPath = Step.HealthProperty.IsEmpty() ? FindHealth(*Me) : Step.HealthProperty;
	if (ReadHealth(Me, HealthPath, Health))
	{
		StartHealth = MinHealth = Health;
	}
	StartedAt = LastTickAt = Now = W->GetRealTimeSeconds();
	StartLocation = LastLocation = MovedFrom = Me->GetActorLocation();
	LastProgressAt = LastMovedAt = Now;
	Trail.Add(StartLocation);
	Visited.Add(CellOf(StartLocation));
	Phase = Random.FRandRange(0.0, 2.0 * UE_DOUBLE_PI);
	EngageRange = Step.Range > 0.f ? Step.Range : Step.Goal == EHyperAIBotGoal::Survive ? 2500.0 : 1500.0;
	if (Step.Goal == EHyperAIBotGoal::Follow && !Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex)))
	{
		return FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
	}
	switch (Step.Goal)
	{
	case EHyperAIBotGoal::Collect:
	case EHyperAIBotGoal::Defeat:
		RefreshTargets();
		if (Targets.IsEmpty())
		{
			return FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
		}
		if (Step.Goal == EHyperAIBotGoal::Defeat)
		{
			FString Code;
			FString Error;
			if (!InputFor(*W, Step.PlayerIndex, Code, Error))
			{
				return FResult::Failed(Code, Error);
			}
		}
		break;
	case EHyperAIBotGoal::Reach:
		if (!Step.Target.IsEmpty() && !Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex)))
		{
			return FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
		}
		break;
	case EHyperAIBotGoal::Survive:
	case EHyperAIBotGoal::Flee:
		RefreshTargets();
		break;
	default:
		break;
	}
	return FResult();
}

FHyperAIBotResult FHyperAIBotBrain::Tick()
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	if (!W)
	{
		return FResult::Failed(TEXT("world_unavailable"), TEXT("The game world has ended."));
	}
	Now = W->GetRealTimeSeconds();
	const double Dt = FMath::Clamp(Now - LastTickAt, 0.0, 0.25);
	LastTickAt = Now;
	if (bDirectJump)
	{
		if (ACharacter* Character = Cast<ACharacter>(Pawn.Get()))
		{
			Character->StopJumping();
		}
		bDirectJump = false;
	}
	APlayerController* Controller = PlayerController(*W, Step.PlayerIndex);
	APawn* Me = Controller ? Controller->GetPawn() : nullptr;
	// The pawn it started with is gone: killed, or out of the world.
	if (!Controller || !Alive(Me) || Me != Pawn.Get())
	{
		return Died(TEXT("died"), TEXT("its pawn is gone"));
	}
	// A game adds its mapping contexts as it starts, maybe after the first frame: read the controls again until they are there.
	if ((!Controls.Get(ERole::Move) || (Step.Goal == EHyperAIBotGoal::Defeat && !Attack)) && Now >= NextControlsAt && Now - StartedAt < 2.0)
	{
		Controls = ReadControls(*W, Step.PlayerIndex);
		Attack = Step.InputAction.IsEmpty() ? Controls.Get(ERole::Attack) : Attack;
		NextControlsAt = Now + 0.25;
	}
	const FVector Here = Me->GetActorLocation();
	Walked += FVector::Dist2D(Here, LastLocation);
	LastLocation = Here;
	if (FVector::Dist(Here, MovedFrom) > 40.0)
	{
		MovedFrom = Here;
		LastMovedAt = Now;
	}
	if (Now >= NextTrailAt && Trail.Num() < MaxTrail)
	{
		Trail.Add(Here);
		NextTrailAt = Now + TrailSeconds;
	}
	Visited.Add(CellOf(Here));
	if (const ACharacter* Character = Cast<ACharacter>(Me); Character && Character->GetCharacterMovement())
	{
		if (Character->GetCharacterMovement()->IsFalling())
		{
			FallFromZ = bFalling ? FMath::Max(FallFromZ, Here.Z) : Here.Z;
			bFalling = true;
		}
		else if (bFalling)
		{
			bFalling = false;
			if (FallFromZ - Here.Z > 500.0)
			{
				AddSpot(Falls, Here);
			}
		}
	}
	bHurt = false;
	double Read = 0.0;
	if (ReadHealth(Me, HealthPath, Read))
	{
		if (Health >= 0.0 && Read < Health)
		{
			DamageTaken += Health - Read;
			bHurt = true;
		}
		Health = Read;
		MinHealth = MinHealth < 0.0 ? Read : FMath::Min(MinHealth, Read);
		if (Read <= 0.0)
		{
			return Died(TEXT("died"), FString::Printf(TEXT("its %s reached %g"), *HealthPath, Read));
		}
	}
	if (Here.Z < StartLocation.Z - 10000.0)
	{
		return Died(TEXT("fell_out_of_world"), FString::Printf(TEXT("it fell to %s"), *Vec(Here)));
	}
	FResult Result;
	switch (Step.Goal)
	{
	case EHyperAIBotGoal::Reach:
		Result = TickReach(*Me, *Controller, Dt);
		break;
	case EHyperAIBotGoal::Collect:
		Result = TickCollect(*Me, *Controller, Dt);
		break;
	case EHyperAIBotGoal::Defeat:
		Result = TickDefeat(*Me, *Controller, Dt);
		break;
	case EHyperAIBotGoal::Survive:
		Result = TickSurvive(*Me, *Controller, Dt);
		break;
	case EHyperAIBotGoal::Explore:
		Result = TickExplore(*Me, *Controller, Dt);
		break;
	case EHyperAIBotGoal::Follow:
		Result = TickFollow(*Me, *Controller, Dt);
		break;
	case EHyperAIBotGoal::Flee:
		Result = TickFlee(*Me, *Controller, Dt);
		break;
	default:
		Result = FResult::Failed(TEXT("unsupported_goal"), TEXT("The bot has no play for this goal."));
		break;
	}
	const bool bTimed = Step.Goal == EHyperAIBotGoal::Reach || Step.Goal == EHyperAIBotGoal::Collect || Step.Goal == EHyperAIBotGoal::Defeat;
	if (!Result.IsDone() && bTimed && Now - StartedAt >= Step.TimeoutSeconds)
	{
		Result = FResult::Failed(TEXT("timed_out"), FString::Printf(TEXT("Player%d did not finish %s in %.1f s%s"),
			Step.PlayerIndex, *GoalName(Step.Goal), Now - StartedAt, *Remaining()));
	}
	return Result.IsDone() ? Finish(Result) : Result;
}

FHyperAIBotResult FHyperAIBotBrain::TickReach(APawn& Me, APlayerController& Controller, const double Dt)
{
	using namespace HyperAIStudio::Bot::Brain;
	FVector Goal = Step.Location;
	const AActor* Target = nullptr;
	if (!Step.Target.IsEmpty())
	{
		Target = Cast<AActor>(FindTarget(*World.Get(), Step.Target, Step.PlayerIndex));
		if (!Target)
		{
			return FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("%s is gone."), *Step.Target));
		}
		Goal = Target->GetActorLocation();
	}
	const FVector Here = Me.GetActorLocation();
	const double Distance = FVector::Dist2D(Here, Goal);
	if (Distance <= Step.Radius && FMath::Abs(Here.Z - Goal.Z) < 250.0)
	{
		return FResult::Passed(FString::Printf(TEXT("Player%d reached %s in %.1f s, %.0f cm away"),
			Step.PlayerIndex, Target ? *Describe(Target) : *Vec(Goal), Now - StartedAt, Distance));
	}
	if (!Travel(Me, Controller, Goal, Dt, /*bLookAhead=*/true, Target))
	{
		return FResult::Failed(TEXT("stuck"), FString::Printf(TEXT("Player%d got no closer to %s for %.0f s at %s, %.0f cm away, after backing off %d times"),
			Step.PlayerIndex, Target ? *Describe(Target) : *Vec(Goal), GiveUpSeconds, *Vec(Here), Distance, Recoveries));
	}
	return FResult();
}

FHyperAIBotResult FHyperAIBotBrain::TickCollect(APawn& Me, APlayerController& Controller, const double Dt)
{
	using namespace HyperAIStudio::Bot::Brain;
	int32 Unused = 0;
	if (Now >= NextRefreshAt && !IsPlayerName(Step.Target, Unused))
	{
		RefreshTargets();
	}
	for (FTarget& Target : Targets)
	{
		if (Target.DoneAt <= 0.0 && !Alive(Target.Actor.Get()))
		{
			Target.DoneAt = Now;
			++Collected;
		}
	}
	if (!Targets.IsValidIndex(Current) || Targets[Current].DoneAt > 0.0)
	{
		Current = NextTarget(Me.GetActorLocation(), /*bPreferSeen=*/false, nullptr, nullptr);
		ResetTravel();
	}
	if (Current == INDEX_NONE)
	{
		TArray<FString> Got;
		TArray<FString> Missed;
		for (const FTarget& Target : Targets)
		{
			if (Target.bSkipped)
			{
				Missed.Add(FString::Printf(TEXT("%s %s"), *Target.Name, *Target.Why));
			}
			else
			{
				Got.Add(FString::Printf(TEXT("%s at %.1f s"), *Target.Name, Target.DoneAt - StartedAt));
			}
		}
		const FString Summary = FString::Printf(TEXT("Player%d collected %d of %d: %s"), Step.PlayerIndex, Got.Num(), Targets.Num(), *FString::Join(Got, TEXT(", ")));
		return Missed.IsEmpty() ? FResult::Passed(Summary)
			: FResult::Failed(TEXT("not_collected"), Summary + TEXT("; not collected: ") + FString::Join(Missed, TEXT(", ")));
	}
	FTarget& Target = Targets[Current];
	AActor& Item = *Target.Actor.Get();
	const FVector Here = Me.GetActorLocation();
	const FVector There = Item.GetActorLocation();
	if (FVector::Dist2D(Here, There) <= 45.0 && FMath::Abs(Here.Z - There.Z) < 200.0)
	{
		// Standing on it and it is still there: the game may want the interact button.
		if (Target.ArrivedAt <= 0.0)
		{
			Target.ArrivedAt = Now;
		}
		const UInputAction* Use = Controls.Get(ERole::Interact);
		if (Use && !Target.bTriedInteract && Now - Target.ArrivedAt > 0.4)
		{
			Press(Use, Pressed(*Use));
			Target.bTriedInteract = true;
		}
		if (Now - Target.ArrivedAt > 1.5)
		{
			Target.bSkipped = true;
			Target.DoneAt = Now;
			Target.Why = Target.bTriedInteract && Use
				? FString::Printf(TEXT("stayed when the player stood on it and pressed %s"), *Use->GetName())
				: FString(TEXT("stayed when the player stood on it"));
		}
		Move(Me, Controller, (There - Here).GetSafeNormal2D(), 0.3f);
		return FResult();
	}
	Target.ArrivedAt = 0.0;
	if (!Travel(Me, Controller, There, Dt, /*bLookAhead=*/true, &Item))
	{
		Target.bSkipped = true;
		Target.DoneAt = Now;
		Target.Why = FString::Printf(TEXT("could not be reached: no closer than %.0f cm"), FVector::Dist2D(Here, There));
	}
	return FResult();
}

FHyperAIBotResult FHyperAIBotBrain::TickDefeat(APawn& Me, APlayerController& Controller, const double Dt)
{
	using namespace HyperAIStudio::Bot::Brain;
	if (!Attack)
	{
		return Now - StartedAt < 1.0 ? FResult() : FResult::Failed(TEXT("no_attack_action"), FString::Printf(
			TEXT("The player's controls have no action named like Fire, Shoot or Attack; pass inputAction. Controls: %s"), *Controls.Summary));
	}
	int32 Unused = 0;
	if (Now >= NextRefreshAt && !IsPlayerName(Step.Target, Unused))
	{
		RefreshTargets();
	}
	for (FTarget& Target : Targets)
	{
		if (Target.DoneAt > 0.0)
		{
			continue;
		}
		AActor* Actor = Target.Actor.Get();
		if (!Alive(Actor))
		{
			Target.DoneAt = Now;
			Target.Why = TEXT("gone");
			++Kills;
			continue;
		}
		double Read = 0.0;
		if (ReadHealth(Actor, Target.HealthPath, Read))
		{
			if (Target.Health >= 0.0 && Read < Target.Health)
			{
				DamageDealt += Target.Health - Read;
				Target.HitsSinceDamage = 0;
			}
			Target.Health = Read;
			if (Read <= 0.0)
			{
				Target.DoneAt = Now;
				Target.Why = TEXT("health 0");
				++Kills;
			}
		}
	}
	if (!Targets.IsValidIndex(Current) || Targets[Current].DoneAt > 0.0)
	{
		Current = NextTarget(Me.GetActorLocation(), /*bPreferSeen=*/true, &Me, &Controller);
		ResetTravel();
	}
	if (Current == INDEX_NONE)
	{
		TArray<FString> Done;
		for (const FTarget& Target : Targets)
		{
			Done.Add(FString::Printf(TEXT("%s at %.1f s (%s)"), *Target.Name, Target.DoneAt - StartedAt, *Target.Why));
		}
		return FResult::Passed(FString::Printf(TEXT("Player%d defeated %d of %d: %s"), Step.PlayerIndex, Kills, Targets.Num(), *FString::Join(Done, TEXT(", "))));
	}
	FTarget& Target = Targets[Current];
	AActor& Enemy = *Target.Actor.Get();
	const FVector Here = Me.GetActorLocation();
	const double Distance = FVector::Dist(Here, Enemy.GetActorLocation());
	const bool bSees = Sees(Me, Controller, Enemy);
	if (bSees)
	{
		Target.LastSeen = Enemy.GetActorLocation();
	}
	else
	{
		SeenAt = -1.0;
	}
	if (!bSees || Distance > EngageRange)
	{
		// Close in first, looking ahead; once near, turn to it, ready to shoot.
		const bool bNear = Distance < EngageRange * 1.5;
		if (bNear)
		{
			Aim(Controller, Enemy, Dt);
		}
		if (!Travel(Me, Controller, Enemy.GetActorLocation(), Dt, /*bLookAhead=*/!bNear, &Enemy))
		{
			return FResult::Failed(TEXT("stuck"), FString::Printf(TEXT("Player%d could not get to %s, %.0f cm away%s"),
				Step.PlayerIndex, *Target.Name, Distance, bSees ? TEXT(", though it could see it") : TEXT("")));
		}
		return FResult();
	}
	Aim(Controller, Enemy, Dt);
	if (SeenAt < 0.0)
	{
		// Seen again after losing sight of it: the aim has to find it again too.
		SeenAt = Now;
		AimError = FMath::Max(AimError, Mix(5.0, 1.5, Skill));
	}
	Maneuver(Me, Controller, Enemy, Distance);
	if (Now - SeenAt >= Reaction && Now >= NextShotAt)
	{
		bool bNear = false;
		const bool bOn = CrosshairOn(Controller, Enemy, bNear);
		// A person fires once the crosshair is on it, and now and then a moment early.
		if (bOn || (bNear && Random.FRand() < (1.0 - Skill) * 0.2))
		{
			Press(Attack, Pressed(*Attack));
			++Shots;
			if (ShotsFrom.Num() < 500)
			{
				ShotsFrom.Add(Here);
			}
			if (bOn)
			{
				++OnTarget;
				++Target.HitsSinceDamage;
			}
			NextShotAt = Now + Mix(0.42, 0.2, Skill) * Random.FRandRange(0.8, 1.25);
		}
	}
	// Hits that do nothing: try from closer, as a person would with a short-range weapon, then say so.
	if (Target.Health >= 0.0 && Target.HitsSinceDamage >= 6)
	{
		if (EngageRange > 250.0)
		{
			const double From = FMath::Min(Distance, EngageRange);
			EngageRange = FMath::Max(150.0, From * 0.3);
			Target.HitsSinceDamage = 0;
			Notes.Add(FString::Printf(TEXT("6 hits from %.0f cm did no damage, so it closed to %.0f cm"), From, EngageRange));
		}
		else
		{
			return FResult::Failed(TEXT("no_damage"), FString::Printf(TEXT("Player%d hit %s %d times with the crosshair on it from %.0f cm and its %s stayed %g"),
				Step.PlayerIndex, *Target.Name, Target.HitsSinceDamage, Distance, *Target.HealthPath, Target.Health));
		}
	}
	return FResult();
}

FHyperAIBotResult FHyperAIBotBrain::TickSurvive(APawn& Me, APlayerController& Controller, const double Dt)
{
	using namespace HyperAIStudio::Bot::Brain;
	if (Now - StartedAt >= Step.Seconds)
	{
		return FResult::Passed(FString::Printf(TEXT("Player%d survived %.1f s"), Step.PlayerIndex, Now - StartedAt));
	}
	int32 Unused = 0;
	if (!Step.Target.IsEmpty() && Now >= NextRefreshAt && !IsPlayerName(Step.Target, Unused))
	{
		RefreshTargets();
	}
	const FVector Here = Me.GetActorLocation();
	const AActor* Threat = nullptr;
	double Distance = TNumericLimits<double>::Max();
	for (const FTarget& Target : Targets)
	{
		const AActor* Actor = Target.Actor.Get();
		if (Alive(Actor) && FVector::Dist(Here, Actor->GetActorLocation()) < Distance)
		{
			Threat = Actor;
			Distance = FVector::Dist(Here, Actor->GetActorLocation());
		}
	}
	if (Threat)
	{
		ClosestThreat = ClosestThreat < 0.0 ? Distance : FMath::Min(ClosestThreat, Distance);
	}
	if (!Threat || Distance > EngageRange)
	{
		// Nothing close: keep moving, as a person does, rather than stand in the open.
		Wander(Me, Controller, Dt, /*bSeekNew=*/false);
		return FResult();
	}
	// Keep it in view, back away, and weave so its shots miss.
	Aim(Controller, *Threat, Dt);
	const FVector Away = (Here - Threat->GetActorLocation()).GetSafeNormal2D();
	const FVector Across = FVector::CrossProduct(FVector::UpVector, Away);
	if (Now >= StrafeUntil)
	{
		Strafe = Random.FRand() < 0.5f ? -1 : 1;
		StrafeUntil = Now + Random.FRandRange(0.35, 0.9);
	}
	const double Back = Distance < EngageRange * 0.7 ? 0.7 : 0.0;
	bool bJump = false;
	const FVector Heading = OpenHeading(Me, (Away * Back + Across * Strafe).GetSafeNormal(), Threat, bJump);
	if (bJump)
	{
		TryJump(Me);
	}
	Move(Me, Controller, Heading, 1.f);
	const UInputAction* Dodge = Controls.Get(ERole::Dodge);
	if (Dodge && Now >= NextDodgeAt && (bHurt || Distance < EngageRange * 0.3))
	{
		Press(Dodge, Pressed(*Dodge));
		++Dodges;
		NextDodgeAt = Now + 2.0;
	}
	if (Now >= NextHopAt)
	{
		if (Random.FRand() < 0.3f)
		{
			TryJump(Me);
		}
		NextHopAt = Now + Random.FRandRange(1.2, 3.0);
	}
	return FResult();
}

FHyperAIBotResult FHyperAIBotBrain::TickExplore(APawn& Me, APlayerController& Controller, const double Dt)
{
	using namespace HyperAIStudio::Bot::Brain;
	if (Now - StartedAt >= Step.Seconds)
	{
		return FResult::Passed(FString::Printf(TEXT("Player%d explored for %.1f s"), Step.PlayerIndex, Now - StartedAt));
	}
	Wander(Me, Controller, Dt, /*bSeekNew=*/true);
	return FResult();
}

FHyperAIBotResult FHyperAIBotBrain::Finish(FHyperAIBotResult Result)
{
	Stop();
	Result.Detail += TEXT("; ") + Stats();
	const FString Map = WriteMap();
	if (!Map.IsEmpty())
	{
		Result.File = Map;
	}
	return Result;
}

FHyperAIBotResult FHyperAIBotBrain::Died(const TCHAR* Code, const FString& How)
{
	using namespace HyperAIStudio::Bot::Brain;
	bDied = true;
	const bool bFight = Step.Goal == EHyperAIBotGoal::Defeat;
	const bool bKilled = FCString::Strcmp(Code, TEXT("died")) == 0;
	return Finish(FResult::Failed(bFight && bKilled ? TEXT("defeated") : Code, FString::Printf(TEXT("Player%d %s after %.1f s at %s: %s%s"),
		Step.PlayerIndex, bFight && bKilled ? TEXT("lost the fight") : TEXT("died"), Now - StartedAt, *Vec(LastLocation), *How, *Remaining())));
}

bool FHyperAIBotBrain::Travel(APawn& Me, APlayerController& Controller, const FVector& Goal, const double Dt, const bool bLookAhead, const AActor* Ignore)
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	const FVector Here = Me.GetActorLocation();
	if (Route.IsEmpty() || FVector::Dist2D(Goal, RouteGoal) > 150.0)
	{
		// A goal that moved a little keeps the progress made toward it.
		const bool bNewGoal = Route.IsEmpty() || FVector::Dist2D(Goal, RouteGoal) > 500.0;
		Route.Reset();
		NextPoint = 0;
		RouteGoal = Goal;
		if (W && FNavigationSystem::GetCurrent<UNavigationSystemV1>(W))
		{
			const UNavigationPath* Found = UNavigationSystemV1::FindPathToLocationSynchronously(W, Here, Goal, &Me);
			if (Found && Found->IsValid() && Found->PathPoints.Num() > 1)
			{
				Route = Found->PathPoints;
				Route.RemoveAt(0);
			}
		}
		if (Route.IsEmpty())
		{
			Route.Add(Goal);
		}
		if (bNewGoal)
		{
			BestDistance = FVector::Dist2D(Here, Goal);
			LastProgressAt = Now;
		}
	}
	Route.Last() = Goal;
	while (NextPoint < Route.Num() - 1 && FVector::Dist2D(Here, Route[NextPoint]) < 75.0)
	{
		++NextPoint;
	}
	const double Distance = FVector::Dist2D(Here, Goal);
	if (Distance < BestDistance - 10.0)
	{
		BestDistance = Distance;
		LastProgressAt = Now;
	}
	if (Now - LastProgressAt > GiveUpSeconds)
	{
		AddSpot(StuckSpots, Here);
		return false;
	}
	const FVector Desired = (Route[NextPoint] - Here).GetSafeNormal2D();
	if (Desired.IsNearlyZero())
	{
		return true;
	}
	// Not moving: a person hops first, then backs off and tries another way.
	const double Still = Now - LastMovedAt;
	if (Still > 0.8 && Now >= NextJumpAt)
	{
		TryJump(Me);
		NextJumpAt = Now + 1.5;
	}
	if (Still > 1.8 && Now > EscapeUntil + 1.0)
	{
		Escape = Desired.RotateAngleAxis((Random.FRand() < 0.5f ? -1.0 : 1.0) * Random.FRandRange(100.0, 160.0), FVector::UpVector);
		EscapeUntil = Now + 0.8;
		Side = -Side;
		++Recoveries;
		AddSpot(StuckSpots, Here);
	}
	FVector Heading = Desired;
	if (Now < EscapeUntil)
	{
		Heading = Escape;
	}
	else
	{
		bool bJump = false;
		Heading = OpenHeading(Me, Desired, Ignore, bJump);
		if (bJump)
		{
			TryJump(Me);
		}
	}
	Move(Me, Controller, Heading, 1.f);
	if (bLookAhead)
	{
		// A person looks where they are going, a little down at the ground ahead.
		FRotator View = Heading.Rotation();
		View.Pitch = -10.0;
		Turn(Controller, View, Dt);
	}
	return true;
}

void FHyperAIBotBrain::ResetTravel()
{
	Route.Reset();
	NextPoint = 0;
	Side = 0;
}

FVector FHyperAIBotBrain::OpenHeading(APawn& Me, const FVector& Desired, const AActor* Ignore, bool& bOutJump)
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	if (!W || Desired.IsNearlyZero())
	{
		return Desired;
	}
	float Radius = 34.f;
	float HalfHeight = 88.f;
	Me.GetSimpleCollisionCylinder(Radius, HalfHeight);
	const FVector Feet = Me.GetActorLocation() - FVector(0.0, 0.0, HalfHeight);
	const double Look = FMath::Clamp(Me.GetVelocity().Size2D() * 0.45, 90.0, 260.0) + Radius;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotSteer), false, &Me);
	if (Ignore)
	{
		Params.AddIgnoredActor(Ignore);
	}
	const FCollisionShape Probe = FCollisionShape::MakeSphere(Radius * 0.8f);
	auto Blocked = [&](const FVector& Direction, const double Height)
	{
		FHitResult Hit;
		const FVector From = Feet + FVector(0.0, 0.0, Height);
		// A slope it can walk up does not block it.
		return W->SweepSingleByChannel(Hit, From, From + Direction * Look, FQuat::Identity, Channel, Probe, Params) && Hit.ImpactNormal.Z < 0.7;
	};
	auto Floor = [&](const FVector& Direction)
	{
		FHitResult Hit;
		const FVector Ahead = Feet + Direction * Look;
		return W->LineTraceSingleByChannel(Hit, Ahead + FVector(0.0, 0.0, 60.0), Ahead - FVector(0.0, 0.0, 250.0), Channel, Params);
	};
	// The probe's bottom is about 30 cm up, above the floor, below a low wall; the chest's clears a jumped block.
	const double Knee = Radius * 0.8 + 30.0;
	const double Chest = HalfHeight * 1.5;
	const bool bKnee = Blocked(Desired, Knee);
	const bool bChest = Blocked(Desired, Chest);
	const bool bFloor = Floor(Desired);
	if (!bKnee && !bChest && bFloor)
	{
		ClearSince = ClearSince < 0.0 ? Now : ClearSince;
		if (Now - ClearSince > 0.6)
		{
			Side = 0;
		}
		return Desired;
	}
	ClearSince = -1.0;
	const ACharacter* Character = Cast<ACharacter>(&Me);
	if (bKnee && !bChest && bFloor && Character && Character->CanJump())
	{
		bOutJump = true;
		return Desired;
	}
	const TArray<double> Angles = {30.0, 55.0, 80.0, 105.0, 135.0};
	auto Open = [&](const double Angle, const int32 Sign, FVector& OutDirection)
	{
		OutDirection = Desired.RotateAngleAxis(Angle * Sign, FVector::UpVector);
		return !Blocked(OutDirection, Knee) && !Blocked(OutDirection, Chest) && Floor(OutDirection);
	};
	FVector Direction;
	if (Side == 0)
	{
		// Something new in the way: round it on the side that opens up soonest.
		const int32 First = Random.FRand() < 0.5f ? 1 : -1;
		for (const double Angle : Angles)
		{
			for (const int32 Sign : {First, -First})
			{
				if (Open(Angle, Sign, Direction))
				{
					++Detours;
					Side = Sign;
					return Direction;
				}
			}
		}
	}
	else
	{
		// Going round it: keep to the side chosen, so it does not dither, unless that side is shut.
		for (const int32 Sign : {Side, -Side})
		{
			for (const double Angle : Angles)
			{
				if (Open(Angle, Sign, Direction))
				{
					Side = Sign;
					return Direction;
				}
			}
		}
	}
	// Boxed in: back out the way it came.
	return -Desired;
}

void FHyperAIBotBrain::Maneuver(APawn& Me, APlayerController& Controller, const AActor& Enemy, const double Distance)
{
	using namespace HyperAIStudio::Bot::Brain;
	const FVector Here = Me.GetActorLocation();
	const FVector Toward = (Enemy.GetActorLocation() - Here).GetSafeNormal2D();
	const FVector Across = FVector::CrossProduct(FVector::UpVector, Toward);
	if (Now >= StrafeUntil)
	{
		// A person changes direction every second or so, and now and then stands to shoot.
		Strafe = Random.FRand() < 0.2f ? 0 : Random.FRand() < 0.5f ? -1 : 1;
		StrafeUntil = Now + Random.FRandRange(0.45, 1.3);
	}
	double Approach = Distance > EngageRange * 0.8 ? 1.0 : Distance < EngageRange * 0.4 && EngageRange > 300.0 ? -0.7 : 0.0;
	// Hurt badly: back off while still fighting, as a person gives ground to live.
	if (StartHealth > 0.0 && Health >= 0.0 && Health < StartHealth * 0.35)
	{
		Approach = -1.0;
		if (!bBackedOff)
		{
			bBackedOff = true;
			Notes.Add(FString::Printf(TEXT("backed off at %g health"), Health));
		}
	}
	const FVector Wanted = Toward * Approach + Across * (Strafe * Mix(0.6, 1.0, Skill));
	if (!Wanted.IsNearlyZero())
	{
		bool bJump = false;
		const FVector Heading = OpenHeading(Me, Wanted.GetSafeNormal(), &Enemy, bJump);
		if (bJump)
		{
			TryJump(Me);
		}
		Move(Me, Controller, Heading, 1.f);
	}
	if (Now >= NextHopAt)
	{
		if (Random.FRand() < Mix(0.1, 0.35, Skill))
		{
			TryJump(Me);
		}
		NextHopAt = Now + Random.FRandRange(1.5, 3.5);
	}
	const UInputAction* Dodge = Controls.Get(ERole::Dodge);
	if (bHurt && Dodge && Now >= NextDodgeAt)
	{
		Press(Dodge, Pressed(*Dodge));
		++Dodges;
		NextDodgeAt = Now + 1.5;
	}
}

FHyperAIBotResult FHyperAIBotBrain::TickFollow(APawn& Me, APlayerController& Controller, const double Dt)
{
	using namespace HyperAIStudio::Bot::Brain;
	const AActor* Leader = Cast<AActor>(FindTarget(*World.Get(), Step.Target, Step.PlayerIndex));
	if (!Alive(Leader))
	{
		return FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("%s is gone."), *Step.Target));
	}
	const FVector Here = Me.GetActorLocation();
	const double Distance = FVector::Dist2D(Here, Leader->GetActorLocation());
	// The first two seconds are for catching up; after that it counts how long it kept up.
	if (Now - StartedAt > 2.0)
	{
		FollowCounted += Dt;
		FollowWithin += Distance <= Step.Radius ? Dt : 0.0;
	}
	if (Now - StartedAt >= Step.Seconds)
	{
		const double Share = FollowCounted > 0.0 ? FollowWithin / FollowCounted : 1.0;
		const FString Summary = FString::Printf(TEXT("Player%d kept within %.0f cm of %s %.0f%% of the time"), Step.PlayerIndex, Step.Radius, *Describe(Leader), Share * 100.0);
		return Share >= 0.8 ? FResult::Passed(Summary) : FResult::Failed(TEXT("fell_behind"), Summary + TEXT(", less than 80%"));
	}
	if (Distance > Step.Radius * 0.7)
	{
		// Far behind, a person runs to catch up.
		if (Distance > Step.Radius * 2.0)
		{
			Sprint();
		}
		if (!Travel(Me, Controller, Leader->GetActorLocation(), Dt, /*bLookAhead=*/true, Leader))
		{
			return FResult::Failed(TEXT("stuck"), FString::Printf(TEXT("Player%d could not keep up with %s, %.0f cm away"), Step.PlayerIndex, *Describe(Leader), Distance));
		}
		return FResult();
	}
	// Close enough: wait, watching the leader.
	FRotator View = (Leader->GetActorLocation() - Here).Rotation();
	View.Pitch = -10.0;
	Turn(Controller, View, Dt);
	ResetTravel();
	return FResult();
}

FHyperAIBotResult FHyperAIBotBrain::TickFlee(APawn& Me, APlayerController& Controller, const double Dt)
{
	using namespace HyperAIStudio::Bot::Brain;
	int32 Unused = 0;
	if (Now >= NextRefreshAt && !IsPlayerName(Step.Target, Unused))
	{
		RefreshTargets();
	}
	const FVector Here = Me.GetActorLocation();
	const AActor* Threat = nullptr;
	double Distance = TNumericLimits<double>::Max();
	for (const FTarget& Target : Targets)
	{
		const AActor* Actor = Target.Actor.Get();
		if (Alive(Actor) && FVector::Dist2D(Here, Actor->GetActorLocation()) < Distance)
		{
			Threat = Actor;
			Distance = FVector::Dist2D(Here, Actor->GetActorLocation());
		}
	}
	if (Threat)
	{
		ClosestThreat = ClosestThreat < 0.0 ? Distance : FMath::Min(ClosestThreat, Distance);
	}
	if (Now - StartedAt >= Step.Seconds)
	{
		const FString Summary = Threat ? FString::Printf(TEXT("Player%d is %.0f cm from %s after %.1f s"), Step.PlayerIndex, Distance, *Describe(Threat), Now - StartedAt)
			: FString::Printf(TEXT("Player%d has nothing left to run from"), Step.PlayerIndex);
		return !Threat || Distance >= EngageRange ? FResult::Passed(Summary)
			: FResult::Failed(TEXT("caught"), Summary + FString::Printf(TEXT(", within %.0f"), EngageRange));
	}
	if (!Threat || Distance >= EngageRange * 1.1)
	{
		// Far enough: keep an eye on it.
		if (Threat)
		{
			FRotator View = (Threat->GetActorLocation() - Here).Rotation();
			View.Pitch = -5.0;
			Turn(Controller, View, Dt);
		}
		return FResult();
	}
	// Run: back turned, sprinting, weaving a little, round what is in the way.
	const FVector Away = (Here - Threat->GetActorLocation()).GetSafeNormal2D();
	if (Now >= StrafeUntil)
	{
		Strafe = Random.FRand() < 0.5f ? -1 : 1;
		StrafeUntil = Now + Random.FRandRange(0.5, 1.2);
	}
	const FVector Across = FVector::CrossProduct(FVector::UpVector, Away);
	bool bJump = false;
	const FVector Heading = OpenHeading(Me, (Away + Across * (0.25 * Strafe)).GetSafeNormal(), Threat, bJump);
	if (bJump)
	{
		TryJump(Me);
	}
	Sprint();
	Move(Me, Controller, Heading, 1.f);
	FRotator View = Heading.Rotation();
	View.Pitch = -10.0;
	Turn(Controller, View, Dt);
	return FResult();
}

void FHyperAIBotBrain::Sprint()
{
	using namespace HyperAIStudio::Bot::Brain;
	// Held by injecting it every frame, as holding the key would.
	if (const UInputAction* Action = Controls.Get(ERole::Sprint))
	{
		Press(Action, Pressed(*Action));
	}
}

void FHyperAIBotBrain::Wander(APawn& Me, APlayerController& Controller, const double Dt, const bool bSeekNew)
{
	const FVector Here = Me.GetActorLocation();
	if (!bHasDestination || FVector::Dist2D(Here, Destination) < 150.0 || Now - DestinationSince > 10.0)
	{
		PickDestination(Me, bSeekNew);
	}
	if (!Travel(Me, Controller, Destination, Dt, /*bLookAhead=*/true, nullptr))
	{
		PickDestination(Me, bSeekNew);
	}
}

void FHyperAIBotBrain::PickDestination(APawn& Me, const bool bSeekNew)
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	const FVector Here = Me.GetActorLocation();
	float Radius = 34.f;
	float HalfHeight = 88.f;
	Me.GetSimpleCollisionCylinder(Radius, HalfHeight);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotExplore), false, &Me);
	const double Facing = Me.GetActorRotation().Yaw;
	double Best = -TNumericLimits<double>::Max();
	FVector Choice = Here + FRotator(0.0, Random.FRandRange(0.0, 360.0), 0.0).Vector() * 400.0;
	for (int32 Try = 0; W && Try < 16; ++Try)
	{
		const double Yaw = Try * 22.5 + Random.FRandRange(-10.0, 10.0);
		const FVector Direction = FRotator(0.0, Yaw, 0.0).Vector();
		const double Far = Random.FRandRange(700.0, 1800.0);
		// As far as it can see that way, short of the wall.
		const FVector Eye = Here + FVector(0.0, 0.0, HalfHeight * 0.5);
		FHitResult Hit;
		const double Open = W->LineTraceSingleByChannel(Hit, Eye, Eye + Direction * Far, Channel, Params) ? FMath::Max(0.0, Hit.Distance - Radius * 2.0) : Far;
		if (Open < 300.0)
		{
			continue;
		}
		// Somewhere to stand at the end of it.
		const FVector Point = Here + Direction * Open;
		FHitResult Ground;
		if (!W->LineTraceSingleByChannel(Ground, Point + FVector(0.0, 0.0, 100.0), Point - FVector(0.0, 0.0, HalfHeight + 400.0), Channel, Params))
		{
			continue;
		}
		double Score = Random.FRand() * 0.6 + Open / 1800.0 * 0.4;
		if (bSeekNew)
		{
			// Squares it has not been to count most.
			for (double Along = CellCm; Along <= Open; Along += CellCm)
			{
				Score += Visited.Contains(CellOf(Here + Direction * Along)) ? 0.0 : 1.0;
			}
		}
		// Keep going roughly the way it faces, rather than turn round each time.
		Score -= FMath::Abs(FRotator::NormalizeAxis(Yaw - Facing)) / 180.0 * 0.5;
		if (Score > Best)
		{
			Best = Score;
			Choice = Ground.ImpactPoint + FVector(0.0, 0.0, HalfHeight);
		}
	}
	Destination = Choice;
	bHasDestination = true;
	DestinationSince = Now;
	ResetTravel();
}

void FHyperAIBotBrain::Move(APawn& Me, APlayerController& Controller, const FVector& Heading, const float Scale)
{
	using namespace HyperAIStudio::Bot::Brain;
	if (Heading.IsNearlyZero())
	{
		return;
	}
	const UInputAction* MoveAction = Controls.Get(ERole::Move);
	if (MoveAction && bMoveInputWorks)
	{
		// The stick, relative to where the camera faces, as a player pushes it.
		const FRotator Yaw(0.0, Controller.GetControlRotation().Yaw, 0.0);
		const FVector Forward = Yaw.Vector();
		const FVector Right = FRotationMatrix(Yaw).GetScaledAxis(EAxis::Y);
		Press(MoveAction, FInputActionValue(FVector2D(FVector::DotProduct(Heading, Right), FVector::DotProduct(Heading, Forward)) * Scale));
		// Once: a game can map a move action and move the pawn some other way.
		if (MoveCheckAt < 0.0)
		{
			MoveCheckAt = Now + 0.6;
			MoveCheckFrom = Me.GetActorLocation();
		}
		else if (MoveCheckAt > 0.0 && Now >= MoveCheckAt)
		{
			MoveCheckAt = 0.0;
			if (FVector::Dist2D(Me.GetActorLocation(), MoveCheckFrom) < 15.0)
			{
				bMoveInputWorks = false;
				Notes.Add(FString::Printf(TEXT("%s did not move the player, so it moved the pawn directly"), *MoveAction->GetName()));
			}
		}
		return;
	}
	Me.AddMovementInput(Heading, Scale);
}

void FHyperAIBotBrain::Turn(APlayerController& Controller, const FRotator& Desired, const double Dt)
{
	using namespace HyperAIStudio::Bot::Brain;
	const FRotator Facing = Controller.GetControlRotation();
	const FRotator Delta = (Desired - Facing).GetNormalized();
	// A person turns fast, then eases in, and never faster than a flick of the wrist.
	const double Ease = 1.0 - FMath::Exp(-Dt * Mix(7.0, 16.0, Skill));
	FRotator Change(Delta.Pitch * Ease, Delta.Yaw * Ease, 0.0);
	const double Largest = FMath::Max(FMath::Abs(Change.Yaw), FMath::Abs(Change.Pitch));
	const double Fastest = Mix(220.0, 620.0, Skill) * Dt;
	if (Largest > Fastest && Largest > 0.0)
	{
		Change = Change * (Fastest / Largest);
	}
	FRotator Next = (Facing + Change).GetNormalized();
	Next.Pitch = FMath::Clamp(Next.Pitch, -75.0, 75.0);
	Next.Roll = 0.0;
	Controller.SetControlRotation(Next);
}

void FHyperAIBotBrain::Acquire(const AActor& Target)
{
	using namespace HyperAIStudio::Bot::Brain;
	Aiming = &Target;
	SeenAt = -1.0;
	// A first aim at something new is off by a few degrees, less the better the player.
	AimError = Mix(10.0, 2.5, Skill) * Random.FRandRange(0.7, 1.3);
	ErrorAxis = Random.GetUnitVector();
	Reaction = Mix(0.45, 0.17, Skill) * Random.FRandRange(0.85, 1.2);
}

void FHyperAIBotBrain::Aim(APlayerController& Controller, const AActor& Target, const double Dt)
{
	using namespace HyperAIStudio::Bot::Brain;
	if (Aiming.Get() != &Target)
	{
		Acquire(Target);
	}
	FVector Camera;
	FRotator View;
	Controller.GetPlayerViewPoint(Camera, View);
	// The error shrinks as a person settles on the target; a little shake stays.
	AimError *= FMath::Exp(-Dt / Mix(0.5, 0.16, Skill));
	const double Shake = Mix(1.1, 0.2, Skill) * 0.5 * (FMath::Sin(Now * 2.3 + Phase) + FMath::Sin(Now * 3.7 + Phase * 1.7));
	const FVector Center = AimPoint(Target);
	const FVector Line = Center - Camera;
	const FVector Off = FVector::VectorPlaneProject(ErrorAxis, Line.GetSafeNormal()).GetSafeNormal();
	// A skilled player leads a moving target a little.
	const FVector Point = Center + Off * Line.Size() * FMath::Tan(FMath::DegreesToRadians(AimError + Shake)) + Target.GetVelocity() * Mix(0.0, 0.12, Skill);
	Turn(Controller, (Point - Camera).Rotation(), Dt);
}

bool FHyperAIBotBrain::Sees(const APawn& Me, const APlayerController& Controller, const AActor& Target) const
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	if (!W)
	{
		return false;
	}
	FVector Camera;
	FRotator View;
	Controller.GetPlayerViewPoint(Camera, View);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotSees), true, &Me);
	FHitResult Hit;
	return !W->LineTraceSingleByChannel(Hit, Camera, AimPoint(Target), Channel, Params) || IsPartOf(Hit.GetActor(), Target);
}

bool FHyperAIBotBrain::CrosshairOn(const APlayerController& Controller, const AActor& Target, bool& bOutNear) const
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	bOutNear = false;
	if (!W)
	{
		return false;
	}
	FVector Camera;
	FRotator View;
	Controller.GetPlayerViewPoint(Camera, View);
	// Shots go where the control rotation points; the camera can trail it by a frame.
	const FVector Direction = Controller.GetControlRotation().Vector();
	const FVector ToTarget = AimPoint(Target) - Camera;
	float Radius = 50.f;
	float HalfHeight = 50.f;
	Target.GetSimpleCollisionCylinder(Radius, HalfHeight);
	const double Angle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Direction, ToTarget.GetSafeNormal()), -1.0, 1.0)));
	const double Size = FMath::RadiansToDegrees(FMath::Atan2(FMath::Max(static_cast<double>(Radius), 20.0), FMath::Max(ToTarget.Size(), 1.0)));
	bOutNear = Angle <= Size * 1.8;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotCrosshair), true, Controller.GetPawn());
	FHitResult Hit;
	return W->LineTraceSingleByChannel(Hit, Camera, Camera + Direction * 100000.0, Channel, Params) && IsPartOf(Hit.GetActor(), Target);
}

void FHyperAIBotBrain::Press(const UInputAction* Action, const FInputActionValue& Value)
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	FString Code;
	FString Error;
	// Injected a frame at a time: not injecting it next frame lets go, as a released key does.
	if (UEnhancedInputLocalPlayerSubsystem* Input = W && Action ? InputFor(*W, Step.PlayerIndex, Code, Error) : nullptr)
	{
		Input->InjectInputForAction(Action, Value, {}, {});
	}
}

bool FHyperAIBotBrain::TryJump(APawn& Me)
{
	using namespace HyperAIStudio::Bot::Brain;
	ACharacter* Character = Cast<ACharacter>(&Me);
	if (!Character || !Character->CanJump() || Now < NextHopOk)
	{
		return false;
	}
	if (const UInputAction* JumpAction = Controls.Get(ERole::Jump))
	{
		Press(JumpAction, Pressed(*JumpAction));
	}
	else
	{
		Character->Jump();
		bDirectJump = true;
	}
	++Jumps;
	NextHopOk = Now + 0.7;
	return true;
}

void FHyperAIBotBrain::RefreshTargets()
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	NextRefreshAt = Now + 1.0;
	if (!W || Step.Target.IsEmpty())
	{
		return;
	}
	for (AActor* Actor : FindAll(*W, Step.Target, Step.PlayerIndex))
	{
		if (Actor == Pawn.Get() || Targets.Num() >= MaxTargets
			|| Targets.ContainsByPredicate([Actor](const FTarget& Known) { return Known.Actor.Get() == Actor; }))
		{
			continue;
		}
		FTarget& Target = Targets.AddDefaulted_GetRef();
		Target.Actor = Actor;
		Target.Name = Describe(Actor);
		Target.HealthPath = Step.HealthProperty.IsEmpty() ? FindHealth(*Actor) : Step.HealthProperty;
		if (!ReadHealth(Actor, Target.HealthPath, Target.Health))
		{
			Target.Health = -1.0;
		}
		Target.LastSeen = Actor->GetActorLocation();
	}
}

int32 FHyperAIBotBrain::NextTarget(const FVector& From, const bool bPreferSeen, const APawn* Me, const APlayerController* Controller) const
{
	using namespace HyperAIStudio::Bot::Brain;
	int32 Best = INDEX_NONE;
	double BestScore = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < Targets.Num(); ++Index)
	{
		const FTarget& Target = Targets[Index];
		const AActor* Actor = Target.Actor.Get();
		if (Target.DoneAt > 0.0 || !Alive(Actor))
		{
			continue;
		}
		double Score = FVector::Dist(From, Actor->GetActorLocation());
		// One it can see now beats a nearer one behind a wall.
		if (bPreferSeen && Me && Controller && !Sees(*Me, *Controller, *Actor))
		{
			Score += 5000.0;
		}
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Index;
		}
	}
	return Best;
}

bool FHyperAIBotBrain::ReadHealth(AActor* Actor, const FString& Path, double& OutHealth) const
{
	using namespace HyperAIStudio::Bot::Brain;
	if (!Actor || Path.IsEmpty())
	{
		return false;
	}
	const FReading Reading = ReadProperty(Actor, Path);
	if (!Reading.bOk || !Reading.bNumeric)
	{
		return false;
	}
	OutHealth = Reading.Number;
	return true;
}

void FHyperAIBotBrain::AddSpot(TArray<FVector>& Points, const FVector& Point)
{
	if (Points.Num() < 16 && !Points.ContainsByPredicate([&Point](const FVector& Known) { return FVector::Dist2D(Known, Point) < 300.0; }))
	{
		Points.Add(Point);
	}
}

FString FHyperAIBotBrain::Remaining() const
{
	TArray<FString> Parts;
	for (const FTarget& Target : Targets)
	{
		if (Target.DoneAt > 0.0)
		{
			continue;
		}
		if (Target.Health >= 0.0)
		{
			Parts.Add(FString::Printf(TEXT("%s (health %g)"), *Target.Name, Target.Health));
		}
		else
		{
			Parts.Add(Step.Goal == EHyperAIBotGoal::Defeat ? FString::Printf(TEXT("%s (health unread; name it with healthProperty)"), *Target.Name) : Target.Name);
		}
	}
	const bool bToDo = Step.Goal == EHyperAIBotGoal::Collect || Step.Goal == EHyperAIBotGoal::Defeat;
	return Parts.IsEmpty() || !bToDo ? FString() : TEXT("; still to do: ") + FString::Join(Parts, TEXT(", "));
}

FString FHyperAIBotBrain::Stats() const
{
	using namespace HyperAIStudio::Bot::Brain;
	TArray<FString> Parts;
	Parts.Add(FString::Printf(TEXT("walked %.0f m in %.1f s"), Walked / 100.0, Now - StartedAt));
	if (Shots > 0 || Step.Goal == EHyperAIBotGoal::Defeat)
	{
		Parts.Add(FString::Printf(TEXT("%d shots, %d with the crosshair on target (%d%%)"), Shots, OnTarget,
			Shots > 0 ? FMath::RoundToInt(100.0 * OnTarget / Shots) : 0));
	}
	if (DamageDealt > 0.0)
	{
		Parts.Add(FString::Printf(TEXT("dealt %g damage"), DamageDealt));
	}
	if (StartHealth >= 0.0)
	{
		Parts.Add(FString::Printf(TEXT("health %g of %g, took %g damage"), FMath::Max(Health, 0.0), StartHealth, DamageTaken));
	}
	else if (Step.Goal == EHyperAIBotGoal::Survive || Step.Goal == EHyperAIBotGoal::Defeat)
	{
		Parts.Add(TEXT("its own health unread; name it with healthProperty"));
	}
	if (Jumps > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d jumps"), Jumps));
	}
	if (Dodges > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d dodges"), Dodges));
	}
	if (Detours > 0)
	{
		Parts.Add(FString::Printf(TEXT("went round %d obstacles"), Detours));
	}
	if (Recoveries > 0)
	{
		Parts.Add(FString::Printf(TEXT("backed off %d times when stuck"), Recoveries));
	}
	if (ClosestThreat >= 0.0)
	{
		Parts.Add(FString::Printf(TEXT("closest threat %.0f cm"), ClosestThreat));
	}
	if (Step.Goal == EHyperAIBotGoal::Explore)
	{
		Parts.Add(FString::Printf(TEXT("visited %d squares of 4 m (%.0f m2)"), Visited.Num(), Visited.Num() * CellCm * CellCm / 10000.0));
	}
	if (const UInputAction* Run = Controls.Get(ERole::Sprint); Run && (Step.Goal == EHyperAIBotGoal::Follow || Step.Goal == EHyperAIBotGoal::Flee))
	{
		Parts.Add(TEXT("sprinted with ") + Run->GetName());
	}
	if (!StuckSpots.IsEmpty())
	{
		Parts.Add(TEXT("stuck at ") + Spots(StuckSpots));
	}
	if (!Falls.IsEmpty())
	{
		Parts.Add(TEXT("fell more than 5 m at ") + Spots(Falls));
	}
	TArray<FString> Used;
	for (const ERole Role : {ERole::Move, ERole::Jump, ERole::Dodge, ERole::Interact})
	{
		if (const UInputAction* Action = Controls.Get(Role))
		{
			Used.Add(Action->GetName());
		}
	}
	if (Attack)
	{
		Used.AddUnique(Attack->GetName());
	}
	Parts.Add(Used.IsEmpty() ? FString(TEXT("the game maps no input it knows, so it moved the pawn directly")) : TEXT("input ") + FString::Join(Used, TEXT(", ")));
	Parts.Append(Notes);
	return FString::Join(Parts, TEXT("; "));
}

FString FHyperAIBotBrain::Progress() const
{
	using namespace HyperAIStudio::Bot::Brain;
	TArray<FString> Parts;
	Parts.Add(FString::Printf(TEXT("Player%d %s, %.1f s in"), Step.PlayerIndex, *GoalName(Step.Goal), Now - StartedAt));
	if (Shots > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d shots, %d on target"), Shots, OnTarget));
	}
	if (DamageDealt > 0.0)
	{
		Parts.Add(FString::Printf(TEXT("dealt %g damage"), DamageDealt));
	}
	if (Health >= 0.0)
	{
		Parts.Add(FString::Printf(TEXT("health %g"), Health));
	}
	return FString::Join(Parts, TEXT(", "));
}

FString FHyperAIBotBrain::WriteMap() const
{
	using namespace HyperAIStudio::Bot::Brain;
	UWorld* W = World.Get();
	if (!W || MapPath.IsEmpty() || Trail.IsEmpty())
	{
		return FString();
	}
	// What to show: the trail, the targets, the goal and where it got stuck, with a margin.
	FBox Box(ForceInit);
	for (const FVector& Point : Trail)
	{
		Box += Point;
	}
	for (const FTarget& Target : Targets)
	{
		Box += Target.LastSeen;
	}
	for (const FVector& Spot : StuckSpots)
	{
		Box += Spot;
	}
	const bool bGoal = Step.Goal == EHyperAIBotGoal::Reach && !RouteGoal.IsZero();
	if (bGoal)
	{
		Box += RouteGoal;
	}
	const FVector Size = Box.GetSize();
	const double Span = FMath::Max3(Size.X, Size.Y, 1000.0) + 1200.0;
	const FVector Center = Box.GetCenter();
	const double Left = Center.X - Span * 0.5;
	const double Top = Center.Y - Span * 0.5;
	constexpr int32 Px = 512;
	constexpr int32 Cells = 128;
	constexpr int32 CellPx = Px / Cells;
	const double PxPerCm = Px / Span;
	float Radius = 34.f;
	float HalfHeight = 88.f;
	if (const APawn* Me = Pawn.Get())
	{
		Me->GetSimpleCollisionCylinder(Radius, HalfHeight);
	}
	const double FeetZ = StartLocation.Z - HalfHeight;
	TArray<FColor> Pixels;
	Pixels.Init(FColor(22, 24, 30), Px * Px);
	// The level from above, as it stands for a pawn: walls, the floor and how far down it is, and holes.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotMap), false);
	for (TActorIterator<APawn> It(W); It; ++It)
	{
		Params.AddIgnoredActor(*It);
	}
	const FCollisionShape Post = FCollisionShape::MakeBox(FVector(Span / Cells * 0.4, Span / Cells * 0.4, 30.0));
	for (int32 CellY = 0; CellY < Cells; ++CellY)
	{
		for (int32 CellX = 0; CellX < Cells; ++CellX)
		{
			const double X = Left + (CellX + 0.5) * Span / Cells;
			const double Y = Top + (CellY + 0.5) * Span / Cells;
			FColor Color(22, 24, 30);
			if (W->OverlapBlockingTestByChannel(FVector(X, Y, FeetZ + 120.0), FQuat::Identity, Channel, Post, Params))
			{
				Color = FColor(74, 80, 98);
			}
			else
			{
				FHitResult Floor;
				if (W->LineTraceSingleByChannel(Floor, FVector(X, Y, FeetZ + 90.0), FVector(X, Y, FeetZ - 1500.0), Channel, Params))
				{
					const double Drop = FMath::Clamp((FeetZ - Floor.ImpactPoint.Z) / 1500.0, -0.3, 1.0);
					const uint8 Shade = static_cast<uint8>(FMath::Clamp(214.0 - Drop * 120.0, 60.0, 240.0));
					Color = FColor(Shade, Shade, static_cast<uint8>(FMath::Min(255, Shade + 6)));
				}
			}
			for (int32 Row = 0; Row < CellPx; ++Row)
			{
				for (int32 Column = 0; Column < CellPx; ++Column)
				{
					Pixels[(CellY * CellPx + Row) * Px + CellX * CellPx + Column] = Color;
				}
			}
		}
	}
	// Grid lines every 10 m, for scale.
	auto Darker = [](const FColor& Pixel)
	{
		return FColor(static_cast<uint8>(Pixel.R * 0.85f), static_cast<uint8>(Pixel.G * 0.85f), static_cast<uint8>(Pixel.B * 0.85f));
	};
	for (double Line = FMath::CeilToDouble(Left / 1000.0) * 1000.0; Line < Left + Span; Line += 1000.0)
	{
		const int32 Column = FMath::FloorToInt((Line - Left) * PxPerCm);
		for (int32 Row = 0; Row < Px && Column >= 0 && Column < Px; ++Row)
		{
			Pixels[Row * Px + Column] = Darker(Pixels[Row * Px + Column]);
		}
	}
	for (double Line = FMath::CeilToDouble(Top / 1000.0) * 1000.0; Line < Top + Span; Line += 1000.0)
	{
		const int32 Row = FMath::FloorToInt((Line - Top) * PxPerCm);
		for (int32 Column = 0; Column < Px && Row >= 0 && Row < Px; ++Column)
		{
			Pixels[Row * Px + Column] = Darker(Pixels[Row * Px + Column]);
		}
	}
	auto ToPx = [&](const FVector& Point) { return FVector2D((Point.X - Left) * PxPerCm, (Point.Y - Top) * PxPerCm); };
	auto Dot = [&](const FVector2D& At, const double Size, const FColor& Color)
	{
		for (int32 Y = FMath::FloorToInt(At.Y - Size); Y <= FMath::CeilToInt(At.Y + Size); ++Y)
		{
			for (int32 X = FMath::FloorToInt(At.X - Size); X <= FMath::CeilToInt(At.X + Size); ++X)
			{
				if (X >= 0 && Y >= 0 && X < Px && Y < Px && FVector2D::DistSquared(FVector2D(X, Y), At) <= Size * Size)
				{
					Pixels[Y * Px + X] = Color;
				}
			}
		}
	};
	auto Ring = [&](const FVector2D& At, const double Size, const FColor& Color)
	{
		for (int32 Y = FMath::FloorToInt(At.Y - Size - 2); Y <= FMath::CeilToInt(At.Y + Size + 2); ++Y)
		{
			for (int32 X = FMath::FloorToInt(At.X - Size - 2); X <= FMath::CeilToInt(At.X + Size + 2); ++X)
			{
				if (X >= 0 && Y >= 0 && X < Px && Y < Px && FMath::Abs(FVector2D::Distance(FVector2D(X, Y), At) - Size) <= 1.2)
				{
					Pixels[Y * Px + X] = Color;
				}
			}
		}
	};
	auto Stroke = [&](const FVector2D& From, const FVector2D& To, const double Width, const FColor& Color)
	{
		const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector2D::Distance(From, To) * 2.0));
		for (int32 Index = 0; Index <= Steps; ++Index)
		{
			Dot(From + (To - From) * (static_cast<double>(Index) / Steps), Width, Color);
		}
	};
	auto Cross = [&](const FVector2D& At, const double Size, const FColor& Color)
	{
		Stroke(At + FVector2D(-Size, -Size), At + FVector2D(Size, Size), 1.3, Color);
		Stroke(At + FVector2D(-Size, Size), At + FVector2D(Size, -Size), 1.3, Color);
	};
	static const FColor PlayerColors[] = {FColor(40, 140, 255), FColor(255, 140, 30), FColor(60, 200, 120), FColor(200, 80, 220)};
	const FColor Mine = PlayerColors[FMath::Clamp(Step.PlayerIndex, 0, 3)];
	for (int32 Index = 1; Index < Trail.Num(); ++Index)
	{
		Stroke(ToPx(Trail[Index - 1]), ToPx(Trail[Index]), 1.6, Mine);
	}
	for (const FVector& From : ShotsFrom)
	{
		Dot(ToPx(From), 1.8, FColor(250, 70, 60));
	}
	for (const FTarget& Target : Targets)
	{
		const FVector2D At = ToPx(Target.LastSeen);
		if (Step.Goal == EHyperAIBotGoal::Collect)
		{
			if (Target.bSkipped)
			{
				Cross(At, 6.0, FColor(255, 150, 20));
			}
			else if (Target.DoneAt > 0.0)
			{
				Ring(At, 5.0, FColor(120, 120, 120));
			}
			else
			{
				Dot(At, 5.0, FColor(250, 210, 40));
			}
		}
		else if (Target.DoneAt > 0.0)
		{
			Dot(At, 6.0, FColor(140, 30, 30));
			Cross(At, 6.0, FColor(255, 255, 255));
		}
		else
		{
			Dot(At, 6.0, FColor(230, 50, 50));
		}
	}
	if (bGoal)
	{
		Ring(ToPx(RouteGoal), 9.0, FColor(40, 200, 90));
		Ring(ToPx(RouteGoal), 6.0, FColor(40, 200, 90));
	}
	for (const FVector& Spot : StuckSpots)
	{
		Ring(ToPx(Spot), 9.0, FColor(220, 60, 200));
	}
	for (const FVector& Spot : Falls)
	{
		Ring(ToPx(Spot), 7.0, FColor(60, 220, 230));
	}
	Dot(ToPx(Trail[0]), 6.0, FColor(20, 20, 20));
	Dot(ToPx(Trail[0]), 4.5, FColor(40, 200, 90));
	const FVector2D End = ToPx(LastLocation);
	if (bDied)
	{
		Dot(End, 7.0, FColor(230, 50, 50));
		Cross(End, 7.0, FColor(10, 10, 10));
	}
	else
	{
		Ring(End, 7.0, FColor(255, 255, 255));
		Dot(End, 3.0, Mine);
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Px, Px, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(MapPath), /*Tree=*/true);
	return !Png.IsEmpty() && FFileHelper::SaveArrayToFile(Png, *MapPath) ? FPaths::ConvertRelativePathToFull(MapPath) : FString();
}

void FHyperAIBotBrain::Stop()
{
	// Input is injected a frame at a time, so letting go is not injecting; only a direct jump needs ending.
	if (bDirectJump)
	{
		if (ACharacter* Character = Cast<ACharacter>(Pawn.Get()))
		{
			Character->StopJumping();
		}
		bDirectJump = false;
	}
}
