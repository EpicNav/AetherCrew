// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "HyperAIStudioBotFixtures.generated.h"

class UEnhancedInputComponent;
class UInputAction;

namespace HyperAIStudio::BotFixtures
{
	/**
	 * Sets up one verification case's copy of a level, from where its player starts: every pawn gets health and a
	 * blaster, and the case by its name gets its game: reach-around-wall, reach-over-block, collect-coins,
	 * defeat-targets, survive-evading, survive-standing and flee-turret. Other names get only the armed pawns.
	 */
	void SetUpCase(UWorld& World, const FString& CaseName);
}

/**
 * Test fixtures for the playtest bot: a small game the tests add to copies of a level. None is placeable or used
 * outside the tests.
 */

/** Hit points. At 0 the owner is destroyed, as a defeated enemy or player would be. */
UCLASS(NotBlueprintable, NotPlaceable, Transient, HideDropdown)
class UHyperAIStudioTestHealth : public UActorComponent
{
	GENERATED_BODY()

public:
	UPROPERTY()
	float Health = 100.f;

	UPROPERTY()
	float MaxHealth = 100.f;

	void TakeHit(float Damage);
};

/** The player's weapon: its fire action, clicked, traces from the camera along the view and hurts what it hits. */
UCLASS(NotBlueprintable, NotPlaceable, Transient, HideDropdown)
class UHyperAIStudioTestBlaster : public UActorComponent
{
	GENERATED_BODY()

public:
	UHyperAIStudioTestBlaster();

	/** IA_Fire, on the left mouse button while a blaster is held. */
	static const UInputAction* FireAction();

	UPROPERTY()
	float Damage = 10.f;

	UPROPERTY()
	int32 Shots = 0;

	UPROPERTY()
	int32 Hits = 0;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;

private:
	void Fire();

	UPROPERTY()
	TObjectPtr<UEnhancedInputComponent> Bindings;
};

/** A turret that fires slow shots at the nearest pawn it can see: one standing still is hit, one moving can dodge. */
UCLASS(NotBlueprintable, NotPlaceable, Transient, HideDropdown)
class UHyperAIStudioTestTurret : public UActorComponent
{
	GENERATED_BODY()

public:
	UHyperAIStudioTestTurret();

	UPROPERTY()
	float Interval = 1.f;

	UPROPERTY()
	float Speed = 1200.f;

	UPROPERTY()
	float Damage = 5.f;

	UPROPERTY()
	float Range = 3000.f;

	UPROPERTY()
	int32 Fired = 0;

	UPROPERTY()
	int32 Landed = 0;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	struct FShot
	{
		FVector At = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Age = 0.f;
	};

	TArray<FShot> InFlight;
	float Cooldown = 0.5f;
};

/** A pickup, taken by walking onto it. */
UCLASS(NotBlueprintable, NotPlaceable, Transient, HideDropdown)
class UHyperAIStudioTestPickup : public UActorComponent
{
	GENERATED_BODY()

public:
	UHyperAIStudioTestPickup();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
};

/**
 * Aims the way an isometric game's aim component does with a mouse, as Aetherfall's does: every frame, before the
 * player's input, it reads the window's cursor (GetMousePosition), counts it as aiming only while it moved more than
 * MovePixels within RecencySeconds, and deprojects it (DeprojectMousePositionToWorld) onto the floor under the player
 * for the aim point. While the right stick is tilted it aims along the stick instead, relative to the camera's yaw.
 * Its slash attack swings along GetAimDirectionFlat, as the game's abilities read it.
 */
UCLASS(NotBlueprintable, NotPlaceable, Transient, HideDropdown)
class UHyperAIStudioTestCursorAim : public UActorComponent
{
	GENERATED_BODY()

public:
	UHyperAIStudioTestCursorAim();

	/** IA_TestSlash on the left mouse button, IA_TestAimStick on the right stick, and IA_TestLookMouse on Mouse XY, as a game maps mouse look. */
	static const UInputAction* SlashAction();
	static const UInputAction* StickAction();
	static const UInputAction* LookAction();

	UFUNCTION(BlueprintPure, Category = "Test")
	FVector GetAimWorldLocation() const { return AimWorldLocation; }

	UFUNCTION(BlueprintPure, Category = "Test")
	FVector GetAimDirection() const { return AimDirection; }

	UFUNCTION(BlueprintPure, Category = "Test")
	FVector GetAimDirectionFlat() const;

	UFUNCTION(BlueprintPure, Category = "Test")
	bool IsMouseAimActive() const;

	float MovePixels = 2.f;
	float RecencySeconds = 0.5f;
	float DeadRadius = 45.f;
	float StickDeadZone = 0.25f;

	/** Slashes swung, the last one's heading, and whether the cursor counted as aiming as it swung. */
	int32 Slashes = 0;
	FVector LastSlash = FVector::ZeroVector;
	bool bLastSlashActive = false;
	/** The right stick as the game read it last, and Mouse XY: samples and their sum. */
	FVector2D Stick = FVector2D::ZeroVector;
	int32 LookSamples = 0;
	FVector2D LookSum = FVector2D::ZeroVector;
	/** The cursor as the game read it last. */
	FVector2D Cursor = FVector2D(-1.0, -1.0);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;

private:
	void Bind();
	void Slash();
	void AimStick(const struct FInputActionValue& Value);
	void Look(const struct FInputActionValue& Value);
	void Commit(const FVector& Point);

	UPROPERTY()
	TObjectPtr<UEnhancedInputComponent> Bindings;

	FVector AimWorldLocation = FVector::ZeroVector;
	FVector AimDirection = FVector::ForwardVector;
	FVector2D LastCursor = FVector2D::ZeroVector;
	bool bHasLastCursor = false;
	double LastMoveTime = -100.0;
};
