// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioBot.h"
#include "HyperAIStudioBotShared.h"
#include "InputActionValue.h"
#include "Math/RandomStream.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class APawn;
class APlayerController;

/**
 * One Play step for one player, a frame at a time, played the way a person plays. It reads the game's controls and
 * moves with the game's own move input relative to the camera, looking where it goes. It steers round what blocks the
 * way, jumps what it can, and backs off and tries another way when stuck. It fights with a person's reaction time,
 * turn speed and an aim error that settles, pressing the attack when the crosshair is on the target, strafing, hopping
 * and dodging meanwhile. It keeps count of what happened and writes a map of it.
 */
class FHyperAIBotBrain
{
public:
	/** The map of the play is written to MapPath when it ends. */
	FHyperAIBotBrain(UWorld& InWorld, const FHyperAIBotStep& InStep, const FString& InMapPath);

	/** Checks the player, its controls and the targets; a result that is done ends the step at once. */
	FHyperAIBotResult Begin();
	FHyperAIBotResult Tick();
	/** Lets go of any input. */
	void Stop();
	/** How the play is going, for another step's report. */
	FString Progress() const;

private:
	struct FTarget
	{
		TWeakObjectPtr<AActor> Actor;
		FString Name;
		FString HealthPath;
		double Health = -1.0;
		FVector LastSeen = FVector::ZeroVector;
		/** When it was defeated, collected or given up on; 0 while it is still to do. */
		double DoneAt = 0.0;
		bool bSkipped = false;
		FString Why;
		int32 HitsSinceDamage = 0;
		double ArrivedAt = 0.0;
		bool bTriedInteract = false;
	};

	FHyperAIBotResult TickReach(APawn& Me, APlayerController& Controller, double Dt);
	FHyperAIBotResult TickCollect(APawn& Me, APlayerController& Controller, double Dt);
	FHyperAIBotResult TickDefeat(APawn& Me, APlayerController& Controller, double Dt);
	FHyperAIBotResult TickSurvive(APawn& Me, APlayerController& Controller, double Dt);
	FHyperAIBotResult TickExplore(APawn& Me, APlayerController& Controller, double Dt);
	FHyperAIBotResult TickFollow(APawn& Me, APlayerController& Controller, double Dt);
	FHyperAIBotResult TickFlee(APawn& Me, APlayerController& Controller, double Dt);
	/** Holds the sprint action this frame, when the game has one. */
	void Sprint();
	FHyperAIBotResult Finish(FHyperAIBotResult Result);
	FHyperAIBotResult Died(const TCHAR* Code, const FString& How);

	/** Walks toward Goal: along the navmesh when the level has one, round what blocks the way. False once it gives up. */
	bool Travel(APawn& Me, APlayerController& Controller, const FVector& Goal, double Dt, bool bLookAhead, const AActor* Ignore);
	void ResetTravel();
	/** The open heading nearest Desired; bOutJump when only something low is in the way. */
	FVector OpenHeading(APawn& Me, const FVector& Desired, const AActor* Ignore, bool& bOutJump);
	/** Combat footwork: keep a distance, strafe, now and then hop, dodge when hurt. */
	void Maneuver(APawn& Me, APlayerController& Controller, const AActor& Enemy, double Distance);
	void Wander(APawn& Me, APlayerController& Controller, double Dt, bool bSeekNew);
	void PickDestination(APawn& Me, bool bSeekNew);
	void Move(APawn& Me, APlayerController& Controller, const FVector& Heading, float Scale);
	void Turn(APlayerController& Controller, const FRotator& Desired, double Dt);
	void Aim(APlayerController& Controller, const AActor& Target, double Dt);
	void Acquire(const AActor& Target);
	bool Sees(const APawn& Me, const APlayerController& Controller, const AActor& Target) const;
	bool CrosshairOn(const APlayerController& Controller, const AActor& Target, bool& bOutNear) const;
	void Press(const UInputAction* Action, const FInputActionValue& Value);
	bool TryJump(APawn& Me);
	void RefreshTargets();
	int32 NextTarget(const FVector& From, bool bPreferSeen, const APawn* Me, const APlayerController* Controller) const;
	bool ReadHealth(AActor* Actor, const FString& Path, double& OutHealth) const;
	void AddSpot(TArray<FVector>& Spots, const FVector& Point);
	/** The targets still to do, for a report that ran out of time. */
	FString Remaining() const;
	FString Stats() const;
	FString WriteMap() const;

	TWeakObjectPtr<UWorld> World;
	FHyperAIBotStep Step;
	FString MapPath;
	FRandomStream Random;
	HyperAIStudio::Bot::Private::FControls Controls;
	const UInputAction* Attack = nullptr;
	double Skill = 0.5;

	double StartedAt = 0.0;
	double LastTickAt = 0.0;
	double Now = 0.0;

	TWeakObjectPtr<APawn> Pawn;
	FString HealthPath;
	double StartHealth = -1.0;
	double Health = -1.0;
	double MinHealth = -1.0;
	bool bHurt = false;
	FVector StartLocation = FVector::ZeroVector;
	FVector LastLocation = FVector::ZeroVector;
	bool bFalling = false;
	double FallFromZ = 0.0;
	bool bDirectJump = false;

	TArray<FTarget> Targets;
	int32 Current = INDEX_NONE;
	double NextRefreshAt = 0.0;

	TArray<FVector> Route;
	int32 NextPoint = 0;
	FVector RouteGoal = FVector::ZeroVector;
	double BestDistance = 0.0;
	double LastProgressAt = 0.0;
	/** Getting no closer is not being stuck: walking along a wall is progress of a kind. Not moving is. */
	FVector MovedFrom = FVector::ZeroVector;
	double LastMovedAt = 0.0;
	double NextJumpAt = 0.0;
	double NextHopOk = 0.0;
	double EscapeUntil = -10.0;
	FVector Escape = FVector::ZeroVector;
	/** Which way it goes round what blocks it: -1 left, 1 right, 0 straight on. */
	int32 Side = 0;
	/** Since when the way straight on has been open; a glimpse of it does not end going round. */
	double ClearSince = -1.0;
	double NextControlsAt = 0.0;
	bool bMoveInputWorks = true;
	double MoveCheckAt = -1.0;
	FVector MoveCheckFrom = FVector::ZeroVector;

	TWeakObjectPtr<const AActor> Aiming;
	double SeenAt = -1.0;
	double Reaction = 0.3;
	double AimError = 0.0;
	FVector ErrorAxis = FVector::UpVector;
	double Phase = 0.0;
	double NextShotAt = 0.0;
	double EngageRange = 1500.0;
	double StrafeUntil = 0.0;
	int32 Strafe = 1;
	double NextDodgeAt = 0.0;
	double NextHopAt = 0.0;
	bool bBackedOff = false;
	/** Follow: seconds counted after the first two, and how many of them within Radius. */
	double FollowCounted = 0.0;
	double FollowWithin = 0.0;

	FVector Destination = FVector::ZeroVector;
	bool bHasDestination = false;
	double DestinationSince = 0.0;
	TSet<FIntPoint> Visited;

	int32 Shots = 0;
	int32 OnTarget = 0;
	int32 Kills = 0;
	int32 Collected = 0;
	int32 Jumps = 0;
	int32 Dodges = 0;
	int32 Detours = 0;
	int32 Recoveries = 0;
	double DamageDealt = 0.0;
	double DamageTaken = 0.0;
	double Walked = 0.0;
	double ClosestThreat = -1.0;
	TArray<FVector> Trail;
	double NextTrailAt = 0.0;
	TArray<FVector> StuckSpots;
	TArray<FVector> Falls;
	TArray<FVector> ShotsFrom;
	TArray<FString> Notes;
	bool bDied = false;
};
