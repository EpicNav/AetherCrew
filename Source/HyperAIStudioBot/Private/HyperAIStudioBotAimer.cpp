// Games by Hyper 2026.

#include "HyperAIStudioBotAimer.h"

#include "Components/ActorComponent.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedPlayerInput.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HyperAIStudioBotShared.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Input/CursorReply.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "Widgets/SViewport.h"

namespace HyperAIStudio::Bot::AimDetail
{
	using namespace HyperAIStudio::Bot::Private;
	using HyperAIStudio::Bot::Aim::FAimReading;

	const TCHAR* const ReaderNames[] = {TEXT("GetAimWorldLocation"), TEXT("GetAimDirection"), TEXT("GetAimDirectionFlat")};

	/** Every aimer holding an aim now, oldest first; the newest of a player is the one that acts. */
	TArray<FHyperAIBotAimer*>& Aimers()
	{
		static TArray<FHyperAIBotAimer*> List;
		return List;
	}

	/** A function that only reads: no inputs, and a vector out. */
	bool IsVectorReader(const UFunction& Function)
	{
		const FStructProperty* Return = CastField<FStructProperty>(Function.GetReturnProperty());
		return Return && Return->Struct == TBaseStructure<FVector>::Get() && Function.NumParms == 1
			&& Function.HasAnyFunctionFlags(FUNC_Const | FUNC_BlueprintPure);
	}

	FVector CallVectorReader(UObject& Object, UFunction& Function)
	{
		TArray<uint8> Parameters;
		Parameters.SetNumZeroed(FMath::Max<int32>(Function.ParmsSize, 1));
		Function.InitializeStruct(Parameters.GetData());
		Object.ProcessEvent(&Function, Parameters.GetData());
		const FVector Value = *Function.GetReturnProperty()->ContainerPtrToValuePtr<FVector>(Parameters.GetData());
		Function.DestroyStruct(Parameters.GetData());
		return Value;
	}

	FSceneViewport* ViewportOf(const APlayerController& Controller, UGameViewportClient** OutClient = nullptr)
	{
		const ULocalPlayer* Local = Controller.GetLocalPlayer();
		UGameViewportClient* Client = Local ? Local->ViewportClient.Get() : nullptr;
		if (OutClient)
		{
			*OutClient = Client;
		}
		return Client ? Client->GetGameViewport() : nullptr;
	}

	/** CommonInput's subsystem for the player, found by name so the bot does not depend on the plugin. */
	UObject* CommonInputOf(const APlayerController& Controller)
	{
		UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/CommonInput.CommonInputSubsystem"));
		const ULocalPlayer* Local = Controller.GetLocalPlayer();
		return Class && Local && Class->IsChildOf(ULocalPlayerSubsystem::StaticClass()) ? Local->GetSubsystemBase(Class) : nullptr;
	}

	/** CommonInput's GetCurrentInputType, by name: MouseAndKeyboard, Gamepad or Touch. */
	FString InputTypeOf(UObject& Subsystem)
	{
		UFunction* Function = Subsystem.FindFunction(TEXT("GetCurrentInputType"));
		const FProperty* Return = Function && Function->NumParms == 1 ? Function->GetReturnProperty() : nullptr;
		const FEnumProperty* Enum = CastField<FEnumProperty>(Return);
		if (!Enum)
		{
			return FString();
		}
		TArray<uint8> Parameters;
		Parameters.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
		Function->InitializeStruct(Parameters.GetData());
		Subsystem.ProcessEvent(Function, Parameters.GetData());
		const int64 Value = Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Enum->ContainerPtrToValuePtr<void>(Parameters.GetData()));
		Function->DestroyStruct(Parameters.GetData());
		return Enum->GetEnum()->GetNameStringByValue(Value);
	}

	/** Where a cursor is put on an actor: just above its feet, where games that put the cursor's aim on the floor find it, and still on its body for games that trace under the cursor. */
	FVector CursorPointOf(const AActor& Actor)
	{
		FVector Origin;
		FVector Extent;
		Actor.GetActorBounds(/*bOnlyCollidingComponents=*/true, Origin, Extent);
		if (Extent.IsNearlyZero())
		{
			Actor.GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
		}
		if (Extent.IsNearlyZero())
		{
			return Actor.GetActorLocation();
		}
		return FVector(Origin.X, Origin.Y, Origin.Z - Extent.Z + FMath::Min(30.0, Extent.Z * 0.3));
	}

	/** Where a view is turned on an actor: the middle of what it collides with. */
	FVector MiddleOf(const AActor& Actor)
	{
		FVector Origin;
		FVector Extent;
		Actor.GetActorBounds(/*bOnlyCollidingComponents=*/true, Origin, Extent);
		return Extent.IsNearlyZero() ? Actor.GetActorLocation() : Origin;
	}

	/**
	 * Whether FSceneViewport::OnMouseMove can be called without touching the real cursor. Its one line that does is
	 * FSlateApplication::SetCursorPos(MousePosBeforeHiddenDueToCapture), run while bCursorHiddenDueToCapture: the
	 * window captured the mouse with the game's cursor showing and hid it, and pins the hidden cursor where it was.
	 * That flag is private, but OnCursorQuery answers None first thing while it is set; its only other None is the
	 * game's own hidden cursor, ruled out by showing the game's cursor for the question and putting it back at once.
	 * When a game keeps its cursor hidden all the same, the flag can still only be set while the window has focus or
	 * capture: AcquireFocusAndCapture sets it with both, and OnFocusLost clears it.
	 */
	bool CursorMoveIsSafe(FSceneViewport& Viewport, UGameViewportClient& Client)
	{
		APlayerController* First = Client.GetGameInstance() ? Client.GetGameInstance()->GetFirstLocalPlayerController() : nullptr;
		const bool bWasShown = First && First->bShowMouseCursor;
		const TEnumAsByte<EMouseCursor::Type> WasShape = First ? First->CurrentMouseCursor : TEnumAsByte<EMouseCursor::Type>(EMouseCursor::Default);
		if (First)
		{
			First->bShowMouseCursor = true;
			if (WasShape == EMouseCursor::None)
			{
				First->CurrentMouseCursor = EMouseCursor::Default;
			}
		}
		const FGeometry& Geometry = Viewport.GetCachedGeometry();
		const FVector2D Corner(Geometry.GetAbsolutePosition());
		const FPointerEvent Probe(IPlatformInputDeviceMapper::Get().GetDefaultInputDevice(), FSlateApplication::CursorPointerIndex,
			Corner, Corner, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState());
		const FCursorReply Reply = Viewport.OnCursorQuery(Geometry, Probe);
		if (First)
		{
			First->bShowMouseCursor = bWasShown;
			First->CurrentMouseCursor = WasShape;
		}
		if (Reply.IsEventHandled() && Reply.GetCursorType() != EMouseCursor::None)
		{
			return true;
		}
		return !Viewport.HasFocus() && !Viewport.HasMouseCapture();
	}

	/** The right stick, as a pad reports it, through the player's own input. */
	void PushStick(APlayerController& Controller, const FVector2D& Tilt, const float DeltaSeconds)
	{
		FSceneViewport* Viewport = ViewportOf(Controller);
		const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(Controller.GetPlatformUserId());
		Controller.InputKey(FInputKeyEventArgs(Viewport, Device, EKeys::Gamepad_RightX, static_cast<float>(Tilt.X), DeltaSeconds, 1, 0));
		Controller.InputKey(FInputKeyEventArgs(Viewport, Device, EKeys::Gamepad_RightY, static_cast<float>(Tilt.Y), DeltaSeconds, 1, 0));
	}

	FString Pixel(const FIntPoint& Point)
	{
		return FString::Printf(TEXT("(%d, %d)"), Point.X, Point.Y);
	}
}

namespace HyperAIStudio::Bot::Aim
{
	using namespace HyperAIStudio::Bot::Private;

	FString ModeName(const EHyperAIBotAim Mode)
	{
		return StaticEnum<EHyperAIBotAim>()->GetNameStringByValue(static_cast<int64>(Mode)).ToLower();
	}

	FChoice Choose(const FEvidence& Evidence)
	{
		FChoice Choice;
		if (Evidence.InputType == TEXT("Gamepad") && !Evidence.StickAction.IsEmpty())
		{
			Choice.Mode = EHyperAIBotAim::Stick;
			Choice.Why = FString::Printf(TEXT("CommonInput says the player is on a gamepad, and %s reads the right stick"), *Evidence.StickAction);
			return Choice;
		}
		if (!Evidence.bViewport || Evidence.bSharedWindow)
		{
			const bool bStick = Evidence.bSharedWindow && !Evidence.StickAction.IsEmpty();
			Choice.Mode = bStick ? EHyperAIBotAim::Stick : EHyperAIBotAim::Look;
			Choice.Why = !Evidence.bViewport
				? FString(TEXT("this world has no window, so no cursor to aim with"))
				: FString::Printf(TEXT("this player shares the first player's window and its cursor%s"),
					bStick ? *FString::Printf(TEXT(", and %s reads the right stick"), *Evidence.StickAction) : TEXT(""));
			return Choice;
		}
		int32 Cursor = 0;
		int32 Look = 0;
		TArray<FString> Reasons;
		if (Evidence.bCursorShown)
		{
			Cursor += 2;
			Reasons.Add(TEXT("the game shows the mouse cursor (cursor +2)"));
		}
		else
		{
			Look += 1;
			Reasons.Add(TEXT("the mouse cursor is hidden (look +1)"));
		}
		if (!Evidence.Reader.IsEmpty())
		{
			Cursor += 1;
			Reasons.Add(FString::Printf(TEXT("the player has an aim of its own, %s (cursor +1)"), *Evidence.Reader));
		}
		if (Evidence.CameraPitch <= -35.0 && Evidence.CameraDistance >= 300.0)
		{
			Cursor += 3;
			Reasons.Add(FString::Printf(TEXT("the camera looks down at %.0f deg from %.0f cm (cursor +3)"), Evidence.CameraPitch, Evidence.CameraDistance));
		}
		else if (Evidence.CameraDistance < 1000.0)
		{
			Look += 1;
			Reasons.Add(FString::Printf(TEXT("the camera is close to the player, %.0f cm at %.0f deg (look +1)"), Evidence.CameraDistance, Evidence.CameraPitch));
		}
		if (Evidence.bMouseLooks)
		{
			Look += 2;
			Reasons.Add(FString::Printf(TEXT("Mouse XY drives %s (look +2)"), *Evidence.MouseAction));
		}
		Choice.Mode = Cursor > Look ? EHyperAIBotAim::Cursor : EHyperAIBotAim::Look;
		Choice.Why = FString::Printf(TEXT("cursor %d to look %d: %s"), Cursor, Look, *FString::Join(Reasons, TEXT(", ")));
		return Choice;
	}

	FEvidence Gather(UWorld& World, const int32 PlayerIndex)
	{
		using namespace HyperAIStudio::Bot::AimDetail;
		FEvidence Evidence;
		APlayerController* Controller = PlayerController(World, PlayerIndex);
		if (!Controller)
		{
			return Evidence;
		}
		APawn* Pawn = Controller->GetPawn();
		UGameViewportClient* Client = nullptr;
		const FSceneViewport* Viewport = ViewportOf(*Controller, &Client);
		Evidence.bViewport = Viewport && Viewport->GetSizeXY().X > 1 && Viewport->GetSizeXY().Y > 1;
		const UGameInstance* Instance = Client ? Client->GetGameInstance() : nullptr;
		Evidence.bSharedWindow = Instance && Instance->GetNumLocalPlayers() > 1 && Instance->GetFirstLocalPlayerController() != Controller;
		Evidence.bCursorShown = Controller->ShouldShowMouseCursor();
		if (Pawn)
		{
			const TArray<FAimReading> Readings = ReadGameAim(*Pawn);
			Evidence.Reader = Readings.IsEmpty() ? FString() : Readings[0].Source;
		}
		FVector Camera;
		FRotator View;
		Controller->GetPlayerViewPoint(Camera, View);
		Evidence.CameraPitch = FRotator::NormalizeAxis(View.Pitch);
		Evidence.CameraDistance = Pawn ? FVector::Dist(Camera, Pawn->GetActorLocation()) : 0.0;
		FString Code;
		FString Error;
		if (UEnhancedInputLocalPlayerSubsystem* Input = InputFor(World, PlayerIndex, Code, Error))
		{
			ApplyMappings(*Input);
		}
		if (const UEnhancedPlayerInput* Input = Cast<UEnhancedPlayerInput>(Controller->PlayerInput))
		{
			for (const FEnhancedActionKeyMapping& Mapping : Input->GetEnhancedActionMappingsView())
			{
				if (!Mapping.Action)
				{
					continue;
				}
				const FString Name = Mapping.Action->GetName();
				if (Evidence.MouseAction.IsEmpty() && (Mapping.Key == EKeys::Mouse2D || Mapping.Key == EKeys::MouseX || Mapping.Key == EKeys::MouseY))
				{
					Evidence.MouseAction = Name;
					const TArray<FString> Parts = Words(Name);
					Evidence.bMouseLooks = (Parts.Contains(TEXT("look")) || Parts.Contains(TEXT("camera")) || Parts.Contains(TEXT("turn")))
						&& !Parts.Contains(TEXT("aim")) && !Parts.Contains(TEXT("cursor"));
				}
				if (Evidence.StickAction.IsEmpty() && (Mapping.Key == EKeys::Gamepad_Right2D || Mapping.Key == EKeys::Gamepad_RightX || Mapping.Key == EKeys::Gamepad_RightY))
				{
					Evidence.StickAction = Name;
				}
			}
		}
		if (UObject* Common = CommonInputOf(*Controller))
		{
			Evidence.InputType = InputTypeOf(*Common);
		}
		return Evidence;
	}

	FVector2D StickToward(const double CameraYaw, const FVector& Direction)
	{
		const FVector Level = FVector(Direction.X, Direction.Y, 0.0).GetSafeNormal();
		if (Level.IsNearlyZero())
		{
			return FVector2D::ZeroVector;
		}
		const FRotator Yaw(0.0, CameraYaw, 0.0);
		const FVector Forward = Yaw.Vector();
		const FVector Right = FRotationMatrix(Yaw).GetScaledAxis(EAxis::Y);
		return FVector2D(FVector::DotProduct(Level, Right), FVector::DotProduct(Level, Forward));
	}

	FVector2D Nudge(const int32 Frame)
	{
		static const FVector2D Turns[] = {FVector2D(1.0, 0.0), FVector2D(0.0, 1.0), FVector2D(-1.0, 0.0), FVector2D(0.0, -1.0)};
		return Turns[((Frame % 4) + 4) % 4] * NudgePixels;
	}

	TArray<FAimReading> ReadGameAim(APawn& Pawn)
	{
		using namespace HyperAIStudio::Bot::AimDetail;
		TArray<FAimReading> Readings;
		TArray<UObject*> Objects;
		Objects.Add(&Pawn);
		for (UActorComponent* Component : Pawn.GetComponents())
		{
			if (Component)
			{
				Objects.Add(Component);
			}
		}
		for (UObject* Object : Objects)
		{
			for (const TCHAR* Name : ReaderNames)
			{
				UFunction* Function = Object->FindFunction(FName(Name));
				if (!Function || !IsVectorReader(*Function))
				{
					continue;
				}
				FAimReading& Reading = Readings.AddDefaulted_GetRef();
				Reading.Source = Object == &Pawn ? FString(Name) : FString::Printf(TEXT("%s.%s"), *Object->GetName(), Name);
				Reading.bPoint = FCString::Strcmp(Name, TEXT("GetAimWorldLocation")) == 0;
				Reading.Value = CallVectorReader(*Object, *Function);
			}
		}
		return Readings;
	}

	double OffBy(const FAimReading& Reading, const FVector& From, const FVector& Aimed)
	{
		if (Reading.bPoint)
		{
			return FVector::Dist2D(Reading.Value, Aimed);
		}
		const FVector Want = (Aimed - From).GetSafeNormal2D();
		const FVector Got = Reading.Value.GetSafeNormal2D();
		if (Want.IsNearlyZero())
		{
			return 0.0;
		}
		if (Got.IsNearlyZero())
		{
			return 180.0;
		}
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Want, Got), -1.0, 1.0)));
	}

	TArray<FView> Active(const UWorld& World)
	{
		TArray<FView> Views;
		for (const FHyperAIBotAimer* Aimer : HyperAIStudio::Bot::AimDetail::Aimers())
		{
			if (Aimer->GetWorld() != &World)
			{
				continue;
			}
			// The newest aimer of each player: later ones replace earlier ones.
			const FView View = Aimer->GetView();
			Views.RemoveAll([&View](const FView& Other) { return Other.PlayerIndex == View.PlayerIndex; });
			Views.Add(View);
		}
		return Views;
	}
}

FHyperAIBotAimer::FHyperAIBotAimer(UWorld& InWorld, const int32 InPlayerIndex)
	: World(&InWorld)
	, PlayerIndex(InPlayerIndex)
{
	LastView.PlayerIndex = InPlayerIndex;
}

FHyperAIBotAimer::~FHyperAIBotAimer()
{
	Stop();
}

bool FHyperAIBotAimer::Start(const EHyperAIBotAim Requested, FString& OutCode, FString& OutWhy)
{
	using namespace HyperAIStudio::Bot::Aim;
	using namespace HyperAIStudio::Bot::Private;
	UWorld* W = World.Get();
	APlayerController* Controller = W ? PlayerController(*W, PlayerIndex) : nullptr;
	if (!Controller || !Controller->GetPawn())
	{
		OutCode = TEXT("no_player");
		OutWhy = FString::Printf(TEXT("There is no Player%d with a pawn to aim."), PlayerIndex);
		return false;
	}
	const FEvidence Evidence = Gather(*W, PlayerIndex);
	if (Requested == EHyperAIBotAim::Off || Requested == EHyperAIBotAim::Auto)
	{
		const FChoice Choice = Choose(Evidence);
		Mode = Choice.Mode;
		Why = TEXT("auto, ") + Choice.Why;
	}
	else
	{
		Mode = Requested;
		Why = TEXT("as the step asked");
	}
	InputType = Evidence.InputType;
	if (Mode == EHyperAIBotAim::Cursor)
	{
		if (!Evidence.bViewport)
		{
			OutCode = TEXT("viewport_unavailable");
			OutWhy = TEXT("Cursor aim needs the game's window, and this world has none: verification copies have no screen. Use hyper_playtest_run, or aim Stick or Look.");
			return false;
		}
		if (Evidence.bSharedWindow)
		{
			OutCode = TEXT("cursor_shared");
			OutWhy = FString::Printf(TEXT("Player%d shares the first player's window, and its one cursor follows the first player's mouse. Aim Stick or Look for this player."), PlayerIndex);
			return false;
		}
		if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().IsMouseAttached())
		{
			OutCode = TEXT("no_mouse");
			OutWhy = TEXT("The engine reports no mouse attached to this machine, so a game reads no cursor position at all.");
			return false;
		}
	}
	TickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FHyperAIBotAimer::Apply);
	HyperAIStudio::Bot::AimDetail::Aimers().Add(this);
	bStarted = true;
	LastView.Mode = Mode;
	return true;
}

void FHyperAIBotAimer::SetTarget(const AActor* Actor, const FVector& InPoint, const float InLead)
{
	Target = Actor;
	bHasTarget = true;
	bHasActor = Actor != nullptr;
	Point = InPoint;
	Lead = InLead;
	bResting = false;
	Point = TargetPoint();
}

void FHyperAIBotAimer::Rest()
{
	if (Mode == EHyperAIBotAim::Stick && bStickPushed)
	{
		// A hand lets go of the stick: it springs back to the middle.
		if (UWorld* W = World.Get())
		{
			if (APlayerController* Controller = HyperAIStudio::Bot::Private::PlayerController(*W, PlayerIndex))
			{
				HyperAIStudio::Bot::AimDetail::PushStick(*Controller, FVector2D::ZeroVector, 0.f);
			}
		}
		bStickPushed = false;
	}
	bResting = true;
}

FVector FHyperAIBotAimer::TargetPoint() const
{
	using namespace HyperAIStudio::Bot::AimDetail;
	const AActor* Actor = Target.Get();
	if (!bHasActor || !IsValid(Actor) || Actor->IsActorBeingDestroyed())
	{
		return Point;
	}
	const FVector Ahead = Actor->GetVelocity() * Lead;
	switch (Mode)
	{
	case EHyperAIBotAim::Cursor:
		return CursorPointOf(*Actor) + Ahead;
	case EHyperAIBotAim::Look:
		return MiddleOf(*Actor) + Ahead;
	default:
		return Actor->GetActorLocation() + Ahead;
	}
}

bool FHyperAIBotAimer::IsCurrent() const
{
	const TArray<FHyperAIBotAimer*>& List = HyperAIStudio::Bot::AimDetail::Aimers();
	for (int32 Index = List.Num() - 1; Index >= 0; --Index)
	{
		if (List[Index]->World == World && List[Index]->PlayerIndex == PlayerIndex)
		{
			return List[Index] == this;
		}
	}
	return false;
}

void FHyperAIBotAimer::Apply(UWorld* InWorld, ELevelTick, const float DeltaSeconds)
{
	using namespace HyperAIStudio::Bot::Private;
	if (!bStarted || !bHasTarget || !InWorld || InWorld != World.Get() || !IsCurrent())
	{
		return;
	}
	APlayerController* Controller = PlayerController(*InWorld, PlayerIndex);
	if (!Controller)
	{
		return;
	}
	if (!bResting)
	{
		Point = TargetPoint();
	}
	const FVector Aimed = Point + Offset;
	bool bApplied = false;
	switch (Mode)
	{
	case EHyperAIBotAim::Cursor:
		bApplied = ApplyCursor(*Controller, Aimed, DeltaSeconds);
		break;
	case EHyperAIBotAim::Stick:
		if (!bResting)
		{
			ApplyStick(*Controller, Aimed, DeltaSeconds);
			bApplied = true;
		}
		break;
	default:
		if (!bResting)
		{
			ApplyLook(*Controller, Aimed);
			bApplied = true;
		}
		break;
	}
	if (bApplied && !bResting)
	{
		++Applied;
	}
	// What a recording draws: the aim, and the player it is aimed from, in the window's pixels.
	LastView.Mode = Mode;
	if (const FSceneViewport* Viewport = HyperAIStudio::Bot::AimDetail::ViewportOf(*Controller))
	{
		LastView.ViewportSize = Viewport->GetSizeXY();
		if (Mode == EHyperAIBotAim::Cursor && CursorPixel.X >= 0)
		{
			LastView.Point = FVector2D(CursorPixel);
			LastView.bPointOnScreen = true;
		}
		else
		{
			LastView.bPointOnScreen = Controller->ProjectWorldLocationToScreen(Aimed, LastView.Point, /*bPlayerViewportRelative=*/false);
		}
		const APawn* Pawn = Controller->GetPawn();
		LastView.bFromOnScreen = Pawn && Controller->ProjectWorldLocationToScreen(Pawn->GetActorLocation(), LastView.From, /*bPlayerViewportRelative=*/false);
	}
}

bool FHyperAIBotAimer::ApplyCursor(APlayerController& Controller, const FVector& Aimed, const float DeltaSeconds)
{
	using namespace HyperAIStudio::Bot::Aim;
	UGameViewportClient* Client = nullptr;
	FSceneViewport* Viewport = HyperAIStudio::Bot::AimDetail::ViewportOf(Controller, &Client);
	const FIntPoint Size = Viewport ? Viewport->GetSizeXY() : FIntPoint::ZeroValue;
	if (!Viewport || !Client || Size.X < 2 || Size.Y < 2)
	{
		return false;
	}
	WindowSize = Size;
	FIntPoint Wanted = CursorPixel;
	if (!bResting)
	{
		FVector2D Screen;
		if (!Controller.ProjectWorldLocationToScreen(Aimed, Screen, /*bPlayerViewportRelative=*/false))
		{
			// Behind the camera: there is no point on screen to put it on, so it stays where it is.
			++BehindFrames;
			return false;
		}
		const FVector2D Inside(FMath::Clamp(Screen.X, 1.0, Size.X - 2.0), FMath::Clamp(Screen.Y, 1.0, Size.Y - 2.0));
		if (!Inside.Equals(Screen, 0.5))
		{
			// Off screen: a person pushes the cursor to the edge toward it.
			++EdgeFrames;
		}
		const FVector2D At = Inside + Nudge(Applied);
		Wanted = FIntPoint(FMath::Clamp(FMath::RoundToInt32(At.X), 0, Size.X - 1), FMath::Clamp(FMath::RoundToInt32(At.Y), 0, Size.Y - 1));
	}
	if (Wanted.X < 0 || Wanted.Y < 0)
	{
		return false;
	}
	const FIntPoint Before(Viewport->GetMouseX(), Viewport->GetMouseY());
	if (!bTookCursor)
	{
		// Where the window's cursor was before the bot first took it, even when an earlier aim of this player still holds it.
		CursorBefore = Before;
		for (const FHyperAIBotAimer* Other : HyperAIStudio::Bot::AimDetail::Aimers())
		{
			if (Other != this && Other->World == World && Other->PlayerIndex == PlayerIndex && Other->bTookCursor)
			{
				CursorBefore = Other->CursorBefore;
				break;
			}
		}
	}
	if (!PlaceCursor(*Viewport, *Client, Wanted))
	{
		++HeldFrames;
		return false;
	}
	bTookCursor = true;
	CursorPixel = Wanted;
	const FIntPoint Now(Viewport->GetMouseX(), Viewport->GetMouseY());
	if (Now != Wanted && ReadbackNote.IsEmpty())
	{
		ReadbackNote = FString::Printf(TEXT("the window read the cursor back at %s, not %s"),
			*HyperAIStudio::Bot::AimDetail::Pixel(Now), *HyperAIStudio::Bot::AimDetail::Pixel(Wanted));
	}
	if (!bResting && Before.X >= 0 && Before.Y >= 0 && Now != Before)
	{
		// The motion a hand makes to put it there, for games that read the mouse's movement (MouseX, MouseY and with
		// them Mouse XY 2D-Axis). Up is positive, as FSceneViewport reports it.
		const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(Controller.GetPlatformUserId());
		Controller.InputKey(FInputKeyEventArgs(Viewport, Device, EKeys::MouseX, static_cast<float>(Now.X - Before.X), DeltaSeconds, 1, 0));
		Controller.InputKey(FInputKeyEventArgs(Viewport, Device, EKeys::MouseY, static_cast<float>(Before.Y - Now.Y), DeltaSeconds, 1, 0));
	}
	return true;
}

bool FHyperAIBotAimer::PlaceCursor(FSceneViewport& Viewport, UGameViewportClient& Client, const FIntPoint& Wanted)
{
	if (!FSlateApplication::IsInitialized() || !HyperAIStudio::Bot::AimDetail::CursorMoveIsSafe(Viewport, Client))
	{
		return false;
	}
	// The window's pixel as a desktop position through the viewport's own geometry, the inverse of what
	// FSceneViewport::UpdateCachedCursorPos does with it, so the cached position comes out as Wanted exactly.
	const FGeometry& Geometry = Viewport.GetCachedGeometry();
	const FVector2D Scale = UE::Slate::Viewport::GetViewportLocalToPixelScale(Geometry, FVector2D(Viewport.GetSizeXY()));
	const FVector2D Absolute(Geometry.LocalToAbsolute(FVector2D(Wanted) / Scale));
	const FPointerEvent Move(IPlatformInputDeviceMapper::Get().GetDefaultInputDevice(), FSlateApplication::CursorPointerIndex,
		Absolute, Absolute, TSet<FKey>(), EKeys::Invalid, 0.f, FModifierKeysState());
	Viewport.OnMouseMove(Geometry, Move);
	return true;
}

void FHyperAIBotAimer::ApplyStick(APlayerController& Controller, const FVector& Aimed, const float DeltaSeconds)
{
	FVector Camera;
	FRotator Facing;
	Controller.GetPlayerViewPoint(Camera, Facing);
	const APawn* Pawn = Controller.GetPawn();
	Stick = HyperAIStudio::Bot::Aim::StickToward(Facing.Yaw, Aimed - (Pawn ? Pawn->GetActorLocation() : Camera));
	StickYaw = Facing.Yaw;
	// Through PlayerController::InputKey, as the viewport passes a pad's stick on, once a frame: a stick's value
	// stays until the next sample, and two samples in a frame would add up.
	HyperAIStudio::Bot::AimDetail::PushStick(Controller, Stick, DeltaSeconds);
	bStickPushed = true;
}

void FHyperAIBotAimer::ApplyLook(APlayerController& Controller, const FVector& Aimed)
{
	const APawn* Pawn = Controller.GetPawn();
	FVector From;
	FRotator Unused;
	if (Pawn)
	{
		From = Pawn->GetPawnViewLocation();
	}
	else
	{
		Controller.GetPlayerViewPoint(From, Unused);
	}
	Controller.SetControlRotation((Aimed - From).Rotation());
}

bool FHyperAIBotAimer::OnTarget(const double ToleranceDegrees, const double ToleranceCm) const
{
	if (Applied < 2)
	{
		return false;
	}
	FString Unused;
	bool bRead = false;
	const bool bOk = Check(ToleranceDegrees, ToleranceCm, Unused, bRead);
	// With nothing of the game's to read, the bot's own aim is all there is: on, unless it is held off the target.
	return bRead ? bOk : Offset.Size() <= ToleranceCm;
}

bool FHyperAIBotAimer::Check(const double ToleranceDegrees, const double ToleranceCm, FString& OutDetail, bool& bOutRead) const
{
	using namespace HyperAIStudio::Bot::Aim;
	using namespace HyperAIStudio::Bot::Private;
	bOutRead = false;
	UWorld* W = World.Get();
	APawn* Pawn = W ? PlayerPawn(*W, PlayerIndex) : nullptr;
	if (!Pawn)
	{
		OutDetail = TEXT("the player has no pawn to read the aim from");
		return false;
	}
	const TArray<FAimReading> Readings = ReadGameAim(*Pawn);
	if (Readings.IsEmpty())
	{
		OutDetail = TEXT("the game has no GetAimWorldLocation, GetAimDirection or GetAimDirectionFlat to read its aim back from");
		return true;
	}
	bOutRead = true;
	const FVector From = Pawn->GetActorLocation();
	const FVector Aimed = TargetPoint();
	// Right under the player there is no bearing to compare.
	const bool bUnder = FVector::Dist2D(From, Aimed) < 60.0;
	bool bOk = true;
	TArray<FString> Parts;
	for (const FAimReading& Reading : Readings)
	{
		if (!Reading.bPoint && bUnder)
		{
			Parts.Add(FString::Printf(TEXT("%s not compared, the target is under the player"), *Reading.Source));
			continue;
		}
		// A cursor sets a point, so a point is measured in cm. A stick or a view sets only a heading, and a game puts
		// the point along it at a distance of its own, so only its bearing counts.
		const bool bDistance = Reading.bPoint && Mode == EHyperAIBotAim::Cursor;
		FAimReading Bearing = Reading;
		if (Reading.bPoint && !bDistance)
		{
			if (FVector::Dist2D(Reading.Value, From) < 1.0)
			{
				Parts.Add(FString::Printf(TEXT("%s not compared, it is on the player"), *Reading.Source));
				continue;
			}
			Bearing.bPoint = false;
			Bearing.Value = Reading.Value - From;
		}
		const double Off = OffBy(Bearing, From, Aimed);
		const bool bWithin = bDistance ? Off <= ToleranceCm : Off <= ToleranceDegrees;
		bOk &= bWithin;
		Parts.Add(bDistance
			? FString::Printf(TEXT("%s %s, %.0f cm off%s"), *Reading.Source, *Vec(Reading.Value), Off,
				bWithin ? TEXT("") : *FString::Printf(TEXT(", more than %.0f"), ToleranceCm))
			: FString::Printf(TEXT("%s%s %.1f deg off%s"), *Reading.Source, Reading.bPoint ? *(TEXT(" ") + Vec(Reading.Value) + TEXT(", bearing")) : TEXT(""), Off,
				bWithin ? TEXT("") : *FString::Printf(TEXT(", more than %.0f"), ToleranceDegrees)));
	}
	OutDetail = FString::Printf(TEXT("the game's own aim: %s"), *FString::Join(Parts, TEXT(", ")));
	return bOk;
}

FString FHyperAIBotAimer::Report() const
{
	using namespace HyperAIStudio::Bot::AimDetail;
	FString Out = FString::Printf(TEXT("aimed by %s (%s)"), *HyperAIStudio::Bot::Aim::ModeName(Mode), *Why);
	if (Mode == EHyperAIBotAim::Cursor)
	{
		Out += CursorPixel.X >= 0
			? FString::Printf(TEXT(", the window's cursor at %s of %dx%d over %d frames, moving %.1f px a frame round the target; the real cursor stayed put"),
				*Pixel(CursorPixel), WindowSize.X, WindowSize.Y, Applied, HyperAIStudio::Bot::Aim::NudgePixels * UE_SQRT_2)
			: FString(TEXT(", but the cursor was never placed"));
		if (EdgeFrames > 0)
		{
			Out += FString::Printf(TEXT("; the target was off screen for %d frames, so the cursor kept to the edge toward it"), EdgeFrames);
		}
		if (BehindFrames > 0)
		{
			Out += FString::Printf(TEXT("; the target was behind the camera for %d frames"), BehindFrames);
		}
		if (HeldFrames > 0)
		{
			Out += FString::Printf(TEXT("; for %d frames the game window had captured the real mouse with its cursor hidden, as after a click in it, so the bot left the cursor alone"), HeldFrames);
		}
		if (!ReadbackNote.IsEmpty())
		{
			Out += TEXT("; ") + ReadbackNote;
		}
	}
	else if (Mode == EHyperAIBotAim::Stick)
	{
		Out += FString::Printf(TEXT(", the right stick at (%.2f, %.2f) against camera yaw %.0f over %d frames"), Stick.X, Stick.Y, StickYaw, Applied);
		if (!InputType.IsEmpty() && InputType != TEXT("Gamepad"))
		{
			Out += FString::Printf(TEXT("; CommonInput still says %s, since the bot's stick does not pass through Slate and the bot does not switch it: CommonUI would hide the real cursor and move it. A game that picks its aim by CommonInput reads the mouse meanwhile"), *InputType);
		}
	}
	else
	{
		Out += FString::Printf(TEXT(", the view turned to %s"), *HyperAIStudio::Bot::Private::Vec(Point));
	}
	return Out;
}

void FHyperAIBotAimer::Stop()
{
	if (!bStarted)
	{
		return;
	}
	bStarted = false;
	FWorldDelegates::OnWorldPreActorTick.Remove(TickHandle);
	TickHandle.Reset();
	HyperAIStudio::Bot::AimDetail::Aimers().Remove(this);
	Restore();
}

void FHyperAIBotAimer::Restore()
{
	using namespace HyperAIStudio::Bot::AimDetail;
	UWorld* W = World.Get();
	APlayerController* Controller = W ? HyperAIStudio::Bot::Private::PlayerController(*W, PlayerIndex) : nullptr;
	if (!Controller)
	{
		return;
	}
	if (Mode == EHyperAIBotAim::Stick && bStickPushed)
	{
		PushStick(*Controller, FVector2D::ZeroVector, 0.f);
		bStickPushed = false;
	}
	if (Mode != EHyperAIBotAim::Cursor || !bTookCursor)
	{
		return;
	}
	// Another aimer of this player holds the cursor now; it puts it back when it lets go.
	for (const FHyperAIBotAimer* Other : Aimers())
	{
		if (Other->World == World && Other->PlayerIndex == PlayerIndex && Other->Mode == EHyperAIBotAim::Cursor)
		{
			return;
		}
	}
	UGameViewportClient* Client = nullptr;
	FSceneViewport* Viewport = ViewportOf(*Controller, &Client);
	if (!Viewport || !Client || !FSlateApplication::IsInitialized())
	{
		return;
	}
	// The window's cursor goes back to the real one when that is over the window, and otherwise to where it was.
	const FGeometry& Geometry = Viewport->GetCachedGeometry();
	const FVector2D Real(FSlateApplication::Get().GetCursorPos());
	const FVector2D Local(Geometry.AbsoluteToLocal(Real));
	const FVector2D LocalSize(Geometry.GetLocalSize());
	FIntPoint Back = CursorBefore;
	if (Local.X >= 0.0 && Local.Y >= 0.0 && Local.X < LocalSize.X && Local.Y < LocalSize.Y)
	{
		const FVector2D Scale = UE::Slate::Viewport::GetViewportLocalToPixelScale(Geometry, FVector2D(Viewport->GetSizeXY()));
		Back = FIntPoint(FMath::RoundToInt32(Local.X * Scale.X), FMath::RoundToInt32(Local.Y * Scale.Y));
	}
	PlaceCursor(*Viewport, *Client, Back);
	bTookCursor = false;
}
