// Games by Hyper 2026.

#include "Tests/HyperAIStudioBotFixtures.h"

#include "CollisionQueryParams.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace HyperAIStudio::BotFixtures
{
	UInputMappingContext* FireContext()
	{
		static TStrongObjectPtr<UInputMappingContext> Context;
		if (!Context.IsValid())
		{
			Context.Reset(NewObject<UInputMappingContext>(GetTransientPackage(), TEXT("IMC_HyperAITestBlaster")));
			Context->MapKey(UHyperAIStudioTestBlaster::FireAction(), EKeys::LeftMouseButton);
		}
		return Context.Get();
	}

	UHyperAIStudioTestHealth* HealthOf(AActor* Actor)
	{
		return Actor ? Actor->FindComponentByClass<UHyperAIStudioTestHealth>() : nullptr;
	}

	template <typename T>
	T* AddPart(AActor& Actor, const TCHAR* Name)
	{
		T* Part = NewObject<T>(&Actor, Name);
		Actor.AddInstanceComponent(Part);
		Part->RegisterComponent();
		return Part;
	}

	AStaticMeshActor* Box(UWorld& World, const FVector& Center, const FRotator& Facing, const FVector& SizeCm, const TCHAR* Label, const bool bSolid = true)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(Center, Facing, Params);
		UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		if (!bSolid)
		{
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		Actor->SetActorScale3D(SizeCm / 100.0);
		Actor->SetActorLabel(Label);
		return Actor;
	}

	/** Every pawn can be hurt and carries a blaster. */
	void Arm(APawn* Pawn)
	{
		if (Pawn && !Pawn->FindComponentByClass<UHyperAIStudioTestHealth>())
		{
			AddPart<UHyperAIStudioTestHealth>(*Pawn, TEXT("Health"));
			AddPart<UHyperAIStudioTestBlaster>(*Pawn, TEXT("Blaster"));
		}
	}

	/** Each case's copy of the level gets the little game its case plays, placed from where the player starts. */
	void SetUpCase(UWorld& World, const FString& Case)
	{
		const APlayerController* Controller = World.GetFirstPlayerController();
		const APawn* Player = Controller ? Controller->GetPawn() : nullptr;
		if (!Player)
		{
			return;
		}
		for (TActorIterator<APawn> It(&World); It; ++It)
		{
			Arm(*It);
		}
		// Players who join later are armed as they arrive.
		World.AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* Actor) { Arm(Cast<APawn>(Actor)); }));
		float Radius = 34.f;
		float HalfHeight = 88.f;
		Player->GetSimpleCollisionCylinder(Radius, HalfHeight);
		const FVector Feet = Player->GetActorLocation() - FVector(0.0, 0.0, HalfHeight);
		const FRotator Facing(0.0, Player->GetActorRotation().Yaw, 0.0);
		const FVector Ahead = Facing.Vector();
		const FVector Right = FRotationMatrix(Facing).GetScaledAxis(EAxis::Y);
		const FVector Up = FVector::UpVector;
		auto Mark = [&World](const FVector& At, const TCHAR* Label)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ATargetPoint* Point = World.SpawnActor<ATargetPoint>(At, FRotator::ZeroRotator, Params);
			Point->SetActorLabel(Label);
			Point->Tags.Add(Label);
		};
		if (Case == TEXT("reach-around-wall"))
		{
			Box(World, Feet + Ahead * 450.0 + Up * 150.0, Facing, FVector(50.0, 700.0, 300.0), TEXT("Wall"));
			Mark(Feet + Ahead * 950.0 + Up * 90.0, TEXT("Goal"));
		}
		else if (Case == TEXT("reach-over-block"))
		{
			Box(World, Feet + Ahead * 400.0 + Up * 25.0, Facing, FVector(100.0, 2400.0, 50.0), TEXT("LowBlock"));
			Mark(Feet + Ahead * 800.0 + Up * 90.0, TEXT("Goal"));
		}
		else if (Case == TEXT("collect-coins"))
		{
			const TArray<FVector> Spots = {Ahead * 650.0 + Right * 300.0, Ahead * 250.0 - Right * 550.0, Right * 250.0 - Ahead * 350.0};
			for (int32 Index = 0; Index < Spots.Num(); ++Index)
			{
				AStaticMeshActor* Coin = Box(World, Feet + Spots[Index] + Up * 50.0, Facing, FVector(30.0), *FString::Printf(TEXT("Coin_%d"), Index + 1), /*bSolid=*/false);
				Coin->Tags.Add(TEXT("Coin"));
				AddPart<UHyperAIStudioTestPickup>(*Coin, TEXT("Pickup"));
			}
		}
		else if (Case == TEXT("defeat-targets"))
		{
			const TArray<FVector> Spots = {Ahead * 1200.0 + Right * 350.0, Ahead * 1300.0 - Right * 450.0};
			for (int32 Index = 0; Index < Spots.Num(); ++Index)
			{
				AStaticMeshActor* Target = Box(World, Feet + Spots[Index] + Up * 100.0, Facing, FVector(100.0, 100.0, 200.0), Index == 0 ? TEXT("Target_A") : TEXT("Target_B"));
				Target->Tags.Add(TEXT("Enemy"));
				AddPart<UHyperAIStudioTestHealth>(*Target, TEXT("Health"));
			}
		}
		else if (Case.StartsWith(TEXT("survive")) || Case.StartsWith(TEXT("flee")))
		{
			AStaticMeshActor* Turret = Box(World, Feet + Ahead * 1500.0 + Up * 150.0, Facing, FVector(100.0, 100.0, 300.0), TEXT("Turret"));
			Turret->Tags.Add(TEXT("Turret"));
			AddPart<UHyperAIStudioTestTurret>(*Turret, TEXT("Gun"));
		}
	}
}

void UHyperAIStudioTestHealth::TakeHit(const float Damage)
{
	if (Health <= 0.f)
	{
		return;
	}
	Health = FMath::Max(0.f, Health - Damage);
	if (Health <= 0.f)
	{
		if (AActor* Owner = GetOwner())
		{
			Owner->Destroy();
		}
	}
}

UHyperAIStudioTestBlaster::UHyperAIStudioTestBlaster()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

const UInputAction* UHyperAIStudioTestBlaster::FireAction()
{
	static TStrongObjectPtr<UInputAction> Action;
	if (!Action.IsValid())
	{
		Action.Reset(NewObject<UInputAction>(GetTransientPackage(), TEXT("IA_Fire")));
		Action->ValueType = EInputActionValueType::Boolean;
	}
	return Action.Get();
}

void UHyperAIStudioTestBlaster::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (Bindings)
	{
		return;
	}
	// Bound once the pawn has a player: pawns are spawned before they are possessed.
	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* Controller = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	ULocalPlayer* Player = Controller ? Controller->GetLocalPlayer() : nullptr;
	UEnhancedInputLocalPlayerSubsystem* Input = Player ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Player) : nullptr;
	if (!Input)
	{
		return;
	}
	Bindings = NewObject<UEnhancedInputComponent>(Controller);
	Bindings->RegisterComponent();
	Bindings->BindAction(FireAction(), ETriggerEvent::Started, this, &UHyperAIStudioTestBlaster::Fire);
	Controller->PushInputComponent(Bindings);
	FModifyContextOptions Options;
	Options.bForceImmediately = true;
	Input->AddMappingContext(HyperAIStudio::BotFixtures::FireContext(), 1, Options);
}

void UHyperAIStudioTestBlaster::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Bindings)
	{
		if (APlayerController* Controller = Cast<APlayerController>(Bindings->GetOwner()))
		{
			Controller->PopInputComponent(Bindings);
		}
		Bindings = nullptr;
	}
	Super::EndPlay(Reason);
}

void UHyperAIStudioTestBlaster::Fire()
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const AController* Controller = Pawn ? Pawn->GetController() : nullptr;
	UWorld* World = GetWorld();
	if (!Controller || !World)
	{
		return;
	}
	++Shots;
	FVector From;
	FRotator View;
	Controller->GetPlayerViewPoint(From, View);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAITestBlaster), true, Pawn);
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, From, From + Controller->GetControlRotation().Vector() * 20000.0, ECC_Pawn, Params))
	{
		if (UHyperAIStudioTestHealth* Health = HyperAIStudio::BotFixtures::HealthOf(Hit.GetActor()))
		{
			++Hits;
			Health->TakeHit(Damage);
		}
	}
}

UHyperAIStudioTestTurret::UHyperAIStudioTestTurret()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UHyperAIStudioTestTurret::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!World || !Owner)
	{
		return;
	}
	// Walls stop sight and shots; pawns do not block the visibility channel, and are hit by distance.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAITestTurret), false, Owner);
	// Shots fly straight at Speed; one that passes within 70 cm of a pawn's middle hits it.
	for (int32 Index = InFlight.Num() - 1; Index >= 0; --Index)
	{
		FShot& Shot = InFlight[Index];
		const FVector Next = Shot.At + Shot.Velocity * DeltaTime;
		Shot.Age += DeltaTime;
		bool bDone = Shot.Age > 4.f;
		for (TActorIterator<APawn> It(World); It && !bDone; ++It)
		{
			const FVector Middle = It->GetActorLocation();
			if (FVector::Dist(FMath::ClosestPointOnSegment(Middle, Shot.At, Next), Middle) < 70.0)
			{
				if (UHyperAIStudioTestHealth* Health = HyperAIStudio::BotFixtures::HealthOf(*It))
				{
					++Landed;
					Health->TakeHit(Damage);
				}
				bDone = true;
			}
		}
		FHitResult Wall;
		bDone = bDone || World->LineTraceSingleByChannel(Wall, Shot.At, Next, ECC_Visibility, Params);
		if (bDone)
		{
			InFlight.RemoveAtSwap(Index);
		}
		else
		{
			Shot.At = Next;
		}
	}
	Cooldown -= DeltaTime;
	if (Cooldown > 0.f)
	{
		return;
	}
	// The nearest pawn in range with nothing in the way.
	const FVector Muzzle = Owner->GetActorLocation() + FVector(0.0, 0.0, 100.0);
	const APawn* Target = nullptr;
	double Nearest = Range;
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		const double Distance = FVector::Dist(Muzzle, It->GetActorLocation());
		FHitResult Blocked;
		if (Distance < Nearest && (!World->LineTraceSingleByChannel(Blocked, Muzzle, It->GetActorLocation(), ECC_Visibility, Params) || Blocked.GetActor() == *It))
		{
			Nearest = Distance;
			Target = *It;
		}
	}
	if (Target)
	{
		Cooldown = Interval;
		FShot& Shot = InFlight.AddDefaulted_GetRef();
		Shot.At = Muzzle;
		Shot.Velocity = (Target->GetActorLocation() - Muzzle).GetSafeNormal() * Speed;
		++Fired;
	}
}

namespace HyperAIStudio::BotFixtures
{
	const UInputAction* TestAction(TStrongObjectPtr<UInputAction>& Action, const TCHAR* Name, const EInputActionValueType Type)
	{
		if (!Action.IsValid())
		{
			Action.Reset(NewObject<UInputAction>(GetTransientPackage(), Name));
			Action->ValueType = Type;
		}
		return Action.Get();
	}

	UInputMappingContext* CursorAimContext()
	{
		static TStrongObjectPtr<UInputMappingContext> Context;
		if (!Context.IsValid())
		{
			Context.Reset(NewObject<UInputMappingContext>(GetTransientPackage(), TEXT("IMC_HyperAITestCursorAim")));
			Context->MapKey(UHyperAIStudioTestCursorAim::SlashAction(), EKeys::LeftMouseButton);
			Context->MapKey(UHyperAIStudioTestCursorAim::StickAction(), EKeys::Gamepad_Right2D);
			Context->MapKey(UHyperAIStudioTestCursorAim::LookAction(), EKeys::Mouse2D);
		}
		return Context.Get();
	}
}

UHyperAIStudioTestCursorAim::UHyperAIStudioTestCursorAim()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

const UInputAction* UHyperAIStudioTestCursorAim::SlashAction()
{
	static TStrongObjectPtr<UInputAction> Action;
	return HyperAIStudio::BotFixtures::TestAction(Action, TEXT("IA_TestSlash"), EInputActionValueType::Boolean);
}

const UInputAction* UHyperAIStudioTestCursorAim::StickAction()
{
	static TStrongObjectPtr<UInputAction> Action;
	return HyperAIStudio::BotFixtures::TestAction(Action, TEXT("IA_TestAimStick"), EInputActionValueType::Axis2D);
}

const UInputAction* UHyperAIStudioTestCursorAim::LookAction()
{
	static TStrongObjectPtr<UInputAction> Action;
	return HyperAIStudio::BotFixtures::TestAction(Action, TEXT("IA_TestLookMouse"), EInputActionValueType::Axis2D);
}

FVector UHyperAIStudioTestCursorAim::GetAimDirectionFlat() const
{
	const FVector Flat(AimDirection.X, AimDirection.Y, 0.0);
	return Flat.IsNearlyZero() ? FVector::ForwardVector : Flat.GetSafeNormal();
}

bool UHyperAIStudioTestCursorAim::IsMouseAimActive() const
{
	const UWorld* World = GetWorld();
	return World && World->GetTimeSeconds() - LastMoveTime <= RecencySeconds;
}

void UHyperAIStudioTestCursorAim::Bind()
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* Controller = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	ULocalPlayer* Player = Controller ? Controller->GetLocalPlayer() : nullptr;
	UEnhancedInputLocalPlayerSubsystem* Input = Player ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Player) : nullptr;
	if (!Input)
	{
		return;
	}
	Bindings = NewObject<UEnhancedInputComponent>(Controller);
	Bindings->RegisterComponent();
	Bindings->BindAction(SlashAction(), ETriggerEvent::Started, this, &UHyperAIStudioTestCursorAim::Slash);
	Bindings->BindAction(StickAction(), ETriggerEvent::Triggered, this, &UHyperAIStudioTestCursorAim::AimStick);
	Bindings->BindAction(StickAction(), ETriggerEvent::Completed, this, &UHyperAIStudioTestCursorAim::AimStick);
	Bindings->BindAction(LookAction(), ETriggerEvent::Triggered, this, &UHyperAIStudioTestCursorAim::Look);
	Controller->PushInputComponent(Bindings);
	FModifyContextOptions Options;
	Options.bForceImmediately = true;
	Input->AddMappingContext(HyperAIStudio::BotFixtures::CursorAimContext(), 2, Options);
}

void UHyperAIStudioTestCursorAim::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Bindings)
	{
		Bind();
	}
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* Controller = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	const UWorld* World = GetWorld();
	if (!Controller || !World)
	{
		return;
	}
	if (Stick.Size() > StickDeadZone)
	{
		// Gamepad aim: the stick through the camera's yaw, up being where the camera faces.
		FVector Camera;
		FRotator View;
		Controller->GetPlayerViewPoint(Camera, View);
		const FRotator Yaw(0.0, View.Yaw, 0.0);
		const FVector Along = (Yaw.Vector() * Stick.Y + FRotationMatrix(Yaw).GetScaledAxis(EAxis::Y) * Stick.X).GetSafeNormal();
		Commit(Pawn->GetActorLocation() + Along * 1000.0);
		return;
	}
	// Mouse aim: the cursor counts as aiming only while it moves.
	float X = 0.f;
	float Y = 0.f;
	if (Controller->GetMousePosition(X, Y))
	{
		Cursor = FVector2D(X, Y);
		if (!bHasLastCursor)
		{
			LastCursor = Cursor;
			bHasLastCursor = true;
		}
		else if (FVector2D::Distance(Cursor, LastCursor) > MovePixels)
		{
			LastCursor = Cursor;
			LastMoveTime = World->GetTimeSeconds();
		}
	}
	FVector Origin;
	FVector Direction;
	if (!Controller->DeprojectMousePositionToWorld(Origin, Direction) || FMath::Abs(Direction.Z) < UE_KINDA_SMALL_NUMBER)
	{
		return;
	}
	// The floor is the plane under the player's feet.
	float Radius = 0.f;
	float HalfHeight = 0.f;
	Pawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
	const double Along = (Pawn->GetActorLocation().Z - HalfHeight - Origin.Z) / Direction.Z;
	if (Along > 0.0)
	{
		Commit(Origin + Direction * Along);
	}
}

void UHyperAIStudioTestCursorAim::Commit(const FVector& Point)
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const FVector Feet = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
	AimWorldLocation = Point;
	const FVector Level(Point.X - Feet.X, Point.Y - Feet.Y, 0.0);
	// A cursor on the player gives no heading; the last one stays.
	if (Level.Size() >= DeadRadius)
	{
		AimDirection = FVector(Level.X, Level.Y, Point.Z - Feet.Z).GetSafeNormal();
	}
}

void UHyperAIStudioTestCursorAim::Slash()
{
	++Slashes;
	LastSlash = GetAimDirectionFlat();
	bLastSlashActive = IsMouseAimActive();
}

void UHyperAIStudioTestCursorAim::AimStick(const FInputActionValue& Value)
{
	Stick = Value.Get<FVector2D>();
}

void UHyperAIStudioTestCursorAim::Look(const FInputActionValue& Value)
{
	++LookSamples;
	LookSum += Value.Get<FVector2D>();
}

void UHyperAIStudioTestCursorAim::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Bindings)
	{
		if (APlayerController* Controller = Cast<APlayerController>(Bindings->GetOwner()))
		{
			Controller->PopInputComponent(Bindings);
		}
		Bindings = nullptr;
	}
	Super::EndPlay(Reason);
}

UHyperAIStudioTestPickup::UHyperAIStudioTestPickup()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UHyperAIStudioTestPickup::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return;
	}
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		const FVector Offset = It->GetActorLocation() - Owner->GetActorLocation();
		if (Offset.Size2D() < 90.0 && FMath::Abs(Offset.Z) < 200.0)
		{
			Owner->Destroy();
			return;
		}
	}
}
