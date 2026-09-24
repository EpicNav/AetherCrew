// Games by Hyper 2026.

#include "HyperAIStudioBot.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "CollisionQueryParams.h"
#include "Components/Button.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/RichTextBlock.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"
#include "Widgets/Input/SButton.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, HyperAIStudioBot)

namespace HyperAIStudio::Bot::Private
{
	constexpr int32 MaxRecordFrames = 240;
	constexpr double StuckSeconds = 3.0;
	constexpr double WaypointReachedCm = 75.0;

	using FResult = FHyperAIBotResult;

	FString Vec(const FVector& Value)
	{
		return FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Value.X, Value.Y, Value.Z);
	}

	FString Describe(const UObject* Object)
	{
		if (!Object)
		{
			return TEXT("nothing");
		}
#if WITH_EDITOR
		if (const AActor* Actor = Cast<AActor>(Object))
		{
			return Actor->GetActorLabel();
		}
#endif
		return Object->GetName();
	}

	APawn* PlayerPawn(UWorld& World)
	{
		const APlayerController* Controller = World.GetFirstPlayerController();
		return Controller ? Controller->GetPawn() : nullptr;
	}

	bool MatchesClass(const AActor& Actor, const FString& Name)
	{
		FString Class = Actor.GetClass()->GetName();
		if (Class.Equals(Name, ESearchCase::IgnoreCase))
		{
			return true;
		}
		Class.RemoveFromEnd(TEXT("_C"));
		return Class.Equals(Name, ESearchCase::IgnoreCase);
	}

	/** An exact name wins; otherwise the first actor with the label, then the tag, then the class. */
	UObject* FindTarget(UWorld& World, const FString& Name)
	{
		APlayerController* Controller = World.GetFirstPlayerController();
		if (Name.IsEmpty() || Name.Equals(TEXT("Player"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase))
		{
			return PlayerPawn(World);
		}
		if (Name.Equals(TEXT("PlayerController"), ESearchCase::IgnoreCase))
		{
			return Controller;
		}
		if (Name.Equals(TEXT("PlayerState"), ESearchCase::IgnoreCase))
		{
			return Controller ? Controller->PlayerState.Get() : nullptr;
		}
		if (Name.Equals(TEXT("GameState"), ESearchCase::IgnoreCase))
		{
			return World.GetGameState();
		}
		if (Name.Equals(TEXT("GameMode"), ESearchCase::IgnoreCase))
		{
			return World.GetAuthGameMode();
		}
		AActor* ByLabel = nullptr;
		AActor* ByTag = nullptr;
		AActor* ByClass = nullptr;
		const FName Tag(*Name);
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (Actor->GetName() == Name)
			{
				return Actor;
			}
#if WITH_EDITOR
			if (!ByLabel && Actor->GetActorLabel() == Name)
			{
				ByLabel = Actor;
			}
#endif
			if (!ByTag && Actor->ActorHasTag(Tag))
			{
				ByTag = Actor;
			}
			if (!ByClass && MatchesClass(*Actor, Name))
			{
				ByClass = Actor;
			}
		}
		return ByLabel ? ByLabel : ByTag ? ByTag : ByClass;
	}

	struct FReading
	{
		bool bOk = false;
		bool bNumeric = false;
		double Number = 0.0;
		FString Text;
		FString Error;
	};

	FReading Numeric(const double Number, const FString& Text)
	{
		FReading Reading;
		Reading.bOk = true;
		Reading.bNumeric = true;
		Reading.Number = Number;
		Reading.Text = Text;
		return Reading;
	}

	FProperty* FindProperty(const UObject& Object, const FString& Name)
	{
		if (FProperty* Exact = FindFProperty<FProperty>(Object.GetClass(), FName(*Name)))
		{
			return Exact;
		}
		// Blueprint variables can be authored under a friendlier name than their field.
		for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
		{
			if (It->GetName().Equals(Name, ESearchCase::IgnoreCase) || It->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	/** Path is property names joined by dots; a part may also name one of an actor's components. */
	FReading ReadProperty(UObject* Root, const FString& Path)
	{
		FReading Out;
		if (!Root)
		{
			Out.Error = TEXT("the target does not exist");
			return Out;
		}
		if (const AActor* Actor = Cast<AActor>(Root))
		{
			if (Path.Equals(TEXT("Location"), ESearchCase::IgnoreCase))
			{
				Out.bOk = true;
				Out.Text = Vec(Actor->GetActorLocation());
				return Out;
			}
			if (Path.Equals(TEXT("Rotation"), ESearchCase::IgnoreCase))
			{
				Out.bOk = true;
				Out.Text = Actor->GetActorRotation().ToCompactString();
				return Out;
			}
			if (Path.Equals(TEXT("Velocity"), ESearchCase::IgnoreCase))
			{
				Out.bOk = true;
				Out.Text = Vec(Actor->GetVelocity());
				return Out;
			}
			if (Path.Equals(TEXT("Speed"), ESearchCase::IgnoreCase))
			{
				const double Speed = Actor->GetVelocity().Size();
				return Numeric(Speed, FString::Printf(TEXT("%.0f"), Speed));
			}
		}
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty=*/true);
		UObject* Object = Root;
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			const FString& Part = Parts[Index];
			const bool bLast = Index == Parts.Num() - 1;
			FProperty* Property = FindProperty(*Object, Part);
			if (!Property && !bLast)
			{
				UObject* Component = nullptr;
				if (const AActor* Owner = Cast<AActor>(Object))
				{
					for (UActorComponent* Candidate : Owner->GetComponents())
					{
						if (Candidate && Candidate->GetName().Equals(Part, ESearchCase::IgnoreCase))
						{
							Component = Candidate;
							break;
						}
					}
				}
				if (Component)
				{
					Object = Component;
					continue;
				}
			}
			if (!Property)
			{
				Out.Error = FString::Printf(TEXT("%s has no property or component named %s"), *Describe(Object), *Part);
				return Out;
			}
			const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
			if (!bLast)
			{
				const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
				UObject* Next = ObjectProperty ? ObjectProperty->GetObjectPropertyValue(Value) : nullptr;
				if (!Next)
				{
					Out.Error = FString::Printf(TEXT("%s.%s is not set, or not an object"), *Describe(Object), *Part);
					return Out;
				}
				Object = Next;
				continue;
			}
			if (const FNumericProperty* Number = CastField<FNumericProperty>(Property); Number && !Number->IsEnum())
			{
				const double Read = Number->IsFloatingPoint()
					? Number->GetFloatingPointPropertyValue(Value)
					: static_cast<double>(Number->GetSignedIntPropertyValue(Value));
				return Numeric(Read, Number->IsFloatingPoint() ? FString::SanitizeFloat(Read) : FString::Printf(TEXT("%lld"), static_cast<int64>(Read)));
			}
			if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
			{
				Out.Text = Bool->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
			}
			else
			{
				Property->ExportTextItem_Direct(Out.Text, Value, nullptr, nullptr, PPF_None);
			}
			Out.bOk = true;
		}
		return Out;
	}

	bool Holds(const FReading& Reading, const FString& Compare, const FString& Expected)
	{
		const FString Want = Expected.TrimStartAndEnd();
		if (Compare == TEXT("contains"))
		{
			return Reading.Text.Contains(Want);
		}
		double Number = 0.0;
		if (Reading.bNumeric && LexTryParseString(Number, *Want))
		{
			if (Compare == TEXT("==")) return FMath::IsNearlyEqual(Reading.Number, Number, 1e-4);
			if (Compare == TEXT("!=")) return !FMath::IsNearlyEqual(Reading.Number, Number, 1e-4);
			if (Compare == TEXT("<")) return Reading.Number < Number;
			if (Compare == TEXT("<=")) return Reading.Number <= Number;
			if (Compare == TEXT(">")) return Reading.Number > Number;
			if (Compare == TEXT(">=")) return Reading.Number >= Number;
			return false;
		}
		const bool bEqual = Reading.Text.TrimStartAndEnd().Equals(Want, ESearchCase::IgnoreCase);
		return Compare == TEXT("==") ? bEqual : Compare == TEXT("!=") ? !bEqual : false;
	}

	/** Turns the player's view toward Point. */
	bool Face(UWorld& World, const FVector& Point)
	{
		APawn* Pawn = PlayerPawn(World);
		AController* Controller = Pawn ? Pawn->GetController() : static_cast<AController*>(World.GetFirstPlayerController());
		if (!Controller)
		{
			return false;
		}
		FVector From = FVector::ZeroVector;
		FRotator Unused;
		if (Pawn)
		{
			From = Pawn->GetPawnViewLocation();
		}
		else
		{
			Controller->GetPlayerViewPoint(From, Unused);
		}
		Controller->SetControlRotation((Point - From).Rotation());
		return true;
	}

	UEnhancedInputLocalPlayerSubsystem* InputFor(UWorld& World, FString& OutCode, FString& OutError)
	{
		APlayerController* Controller = World.GetFirstPlayerController();
		ULocalPlayer* Player = Controller ? Controller->GetLocalPlayer() : nullptr;
		UEnhancedInputLocalPlayerSubsystem* Input = Player ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Player) : nullptr;
		if (!Input)
		{
			OutCode = Controller ? TEXT("no_local_player") : TEXT("no_player");
			OutError = TEXT("Enhanced Input needs a local player controller; this world has none.");
		}
		return Input;
	}

	/**
	 * Mapping contexts the game added apply at Enhanced Input's next tick, which only runs where the editor ticks the
	 * world itself; forcing the rebuild makes them apply now, wherever the world is ticked from.
	 */
	void ApplyMappings(UEnhancedInputLocalPlayerSubsystem& Input)
	{
		FModifyContextOptions Options;
		Options.bForceImmediately = true;
		Input.RequestRebuildControlMappings(Options);
	}

	const UInputAction* FindInputAction(const FString& NameOrPath)
	{
		if (NameOrPath.StartsWith(TEXT("/")))
		{
			return LoadObject<UInputAction>(nullptr, *NameOrPath);
		}
		const UInputAction* Loose = nullptr;
		for (TObjectIterator<UInputAction> It; It; ++It)
		{
			if (It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
			{
				continue;
			}
			if (It->GetName() == NameOrPath)
			{
				return *It;
			}
			if (!Loose && It->GetName().Equals(NameOrPath, ESearchCase::IgnoreCase))
			{
				Loose = *It;
			}
		}
		return Loose;
	}

	/** The game's own interact action, found by the names games usually give it. */
	const UInputAction* FindInteractAction()
	{
		for (const TCHAR* Word : {TEXT("Interact"), TEXT("Use"), TEXT("Activate")})
		{
			for (TObjectIterator<UInputAction> It; It; ++It)
			{
				if (!It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) && It->GetName().Contains(Word))
				{
					return *It;
				}
			}
		}
		return nullptr;
	}

	FInputActionValue InputValue(const UInputAction& Action, const FString& Text)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/true);
		auto Part = [&Parts](const int32 Index, const float Default) { return Parts.IsValidIndex(Index) ? FCString::Atof(*Parts[Index]) : Default; };
		switch (Action.ValueType)
		{
		case EInputActionValueType::Boolean:
			return FInputActionValue(Part(0, 1.f) != 0.f);
		case EInputActionValueType::Axis1D:
			return FInputActionValue(Part(0, 1.f));
		case EInputActionValueType::Axis2D:
			// Unset, a 2D action such as Move goes forward.
			return FInputActionValue(FVector2D(Part(0, 0.f), Part(1, Parts.IsEmpty() ? 1.f : 0.f)));
		default:
			return FInputActionValue(FVector(Part(0, 1.f), Part(1, 0.f), Part(2, 0.f)));
		}
	}

	FString SafeName(const FString& Text, const TCHAR* Fallback)
	{
		FString Name;
		for (const TCHAR C : Text.Left(64))
		{
			Name.AppendChar(FChar::IsAlnum(C) && C < 128 ? C : TEXT('_'));
		}
		return Name.IsEmpty() ? FString(Fallback) : Name;
	}

	UWidget* FindWidget(UWorld& World, const FString& Name)
	{
		FString Outer;
		FString Inner = Name;
		if (Name.Contains(TEXT(".")))
		{
			Name.Split(TEXT("."), &Outer, &Inner);
		}
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* User = *It;
			if (!IsValid(User) || User->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || User->GetWorld() != &World || !User->WidgetTree)
			{
				continue;
			}
			FString Class = User->GetClass()->GetName();
			Class.RemoveFromEnd(TEXT("_C"));
			if (!Outer.IsEmpty() && !Class.Equals(Outer, ESearchCase::IgnoreCase) && !User->GetName().Equals(Outer, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (UWidget* Found = User->WidgetTree->FindWidget(FName(*Inner)))
			{
				return Found;
			}
			if (Outer.IsEmpty() && Class.Equals(Inner, ESearchCase::IgnoreCase))
			{
				return User;
			}
		}
		return nullptr;
	}

	bool ReadWidgetText(const UWidget& Widget, FString& OutText)
	{
		if (const UTextBlock* Text = Cast<UTextBlock>(&Widget)) { OutText = Text->GetText().ToString(); return true; }
		if (const URichTextBlock* Rich = Cast<URichTextBlock>(&Widget)) { OutText = Rich->GetText().ToString(); return true; }
		if (const UEditableTextBox* Box = Cast<UEditableTextBox>(&Widget)) { OutText = Box->GetText().ToString(); return true; }
		if (const UEditableText* Edit = Cast<UEditableText>(&Widget)) { OutText = Edit->GetText().ToString(); return true; }
		if (const UMultiLineEditableTextBox* Multi = Cast<UMultiLineEditableTextBox>(&Widget)) { OutText = Multi->GetText().ToString(); return true; }
		return false;
	}

	bool IsAssertion(const EHyperAIBotAction Action)
	{
		return Action == EHyperAIBotAction::AssertProperty || Action == EHyperAIBotAction::AssertNear
			|| Action == EHyperAIBotAction::AssertOnScreen || Action == EHyperAIBotAction::AssertExists
			|| Action == EHyperAIBotAction::AssertGone;
	}
}

FHyperAIBotResult FHyperAIBotResult::Passed(const FString& Detail, const FString& File)
{
	FHyperAIBotResult Result;
	Result.State = EState::Passed;
	Result.Code = TEXT("passed");
	Result.Detail = Detail;
	Result.File = File;
	return Result;
}

FHyperAIBotResult FHyperAIBotResult::Failed(const FString& Code, const FString& Detail)
{
	FHyperAIBotResult Result;
	Result.State = EState::Failed;
	Result.Code = Code;
	Result.Detail = Detail;
	return Result;
}

FHyperAIBotExecutor::FHyperAIBotExecutor(UWorld& InWorld, const FString& InOutputDirectory)
	: World(&InWorld)
	, OutputDirectory(FPaths::ConvertRelativePathToFull(InOutputDirectory))
{
}

FHyperAIBotExecutor::~FHyperAIBotExecutor()
{
	Stop();
}

FString FHyperAIBotExecutor::ActionName(const EHyperAIBotAction Action)
{
	return StaticEnum<EHyperAIBotAction>()->GetNameStringByValue(static_cast<int64>(Action));
}

bool FHyperAIBotExecutor::Validate(const FHyperAIBotStep& Step, FString& OutError)
{
	using namespace HyperAIStudio::Bot::Private;
	auto Bounded = [&OutError](const FString& Value, const int32 Max, const TCHAR* Field)
	{
		if (Value.Len() <= Max)
		{
			return true;
		}
		OutError = FString::Printf(TEXT("bot.%s is longer than %d characters."), Field, Max);
		return false;
	};
	if (!Bounded(Step.Target, 256, TEXT("target")) || !Bounded(Step.Other, 256, TEXT("other")) || !Bounded(Step.Property, 256, TEXT("property"))
		|| !Bounded(Step.Value, 512, TEXT("value")) || !Bounded(Step.InputAction, 512, TEXT("inputAction"))
		|| !Bounded(Step.Widget, 256, TEXT("widget")) || !Bounded(Step.Text, 512, TEXT("text")))
	{
		return false;
	}
	if (!StaticEnum<EHyperAIBotAction>()->IsValidEnumValue(static_cast<int64>(Step.Action))
		|| !StaticEnum<EHyperAIBotAction>()->IsValidEnumValue(static_cast<int64>(Step.Until)))
	{
		OutError = TEXT("bot.action is not a bot action.");
		return false;
	}
	static const TSet<FString> Compares = {TEXT("=="), TEXT("!="), TEXT("<"), TEXT("<="), TEXT(">"), TEXT(">="), TEXT("contains")};
	if (!Compares.Contains(Step.Compare))
	{
		OutError = TEXT("bot.compare must be ==, !=, <, <=, >, >= or contains.");
		return false;
	}
	if (!FMath::IsFinite(Step.Seconds) || Step.Seconds < 0.f || Step.Seconds > 60.f
		|| !FMath::IsFinite(Step.Radius) || Step.Radius < 1.f || Step.Radius > 100000.f
		|| !FMath::IsFinite(Step.TimeoutSeconds) || Step.TimeoutSeconds < 0.1f || Step.TimeoutSeconds > 60.f
		|| Step.Location.ContainsNaN())
	{
		OutError = TEXT("bot.seconds must be 0 to 60, bot.radius 1 to 100000 cm, bot.timeoutSeconds 0.1 to 60, and bot.location finite.");
		return false;
	}
	const EHyperAIBotAction Checked = Step.Action == EHyperAIBotAction::WaitUntil ? Step.Until : Step.Action;
	auto Require = [&OutError, &Step](const bool bHas, const TCHAR* What)
	{
		if (!bHas)
		{
			OutError = FString::Printf(TEXT("%s needs %s."), *ActionName(Step.Action), What);
		}
		return bHas;
	};
	if (Step.Action == EHyperAIBotAction::WaitUntil && !Require(IsAssertion(Step.Until), TEXT("until set to AssertProperty, AssertNear, AssertOnScreen, AssertExists or AssertGone")))
	{
		return false;
	}
	switch (Step.Action)
	{
	case EHyperAIBotAction::Wait:
	case EHyperAIBotAction::Sample:
		if (!Require(Step.Seconds > 0.f, TEXT("seconds"))) return false;
		break;
	case EHyperAIBotAction::Hold:
		if (!Require(Step.Seconds > 0.f, TEXT("seconds")) || !Require(!Step.InputAction.IsEmpty(), TEXT("inputAction"))) return false;
		break;
	case EHyperAIBotAction::Press:
		if (!Require(!Step.InputAction.IsEmpty(), TEXT("inputAction"))) return false;
		break;
	case EHyperAIBotAction::Interact:
		if (!Require(!Step.Target.IsEmpty(), TEXT("target"))) return false;
		break;
	case EHyperAIBotAction::UiType:
		if (!Require(!Step.Text.IsEmpty(), TEXT("text"))) return false;
		// Every UI step needs a widget.
		[[fallthrough]];
	case EHyperAIBotAction::UiClick:
	case EHyperAIBotAction::UiAssert:
		if (!Require(!Step.Widget.IsEmpty(), TEXT("widget"))) return false;
		break;
	default:
		break;
	}
	if (Checked == EHyperAIBotAction::AssertProperty && !Require(!Step.Property.IsEmpty(), TEXT("property")))
	{
		return false;
	}
	if ((Checked == EHyperAIBotAction::AssertExists || Checked == EHyperAIBotAction::AssertGone || Checked == EHyperAIBotAction::AssertOnScreen)
		&& !Require(!Step.Target.IsEmpty(), TEXT("target")))
	{
		return false;
	}
	return true;
}

FHyperAIBotResult FHyperAIBotExecutor::Begin(const FHyperAIBotStep& InStep)
{
	using namespace HyperAIStudio::Bot::Private;
	FString Error;
	if (!Validate(InStep, Error))
	{
		return FResult::Failed(TEXT("invalid_step"), Error);
	}
	UWorld* W = World.Get();
	if (!W)
	{
		return FResult::Failed(TEXT("world_unavailable"), TEXT("The game world has ended."));
	}
	if (bRunning)
	{
		return FResult::Failed(TEXT("step_running"), TEXT("The previous step has not finished."));
	}
	Step = InStep;
	bRunning = true;
	StartedAt = W->GetRealTimeSeconds();
	LastCheck = FResult();
	FResult Result;
	switch (Step.Action)
	{
	case EHyperAIBotAction::Wait:
	case EHyperAIBotAction::WaitUntil:
		Result = Tick();
		break;
	case EHyperAIBotAction::MoveTo:
	{
		APawn* Pawn = PlayerPawn(*W);
		FVector Goal = Step.Location;
		AActor* Target = Step.Target.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Target));
		if (!Pawn)
		{
			Result = FResult::Failed(TEXT("no_player"), TEXT("There is no player pawn to move."));
			break;
		}
		if (!Step.Target.IsEmpty() && !Target)
		{
			Result = FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
			break;
		}
		if (Target)
		{
			Goal = Target->GetActorLocation();
		}
		Path.Reset();
		NextPoint = 0;
		if (UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W))
		{
			const UNavigationPath* Route = UNavigationSystemV1::FindPathToLocationSynchronously(W, Pawn->GetActorLocation(), Goal, Pawn);
			if (Route && Route->IsValid() && Route->PathPoints.Num() > 1)
			{
				Path = Route->PathPoints;
				Path.RemoveAt(0);
			}
		}
		if (Path.IsEmpty())
		{
			Path.Add(Goal);
		}
		BestDistance = FVector::Dist2D(Pawn->GetActorLocation(), Goal);
		LastProgressAt = StartedAt;
		Result = TickMove();
		break;
	}
	case EHyperAIBotAction::LookAt:
	{
		AActor* Target = Step.Target.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Target));
		if (!Step.Target.IsEmpty() && !Target)
		{
			Result = FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
			break;
		}
		const FVector Point = Target ? Target->GetActorLocation() : Step.Location;
		Result = Face(*W, Point)
			? FResult::Passed(FString::Printf(TEXT("facing %s"), Target ? *Describe(Target) : *Vec(Point)))
			: FResult::Failed(TEXT("no_player"), TEXT("There is no player controller to turn."));
		break;
	}
	case EHyperAIBotAction::Jump:
	{
		ACharacter* Character = Cast<ACharacter>(PlayerPawn(*W));
		if (!Character)
		{
			Result = FResult::Failed(TEXT("not_a_character"), TEXT("Only a Character can jump; the player pawn is not one."));
			break;
		}
		bLeftGround = false;
		StartZ = ApexZ = Character->GetActorLocation().Z;
		Character->Jump();
		break;
	}
	case EHyperAIBotAction::Press:
	case EHyperAIBotAction::Hold:
	case EHyperAIBotAction::Interact:
	{
		FString Code;
		UEnhancedInputLocalPlayerSubsystem* Input = InputFor(*W, Code, Error);
		if (!Input)
		{
			Result = FResult::Failed(Code, Error);
			break;
		}
		const UInputAction* Action = nullptr;
		FString Facing;
		if (Step.Action == EHyperAIBotAction::Interact)
		{
			const AActor* Target = Cast<AActor>(FindTarget(*W, Step.Target));
			if (!Target)
			{
				Result = FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
				break;
			}
			Face(*W, Target->GetActorLocation());
			const APawn* Pawn = PlayerPawn(*W);
			Facing = FString::Printf(TEXT(" facing %s at %.0f cm"), *Describe(Target),
				Pawn ? FVector::Dist(Pawn->GetActorLocation(), Target->GetActorLocation()) : 0.0);
			Action = Step.InputAction.IsEmpty() ? FindInteractAction() : FindInputAction(Step.InputAction);
			if (!Action)
			{
				Result = FResult::Failed(TEXT("no_interact_action"), Step.InputAction.IsEmpty()
					? TEXT("No loaded input action is named like Interact, Use or Activate; pass inputAction.")
					: FString::Printf(TEXT("No input action matches %s."), *Step.InputAction));
				break;
			}
		}
		else
		{
			Action = FindInputAction(Step.InputAction);
			if (!Action)
			{
				Result = FResult::Failed(TEXT("input_action_not_found"), FString::Printf(TEXT("No input action matches %s."), *Step.InputAction));
				break;
			}
		}
		ApplyMappings(*Input);
		if (Step.Action == EHyperAIBotAction::Hold)
		{
			// Held by injecting again every frame, as Enhanced Input's continuous injection would, but from the bot's
			// own tick, so it also works in worlds the editor loop does not tick.
			Input->InjectInputForAction(Action, InputValue(*Action, Step.Value), {}, {});
			Held = Action;
			break;
		}
		Input->InjectInputForAction(Action, InputValue(*Action, Step.Value), {}, {});
		Pressed = FString::Printf(TEXT("pressed %s%s"), *Action->GetName(), *Facing);
		// The injected input is read on the player's next input tick; the step ends after it.
		FramesLeft = 2;
		break;
	}
	case EHyperAIBotAction::Capture:
		Result = CaptureTo(FPaths::Combine(OutputDirectory, FString::Printf(TEXT("%s-%02d.png"), *SafeName(Step.Text, TEXT("capture")), ++Captures)));
		break;
	case EHyperAIBotAction::Survey:
	{
		APlayerController* Controller = W->GetFirstPlayerController();
		APawn* Pawn = PlayerPawn(*W);
		TMap<FString, int32> Classes;
		int32 Count = 0;
		for (TActorIterator<AActor> It(W); It; ++It)
		{
			++Count;
			FString Class = It->GetClass()->GetName();
			Class.RemoveFromEnd(TEXT("_C"));
			++Classes.FindOrAdd(Class);
		}
		Classes.ValueSort([](const int32 A, const int32 B) { return A > B; });
		TArray<FString> Census;
		for (const TPair<FString, int32>& Pair : Classes)
		{
			if (Census.Num() == 8)
			{
				break;
			}
			Census.Add(FString::Printf(TEXT("%s %d"), *Pair.Key, Pair.Value));
		}
		FString Detail = Pawn
			? FString::Printf(TEXT("player %s at %s facing yaw %.0f"), *Describe(Pawn), *Vec(Pawn->GetActorLocation()), Pawn->GetActorRotation().Yaw)
			: FString(TEXT("no player pawn"));
		if (Controller)
		{
			FVector Camera;
			FRotator View;
			Controller->GetPlayerViewPoint(Camera, View);
			Detail += FString::Printf(TEXT("; camera at %s, pitch %.0f yaw %.0f"), *Vec(Camera), View.Pitch, View.Yaw);
		}
		Detail += FString::Printf(TEXT("; %d actors: %s"), Count, *FString::Join(Census, TEXT(", ")));
		if (!Step.Target.IsEmpty())
		{
			const AActor* Target = Cast<AActor>(FindTarget(*W, Step.Target));
			if (!Target)
			{
				Detail += FString::Printf(TEXT("; no actor matches %s"), *Step.Target);
			}
			else if (Pawn)
			{
				const UNavigationPath* Route = UNavigationSystemV1::FindPathToLocationSynchronously(W, Pawn->GetActorLocation(), Target->GetActorLocation(), Pawn);
				Detail += FString::Printf(TEXT("; %s at %s, %.0f cm away, %s"), *Describe(Target), *Vec(Target->GetActorLocation()),
					FVector::Dist(Pawn->GetActorLocation(), Target->GetActorLocation()),
					Route && Route->IsValid() && !Route->IsPartial()
						? *FString::Printf(TEXT("reachable on the navmesh (path %.0f cm)"), Route->GetPathLength())
						: TEXT("with no complete navmesh path"));
			}
		}
		Result = FResult::Passed(Detail);
		break;
	}
	case EHyperAIBotAction::Sample:
	{
		const AActor* Target = Cast<AActor>(FindTarget(*W, Step.Target));
		Samples = 0;
		LastText.Reset();
		SampleStart = Target ? Target->GetActorLocation() : FVector::ZeroVector;
		Result = TickSample();
		break;
	}
	case EHyperAIBotAction::AssertProperty:
	case EHyperAIBotAction::AssertNear:
	case EHyperAIBotAction::AssertOnScreen:
	case EHyperAIBotAction::AssertExists:
	case EHyperAIBotAction::AssertGone:
		Result = Evaluate(Step.Action);
		break;
	case EHyperAIBotAction::RecordStart:
	{
		if (Recorder.IsValid())
		{
			Result = FResult::Failed(TEXT("already_recording"), TEXT("A recording is already running; RecordStop it first."));
			break;
		}
		RecordFolder = FPaths::Combine(OutputDirectory, SafeName(Step.Text, TEXT("recording")));
		RecordFrames = 0;
		const float Interval = Step.Seconds > 0.f ? Step.Seconds : 0.5f;
		Recorder = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
		{
			if (World.IsValid() && RecordFrames < HyperAIStudio::Bot::Private::MaxRecordFrames)
			{
				const int32 Frame = RecordFrames + 1;
				if (CaptureTo(FPaths::Combine(RecordFolder, FString::Printf(TEXT("frame_%04d.png"), Frame))).State == FResult::EState::Passed)
				{
					RecordFrames = Frame;
				}
			}
			return true;
		}), Interval);
		Result = FResult::Passed(FString::Printf(TEXT("recording a frame every %.2f s"), Interval), RecordFolder);
		break;
	}
	case EHyperAIBotAction::RecordStop:
		if (!Recorder.IsValid())
		{
			Result = FResult::Failed(TEXT("not_recording"), TEXT("No recording is running."));
			break;
		}
		StopRecording();
		Result = FResult::Passed(FString::Printf(TEXT("%d frames recorded"), RecordFrames), RecordFolder);
		break;
	case EHyperAIBotAction::UiClick:
	case EHyperAIBotAction::UiType:
	case EHyperAIBotAction::UiAssert:
	{
		UWidget* Widget = FindWidget(*W, Step.Widget);
		if (!Widget)
		{
			Result = FResult::Failed(TEXT("widget_not_found"), FString::Printf(TEXT("No UI widget on screen is named %s."), *Step.Widget));
			break;
		}
		if (Step.Action == EHyperAIBotAction::UiClick)
		{
			UButton* Button = Cast<UButton>(Widget);
			if (!Button)
			{
				Result = FResult::Failed(TEXT("not_a_button"), FString::Printf(TEXT("%s is a %s, not a button."), *Step.Widget, *Widget->GetClass()->GetName()));
				break;
			}
			if (!Button->GetIsEnabled() || !Button->IsVisible())
			{
				Result = FResult::Failed(TEXT("button_unavailable"), FString::Printf(TEXT("%s is disabled or hidden."), *Step.Widget));
				break;
			}
			// A Slate click on the button itself: the game's handlers run, and the OS cursor never moves.
			const TSharedPtr<SWidget> Slate = Button->GetCachedWidget();
			if (Slate.IsValid() && Slate->GetType() == TEXT("SButton"))
			{
				StaticCastSharedPtr<SButton>(Slate)->SimulateClick();
			}
			else
			{
				Button->OnClicked.Broadcast();
			}
			Result = FResult::Passed(FString::Printf(TEXT("clicked %s"), *Step.Widget));
			break;
		}
		if (Step.Action == EHyperAIBotAction::UiType)
		{
			const FText Typed = FText::FromString(Step.Text);
			if (UEditableTextBox* Box = Cast<UEditableTextBox>(Widget))
			{
				Box->SetText(Typed);
				Box->OnTextChanged.Broadcast(Typed);
				Box->OnTextCommitted.Broadcast(Typed, ETextCommit::OnEnter);
			}
			else if (UEditableText* Edit = Cast<UEditableText>(Widget))
			{
				Edit->SetText(Typed);
				Edit->OnTextChanged.Broadcast(Typed);
				Edit->OnTextCommitted.Broadcast(Typed, ETextCommit::OnEnter);
			}
			else if (UMultiLineEditableTextBox* Multi = Cast<UMultiLineEditableTextBox>(Widget))
			{
				Multi->SetText(Typed);
				Multi->OnTextChanged.Broadcast(Typed);
				Multi->OnTextCommitted.Broadcast(Typed, ETextCommit::OnEnter);
			}
			else
			{
				Result = FResult::Failed(TEXT("not_a_text_box"), FString::Printf(TEXT("%s is a %s, not a text box."), *Step.Widget, *Widget->GetClass()->GetName()));
				break;
			}
			Result = FResult::Passed(FString::Printf(TEXT("typed \"%s\" into %s"), *Step.Text, *Step.Widget));
			break;
		}
		if (!Widget->IsVisible() || !Widget->GetCachedWidget().IsValid())
		{
			Result = FResult::Failed(TEXT("not_visible"), FString::Printf(TEXT("%s exists but is not on screen."), *Step.Widget));
			break;
		}
		if (Step.Value.IsEmpty())
		{
			Result = FResult::Passed(FString::Printf(TEXT("%s is visible"), *Step.Widget));
			break;
		}
		FReading Shown;
		if (!ReadWidgetText(*Widget, Shown.Text))
		{
			Result = FResult::Failed(TEXT("no_text"), FString::Printf(TEXT("%s shows no text to compare."), *Step.Widget));
			break;
		}
		Shown.bOk = true;
		Shown.bNumeric = LexTryParseString(Shown.Number, *Shown.Text.TrimStartAndEnd());
		Result = Holds(Shown, Step.Compare, Step.Value)
			? FResult::Passed(FString::Printf(TEXT("%s shows \"%s\""), *Step.Widget, *Shown.Text))
			: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s shows \"%s\", expected %s %s"), *Step.Widget, *Shown.Text, *Step.Compare, *Step.Value));
		break;
	}
	default:
		Result = FResult::Failed(TEXT("unsupported_action"), TEXT("The bot has no route for this action."));
		break;
	}
	if (Result.IsDone())
	{
		bRunning = false;
	}
	return Result;
}

FHyperAIBotResult FHyperAIBotExecutor::Tick()
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	if (!bRunning)
	{
		return FResult::Failed(TEXT("no_step"), TEXT("No step is running."));
	}
	if (!W)
	{
		bRunning = false;
		return FResult::Failed(TEXT("world_unavailable"), TEXT("The game world has ended."));
	}
	const double Elapsed = W->GetRealTimeSeconds() - StartedAt;
	FResult Result;
	switch (Step.Action)
	{
	case EHyperAIBotAction::Wait:
		if (Elapsed >= Step.Seconds)
		{
			Result = FResult::Passed(FString::Printf(TEXT("waited %.1f s"), Elapsed));
		}
		break;
	case EHyperAIBotAction::WaitUntil:
	{
		const FResult Check = Evaluate(Step.Until);
		if (Check.State == FResult::EState::Passed)
		{
			Result = FResult::Passed(FString::Printf(TEXT("after %.1f s: %s"), Elapsed, *Check.Detail));
		}
		else if (Elapsed >= Step.TimeoutSeconds)
		{
			Result = FResult::Failed(TEXT("timed_out"), FString::Printf(TEXT("still not true after %.1f s: %s"), Elapsed, *Check.Detail));
		}
		break;
	}
	case EHyperAIBotAction::MoveTo:
		Result = TickMove();
		break;
	case EHyperAIBotAction::Jump:
	{
		ACharacter* Character = Cast<ACharacter>(PlayerPawn(*W));
		const UCharacterMovementComponent* Movement = Character ? Character->GetCharacterMovement() : nullptr;
		if (!Character || !Movement)
		{
			Result = FResult::Failed(TEXT("no_player"), TEXT("The player character is gone."));
			break;
		}
		ApexZ = FMath::Max(ApexZ, Character->GetActorLocation().Z);
		if (Movement->IsFalling())
		{
			bLeftGround = true;
		}
		else if (bLeftGround)
		{
			Character->StopJumping();
			Result = FResult::Passed(FString::Printf(TEXT("jumped %.0f cm high and landed after %.1f s"), ApexZ - StartZ, Elapsed));
			break;
		}
		if (Elapsed >= Step.TimeoutSeconds)
		{
			Character->StopJumping();
			Result = bLeftGround
				? FResult::Failed(TEXT("still_airborne"), FString::Printf(TEXT("left the ground but had not landed after %.1f s"), Elapsed))
				: FResult::Failed(TEXT("did_not_jump"), TEXT("The character never left the ground; jumping may be disabled or blocked."));
		}
		break;
	}
	case EHyperAIBotAction::Press:
	case EHyperAIBotAction::Interact:
		if (--FramesLeft <= 0)
		{
			Result = FResult::Passed(Pressed);
		}
		break;
	case EHyperAIBotAction::Hold:
		if (Elapsed >= Step.Seconds)
		{
			const FString Name = Held.IsValid() ? Held->GetName() : Step.InputAction;
			StopHold();
			Result = FResult::Passed(FString::Printf(TEXT("held %s for %.1f s"), *Name, Elapsed));
		}
		else
		{
			FString Code;
			FString Error;
			UEnhancedInputLocalPlayerSubsystem* Input = InputFor(*W, Code, Error);
			if (Input && Held.IsValid())
			{
				Input->InjectInputForAction(Held.Get(), InputValue(*Held, Step.Value), {}, {});
			}
		}
		break;
	case EHyperAIBotAction::Sample:
		Result = TickSample();
		break;
	default:
		Result = FResult::Failed(TEXT("not_running"), TEXT("This step finished when it began."));
		break;
	}
	if (Result.IsDone())
	{
		bRunning = false;
	}
	return Result;
}

FHyperAIBotResult FHyperAIBotExecutor::TickMove()
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	APawn* Pawn = W ? PlayerPawn(*W) : nullptr;
	if (!Pawn || Path.IsEmpty())
	{
		return FResult::Failed(TEXT("no_player"), TEXT("The player pawn is gone."));
	}
	// A moving target is followed to wherever it is now.
	if (!Step.Target.IsEmpty())
	{
		if (const AActor* Target = Cast<AActor>(FindTarget(*W, Step.Target)))
		{
			Path.Last() = Target->GetActorLocation();
		}
	}
	const FVector Here = Pawn->GetActorLocation();
	const FVector Goal = Path.Last();
	const double Distance = FVector::Dist2D(Here, Goal);
	const double Now = W->GetRealTimeSeconds();
	const FString Route = Path.Num() > 1 ? FString::Printf(TEXT(", along a %d-point navmesh path"), Path.Num()) : FString(TEXT(", straight"));
	if (Distance <= Step.Radius)
	{
		return FResult::Passed(FString::Printf(TEXT("reached %s in %.1f s, %.0f cm away%s"), *Vec(Goal), Now - StartedAt, Distance, *Route));
	}
	if (Distance < BestDistance - 10.0)
	{
		BestDistance = Distance;
		LastProgressAt = Now;
	}
	if (Now - LastProgressAt > StuckSeconds)
	{
		return FResult::Failed(TEXT("stuck"), FString::Printf(TEXT("no progress for %.0f s at %s, %.0f cm from %s%s; something blocks the way"),
			StuckSeconds, *Vec(Here), Distance, *Vec(Goal), *Route));
	}
	if (Now - StartedAt > Step.TimeoutSeconds)
	{
		return FResult::Failed(TEXT("timed_out"), FString::Printf(TEXT("still %.0f cm from %s after %.1f s%s"), Distance, *Vec(Goal), Now - StartedAt, *Route));
	}
	while (NextPoint < Path.Num() - 1 && FVector::Dist2D(Here, Path[NextPoint]) < WaypointReachedCm)
	{
		++NextPoint;
	}
	const FVector Direction = (Path[NextPoint] - Here).GetSafeNormal2D();
	Pawn->AddMovementInput(Direction, 1.f);
	if (AController* Controller = Pawn->GetController())
	{
		FRotator View = Controller->GetControlRotation();
		View.Yaw = Direction.Rotation().Yaw;
		Controller->SetControlRotation(View);
	}
	return FResult();
}

FHyperAIBotResult FHyperAIBotExecutor::TickSample()
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	UObject* Target = W ? FindTarget(*W, Step.Target) : nullptr;
	if (!Target)
	{
		return FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), Step.Target.IsEmpty() ? TEXT("Player") : *Step.Target));
	}
	double Value = 0.0;
	if (Step.Property.IsEmpty())
	{
		const AActor* Actor = Cast<AActor>(Target);
		Value = Actor ? Actor->GetVelocity().Size() : 0.0;
	}
	else
	{
		const FReading Reading = ReadProperty(Target, Step.Property);
		if (!Reading.bOk)
		{
			return FResult::Failed(TEXT("property_unreadable"), Reading.Error);
		}
		if (!Reading.bNumeric)
		{
			LastText = Reading.Text;
		}
		Value = Reading.Number;
	}
	if (Samples == 0)
	{
		Minimum = Maximum = FirstValue = Value;
	}
	Minimum = FMath::Min(Minimum, Value);
	Maximum = FMath::Max(Maximum, Value);
	LastValue = Value;
	++Samples;
	const double Elapsed = W->GetRealTimeSeconds() - StartedAt;
	if (Elapsed < Step.Seconds)
	{
		return FResult();
	}
	if (Step.Property.IsEmpty())
	{
		const AActor* Actor = Cast<AActor>(Target);
		return FResult::Passed(FString::Printf(TEXT("%s over %.1f s (%d samples): speed %.0f to %.0f cm/s, moved %.0f cm"),
			*Describe(Target), Elapsed, Samples, Minimum, Maximum, Actor ? FVector::Dist(SampleStart, Actor->GetActorLocation()) : 0.0));
	}
	if (!LastText.IsEmpty())
	{
		return FResult::Passed(FString::Printf(TEXT("%s.%s over %.1f s (%d samples): last \"%s\""), *Describe(Target), *Step.Property, Elapsed, Samples, *LastText));
	}
	return FResult::Passed(FString::Printf(TEXT("%s.%s over %.1f s (%d samples): first %g, last %g, min %g, max %g"),
		*Describe(Target), *Step.Property, Elapsed, Samples, FirstValue, LastValue, Minimum, Maximum));
}

FHyperAIBotResult FHyperAIBotExecutor::Evaluate(const EHyperAIBotAction Assertion)
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	if (!W)
	{
		return FResult::Failed(TEXT("world_unavailable"), TEXT("The game world has ended."));
	}
	const FString TargetName = Step.Target.IsEmpty() ? FString(TEXT("Player")) : Step.Target;
	UObject* Target = FindTarget(*W, Step.Target);
	switch (Assertion)
	{
	case EHyperAIBotAction::AssertGone:
		return Target
			? FResult::Failed(TEXT("still_present"), FString::Printf(TEXT("%s is still here%s"), *Describe(Target),
				Cast<AActor>(Target) ? *FString::Printf(TEXT(" at %s"), *Vec(Cast<AActor>(Target)->GetActorLocation())) : TEXT("")))
			: FResult::Passed(FString::Printf(TEXT("%s is gone"), *TargetName));
	case EHyperAIBotAction::AssertExists:
		return Target
			? FResult::Passed(FString::Printf(TEXT("%s exists"), *Describe(Target)))
			: FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *TargetName));
	default:
		break;
	}
	if (!Target)
	{
		return FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *TargetName));
	}
	if (Assertion == EHyperAIBotAction::AssertProperty)
	{
		const FReading Reading = ReadProperty(Target, Step.Property);
		if (!Reading.bOk)
		{
			return FResult::Failed(TEXT("property_unreadable"), Reading.Error);
		}
		return Holds(Reading, Step.Compare, Step.Value)
			? FResult::Passed(FString::Printf(TEXT("%s.%s is %s"), *Describe(Target), *Step.Property, *Reading.Text))
			: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s.%s is %s, expected %s %s"),
				*Describe(Target), *Step.Property, *Reading.Text, *Step.Compare, *Step.Value));
	}
	const AActor* Actor = Cast<AActor>(Target);
	if (!Actor)
	{
		return FResult::Failed(TEXT("not_an_actor"), FString::Printf(TEXT("%s has no place in the world."), *Describe(Target)));
	}
	if (Assertion == EHyperAIBotAction::AssertNear)
	{
		const AActor* Other = Step.Other.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Other));
		if (!Step.Other.IsEmpty() && !Other)
		{
			return FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Other));
		}
		const FVector To = Other ? Other->GetActorLocation() : Step.Location;
		const double Distance = FVector::Dist(Actor->GetActorLocation(), To);
		const FString What = Other ? Describe(Other) : Vec(To);
		return Distance <= Step.Radius
			? FResult::Passed(FString::Printf(TEXT("%s is %.0f cm from %s"), *Describe(Actor), Distance, *What))
			: FResult::Failed(TEXT("too_far"), FString::Printf(TEXT("%s is %.0f cm from %s, more than %.0f"), *Describe(Actor), Distance, *What, Step.Radius));
	}
	// AssertOnScreen: projected into the viewport, and nothing between the camera and it.
	APlayerController* Controller = W->GetFirstPlayerController();
	if (!Controller)
	{
		return FResult::Failed(TEXT("no_player"), TEXT("There is no player camera."));
	}
	FVector Center;
	FVector Extent;
	Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Center, Extent);
	int32 Width = 0;
	int32 Height = 0;
	Controller->GetViewportSize(Width, Height);
	FVector2D Screen;
	if (Width <= 0 || Height <= 0)
	{
		return FResult::Failed(TEXT("viewport_unavailable"), TEXT("The game has no viewport."));
	}
	if (!Controller->ProjectWorldLocationToScreen(Center, Screen, /*bPlayerViewportRelative=*/true)
		|| Screen.X < 0 || Screen.Y < 0 || Screen.X > Width || Screen.Y > Height)
	{
		return FResult::Failed(TEXT("off_screen"), FString::Printf(TEXT("%s is outside the camera's view"), *Describe(Actor)));
	}
	FVector Camera;
	FRotator View;
	Controller->GetPlayerViewPoint(Camera, View);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotOnScreen), /*bTraceComplex=*/true);
	Params.AddIgnoredActor(Controller->GetPawn());
	FHitResult Hit;
	if (W->LineTraceSingleByChannel(Hit, Camera, Center, ECC_Visibility, Params) && Hit.GetActor() && Hit.GetActor() != Actor
		&& !Hit.GetActor()->IsAttachedTo(Actor) && !Actor->IsAttachedTo(Hit.GetActor()))
	{
		return FResult::Failed(TEXT("hidden"), FString::Printf(TEXT("%s is on screen but behind %s"), *Describe(Actor), *Describe(Hit.GetActor())));
	}
	return FResult::Passed(FString::Printf(TEXT("%s is on screen at (%.0f, %.0f) of %dx%d"), *Describe(Actor), Screen.X, Screen.Y, Width, Height));
}

FHyperAIBotResult FHyperAIBotExecutor::CaptureTo(const FString& FilePath)
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	UGameViewportClient* Client = W ? W->GetGameViewport() : nullptr;
	FViewport* Viewport = Client ? Client->Viewport : nullptr;
	if (!Viewport)
	{
		return FResult::Failed(TEXT("viewport_unavailable"), TEXT("The game has no viewport to capture; a worker without rendering cannot."));
	}
	const FIntPoint Size = Viewport->GetSizeXY();
	TArray<FColor> Pixels;
	if (Size.X < 1 || Size.Y < 1 || !Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
	{
		return FResult::Failed(TEXT("pixel_read_failed"), TEXT("The game view could not be read."));
	}
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree=*/true);
	if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *FilePath))
	{
		return FResult::Failed(TEXT("write_failed"), FString::Printf(TEXT("Could not write %s."), *FilePath));
	}
	return FResult::Passed(FString::Printf(TEXT("%dx%d PNG"), Size.X, Size.Y), FPaths::ConvertRelativePathToFull(FilePath));
}

void FHyperAIBotExecutor::StopHold()
{
	// Held input is injected frame by frame, so stopping is not injecting any more.
	Held.Reset();
}

void FHyperAIBotExecutor::StopRecording()
{
	if (Recorder.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Recorder);
		Recorder.Reset();
	}
}

void FHyperAIBotExecutor::Stop()
{
	StopHold();
	StopRecording();
	if (bRunning && Step.Action == EHyperAIBotAction::Jump)
	{
		if (UWorld* W = World.Get())
		{
			if (ACharacter* Character = Cast<ACharacter>(HyperAIStudio::Bot::Private::PlayerPawn(*W)))
			{
				Character->StopJumping();
			}
		}
	}
	bRunning = false;
}
