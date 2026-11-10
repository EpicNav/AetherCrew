// Games by Hyper 2026.

#include "HyperAIStudioBot.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Components/Button.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/ProgressBar.h"
#include "Components/RichTextBlock.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Field/FieldSystemObjects.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GeometryCollectionProxyData.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioBotAimer.h"
#include "HyperAIStudioBotBrain.h"
#include "HyperAIStudioBotGameplay.h"
#include "HyperAIStudioBotSurveyor.h"
#include "HyperAIStudioBotShared.h"
#include "ImageUtils.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "TextureResource.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"
#include "Widgets/Input/SButton.h"
#if WITH_EDITOR
#include "ShaderCompiler.h"
#endif

IMPLEMENT_MODULE(FDefaultModuleImpl, HyperAIStudioBot)

namespace HyperAIStudio::Bot::Private
{
	constexpr int32 MaxRecordFrames = 240;
	constexpr double StuckSeconds = 3.0;
	constexpr double WaypointReachedCm = 75.0;
	/** An aimed press waits this long at most for the game's aim to come onto the target, as a person settles before firing. */
	constexpr double AimSettleSeconds = 0.5;
	/** AimAt aims this long unless the step says. */
	constexpr double AimSeconds = 0.5;
	/** AssertCount counts every match up to this. */
	constexpr int32 MaxCountedActors = 100000;

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

	APlayerController* PlayerController(UWorld& World, const int32 Index)
	{
		int32 At = 0;
		for (FConstPlayerControllerIterator It = World.GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* Controller = It->Get();
			if (Controller && At++ == Index)
			{
				return Controller;
			}
		}
		return nullptr;
	}

	APawn* PlayerPawn(UWorld& World, const int32 Index)
	{
		const APlayerController* Controller = PlayerController(World, Index);
		return Controller ? Controller->GetPawn() : nullptr;
	}

	bool IsPlayerName(const FString& Name, int32& OutIndex)
	{
		if (Name.Len() != 7 || !Name.StartsWith(TEXT("Player"), ESearchCase::IgnoreCase) || Name[6] < TEXT('0') || Name[6] > TEXT('7'))
		{
			return false;
		}
		OutIndex = Name[6] - TEXT('0');
		return true;
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

	UObject* FindTarget(UWorld& World, const FString& Name, const int32 PlayerIndex)
	{
		APlayerController* Controller = PlayerController(World, PlayerIndex);
		int32 Other = 0;
		if (IsPlayerName(Name, Other))
		{
			return PlayerPawn(World, Other);
		}
		if (Name.IsEmpty() || Name.Equals(TEXT("Player"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase))
		{
			return Controller ? Controller->GetPawn() : nullptr;
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

	TArray<AActor*> FindAll(UWorld& World, const FString& Name, const int32 PlayerIndex, const int32 Max)
	{
		TArray<AActor*> Found;
		int32 Index = 0;
		if (Name.IsEmpty() || IsPlayerName(Name, Index) || Name.Equals(TEXT("Player"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase))
		{
			if (AActor* Actor = Cast<AActor>(FindTarget(World, Name, PlayerIndex)))
			{
				Found.Add(Actor);
			}
			return Found;
		}
		const FName Tag(*Name);
		for (TActorIterator<AActor> It(&World); It && Found.Num() < Max; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			bool bMatch = Actor->GetName() == Name || Actor->ActorHasTag(Tag) || MatchesClass(*Actor, Name);
#if WITH_EDITOR
			bMatch = bMatch || Actor->GetActorLabel() == Name;
#endif
			if (bMatch)
			{
				Found.Add(Actor);
			}
		}
		return Found;
	}

	FReading Numeric(const double Number, const FString& Text)
	{
		FReading Reading;
		Reading.bOk = true;
		Reading.bNumeric = true;
		Reading.Number = Number;
		Reading.Text = Text;
		return Reading;
	}

	FProperty* FindProperty(const UStruct& Type, const FString& Name)
	{
		if (FProperty* Exact = FindFProperty<FProperty>(&Type, FName(*Name)))
		{
			return Exact;
		}
		// Blueprint variables can be authored under a friendlier name than their field.
		for (TFieldIterator<FProperty> It(&Type); It; ++It)
		{
			if (It->GetName().Equals(Name, ESearchCase::IgnoreCase) || It->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	UActorComponent* ComponentNamed(UObject& Object, const FString& Name)
	{
		if (const AActor* Owner = Cast<AActor>(&Object))
		{
			for (UActorComponent* Candidate : Owner->GetComponents())
			{
				if (Candidate && Candidate->GetName().Equals(Name, ESearchCase::IgnoreCase))
				{
					return Candidate;
				}
			}
		}
		return nullptr;
	}

	/** A const or pure function with no inputs that answers a number or a bool: calling it only reads. */
	bool IsPureReader(const UFunction& Function)
	{
		const FProperty* Return = Function.GetReturnProperty();
		return Return && Function.NumParms == 1 && Function.HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure)
			&& (CastField<FNumericProperty>(Return) || CastField<FBoolProperty>(Return));
	}

	FReading ReadValue(const FProperty& Property, const void* Value)
	{
		if (const FNumericProperty* Number = CastField<FNumericProperty>(&Property); Number && !Number->IsEnum())
		{
			const double Read = Number->IsFloatingPoint()
				? Number->GetFloatingPointPropertyValue(Value)
				: static_cast<double>(Number->GetSignedIntPropertyValue(Value));
			return Numeric(Read, Number->IsFloatingPoint() ? FString::SanitizeFloat(Read) : FString::Printf(TEXT("%lld"), static_cast<int64>(Read)));
		}
		// A gameplay attribute reads as its current value.
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
		{
			if (const FProperty* Current = FindFProperty<FProperty>(Struct->Struct, TEXT("CurrentValue")); Current && CastField<FNumericProperty>(Current))
			{
				return ReadValue(*Current, Current->ContainerPtrToValuePtr<void>(Value));
			}
		}
		FReading Out;
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		{
			Out.Text = Bool->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
		}
		else
		{
			Property.ExportTextItem_Direct(Out.Text, Value, nullptr, nullptr, PPF_None);
		}
		Out.bOk = true;
		return Out;
	}

	FReading CallReader(UObject& Object, UFunction& Function)
	{
		TArray<uint8> Parameters;
		Parameters.SetNumZeroed(FMath::Max<int32>(Function.ParmsSize, 1));
		Function.InitializeStruct(Parameters.GetData());
		Object.ProcessEvent(&Function, Parameters.GetData());
		const FProperty* Return = Function.GetReturnProperty();
		FReading Out = ReadValue(*Return, Return->ContainerPtrToValuePtr<void>(Parameters.GetData()));
		Function.DestroyStruct(Parameters.GetData());
		return Out;
	}

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
		const UStruct* Type = Root->GetClass();
		const void* Container = Root;
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			const FString& Part = Parts[Index];
			const bool bLast = Index == Parts.Num() - 1;
			// Components and reading functions belong to objects, not to the structs inside them.
			const bool bOnObject = Container == Object;
			FProperty* Property = FindProperty(*Type, Part);
			if (!Property && bOnObject && !bLast)
			{
				if (UActorComponent* Component = ComponentNamed(*Object, Part))
				{
					Object = Component;
					Type = Component->GetClass();
					Container = Component;
					continue;
				}
			}
			if (!Property && bOnObject && bLast)
			{
				if (UFunction* Function = Object->FindFunction(FName(*Part)); Function && IsPureReader(*Function))
				{
					return CallReader(*Object, *Function);
				}
			}
			if (!Property)
			{
				Out.Error = FString::Printf(TEXT("%s has no property, component or reading function named %s"), *Describe(Object), *Part);
				return Out;
			}
			const void* Value = Property->ContainerPtrToValuePtr<void>(Container);
			if (bLast)
			{
				return ReadValue(*Property, Value);
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			{
				Type = Struct->Struct;
				Container = Value;
				continue;
			}
			const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
			UObject* Next = ObjectProperty ? ObjectProperty->GetObjectPropertyValue(Value) : nullptr;
			if (!Next)
			{
				Out.Error = FString::Printf(TEXT("%s.%s is not set, or not an object or a struct"), *Describe(Object), *Part);
				return Out;
			}
			Object = Next;
			Type = Next->GetClass();
			Container = Next;
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
	bool Face(UWorld& World, const int32 PlayerIndex, const FVector& Point)
	{
		APlayerController* Player = PlayerController(World, PlayerIndex);
		APawn* Pawn = Player ? Player->GetPawn() : nullptr;
		AController* Controller = Pawn ? Pawn->GetController() : static_cast<AController*>(Player);
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

	UEnhancedInputLocalPlayerSubsystem* InputFor(UWorld& World, const int32 PlayerIndex, FString& OutCode, FString& OutError)
	{
		APlayerController* Controller = PlayerController(World, PlayerIndex);
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

	bool IsShown(const UWidget& Widget);

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
			// Or a widget of that class inside it, such as WBP_HUD.ProgressBar: the first one shown, else the first.
			UWidget* OfClass = nullptr;
			User->WidgetTree->ForEachWidget([&OfClass, &Inner](UWidget* Child)
			{
				FString ChildClass = Child ? Child->GetClass()->GetName() : FString();
				ChildClass.RemoveFromEnd(TEXT("_C"));
				if (Child && (ChildClass.Equals(Inner, ESearchCase::IgnoreCase) || ChildClass.Equals(TEXT("U") + Inner, ESearchCase::IgnoreCase))
					&& (!OfClass || (!IsShown(*OfClass) && IsShown(*Child))))
				{
					OfClass = Child;
				}
			});
			if (OfClass)
			{
				return OfClass;
			}
		}
		return nullptr;
	}

	bool IsShown(const UWidget& Widget)
	{
		// Visible itself and in every parent, up through the user widgets it sits in, to one on the screen.
		const UWidget* At = &Widget;
		while (At)
		{
			if (!At->IsVisible())
			{
				return false;
			}
			const UWidget* Parent = At->GetParent();
			if (!Parent)
			{
				// The root of a user widget's tree: carry on from the user widget that holds it.
				const UUserWidget* Holder = At->GetTypedOuter<UUserWidget>();
				if (const UUserWidget* User = Cast<UUserWidget>(At); User && User->IsInViewport())
				{
					return Widget.GetCachedWidget().IsValid();
				}
				if (Holder && Holder != At)
				{
					Parent = Holder;
				}
			}
			At = Parent;
		}
		return Widget.GetCachedWidget().IsValid();
	}

	bool ReadWidgetText(const UWidget& Widget, FString& OutText);

	/** UiAssert's check of a widget it found: shown (unless Property says Hidden), then what Property names compared with Value. */
	FResult CheckWidget(const UWidget& Widget, const FHyperAIBotStep& Step)
	{
		const FString Check = Step.Property.IsEmpty() ? FString(TEXT("Visible")) : Step.Property.TrimStartAndEnd();
		const FString Name = Step.Widget;
		const bool bShown = IsShown(Widget);
		if (Check.Equals(TEXT("Hidden"), ESearchCase::IgnoreCase))
		{
			return bShown ? FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s is on screen"), *Name))
				: FResult::Passed(FString::Printf(TEXT("%s is not on screen"), *Name));
		}
		if (!bShown)
		{
			return FResult::Failed(TEXT("not_visible"), FString::Printf(TEXT("%s (a %s) exists but is not on screen: it, or a widget it sits in, is hidden or not added to the viewport."),
				*Name, *Widget.GetClass()->GetName()));
		}
		FReading Shown;
		Shown.bOk = true;
		FString What;
		if (Check.Equals(TEXT("Visible"), ESearchCase::IgnoreCase) || Check.Equals(TEXT("Text"), ESearchCase::IgnoreCase))
		{
			if (Check.Equals(TEXT("Visible"), ESearchCase::IgnoreCase) && Step.Value.IsEmpty())
			{
				return FResult::Passed(FString::Printf(TEXT("%s is visible"), *Name));
			}
			if (!ReadWidgetText(Widget, Shown.Text))
			{
				return FResult::Failed(TEXT("no_text"), FString::Printf(TEXT("%s is a %s and shows no text to compare."), *Name, *Widget.GetClass()->GetName()));
			}
			Shown.bNumeric = LexTryParseString(Shown.Number, *Shown.Text.TrimStartAndEnd());
			What = TEXT("shows");
		}
		else if (Check.Equals(TEXT("Enabled"), ESearchCase::IgnoreCase) || Check.Equals(TEXT("Disabled"), ESearchCase::IgnoreCase))
		{
			const bool bWantEnabled = Check.Equals(TEXT("Enabled"), ESearchCase::IgnoreCase);
			return Widget.GetIsEnabled() == bWantEnabled
				? FResult::Passed(FString::Printf(TEXT("%s is %s"), *Name, Widget.GetIsEnabled() ? TEXT("enabled") : TEXT("disabled")))
				: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s is %s, expected %s"), *Name, Widget.GetIsEnabled() ? TEXT("enabled") : TEXT("disabled"), *Check.ToLower()));
		}
		else if (Check.Equals(TEXT("Focused"), ESearchCase::IgnoreCase))
		{
			const bool bFocused = Widget.HasAnyUserFocus() || Widget.HasKeyboardFocus() || Widget.HasFocusedDescendants();
			return bFocused ? FResult::Passed(FString::Printf(TEXT("%s has focus"), *Name))
				: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s does not have focus"), *Name));
		}
		else if (Check.Equals(TEXT("Percent"), ESearchCase::IgnoreCase) || Check.Equals(TEXT("Progress"), ESearchCase::IgnoreCase))
		{
			const UProgressBar* Bar = Cast<UProgressBar>(&Widget);
			if (!Bar)
			{
				return FResult::Failed(TEXT("not_a_progress_bar"), FString::Printf(TEXT("%s is a %s, not a progress bar."), *Name, *Widget.GetClass()->GetName()));
			}
			Shown.bNumeric = true;
			Shown.Number = Bar->GetPercent();
			Shown.Text = FString::Printf(TEXT("%g"), Shown.Number);
			What = TEXT("is filled to");
			if (Step.Value.IsEmpty())
			{
				return FResult::Passed(FString::Printf(TEXT("%s %s %s"), *Name, *What, *Shown.Text));
			}
		}
		else
		{
			return FResult::Failed(TEXT("invalid_step"), TEXT("UiAssert's property is Visible, Text, Enabled, Disabled, Focused, Hidden or Percent."));
		}
		return Holds(Shown, Step.Compare, Step.Value)
			? FResult::Passed(FString::Printf(TEXT("%s %s \"%s\""), *Name, *What, *Shown.Text))
			: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s %s \"%s\", expected %s %s"), *Name, *What, *Shown.Text, *Step.Compare, *Step.Value));
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
			|| Action == EHyperAIBotAction::AssertGone || Action == EHyperAIBotAction::AssertBroken
			|| Action == EHyperAIBotAction::AssertCount || Action == EHyperAIBotAction::AssertDistance
			|| Action == EHyperAIBotAction::AssertAttribute || Action == EHyperAIBotAction::AssertTag
			|| Action == EHyperAIBotAction::AssertAbility || Action == EHyperAIBotAction::AssertEffect;
	}

	TArray<FString> Words(const FString& Name)
	{
		TArray<FString> Out;
		FString Word;
		auto Flush = [&Out, &Word]()
		{
			if (!Word.IsEmpty())
			{
				Out.Add(Word.ToLower());
				Word.Reset();
			}
		};
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR Char = Name[Index];
			if (!FChar::IsAlnum(Char))
			{
				Flush();
				continue;
			}
			// A capital starts a word after a small letter, or before one: ComboAttack, ADSToggle.
			if (FChar::IsUpper(Char) && !Word.IsEmpty()
				&& (FChar::IsLower(Word[Word.Len() - 1]) || (Index + 1 < Name.Len() && FChar::IsLower(Name[Index + 1]))))
			{
				Flush();
			}
			Word.AppendChar(Char);
		}
		Flush();
		return Out;
	}

	bool LooksLikeHealth(const FString& Name)
	{
		static const TSet<FString> Not = {TEXT("max"), TEXT("maximum"), TEXT("regen"), TEXT("regeneration"), TEXT("rate"), TEXT("bar"),
			TEXT("widget"), TEXT("percent"), TEXT("pct"), TEXT("ratio"), TEXT("delay"), TEXT("damage"), TEXT("bonus"), TEXT("scale"),
			TEXT("multiplier"), TEXT("component"), TEXT("base"), TEXT("default"), TEXT("initial"), TEXT("start"), TEXT("starting"),
			TEXT("min"), TEXT("threshold"), TEXT("low"), TEXT("critical"), TEXT("set"), TEXT("on"), TEXT("changed"), TEXT("event"),
			TEXT("normalized"), TEXT("fraction"), TEXT("alpha")};
		const TArray<FString> Parts = Words(Name);
		bool bHealth = false;
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			if (Not.Contains(Parts[Index]))
			{
				return false;
			}
			bHealth |= Parts[Index] == TEXT("health") || Parts[Index] == TEXT("hp") || Parts[Index] == TEXT("hitpoints")
				|| (Parts[Index] == TEXT("hit") && Parts.IsValidIndex(Index + 1) && Parts[Index + 1] == TEXT("points"));
		}
		return bHealth;
	}

	FString FindHealth(AActor& Actor)
	{
		auto IsNumber = [](const FProperty* Property)
		{
			const FNumericProperty* Number = CastField<FNumericProperty>(Property);
			return Number && !Number->IsEnum();
		};
		auto On = [&IsNumber](UObject& Object, const FString& Prefix) -> FString
		{
			for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
			{
				if (!LooksLikeHealth(It->GetAuthoredName()))
				{
					continue;
				}
				if (IsNumber(*It))
				{
					return Prefix + It->GetAuthoredName();
				}
				// A gameplay attribute, or a small struct holding the number.
				if (const FStructProperty* Struct = CastField<FStructProperty>(*It))
				{
					for (const TCHAR* Inner : {TEXT("CurrentValue"), TEXT("Current"), TEXT("Value")})
					{
						if (IsNumber(FindFProperty<FProperty>(Struct->Struct, Inner)))
						{
							return Prefix + It->GetAuthoredName() + TEXT(".") + Inner;
						}
					}
				}
			}
			for (TFieldIterator<UFunction> It(Object.GetClass()); It; ++It)
			{
				if (LooksLikeHealth(It->GetName()) && IsPureReader(**It))
				{
					return Prefix + It->GetName();
				}
			}
			return FString();
		};
		FString Found = On(Actor, FString());
		for (UActorComponent* Component : Actor.GetComponents())
		{
			if (!Found.IsEmpty())
			{
				break;
			}
			if (Component)
			{
				Found = On(*Component, Component->GetName() + TEXT("."));
			}
		}
		return Found;
	}

	const TCHAR* RoleName(const ERole Role)
	{
		static const TCHAR* Names[] = {TEXT("move"), TEXT("look"), TEXT("jump"), TEXT("attack"), TEXT("aim"), TEXT("reload"),
			TEXT("crouch"), TEXT("sprint"), TEXT("dodge"), TEXT("interact"), TEXT("heal")};
		static_assert(UE_ARRAY_COUNT(Names) == static_cast<int32>(ERole::Count), "a name for every role");
		return Names[static_cast<int32>(Role)];
	}

	FControls ReadControls(UWorld& World, const int32 PlayerIndex)
	{
		FControls Out;
		APlayerController* Controller = PlayerController(World, PlayerIndex);
		const UEnhancedPlayerInput* Input = Controller ? Cast<UEnhancedPlayerInput>(Controller->PlayerInput) : nullptr;
		if (!Input)
		{
			Out.Summary = TEXT("none: the player has no Enhanced Input");
			return Out;
		}
		FString Code;
		FString Error;
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = InputFor(World, PlayerIndex, Code, Error))
		{
			ApplyMappings(*Subsystem);
		}
		TArray<const UInputAction*> Actions;
		TMap<const UInputAction*, TArray<FString>> Keys;
		for (const FEnhancedActionKeyMapping& Mapping : Input->GetEnhancedActionMappingsView())
		{
			if (Mapping.Action)
			{
				Actions.AddUnique(Mapping.Action);
				Keys.FindOrAdd(Mapping.Action).AddUnique(Mapping.Key.GetDisplayName(/*bLongDisplayName=*/false).ToString());
			}
		}
		// Roles by the words games name their actions with, the likeliest word first. Moving and looking are sticks.
		struct FRule
		{
			ERole Role;
			TArray<const TCHAR*> Words;
		};
		static const FRule Rules[] = {
			{ERole::Move, {TEXT("move"), TEXT("walk"), TEXT("locomotion")}},
			{ERole::Look, {TEXT("look"), TEXT("camera"), TEXT("turn")}},
			{ERole::Jump, {TEXT("jump")}},
			{ERole::Attack, {TEXT("fire"), TEXT("shoot"), TEXT("attack"), TEXT("primary"), TEXT("combo"), TEXT("melee"), TEXT("punch"),
				TEXT("swing"), TEXT("slash"), TEXT("strike")}},
			{ERole::Aim, {TEXT("aim"), TEXT("ads"), TEXT("zoom")}},
			{ERole::Reload, {TEXT("reload")}},
			{ERole::Crouch, {TEXT("crouch")}},
			{ERole::Sprint, {TEXT("sprint"), TEXT("run")}},
			{ERole::Dodge, {TEXT("dash"), TEXT("dodge"), TEXT("roll"), TEXT("evade")}},
			{ERole::Interact, {TEXT("interact"), TEXT("use"), TEXT("activate"), TEXT("pickup")}},
			{ERole::Heal, {TEXT("heal")}},
		};
		TMap<const UInputAction*, ERole> RoleOf;
		for (const FRule& Rule : Rules)
		{
			const bool bStick = Rule.Role == ERole::Move || Rule.Role == ERole::Look;
			for (const TCHAR* Word : Rule.Words)
			{
				for (const UInputAction* Action : Actions)
				{
					if (!Out.Roles[static_cast<int32>(Rule.Role)] && !RoleOf.Contains(Action)
						&& bStick == (Action->ValueType == EInputActionValueType::Axis2D) && Words(Action->GetName()).Contains(Word))
					{
						Out.Roles[static_cast<int32>(Rule.Role)] = Action;
						RoleOf.Add(Action, Rule.Role);
					}
				}
			}
		}
		TArray<FString> Lines;
		for (const UInputAction* Action : Actions)
		{
			const TArray<FString>& Bound = Keys[Action];
			FString KeyText = FString::Join(TArrayView<const FString>(Bound.GetData(), FMath::Min(Bound.Num(), 4)), TEXT(", "));
			if (Bound.Num() > 4)
			{
				KeyText += FString::Printf(TEXT(" +%d"), Bound.Num() - 4);
			}
			const ERole* Role = RoleOf.Find(Action);
			Lines.Add(FString::Printf(TEXT("%s%s = %s"), *Action->GetName(), Role ? *FString::Printf(TEXT(" (%s)"), RoleName(*Role)) : TEXT(""), *KeyText));
		}
		Out.Summary = Lines.IsEmpty() ? FString(TEXT("no actions mapped yet")) : FString::Join(Lines, TEXT(" | "));
		return Out;
	}

	// --- Destruction ------------------------------------------------------------------------------------------------

	/** The geometry collections of the actors matching Target, or of every actor when Target is empty. */
	TArray<UGeometryCollectionComponent*> GeometryCollections(UWorld& World, const FString& Target, const int32 PlayerIndex)
	{
		TArray<AActor*> Actors;
		if (Target.IsEmpty())
		{
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (IsValid(*It) && !It->IsActorBeingDestroyed()) Actors.Add(*It);
			}
		}
		else
		{
			Actors = FindAll(World, Target, PlayerIndex);
		}
		TArray<UGeometryCollectionComponent*> Out;
		for (AActor* Actor : Actors)
		{
			TInlineComponentArray<UGeometryCollectionComponent*> Components(Actor);
			for (UGeometryCollectionComponent* Component : Components)
			{
				if (Component && Component->GetRestCollection() && Component->GetRestCollection()->GetGeometryCollection().IsValid()) Out.Add(Component);
			}
		}
		return Out;
	}

	FBox BoundsOf(const TArray<UGeometryCollectionComponent*>& Components)
	{
		FBox Bounds(ForceInit);
		for (const UGeometryCollectionComponent* Component : Components)
		{
			Bounds += Component->Bounds.GetBox();
		}
		return Bounds;
	}

	/** The strain a Damage step applies when its Value is empty: twice the highest threshold any of them holds. */
	float StrainFor(const TArray<UGeometryCollectionComponent*>& Components)
	{
		float Highest = 0.f;
		for (UGeometryCollectionComponent* Component : Components)
		{
			for (const float Threshold : Component->GetDamageThreshold()) Highest = FMath::Max(Highest, Threshold);
			if (Component->bUseSizeSpecificDamageThreshold)
			{
				for (const FGeometryCollectionSizeSpecificData& Data : Component->GetRestCollection()->SizeSpecificData)
				{
					Highest = FMath::Max(Highest, static_cast<float>(Data.DamageThreshold));
				}
			}
		}
		return FMath::Max(2.f * Highest, 1000.f);
	}

	/**
	 * The "master field" way to break a collection: an external strain field, a sphere of Strain about At reaching every
	 * piece (Maximum resolution), not only the clusters whole at the moment; and a dynamic-state field over the same
	 * sphere, so kinematic or static pieces it frees fall instead of hanging where they were.
	 */
	void ApplyStrain(const TArray<TWeakObjectPtr<UGeometryCollectionComponent>>& Components, const FVector& At, const float Radius, const float Strain)
	{
		for (const TWeakObjectPtr<UGeometryCollectionComponent>& Weak : Components)
		{
			UGeometryCollectionComponent* Component = Weak.Get();
			if (!Component) continue;
			URadialFalloff* Falloff = NewObject<URadialFalloff>(Component);
			Falloff->SetRadialFalloff(Strain, 0.f, 1.f, 0.f, Radius, At, EFieldFalloffType::Field_FallOff_None);
			UFieldSystemMetaDataProcessingResolution* Resolution = NewObject<UFieldSystemMetaDataProcessingResolution>(Component);
			Resolution->ResolutionType = EFieldResolutionType::Field_Resolution_Maximum;
			Component->ApplyPhysicsField(true, EGeometryCollectionPhysicsTypeEnum::Chaos_ExternalClusterStrain, Resolution, Falloff);
			// Only inside the sphere: the culling field leaves every piece outside it as it was.
			URadialFalloff* Inside = NewObject<URadialFalloff>(Component);
			Inside->SetRadialFalloff(1.f, 0.f, 1.f, 0.f, Radius, At, EFieldFalloffType::Field_FallOff_None);
			UUniformInteger* Dynamic = NewObject<UUniformInteger>(Component);
			Dynamic->SetUniformInteger(static_cast<int32>(EObjectStateTypeEnum::Chaos_Object_Dynamic));
			UCullingField* Culled = NewObject<UCullingField>(Component);
			Culled->SetCullingField(Inside, Dynamic, EFieldCullingOperationType::Field_Culling_Outside);
			Component->ApplyPhysicsField(true, EGeometryCollectionPhysicsTypeEnum::Chaos_DynamicState, nullptr, Culled);
			// And straight onto the children of each cluster still whole and active, as the component's ApplyExternalStrain
			// does, measured from where their cluster is now.
			const UGeometryCollection* Rest = Component->GetRestCollection();
			FGeometryDynamicCollection* Live = Component->GetDynamicCollection();
			if (!Rest || !Rest->GetGeometryCollection().IsValid() || !Live) continue;
			const FGeometryCollection& Collection = *Rest->GetGeometryCollection();
			const int32 Num = Collection.NumElements(FGeometryCollection::TransformGroup);
			if (Live->GetNumTransforms() != Num) continue;
			FGeometryCollectionDynamicStateFacade State(*Live);
			if (!State.IsValid()) continue;
			for (int32 Index = 0; Index < Num; ++Index)
			{
				if (!Collection.Children[Index].IsEmpty() && State.IsActive(Index))
				{
					Component->ApplyExternalStrain(Index, At, Radius, /*PropagationDepth=*/0, /*PropagationFactor=*/1.f, Strain);
				}
			}
		}
	}

	/** How Components broke: pieces broken off, clusters broken, and what fell how far from where it began. */
	struct FBreakCount
	{
		int32 BrokenOff = 0;
		int32 Pieces = 0;
		int32 ClustersBroken = 0;
		int32 Clusters = 0;
		int32 Fell = 0;
		double Lowest = 0.0;
		double Farthest = 0.0;
		int32 NotSimulating = 0;
		TArray<TPair<double, FString>> Falls;
	};

	FBreakCount CountBreaks(const TArray<UGeometryCollectionComponent*>& Components)
	{
		FBreakCount Count;
		for (UGeometryCollectionComponent* Component : Components)
		{
			const FGeometryCollection& Rest = *Component->GetRestCollection()->GetGeometryCollection();
			const int32 Num = Rest.NumElements(FGeometryCollection::TransformGroup);
			FGeometryDynamicCollection* Dynamic = Component->GetDynamicCollection();
			if (!Dynamic || Dynamic->GetNumTransforms() != Num)
			{
				++Count.NotSimulating;
				continue;
			}
			FGeometryCollectionDynamicStateFacade State(*Dynamic);
			if (!State.IsValid())
			{
				++Count.NotSimulating;
				continue;
			}
			TArray<FTransform> RestGlobals;
			GeometryCollectionAlgo::GlobalMatrices(Rest.Transform, Rest.Parent, RestGlobals);
			const TArray<FTransform3f>& Now = Component->GetComponentSpaceTransforms3f();
			const FTransform ToWorld = Component->GetComponentTransform();
			const FString Owner = Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : Component->GetName();
			// A piece is off once it is a body of its own (active, no parent) or sits in a cluster the break made; kinematic or
			// anchored pieces count too, where the engine's HasBrokenOff counts only those that move.
			auto Released = [&State, &Rest, Dynamic](const int32 Index)
			{
				return Rest.Parent[Index] != INDEX_NONE
					&& ((State.IsActive(Index) && !Dynamic->GetHasParent(Index)) || State.HasInternalClusterParent(Index));
			};
			for (int32 Index = 0; Index < Num; ++Index)
			{
				const bool bLeaf = Rest.Children[Index].IsEmpty();
				if (Released(Index)) ++Count.BrokenOff;
				if (!bLeaf)
				{
					++Count.Clusters;
					for (const int32 Child : Rest.Children[Index])
					{
						if (Released(Child))
						{
							++Count.ClustersBroken;
							break;
						}
					}
					continue;
				}
				++Count.Pieces;
				const int32 Geometry = Rest.TransformToGeometryIndex[Index];
				if (Geometry == INDEX_NONE || !Now.IsValidIndex(Index)) continue;
				const FVector Centre = Rest.BoundingBox[Geometry].GetCenter();
				const FVector Was = ToWorld.TransformPosition(RestGlobals[Index].TransformPosition(Centre));
				const FVector Is = ToWorld.TransformPosition(FTransform(Now[Index]).TransformPosition(Centre));
				const double Drop = Was.Z - Is.Z;
				Count.Farthest = FMath::Max(Count.Farthest, FVector::Dist(Was, Is));
				if (Drop > 50.0)
				{
					++Count.Fell;
					Count.Lowest = FMath::Max(Count.Lowest, Drop);
					const FString Name = Rest.BoneName.IsValidIndex(Index) && !Rest.BoneName[Index].IsEmpty() ? Rest.BoneName[Index] : FString::Printf(TEXT("piece %d"), Index);
					Count.Falls.Emplace(Drop, FString::Printf(TEXT("%s %s"), *Owner, *Name));
				}
			}
		}
		Count.Falls.Sort([](const TPair<double, FString>& A, const TPair<double, FString>& B) { return A.Key > B.Key; });
		return Count;
	}

	FString DescribeBreaks(const FBreakCount& Count, const int32 Collections)
	{
		FString Text = FString::Printf(TEXT("%d piece(s) broken off in %d geometry collection(s) (%d leaves, %d of %d clusters broken)"), Count.BrokenOff, Collections,
			Count.Pieces, Count.ClustersBroken, Count.Clusters);
		if (Count.Fell > 0)
		{
			TArray<FString> What;
			for (int32 Index = 0; Index < Count.Falls.Num() && Index < 5; ++Index)
			{
				What.Add(FString::Printf(TEXT("%s %.0f cm"), *Count.Falls[Index].Value, Count.Falls[Index].Key));
			}
			Text += FString::Printf(TEXT("; %d fell more than 50 cm, the furthest down %.0f cm (%s%s)"), Count.Fell, Count.Lowest, *FString::Join(What, TEXT(", ")),
				Count.Falls.Num() > 5 ? TEXT(", ...") : TEXT(""));
		}
		else
		{
			Text += TEXT("; nothing fell more than 50 cm");
		}
		Text += FString::Printf(TEXT("; the farthest piece moved %.0f cm"), Count.Farthest);
		if (Count.NotSimulating > 0) Text += FString::Printf(TEXT("; %d not simulating (no physics state yet)"), Count.NotSimulating);
		return Text;
	}

	// --- Gameplay Ability System ------------------------------------------------------------------------------------

	/** AssertAttribute, AssertTag, AssertAbility and AssertEffect on Actor, read through its ability system without linking GAS. */
	FResult EvaluateGameplay(const EHyperAIBotAction Assertion, AActor& Actor, const FHyperAIBotStep& Step)
	{
		namespace Gameplay = HyperAIStudio::Bot::Gameplay;
		const FString Who = Describe(&Actor);
		if (Assertion == EHyperAIBotAction::AssertTag)
		{
			FGameplayTagContainer Tags;
			if (!Gameplay::ReadTags(Actor, Tags))
			{
				return FResult::Failed(TEXT("no_tags"), FString::Printf(TEXT("%s has no ability system and no gameplay tags of its own."), *Who));
			}
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Step.Property.TrimStartAndEnd()), /*ErrorIfNotFound=*/false);
			const bool bWantAbsent = Step.Value.Equals(TEXT("absent"), ESearchCase::IgnoreCase);
			const bool bHas = Tag.IsValid() && Tags.HasTag(Tag);
			const FString Owned = Tags.IsEmpty() ? FString(TEXT("no tags")) : Tags.ToStringSimple();
			if (!Tag.IsValid() && !bWantAbsent)
			{
				return FResult::Failed(TEXT("unknown_tag"), FString::Printf(TEXT("%s is not a registered gameplay tag; %s has %s"), *Step.Property, *Who, *Owned));
			}
			return bHas != bWantAbsent
				? FResult::Passed(FString::Printf(TEXT("%s %s %s; it has %s"), *Who, bHas ? TEXT("has") : TEXT("does not have"), *Step.Property, *Owned))
				: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s %s %s; it has %s"), *Who, bHas ? TEXT("has") : TEXT("does not have"), *Step.Property, *Owned));
		}
		UActorComponent* AbilitySystem = Gameplay::FindAbilitySystem(Actor);
		if (!AbilitySystem)
		{
			return FResult::Failed(TEXT("no_ability_system"), FString::Printf(TEXT("%s has no ability system component, on itself or its player state."), *Who));
		}
		if (Assertion == EHyperAIBotAction::AssertAttribute)
		{
			const FReading Reading = Gameplay::ReadAttribute(*AbilitySystem, Step.Property);
			if (!Reading.bOk)
			{
				return FResult::Failed(TEXT("attribute_not_found"), Reading.Error);
			}
			return Holds(Reading, Step.Compare, Step.Value)
				? FResult::Passed(FString::Printf(TEXT("%s's %s is %s"), *Who, *Step.Property, *Reading.Text))
				: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s's %s is %s, expected %s %s"), *Who, *Step.Property, *Reading.Text, *Step.Compare, *Step.Value));
		}
		if (Assertion == EHyperAIBotAction::AssertAbility)
		{
			const TArray<Gameplay::FAbility> Abilities = Gameplay::ReadAbilities(*AbilitySystem);
			const Gameplay::FAbility* Found = Abilities.FindByPredicate([&Step](const Gameplay::FAbility& Ability) { return Gameplay::NameMatches(Ability.Name, Step.Property); });
			TArray<FString> Granted;
			for (const Gameplay::FAbility& Ability : Abilities)
			{
				Granted.Add(Ability.ActiveCount > 0 ? Ability.Name + TEXT(" (active)") : Ability.Name);
			}
			const FString Has = Granted.IsEmpty() ? FString(TEXT("no abilities")) : FString::Join(Granted, TEXT(", "));
			const FString Want = Step.Value.IsEmpty() ? FString(TEXT("granted")) : Step.Value.ToLower();
			const FString State = !Found ? TEXT("absent") : Found->ActiveCount > 0 ? TEXT("active") : TEXT("inactive");
			const bool bOk = Want == TEXT("granted") ? Found != nullptr : Want == State;
			const FString Detail = FString::Printf(TEXT("%s on %s is %s%s; granted: %s"), *Step.Property, *Who, *State,
				Found ? *FString::Printf(TEXT(" (level %d)"), Found->Level) : TEXT(""), *Has);
			return bOk ? FResult::Passed(Detail) : FResult::Failed(TEXT("assertion_failed"), Detail + FString::Printf(TEXT("; expected %s"), *Want));
		}
		// AssertEffect: the matching active effects' stacks, inhibited ones aside.
		const TArray<Gameplay::FEffect> Effects = Gameplay::ReadEffects(*AbilitySystem);
		int32 Stacks = 0;
		int32 Inhibited = 0;
		TArray<FString> Active;
		for (const Gameplay::FEffect& Effect : Effects)
		{
			Active.Add(FString::Printf(TEXT("%s x%d%s"), *Effect.Name, Effect.Stacks, Effect.bInhibited ? TEXT(" (inhibited)") : TEXT("")));
			if (Gameplay::NameMatches(Effect.Name, Step.Property))
			{
				(Effect.bInhibited ? Inhibited : Stacks) += Effect.Stacks;
			}
		}
		const FString Want = Step.Value.IsEmpty() ? FString(TEXT("1")) : Step.Value;
		const FString Compare = Step.Value.IsEmpty() ? FString(TEXT(">=")) : Step.Compare;
		const FString Detail = FString::Printf(TEXT("%s on %s: %d stack(s)%s; active effects: %s"), *Step.Property, *Who, Stacks,
			Inhibited > 0 ? *FString::Printf(TEXT(", %d more inhibited"), Inhibited) : TEXT(""),
			Active.IsEmpty() ? TEXT("none") : *FString::Join(Active, TEXT(", ")));
		return Holds(Numeric(Stacks, FString::FromInt(Stacks)), Compare, Want)
			? FResult::Passed(Detail)
			: FResult::Failed(TEXT("assertion_failed"), Detail + FString::Printf(TEXT("; expected %s %s"), *Compare, *Want));
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
		|| !Bounded(Step.Widget, 256, TEXT("widget")) || !Bounded(Step.Text, 512, TEXT("text"))
		|| !Bounded(Step.HealthProperty, 256, TEXT("healthProperty")))
	{
		return false;
	}
	if (!StaticEnum<EHyperAIBotAction>()->IsValidEnumValue(static_cast<int64>(Step.Action))
		|| !StaticEnum<EHyperAIBotAction>()->IsValidEnumValue(static_cast<int64>(Step.Until)))
	{
		OutError = TEXT("bot.action is not a bot action.");
		return false;
	}
	if (!StaticEnum<EHyperAIBotGoal>()->IsValidEnumValue(static_cast<int64>(Step.Goal)))
	{
		OutError = TEXT("bot.goal is Reach, Collect, Defeat, Survive, Explore, Follow or Flee.");
		return false;
	}
	if (!StaticEnum<EHyperAIBotAim>()->IsValidEnumValue(static_cast<int64>(Step.Aim)))
	{
		OutError = TEXT("bot.aim is Off, Auto, Cursor, Stick or Look.");
		return false;
	}
	const bool bAims = Step.Action == EHyperAIBotAction::AimAt || Step.Action == EHyperAIBotAction::Press || Step.Action == EHyperAIBotAction::Hold
		|| Step.Action == EHyperAIBotAction::Interact || Step.Action == EHyperAIBotAction::Play;
	if (Step.Aim != EHyperAIBotAim::Off && !bAims)
	{
		OutError = TEXT("bot.aim works for AimAt, Press, Hold, Interact and Play.");
		return false;
	}
	if (!FMath::IsFinite(Step.Lead) || Step.Lead < 0.f || Step.Lead > 2.f || !FMath::IsFinite(Step.Tolerance) || Step.Tolerance < 0.f || Step.Tolerance > 90.f)
	{
		OutError = TEXT("bot.lead must be 0 to 2 seconds, and bot.tolerance 0 to 90 degrees.");
		return false;
	}
	const bool bAimed = Step.Action == EHyperAIBotAction::AimAt
		|| ((Step.Action == EHyperAIBotAction::Press || Step.Action == EHyperAIBotAction::Hold) && Step.Aim != EHyperAIBotAim::Off);
	if (bAimed && Step.Target.IsEmpty() && Step.Location.IsZero())
	{
		OutError = FString::Printf(TEXT("%s needs a target or a location to aim at."), *ActionName(Step.Action));
		return false;
	}
	if (Step.PlayerIndex < 0 || Step.PlayerIndex > 7 || !FMath::IsFinite(Step.Skill) || Step.Skill < 0.f || Step.Skill > 1.f
		|| !FMath::IsFinite(Step.Range) || Step.Range < 0.f || Step.Range > 100000.f)
	{
		OutError = TEXT("bot.playerIndex must be 0 to 7, bot.skill 0 to 1, and bot.range 0 to 100000 cm.");
		return false;
	}
	if (Step.bBackground && Step.Action != EHyperAIBotAction::Play && Step.Action != EHyperAIBotAction::MoveTo && Step.Action != EHyperAIBotAction::Hold)
	{
		OutError = TEXT("bot.bBackground works for Play, MoveTo and Hold.");
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
	if (Step.Action == EHyperAIBotAction::WaitUntil && !Require(IsAssertion(Step.Until) || Step.Until == EHyperAIBotAction::UiAssert,
		TEXT("until set to an Assert action (AssertProperty, AssertNear, AssertOnScreen, AssertExists, AssertGone, AssertBroken, AssertCount, AssertDistance, AssertAttribute, AssertTag, AssertAbility or AssertEffect) or UiAssert")))
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
	case EHyperAIBotAction::Play:
		if ((Step.Goal == EHyperAIBotGoal::Collect || Step.Goal == EHyperAIBotGoal::Defeat) && Step.Target.IsEmpty())
		{
			OutError = TEXT("Play Collect and Play Defeat need target.");
			return false;
		}
		if ((Step.Goal == EHyperAIBotGoal::Follow || Step.Goal == EHyperAIBotGoal::Flee) && Step.Target.IsEmpty())
		{
			OutError = TEXT("Play Follow and Play Flee need target.");
			return false;
		}
		if (Step.Goal != EHyperAIBotGoal::Reach && Step.Goal != EHyperAIBotGoal::Collect && Step.Goal != EHyperAIBotGoal::Defeat && Step.Seconds <= 0.f)
		{
			OutError = TEXT("Play Survive, Explore, Follow and Flee need seconds.");
			return false;
		}
		break;
	case EHyperAIBotAction::UiType:
		if (!Require(!Step.Text.IsEmpty(), TEXT("text"))) return false;
		// Every UI step needs a widget.
		[[fallthrough]];
	case EHyperAIBotAction::UiClick:
	case EHyperAIBotAction::UiAssert:
		if (!Require(!Step.Widget.IsEmpty(), TEXT("widget"))) return false;
		break;
	case EHyperAIBotAction::Damage:
	case EHyperAIBotAction::Fire:
	{
		if (!Require(!Step.Target.IsEmpty() || !Step.Location.IsZero(), TEXT("target or location"))) return false;
		double Number = 0.0;
		if (!Step.Value.IsEmpty() && (!LexTryParseString(Number, *Step.Value.TrimStartAndEnd()) || !FMath::IsFinite(Number) || Number < 0.0 || Number > 1.0e12))
		{
			OutError = FString::Printf(TEXT("%s's value is a number of 0 or more: %s."), *ActionName(Step.Action),
				Step.Action == EHyperAIBotAction::Damage ? TEXT("the strain") : TEXT("the speed in cm/s"));
			return false;
		}
		break;
	}
	default:
		break;
	}
	if (Checked == EHyperAIBotAction::AssertBroken)
	{
		double Number = 0.0;
		if (!Require(LexTryParseString(Number, *Step.Value.TrimStartAndEnd()), TEXT("value, the number of pieces to compare with")) || Step.Compare == TEXT("contains"))
		{
			if (Step.Compare == TEXT("contains")) OutError = TEXT("AssertBroken compares a count: ==, !=, <, <=, > or >=.");
			return false;
		}
	}
	if (Checked == EHyperAIBotAction::AssertProperty && !Require(!Step.Property.IsEmpty(), TEXT("property")))
	{
		return false;
	}
	auto IsNumber = [](const FString& Text)
	{
		double Number = 0.0;
		return LexTryParseString(Number, *Text.TrimStartAndEnd()) && FMath::IsFinite(Number);
	};
	const bool bCompareNumber = Step.Compare != TEXT("contains");
	switch (Checked)
	{
	case EHyperAIBotAction::AssertCount:
		if (!Require(!Step.Target.IsEmpty(), TEXT("target, the class, tag, label or name to count"))) return false;
		if (!IsNumber(Step.Value) || !bCompareNumber)
		{
			OutError = TEXT("AssertCount compares a count: value a number, compare ==, !=, <, <=, > or >=.");
			return false;
		}
		break;
	case EHyperAIBotAction::AssertDistance:
		if (!IsNumber(Step.Value) || !bCompareNumber)
		{
			OutError = TEXT("AssertDistance compares a distance in cm: value a number, compare ==, !=, <, <=, > or >=.");
			return false;
		}
		if (!Require(!Step.Other.IsEmpty() || !Step.Location.IsZero(), TEXT("other or location to measure to"))) return false;
		break;
	case EHyperAIBotAction::AssertAttribute:
		if (!Require(!Step.Property.IsEmpty(), TEXT("property, the attribute (Health, or HealthSet.Health)")) || !Require(!Step.Value.IsEmpty(), TEXT("value"))) return false;
		break;
	case EHyperAIBotAction::AssertTag:
		if (!Require(!Step.Property.IsEmpty(), TEXT("property, the gameplay tag"))) return false;
		if (!Step.Value.IsEmpty() && !Step.Value.Equals(TEXT("present"), ESearchCase::IgnoreCase) && !Step.Value.Equals(TEXT("absent"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("AssertTag's value is present or absent.");
			return false;
		}
		break;
	case EHyperAIBotAction::AssertAbility:
	{
		if (!Require(!Step.Property.IsEmpty(), TEXT("property, the ability's class"))) return false;
		static const TSet<FString> States = {TEXT(""), TEXT("granted"), TEXT("active"), TEXT("inactive"), TEXT("absent")};
		if (!States.Contains(Step.Value.ToLower()))
		{
			OutError = TEXT("AssertAbility's value is granted, active, inactive or absent.");
			return false;
		}
		break;
	}
	case EHyperAIBotAction::AssertEffect:
		if (!Require(!Step.Property.IsEmpty(), TEXT("property, the effect's class"))) return false;
		if ((!Step.Value.IsEmpty() && !IsNumber(Step.Value)) || !bCompareNumber)
		{
			OutError = TEXT("AssertEffect compares the stack count: value a number (>= 1 unless set), compare ==, !=, <, <=, > or >=.");
			return false;
		}
		break;
	case EHyperAIBotAction::UiAssert:
	{
		static const TSet<FString> Checks = {TEXT(""), TEXT("visible"), TEXT("text"), TEXT("enabled"), TEXT("disabled"), TEXT("focused"), TEXT("hidden"), TEXT("percent"), TEXT("progress")};
		if (!Checks.Contains(Step.Property.ToLower()))
		{
			OutError = TEXT("UiAssert's property is Visible, Text, Enabled, Disabled, Focused, Hidden or Percent.");
			return false;
		}
		if (!Require(!Step.Widget.IsEmpty(), TEXT("widget"))) return false;
		break;
	}
	default:
		break;
	}
	if (Step.Action == EHyperAIBotAction::SurveyLevel)
	{
		int32 Cells = 0;
		if (!Step.Value.IsEmpty() && (!LexTryParseString(Cells, *Step.Value.TrimStartAndEnd()) || Cells < HyperAIStudio::Bot::Survey::MinCells || Cells > HyperAIStudio::Bot::Survey::MaxCells))
		{
			OutError = FString::Printf(TEXT("SurveyLevel's value is the grid's cells a side, %d to %d."), HyperAIStudio::Bot::Survey::MinCells, HyperAIStudio::Bot::Survey::MaxCells);
			return false;
		}
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
	if (InStep.bBackground)
	{
		return StartBackground(InStep);
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
		APawn* Pawn = PlayerPawn(*W, Step.PlayerIndex);
		FVector Goal = Step.Location;
		AActor* Target = Step.Target.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex));
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
		AActor* Target = Step.Target.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex));
		if (!Step.Target.IsEmpty() && !Target)
		{
			Result = FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
			break;
		}
		const FVector Point = Target ? Target->GetActorLocation() : Step.Location;
		Result = Face(*W, Step.PlayerIndex, Point)
			? FResult::Passed(FString::Printf(TEXT("facing %s"), Target ? *Describe(Target) : *Vec(Point)))
			: FResult::Failed(TEXT("no_player"), TEXT("There is no player controller to turn."));
		break;
	}
	case EHyperAIBotAction::Jump:
	{
		ACharacter* Character = Cast<ACharacter>(PlayerPawn(*W, Step.PlayerIndex));
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
		UEnhancedInputLocalPlayerSubsystem* Input = InputFor(*W, Step.PlayerIndex, Code, Error);
		if (!Input)
		{
			Result = FResult::Failed(Code, Error);
			break;
		}
		const UInputAction* Action = nullptr;
		FString Facing;
		const AActor* AimActor = nullptr;
		if (Step.Action == EHyperAIBotAction::Interact)
		{
			const AActor* Target = Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex));
			if (!Target)
			{
				Result = FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
				break;
			}
			AimActor = Target;
			if (Step.Aim == EHyperAIBotAim::Off)
			{
				Face(*W, Step.PlayerIndex, Target->GetActorLocation());
			}
			const APawn* Pawn = PlayerPawn(*W, Step.PlayerIndex);
			Facing = FString::Printf(TEXT(" facing %s at %.0f cm"), *Describe(Target),
				Pawn ? FVector::Dist(Pawn->GetActorLocation(), Target->GetActorLocation()) : 0.0);
			// The interact action the player's controls map, or else a loaded one named the way games name it.
			Action = !Step.InputAction.IsEmpty() ? FindInputAction(Step.InputAction) : ReadControls(*W, Step.PlayerIndex).Get(ERole::Interact);
			Action = Action || !Step.InputAction.IsEmpty() ? Action : FindInteractAction();
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
			AimActor = Step.Aim == EHyperAIBotAim::Off || Step.Target.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex));
			if (Step.Aim != EHyperAIBotAim::Off && !Step.Target.IsEmpty() && !AimActor)
			{
				Result = FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
				break;
			}
		}
		ApplyMappings(*Input);
		if (Step.Aim != EHyperAIBotAim::Off)
		{
			// Aimed: the aim goes on first, and the press follows once the game's own aim is on the target.
			AimedAction = Action;
			Result = StartAim(AimActor, Step.Location);
			break;
		}
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
		APlayerController* Controller = PlayerController(*W, Step.PlayerIndex);
		APawn* Pawn = PlayerPawn(*W, Step.PlayerIndex);
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
		// What a player learns before playing: the controls, their own health, and who else is here.
		Detail += FString::Printf(TEXT("; controls: %s"), *ReadControls(*W, Step.PlayerIndex).Summary);
		if (Pawn)
		{
			const FString HealthPath = FindHealth(*Pawn);
			const FReading Health = HealthPath.IsEmpty() ? FReading() : ReadProperty(Pawn, HealthPath);
			Detail += Health.bOk ? FString::Printf(TEXT("; health %s = %s"), *HealthPath, *Health.Text) : FString(TEXT("; no health found on the player"));
		}
		TArray<TPair<double, FString>> Others;
		for (TActorIterator<APawn> It(W); It; ++It)
		{
			if (*It == Pawn || !IsValid(*It))
			{
				continue;
			}
			const double Distance = Pawn ? FVector::Dist(Pawn->GetActorLocation(), It->GetActorLocation()) : 0.0;
			const FString HealthPath = FindHealth(**It);
			const FReading Health = HealthPath.IsEmpty() ? FReading() : ReadProperty(*It, HealthPath);
			// In sight from the player's camera, as a player would see it, or behind something.
			bool bInSight = false;
			if (Controller)
			{
				FVector Eye;
				FRotator Unused;
				Controller->GetPlayerViewPoint(Eye, Unused);
				FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotSurvey), false, Pawn);
				FHitResult Hit;
				bInSight = !W->LineTraceSingleByChannel(Hit, Eye, It->GetActorLocation(), ECC_Pawn, Params) || Hit.GetActor() == *It;
			}
			Others.Emplace(Distance, FString::Printf(TEXT("%s%s %.0f cm, %s%s"), *Describe(*It), It->IsPlayerControlled() ? TEXT(" (a player)") : TEXT(""),
				Distance, bInSight ? TEXT("in sight") : TEXT("out of sight"), Health.bOk ? *FString::Printf(TEXT(", health %s"), *Health.Text) : TEXT("")));
		}
		Others.Sort([](const TPair<double, FString>& A, const TPair<double, FString>& B) { return A.Key < B.Key; });
		TArray<FString> Nearest;
		for (int32 Index = 0; Index < Others.Num() && Index < 6; ++Index)
		{
			Nearest.Add(Others[Index].Value);
		}
		if (!Nearest.IsEmpty())
		{
			Detail += FString::Printf(TEXT("; other pawns: %s"), *FString::Join(Nearest, TEXT(", ")));
		}
		if (W->GetNumPlayerControllers() > 1)
		{
			Detail += FString::Printf(TEXT("; %d players"), W->GetNumPlayerControllers());
		}
		if (!Step.Target.IsEmpty())
		{
			const AActor* Target = Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex));
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
		const AActor* Target = Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex));
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
		Result = CheckWidget(*Widget, Step);
		break;
	}
	case EHyperAIBotAction::Play:
		Brain = MakeUnique<FHyperAIBotBrain>(*W, Step, FPaths::Combine(OutputDirectory, FString::Printf(TEXT("play-%s-p%d-%02d.png"),
			*StaticEnum<EHyperAIBotGoal>()->GetNameStringByValue(static_cast<int64>(Step.Goal)).ToLower(), Step.PlayerIndex, ++Captures)));
		Result = Brain->Begin();
		break;
	case EHyperAIBotAction::AddPlayer:
		Result = AddPlayer();
		break;
	case EHyperAIBotAction::Damage:
	{
		TArray<UGeometryCollectionComponent*> Components = GeometryCollections(*W, Step.Target, Step.PlayerIndex);
		DamageAt = Step.Location.IsZero() && !Components.IsEmpty() ? BoundsOf(Components).GetCenter() : Step.Location;
		if (Step.Target.IsEmpty())
		{
			const double Reach = static_cast<double>(Step.Radius) * Step.Radius;
			Components.RemoveAll([this, Reach](const UGeometryCollectionComponent* Component) { return Component->Bounds.GetBox().ComputeSquaredDistanceToPoint(DamageAt) > Reach; });
		}
		if (Components.IsEmpty())
		{
			Result = FResult::Failed(TEXT("not_found"), Step.Target.IsEmpty()
				? FString::Printf(TEXT("No geometry collection is within %.0f cm of %s."), Step.Radius, *Vec(DamageAt))
				: FString::Printf(TEXT("No geometry collection matches %s."), *Step.Target));
			break;
		}
		double Strain = 0.0;
		DamageStrain = LexTryParseString(Strain, *Step.Value.TrimStartAndEnd()) ? static_cast<float>(Strain) : StrainFor(Components);
		Damaged.Reset();
		for (UGeometryCollectionComponent* Component : Components) Damaged.Add(Component);
		// Applied now and again each frame until Tick ends the step (a tenth of a second unless Seconds says), so clusters
		// a first break frees are reached too.
		ApplyStrain(Damaged, DamageAt, Step.Radius, DamageStrain);
		break;
	}
	case EHyperAIBotAction::Fire:
	{
		const TArray<UGeometryCollectionComponent*> Collections = Step.Target.IsEmpty() ? TArray<UGeometryCollectionComponent*>() : GeometryCollections(*W, Step.Target, Step.PlayerIndex);
		const AActor* Aimed = Step.Target.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex));
		if (!Step.Target.IsEmpty() && !Aimed)
		{
			Result = FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
			break;
		}
		FVector Origin;
		FVector Extent;
		if (Aimed) Aimed->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
		const FVector Aim = !Collections.IsEmpty() ? BoundsOf(Collections).GetCenter() : Aimed ? Origin : Step.Location;
		UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Sphere)
		{
			Result = FResult::Failed(TEXT("no_test_ball"), TEXT("The engine's sphere mesh (/Engine/BasicShapes/Sphere) is missing."));
			break;
		}
		FVector From = Aim + FVector(0.0, 0.0, Step.Range);
		FVector Velocity = FVector::ZeroVector;
		double Speed = 3000.0;
		LexTryParseString(Speed, *Step.Value.TrimStartAndEnd());
		if (Step.Range <= 0.f)
		{
			FVector Eye = Aim + FVector(-1000.0, 0.0, 200.0);
			FRotator Ignored;
			if (APlayerController* Controller = PlayerController(*W, Step.PlayerIndex))
			{
				Controller->GetPlayerViewPoint(Eye, Ignored);
			}
			const FVector Direction = (Aim - Eye).GetSafeNormal();
			// Clear of the player's own capsule.
			From = Eye + Direction * 150.0;
			Velocity = Direction * Speed;
		}
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Spawn.ObjectFlags |= RF_Transient;
		AStaticMeshActor* Ball = W->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(From), Spawn);
		UStaticMeshComponent* Mesh = Ball ? Ball->GetStaticMeshComponent() : nullptr;
		if (!Mesh)
		{
			Result = FResult::Failed(TEXT("spawn_failed"), TEXT("The test ball could not be spawned."));
			break;
		}
		Ball->SetMobility(EComponentMobility::Movable);
		Mesh->SetStaticMesh(Sphere);
		// The engine sphere is 100 cm across.
		Mesh->SetWorldScale3D(FVector(0.4));
		Mesh->SetCollisionProfileName(TEXT("PhysicsActor"));
		Mesh->SetSimulatePhysics(true);
		Mesh->SetMassOverrideInKg(NAME_None, 200.f, true);
		Mesh->SetNotifyRigidBodyCollision(true);
		Mesh->SetPhysicsLinearVelocity(Velocity);
		Ball->Tags.Add(TEXT("HyperAITestBall"));
		Result = FResult::Passed(Step.Range > 0.f
			? FString::Printf(TEXT("dropped a 40 cm, 200 kg test ball from %.0f cm above %s"), Step.Range, *Vec(Aim))
			: FString::Printf(TEXT("fired a 40 cm, 200 kg test ball from %s at %s, %.0f cm/s"), *Vec(From), *Vec(Aim), Speed));
		break;
	}
	case EHyperAIBotAction::AssertBroken:
	case EHyperAIBotAction::AssertCount:
	case EHyperAIBotAction::AssertDistance:
	case EHyperAIBotAction::AssertAttribute:
	case EHyperAIBotAction::AssertTag:
	case EHyperAIBotAction::AssertAbility:
	case EHyperAIBotAction::AssertEffect:
		Result = Evaluate(Step.Action);
		break;
	case EHyperAIBotAction::SurveyLevel:
		Surveyor = MakeUnique<FHyperAIBotSurveyor>(*W, Step, FPaths::Combine(OutputDirectory, SafeName(Step.Text, TEXT("survey"))));
		Result = Surveyor->Begin();
		break;
	case EHyperAIBotAction::AimAt:
	{
		const AActor* Target = Step.Target.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex));
		if (!Step.Target.IsEmpty() && !Target)
		{
			Result = FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Target));
			break;
		}
		Result = StartAim(Target, Step.Location);
		break;
	}
	default:
		Result = FResult::Failed(TEXT("unsupported_action"), TEXT("The bot has no route for this action."));
		break;
	}
	if (Result.IsDone())
	{
		bRunning = false;
		Brain.Reset();
		Surveyor.Reset();
		Aimer.Reset();
	}
	return WithBackground(Result);
}

double FHyperAIBotExecutor::AimToleranceDegrees() const
{
	return Step.Tolerance > 0.f ? Step.Tolerance : 10.0;
}

FHyperAIBotResult FHyperAIBotExecutor::StartAim(const AActor* Target, const FVector& Point)
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	if (!W)
	{
		return FResult::Failed(TEXT("world_unavailable"), TEXT("The game world has ended."));
	}
	AimedName = Target ? Describe(Target) : Vec(Point);
	PressedAt = 0.0;
	FramesSincePress = 0;
	Aimer = MakeUnique<FHyperAIBotAimer>(*W, Step.PlayerIndex);
	FString Code;
	FString Why;
	if (!Aimer->Start(Step.Aim == EHyperAIBotAim::Off ? EHyperAIBotAim::Auto : Step.Aim, Code, Why))
	{
		Aimer.Reset();
		return FResult::Failed(Code, Why);
	}
	Aimer->SetTarget(Target, Point, Step.Lead);
	return FResult();
}

FHyperAIBotResult FHyperAIBotExecutor::TickAimed()
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	const double Now = W ? W->GetRealTimeSeconds() : StartedAt;
	if (Step.Action == EHyperAIBotAction::AimAt)
	{
		const double Seconds = Step.Seconds > 0.f ? Step.Seconds : AimSeconds;
		// At least two frames of aim, so the game has read it; an aim that cannot be applied ends a second later.
		return Now - StartedAt < Seconds || (Aimer->Frames() < 2 && Now - StartedAt < Seconds + 1.0)
			? FResult() : FinishAimed(FString::Printf(TEXT("aimed at %s for %.1f s"), *AimedName, Now - StartedAt));
	}
	FString Code;
	FString Error;
	UEnhancedInputLocalPlayerSubsystem* Input = W ? InputFor(*W, Step.PlayerIndex, Code, Error) : nullptr;
	const UInputAction* Action = AimedAction.Get();
	if (!Input || !Action)
	{
		return FResult::Failed(Input ? TEXT("input_action_lost") : Code, Input ? TEXT("The input action was unloaded mid-step.") : Error);
	}
	if (PressedAt <= 0.0)
	{
		// A person fires once the aim is on the target, or after settling a moment.
		if (!Aimer->OnTarget(AimToleranceDegrees(), Step.Radius) && Now - StartedAt < AimSettleSeconds)
		{
			return FResult();
		}
		Input->InjectInputForAction(Action, InputValue(*Action, Step.Value), {}, {});
		PressedAt = Now;
		FramesSincePress = 0;
		return FResult();
	}
	++FramesSincePress;
	if (Step.Action == EHyperAIBotAction::Hold)
	{
		if (Now - PressedAt < Step.Seconds)
		{
			Input->InjectInputForAction(Action, InputValue(*Action, Step.Value), {}, {});
			return FResult();
		}
		return FinishAimed(FString::Printf(TEXT("held %s for %.1f s at %s"), *Action->GetName(), Now - PressedAt, *AimedName));
	}
	// The injected press is read on the player's next input tick; the aim stays on a frame after it.
	return FramesSincePress < 2 ? FResult()
		: FinishAimed(FString::Printf(TEXT("pressed %s at %s, %.2f s after the aim began"), *Action->GetName(), *AimedName, PressedAt - StartedAt));
}

FHyperAIBotResult FHyperAIBotExecutor::FinishAimed(const FString& What)
{
	using namespace HyperAIStudio::Bot::Private;
	FString Read;
	bool bRead = false;
	const bool bOn = Aimer->Check(AimToleranceDegrees(), Step.Radius, Read, bRead);
	const FString Report = Aimer->Report();
	const EHyperAIBotAim Mode = Aimer->GetMode();
	const bool bApplied = Aimer->Frames() > 0;
	if (Mode == EHyperAIBotAim::Cursor)
	{
		// The cursor stays where the step left it, as a hand rests on a mouse, until the bot stops.
		Aimer->Rest();
		RestingCursor = MoveTemp(Aimer);
	}
	Aimer.Reset();
	const FString Detail = FString::Printf(TEXT("%s; %s; %s"), *What, *Report, *Read);
	if (!bApplied)
	{
		return FResult::Failed(TEXT("aim_not_applied"), Detail + TEXT(". The aim never took hold; the report says what was in the way."));
	}
	if (bOn)
	{
		return FResult::Passed(Detail);
	}
	return FResult::Failed(TEXT("aim_off_target"), Detail + FString::Printf(TEXT(". The game aims elsewhere than the bot aimed by %s: try another aim (Cursor, Stick or Look)."),
		*HyperAIStudio::Bot::Aim::ModeName(Mode)));
}

FHyperAIBotResult FHyperAIBotExecutor::Tick()
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	TickBackground();
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
	switch (Aimer.IsValid() ? EHyperAIBotAction::AimAt : Step.Action)
	{
	case EHyperAIBotAction::AimAt:
		Result = Aimer.IsValid() ? TickAimed() : FResult::Failed(TEXT("aim_lost"), TEXT("The step's aim ended early."));
		break;
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
		ACharacter* Character = Cast<ACharacter>(PlayerPawn(*W, Step.PlayerIndex));
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
			UEnhancedInputLocalPlayerSubsystem* Input = InputFor(*W, Step.PlayerIndex, Code, Error);
			if (Input && Held.IsValid())
			{
				Input->InjectInputForAction(Held.Get(), InputValue(*Held, Step.Value), {}, {});
			}
		}
		break;
	case EHyperAIBotAction::Sample:
		Result = TickSample();
		break;
	case EHyperAIBotAction::Play:
		Result = Brain ? Brain->Tick() : FResult::Failed(TEXT("no_step"), TEXT("The player's brain has ended."));
		break;
	case EHyperAIBotAction::SurveyLevel:
		Result = Surveyor ? Surveyor->Tick() : FResult::Failed(TEXT("no_step"), TEXT("The survey has ended."));
		break;
	case EHyperAIBotAction::Damage:
		ApplyStrain(Damaged, DamageAt, Step.Radius, DamageStrain);
		if (Elapsed >= FMath::Max(Step.Seconds, 0.1f))
		{
			Result = FResult::Passed(FString::Printf(TEXT("strain %.0f within %.0f cm of %s on %d geometry collection(s) for %.1f s"), DamageStrain, Step.Radius, *Vec(DamageAt),
				Damaged.Num(), Elapsed));
		}
		break;
	default:
		Result = FResult::Failed(TEXT("not_running"), TEXT("This step finished when it began."));
		break;
	}
	if (Result.IsDone())
	{
		bRunning = false;
		Brain.Reset();
		Surveyor.Reset();
		Aimer.Reset();
	}
	return WithBackground(Result);
}

FHyperAIBotResult FHyperAIBotExecutor::TickMove()
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	APawn* Pawn = W ? PlayerPawn(*W, Step.PlayerIndex) : nullptr;
	if (!Pawn || Path.IsEmpty())
	{
		return FResult::Failed(TEXT("no_player"), TEXT("The player pawn is gone."));
	}
	// A moving target is followed to wherever it is now.
	if (!Step.Target.IsEmpty())
	{
		if (const AActor* Target = Cast<AActor>(FindTarget(*W, Step.Target, Step.PlayerIndex)))
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
	UObject* Target = W ? FindTarget(*W, Step.Target, Step.PlayerIndex) : nullptr;
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
	if (Assertion == EHyperAIBotAction::AssertBroken)
	{
		const TArray<UGeometryCollectionComponent*> Components = GeometryCollections(*W, Step.Target, Step.PlayerIndex);
		if (Components.IsEmpty())
		{
			return FResult::Failed(TEXT("not_found"), Step.Target.IsEmpty() ? FString(TEXT("The level has no geometry collection."))
				: FString::Printf(TEXT("No geometry collection matches %s."), *Step.Target));
		}
		const FBreakCount Count = CountBreaks(Components);
		FReading Reading;
		Reading.bOk = true;
		Reading.bNumeric = true;
		Reading.Number = Count.BrokenOff;
		Reading.Text = FString::FromInt(Count.BrokenOff);
		const FString Detail = DescribeBreaks(Count, Components.Num());
		return Holds(Reading, Step.Compare, Step.Value)
			? FResult::Passed(Detail)
			: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s; expected %s %s broken off"), *Detail, *Step.Compare, *Step.Value));
	}
	if (Assertion == EHyperAIBotAction::AssertCount)
	{
		const TArray<AActor*> Found = FindAll(*W, Step.Target, Step.PlayerIndex, MaxCountedActors);
		FReading Reading = Numeric(Found.Num(), FString::FromInt(Found.Num()));
		TArray<FString> Some;
		for (int32 Index = 0; Index < Found.Num() && Index < 6; ++Index)
		{
			Some.Add(Describe(Found[Index]));
		}
		const FString Detail = FString::Printf(TEXT("%d actor(s) match %s%s%s"), Found.Num(), *Step.Target, Some.IsEmpty() ? TEXT("") : TEXT(": "),
			*FString::Join(Some, TEXT(", "))) + (Found.Num() > Some.Num() ? TEXT(", ...") : TEXT(""));
		return Holds(Reading, Step.Compare, Step.Value)
			? FResult::Passed(Detail)
			: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s; expected %s %s"), *Detail, *Step.Compare, *Step.Value));
	}
	if (Assertion == EHyperAIBotAction::UiAssert)
	{
		const UWidget* Widget = FindWidget(*W, Step.Widget);
		return Widget ? CheckWidget(*Widget, Step)
			: FResult::Failed(TEXT("widget_not_found"), FString::Printf(TEXT("No UI widget is named %s, or of that class."), *Step.Widget));
	}
	const FString TargetName = Step.Target.IsEmpty() ? FString(TEXT("Player")) : Step.Target;
	UObject* Target = FindTarget(*W, Step.Target, Step.PlayerIndex);
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
	AActor* Actor = Cast<AActor>(Target);
	if (!Actor)
	{
		return FResult::Failed(TEXT("not_an_actor"), FString::Printf(TEXT("%s has no place in the world."), *Describe(Target)));
	}
	if (Assertion == EHyperAIBotAction::AssertDistance)
	{
		const AActor* Other = Step.Other.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Other, Step.PlayerIndex));
		if (!Step.Other.IsEmpty() && !Other)
		{
			return FResult::Failed(TEXT("not_found"), FString::Printf(TEXT("No actor matches %s."), *Step.Other));
		}
		const FVector To = Other ? Other->GetActorLocation() : Step.Location;
		const double Distance = FVector::Dist(Actor->GetActorLocation(), To);
		const FString Detail = FString::Printf(TEXT("%s is %.0f cm from %s"), *Describe(Actor), Distance, Other ? *Describe(Other) : *Vec(To));
		return Holds(Numeric(Distance, FString::Printf(TEXT("%.1f"), Distance)), Step.Compare, Step.Value)
			? FResult::Passed(Detail)
			: FResult::Failed(TEXT("assertion_failed"), FString::Printf(TEXT("%s, expected %s %s"), *Detail, *Step.Compare, *Step.Value));
	}
	if (Assertion == EHyperAIBotAction::AssertAttribute || Assertion == EHyperAIBotAction::AssertTag
		|| Assertion == EHyperAIBotAction::AssertAbility || Assertion == EHyperAIBotAction::AssertEffect)
	{
		return EvaluateGameplay(Assertion, *Actor, Step);
	}
	if (Assertion == EHyperAIBotAction::AssertNear)
	{
		const AActor* Other = Step.Other.IsEmpty() ? nullptr : Cast<AActor>(FindTarget(*W, Step.Other, Step.PlayerIndex));
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
	APlayerController* Controller = PlayerController(*W, Step.PlayerIndex);
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
	FIntPoint Size = Viewport ? Viewport->GetSizeXY() : FIntPoint::ZeroValue;
	TArray<FColor> Pixels;
	if (!Viewport)
	{
		// No screen, as in a verification copy: render what the player's camera sees instead.
		if (!RenderPlayerView(Pixels, Size))
		{
			return FResult::Failed(TEXT("viewport_unavailable"), TEXT("The game has no viewport, and no player camera to render instead."));
		}
	}
	else if (Size.X < 1 || Size.Y < 1 || !Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
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
	if (Brain)
	{
		Brain->Stop();
		Brain.Reset();
	}
	if (Surveyor)
	{
		Surveyor->Stop();
		Surveyor.Reset();
	}
	// Aims let go, and the game's window gets its own cursor back.
	Aimer.Reset();
	RestingCursor.Reset();
	for (const TUniquePtr<FHyperAIBotExecutor>& Child : Background)
	{
		Child->Stop();
	}
	Background.Reset();
	BackgroundNotes.Reset();
	// Unregistered while its world is still there; a world already gone took its scene with it.
	if (SceneCapture.IsValid() && SceneCapture->IsRegistered() && World.IsValid())
	{
		SceneCapture->UnregisterComponent();
	}
	SceneCapture.Reset();
	SceneTarget.Reset();
	if (bRunning && Step.Action == EHyperAIBotAction::Jump)
	{
		if (UWorld* W = World.Get())
		{
			if (ACharacter* Character = Cast<ACharacter>(HyperAIStudio::Bot::Private::PlayerPawn(*W, Step.PlayerIndex)))
			{
				Character->StopJumping();
			}
		}
	}
	bRunning = false;
}

FString FHyperAIBotExecutor::Label() const
{
	const FString What = Step.Action == EHyperAIBotAction::Play
		? StaticEnum<EHyperAIBotGoal>()->GetNameStringByValue(static_cast<int64>(Step.Goal)) : ActionName(Step.Action);
	return FString::Printf(TEXT("Player%d's %s"), Step.PlayerIndex, *What);
}

FString FHyperAIBotExecutor::Progress() const
{
	TArray<FString> Parts;
	if (Brain)
	{
		Parts.Add(Brain->Progress());
	}
	else if (bRunning)
	{
		Parts.Add(Label() + TEXT(" running"));
	}
	for (const TUniquePtr<FHyperAIBotExecutor>& Child : Background)
	{
		Parts.Add(Child->Progress());
	}
	return FString::Join(Parts, TEXT("; "));
}

FHyperAIBotResult FHyperAIBotExecutor::StartBackground(const FHyperAIBotStep& InStep)
{
	FHyperAIBotStep Own = InStep;
	Own.bBackground = false;
	TUniquePtr<FHyperAIBotExecutor> Child = MakeUnique<FHyperAIBotExecutor>(*World.Get(), OutputDirectory);
	// Its files are numbered apart from this executor's.
	Child->Captures = Captures + 100 * (Background.Num() + 1);
	const FHyperAIBotResult Started = Child->Begin(Own);
	if (Started.IsDone())
	{
		return Started;
	}
	const FString Name = Child->Label();
	Background.Add(MoveTemp(Child));
	return FHyperAIBotResult::Passed(FString::Printf(TEXT("%s runs in the background"), *Name));
}

void FHyperAIBotExecutor::TickBackground()
{
	for (int32 Index = 0; Index < Background.Num();)
	{
		const FHyperAIBotResult Result = Background[Index]->Tick();
		if (!Result.IsDone())
		{
			++Index;
			continue;
		}
		BackgroundNotes.Add(FString::Printf(TEXT("%s %s: %s%s"), *Background[Index]->Label(),
			Result.State == FHyperAIBotResult::EState::Passed ? TEXT("passed") : *Result.Code, *Result.Detail.Left(700),
			Result.File.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (map %s)"), *Result.File)));
		Background.RemoveAt(Index);
	}
}

FHyperAIBotResult FHyperAIBotExecutor::WithBackground(FHyperAIBotResult Result)
{
	if (!Result.IsDone() || (BackgroundNotes.IsEmpty() && Background.IsEmpty()))
	{
		return Result;
	}
	TArray<FString> Meanwhile = MoveTemp(BackgroundNotes);
	BackgroundNotes.Reset();
	for (const TUniquePtr<FHyperAIBotExecutor>& Child : Background)
	{
		Meanwhile.Add(Child->Progress());
	}
	Result.Detail += TEXT("; meanwhile ") + FString::Join(Meanwhile, TEXT("; "));
	return Result;
}

FHyperAIBotResult FHyperAIBotExecutor::AddPlayer()
{
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	UGameInstance* Instance = W ? W->GetGameInstance() : nullptr;
	if (!Instance)
	{
		return FResult::Failed(TEXT("no_game_instance"), TEXT("This world has no game for a player to join."));
	}
	const int32 Before = Instance->GetNumLocalPlayers();
	if (Before >= 4)
	{
		return FResult::Failed(TEXT("too_many_players"), TEXT("A game here takes up to 4 local players."));
	}
	const FPlatformUserId User = IPlatformInputDeviceMapper::Get().GetPlatformUserForUserIndex(Before);
	FString Error;
	ULocalPlayer* Joined = nullptr;
	if (Instance->GetGameViewportClient())
	{
		// Play In Editor: the new player gets its own part of the screen.
		Joined = Instance->CreateLocalPlayer(User, Error, /*bSpawnPlayerController=*/true);
	}
	else
	{
		// A copy with no screen: what CreateLocalPlayer does, without a viewport to split.
		Joined = NewObject<ULocalPlayer>(GEngine, GEngine->LocalPlayerClass);
		Instance->AddLocalPlayer(Joined, User);
		if (!Joined->SpawnPlayActor(FString(), Error, W))
		{
			Instance->RemoveLocalPlayer(Joined);
			Joined = nullptr;
		}
	}
	APlayerController* Controller = Joined ? Joined->GetPlayerController(W) : nullptr;
	if (!Controller)
	{
		return FResult::Failed(TEXT("join_failed"), TEXT("The player could not join: ") + (Error.IsEmpty() ? FString(TEXT("the game refused it")) : Error));
	}
	int32 Index = 0;
	for (FConstPlayerControllerIterator It = W->GetPlayerControllerIterator(); It && It->Get() != Controller; ++It)
	{
		++Index;
	}
	APawn* Pawn = Controller->GetPawn();
	if (!Pawn)
	{
		return FResult::Failed(TEXT("no_pawn"), FString::Printf(TEXT("Player%d joined, but the game mode gave it no pawn; the level may need another player start."), Index));
	}
	if (!Step.Location.IsZero())
	{
		// Standing on the ground at Location.
		float Radius = 34.f;
		float HalfHeight = 88.f;
		Pawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
		FVector Stand = Step.Location + FVector(0.0, 0.0, HalfHeight);
		FHitResult Ground;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotJoin), false, Pawn);
		if (W->LineTraceSingleByChannel(Ground, Step.Location + FVector(0.0, 0.0, 200.0), Step.Location - FVector(0.0, 0.0, 1000.0), ECC_Pawn, Params))
		{
			Stand = Ground.ImpactPoint + FVector(0.0, 0.0, HalfHeight + 2.0);
		}
		Pawn->SetActorLocation(Stand, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
	}
	Controller->SetControlRotation(Pawn->GetActorRotation());
	return FResult::Passed(FString::Printf(TEXT("Player%d joined as %s at %s; %d players"), Index, *Describe(Pawn),
		*Vec(Pawn->GetActorLocation()), W->GetNumPlayerControllers()));
}

bool FHyperAIBotExecutor::RenderPlayerView(TArray<FColor>& OutPixels, FIntPoint& OutSize)
{
	using namespace HyperAIStudio::Bot::Private;
	constexpr int32 Width = 960;
	constexpr int32 Height = 540;
	UWorld* W = World.Get();
	APlayerController* Controller = W ? PlayerController(*W, Step.PlayerIndex) : nullptr;
	if (!W || !W->Scene || !Controller)
	{
		return false;
	}
	FVector Location;
	FRotator Rotation;
	Controller->GetPlayerViewPoint(Location, Rotation);
	const bool bFirst = !SceneCapture.IsValid();
	if (bFirst)
	{
		SceneTarget.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient));
		SceneTarget->RenderTargetFormat = RTF_RGBA8;
		SceneTarget->ClearColor = FLinearColor::Black;
		SceneTarget->InitAutoFormat(Width, Height);
		SceneTarget->UpdateResourceImmediate(/*bClearRenderTarget=*/true);
		SceneCapture.Reset(NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient));
		SceneCapture->CaptureSource = SCS_FinalColorLDR;
		SceneCapture->TextureTarget = SceneTarget.Get();
		SceneCapture->bCaptureEveryFrame = false;
		SceneCapture->bCaptureOnMovement = false;
		// Exposure and temporal effects keep their history from one capture to the next.
		SceneCapture->bAlwaysPersistRenderingState = true;
		SceneCapture->RegisterComponentWithWorld(W);
	}
	SceneCapture->FOVAngle = Controller->PlayerCameraManager ? Controller->PlayerCameraManager->GetFOVAngle() : 90.f;
	SceneCapture->SetWorldLocationAndRotation(Location, Rotation);
#if WITH_EDITOR
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
#endif
	bool bRead = false;
	// The first view renders a few times over, so exposure and temporal history settle.
	for (int32 Pass = 0; Pass < (bFirst ? 4 : 1); ++Pass)
	{
		W->SendAllEndOfFrameUpdates();
		SceneCapture->CaptureScene();
		FTextureRenderTargetResource* Resource = SceneTarget->GameThread_GetRenderTargetResource();
		bRead = Resource && Resource->ReadPixels(OutPixels);
	}
	OutSize = FIntPoint(Width, Height);
	return bRead && OutPixels.Num() == Width * Height;
}
