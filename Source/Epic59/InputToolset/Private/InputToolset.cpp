// Copyright Epic Games, Inc. All Rights Reserved.

#include "InputToolset.h"

#include "AsyncFunctionResult/AsyncFunctionResultUtils.h"
#include "AsyncFunctionResult/AsyncFunctionResultVoid.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputKeyEventArgs.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/NotNull.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(InputToolset)

/// Waits DurationSeconds then runs OnTimeout (the release) and completes Result.
///
/// Also waits for at least one FWorldDelegates::OnWorldTickEnd on the target world before completing, even for
/// DurationSeconds == 0: that fires after actor tick, strictly after
/// TickForcedInput/ProcessActionMappingEvent has run, so a zero-duration tap is guaranteed to be
/// consumed at least once before release, regardless of ticker-vs-worldtick ordering within a frame.
///
/// Not in an anonymous namespace: UInputToolset needs to forward-declare it to track and cancel
/// pending watchers from Deinitialize.
class FDelayedHoldWatcher : public TSharedFromThis<FDelayedHoldWatcher>
{
public:
	static TWeakPtr<FDelayedHoldWatcher> Start(
		TNotNull<UAsyncFunctionResultVoid*> InResult,
		double InDurationSeconds,
		TNotNull<UWorld*> InTargetWorld,
		TFunction<void()> InOnTimeout)
	{
		TSharedRef<FDelayedHoldWatcher> Watcher = MakeShared<FDelayedHoldWatcher>();
		Watcher->Result = TStrongObjectPtr<UAsyncFunctionResultVoid>(InResult);
		Watcher->TargetWorld = InTargetWorld;
		Watcher->EndTime = FPlatformTime::Seconds() + InDurationSeconds;
		Watcher->OnTimeout = MoveTemp(InOnTimeout);
		Watcher->TickEndHandle = FWorldDelegates::OnWorldTickEnd.AddRaw(
			&Watcher.Get(), &FDelayedHoldWatcher::OnWorldTickEnd);
		Watcher->CoreTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[This = Watcher.ToSharedPtr()](float) mutable -> bool
			{
				const bool bContinue = This->OnTick();
				if (!bContinue)
				{
					This.Reset();
				}
				return bContinue;
			}));

		return Watcher;
	}

	/// Called by Deinitialize on any watcher that hasn't completed on its own yet.
	void Cancel(const FString& Reason)
	{
		FTSTicker::RemoveTicker(CoreTickerHandle);
		FWorldDelegates::OnWorldTickEnd.Remove(TickEndHandle);
		OnTimeout();
		Result->SetError(Reason);
	}

private:
	// Only TargetWorld's tick counts: other worlds (e.g. the editor world during PIE) can tick
	// independently, without the target PlayerController having processed this input at all.
	void OnWorldTickEnd(UWorld* World, ELevelTick, float)
	{
		if (World != TargetWorld.Get())
		{
			return;
		}
		bHasTickedSinceStart = true;
		FWorldDelegates::OnWorldTickEnd.Remove(TickEndHandle);
	}

	bool OnTick()
	{
		if (!TargetWorld.IsValid())
		{
			// The target world is gone (e.g. map/server travel): it will never tick again, so
			// waiting for bHasTickedSinceStart would hang this hold forever.
			Cancel(TEXT("The target world was destroyed while this hold was pending."));
			return false;
		}
		if (FPlatformTime::Seconds() < EndTime || !bHasTickedSinceStart)
		{
			return true;
		}

		OnTimeout();
		Result->SetCompleted();
		return false;
	}

	TStrongObjectPtr<UAsyncFunctionResultVoid> Result;
	TWeakObjectPtr<UWorld> TargetWorld;
	FTSTicker::FDelegateHandle CoreTickerHandle;
	FDelegateHandle TickEndHandle;
	bool bHasTickedSinceStart = false;
	TFunction<void()> OnTimeout;
	double EndTime = 0.0;
};

namespace
{
	/// Finds the local player to dispatch to: prefers a live PIE world, falls back to Game, skips
	/// worlds with no local player (e.g. a dedicated-server world), and returns null if none is live.
	ULocalPlayer* ResolveLocalPlayer()
	{
		if (!GEngine)
		{
			return nullptr;
		}

		ULocalPlayer* GameWorldLocalPlayer = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World)
			{
				continue;
			}

			ULocalPlayer* LocalPlayer = World->GetFirstLocalPlayerFromController();
			if (!LocalPlayer)
			{
				continue;
			}

			if (Context.WorldType == EWorldType::PIE)
			{
				return LocalPlayer;
			}
			if (Context.WorldType == EWorldType::Game && !GameWorldLocalPlayer)
			{
				GameWorldLocalPlayer = LocalPlayer;
			}
		}

		return GameWorldLocalPlayer;
	}

	/// Validates that every entry sets exactly one of Action or Key, calling ReportError with a
	/// message naming the first offending index otherwise. Returns true only if all entries are valid.
	bool ValidateEntries(const TArray<FInputToolsetEntry>& Entries, TFunctionRef<void(const FString&)> ReportError)
	{
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			const bool bHasAction = Entries[Index].Action != nullptr;
			const bool bHasKey = Entries[Index].Key.IsValid();
			if (bHasAction == bHasKey)
			{
				ReportError(FString::Printf(
					TEXT("Entries[%d] must set exactly one of Action or Key, but sets %s."),
					Index, bHasAction ? TEXT("both") : TEXT("neither")));
				return false;
			}
		}
		return true;
	}

	/// True if Entry's value is zero in every component the target actually reads: just X for a Key, or
	/// the leading N components of an Action's value for its ValueType (matching the same
	/// FMath::Max(1, int32(ValueType)) component count ProcessActionMappingEvent uses).
	bool HasZeroValue(const FInputToolsetEntry& Entry)
	{
		if (!Entry.Action)
		{
			return FMath::IsNearlyZero(Entry.Value.X);
		}

		const int32 NumComponents = FMath::Max(1, static_cast<int32>(Entry.Action->ValueType));
		for (int32 Component = 0; Component < NumComponents; ++Component)
		{
			if (!FMath::IsNearlyZero(Entry.Value[Component]))
			{
				return false;
			}
		}
		return true;
	}

	/// Injects Key on Input, simulated as coming from PC's primary device. Only needs the base
	/// UPlayerInput API (InputKey), so this works whether or not Enhanced Input is present.
	void InjectLegacyKeyEvent(
		const APlayerController& PC, UPlayerInput& Input, const FKey& Key, EInputEvent Event, float AxisValue)
	{
		const FInputDeviceId DeviceToSimulate =
			IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(PC.GetPlatformUserId());

		FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(
			Key, Event, AxisValue, Key.IsAnalog() ? 1 : 0, DeviceToSimulate);
		Args.DeltaTime = PC.GetWorld() ? PC.GetWorld()->GetDeltaSeconds() : (1.0f / 60.0f);

		Input.InputKey(Args);
	}
}

UObject* UInputToolset::ResolveToolDispatchTarget() const
{
	ULocalPlayer* LocalPlayer = ResolveLocalPlayer();
	return LocalPlayer ? LocalPlayer->GetSubsystem<UInputToolset>() : nullptr;
}

void UInputToolset::Deinitialize()
{
	for (const TWeakPtr<FDelayedHoldWatcher>& WeakWatcher : PendingWatchers)
	{
		if (TSharedPtr<FDelayedHoldWatcher> Watcher = WeakWatcher.Pin())
		{
			Watcher->Cancel(TEXT("The local player is shutting down."));
		}
	}
	PendingWatchers.Empty();

	Super::Deinitialize();
}

UAsyncFunctionResultVoid* UInputToolset::ApplyInput(const TArray<FInputToolsetEntry>& Entries, float DurationSeconds)
{
	using namespace UE::AsyncFunctionResult;

	if (Entries.IsEmpty())
	{
		return MakeErrorResult<UAsyncFunctionResultVoid>(TEXT("Entries must not be empty."));
	}
	FString EntriesError;
	if (!ValidateEntries(Entries, [&EntriesError](const FString& Error) { EntriesError = Error; }))
	{
		return MakeErrorResult<UAsyncFunctionResultVoid>(EntriesError);
	}
	if (DurationSeconds < 0.0f)
	{
		return MakeErrorResult<UAsyncFunctionResultVoid>(TEXT("DurationSeconds must not be negative."));
	}
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (HasZeroValue(Entries[Index]))
		{
			return MakeErrorResult<UAsyncFunctionResultVoid>(FString::Printf(
				TEXT("Entries[%d] has a zero Value, which has no effect. Set a nonzero Value, or omit this entry."), Index));
		}
	}

	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	APlayerController* PC = LocalPlayer ? LocalPlayer->GetPlayerController(nullptr) : nullptr;
	if (!PC)
	{
		return MakeErrorResult<UAsyncFunctionResultVoid>(TEXT("No local player controller found. Start a Play In Editor or standalone session first."));
	}

	UPlayerInput* Input = PC->PlayerInput;
	if (!Input)
	{
		return MakeErrorResult<UAsyncFunctionResultVoid>(TEXT("The local player has no player input."));
	}

	// Only Action entries need Enhanced Input; a Key-only call works off PC->PlayerInput alone.
	const bool bHasAnyAction = Entries.ContainsByPredicate([](const FInputToolsetEntry& Entry) { return Entry.Action != nullptr; });
	UEnhancedInputLocalPlayerSubsystem* Subsystem =
		bHasAnyAction ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	if (bHasAnyAction && !Subsystem)
	{
		return MakeErrorResult<UAsyncFunctionResultVoid>(TEXT("The local player has no Enhanced Input subsystem. Is the Enhanced Input plugin enabled?"));
	}

	TArray<UInputModifier*> Modifiers;
	TArray<UInputTrigger*> Triggers;
	TArray<TWeakObjectPtr<const UInputAction>> WeakActions;
	TArray<FKey> Keys;
	for (const FInputToolsetEntry& Entry : Entries)
	{
		if (Entry.Action)
		{
			Subsystem->StartContinuousInputInjectionForAction(Entry.Action, FInputActionValue(Entry.Value), Modifiers, Triggers);
			WeakActions.Add(Entry.Action);
		}
		else
		{
			InjectLegacyKeyEvent(*PC, *Input, Entry.Key, IE_Pressed, Entry.Value.X);
			Keys.Add(Entry.Key);
		}
	}

	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> WeakSubsystem(Subsystem);
	TWeakObjectPtr<APlayerController> WeakPC(PC);
	TWeakObjectPtr<UPlayerInput> WeakInput(Input);
	UAsyncFunctionResultVoid* Result = NewObject<UAsyncFunctionResultVoid>();
	TWeakPtr<FDelayedHoldWatcher> Watcher = FDelayedHoldWatcher::Start(Result, DurationSeconds, PC->GetWorld(),
		[WeakSubsystem, WeakPC, WeakInput, WeakActions, Keys]()
		{
			if (UEnhancedInputLocalPlayerSubsystem* Sys = WeakSubsystem.Get())
			{
				for (const TWeakObjectPtr<const UInputAction>& WeakAction : WeakActions)
				{
					if (const UInputAction* Action = WeakAction.Get())
					{
						Sys->StopContinuousInputInjectionForAction(Action);
					}
				}
			}
			if (APlayerController* Controller = WeakPC.Get())
			{
				if (UPlayerInput* PlayerInput = WeakInput.Get())
				{
					for (const FKey& Key : Keys)
					{
						InjectLegacyKeyEvent(*Controller, *PlayerInput, Key, IE_Released, 0.0f);
					}
				}
			}
		});

	PendingWatchers.RemoveAll([](const TWeakPtr<FDelayedHoldWatcher>& Weak) { return !Weak.IsValid(); });
	PendingWatchers.Add(MoveTemp(Watcher));

	return Result;
}

TArray<bool> UInputToolset::IsInputApplied(const TArray<FInputToolsetEntry>& Entries)
{
	if (!ValidateEntries(Entries, [](const FString& Error) { UKismetSystemLibrary::RaiseScriptError(Error); }))
	{
		return {};
	}

	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	APlayerController* PC = LocalPlayer ? LocalPlayer->GetPlayerController(nullptr) : nullptr;
	UPlayerInput* Input = PC ? PC->PlayerInput.Get() : nullptr;
	if (!Input && Entries.ContainsByPredicate([](const FInputToolsetEntry& Entry) { return !Entry.Action; }))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("The local player has no player input."));
		return {};
	}

	// Only Action entries need Enhanced Input; a Key-only call works off PC->PlayerInput alone.
	UEnhancedInputLocalPlayerSubsystem* Subsystem = nullptr;
	if (Entries.ContainsByPredicate([](const FInputToolsetEntry& Entry) { return Entry.Action != nullptr; }))
	{
		Subsystem = LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
		if (!Subsystem)
		{
			UKismetSystemLibrary::RaiseScriptError(TEXT("The local player has no Enhanced Input subsystem. Is the Enhanced Input plugin enabled?"));
			return {};
		}
	}

	TArray<bool> HeldStates;
	HeldStates.Reserve(Entries.Num());
	for (const FInputToolsetEntry& Entry : Entries)
	{
		if (Entry.Action)
		{
			HeldStates.Add(Subsystem ? Subsystem->HasContinuousInputInjectionForAction(Entry.Action) : false);
		}
		else
		{
			HeldStates.Add(Input ? Input->IsPressed(Entry.Key) : false);
		}
	}
	return HeldStates;
}
