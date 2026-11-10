// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "ToolsetCore/ToolsetProvider.h"

#include "InputToolset.generated.h"

class UInputAction;
class UAsyncFunctionResultVoid;
class FDelayedHoldWatcher;

/// Identifies either an Enhanced Input action or a legacy key (set exactly one, not both) and the
/// value to inject for it, for holding several inputs together in one call.
USTRUCT()
struct FInputToolsetEntry
{
	GENERATED_BODY()

	/** The Enhanced Input action to target. Set this or Key, not both. */
	UPROPERTY()
	TObjectPtr<const UInputAction> Action = nullptr;

	/** The legacy key to target, for mappings not yet on Enhanced Input. Set this or Action, not both. */
	UPROPERTY()
	FKey Key;

	/** The value to inject; must be nonzero. For Action, components map to its ValueType: Boolean/Axis1D
	 * use X, Axis2D uses X/Y, Axis3D uses X/Y/Z. For Key, only X is used, as the depression amount --
	 * same for digital and analog keys; pass 1 for a normal press. */
	UPROPERTY()
	FVector Value = FVector::ZeroVector;
};

/**
 * Drives input on the local player owning this subsystem: presses, holds, and releases Enhanced
 * Input actions and raw legacy keys, for mappings still on the legacy key/axis system.
 */
UCLASS(MinimalAPI)
class UInputToolset : public ULocalPlayerSubsystem, public IToolsetProvider
{
	GENERATED_BODY()

public:
#if !UE59_COMPAT // USubsystem::ShouldRequireModuleLoadedForCreation is UE6-only (56897598)
	virtual bool ShouldRequireModuleLoadedForCreation() const override { return true; }
#endif
	virtual void Deinitialize() override;

	//~ Begin IToolsetProvider
	virtual bool UsesInstanceDispatch() const override { return true; }
	virtual UObject* ResolveToolDispatchTarget() const override;
	//~ End IToolsetProvider

	/**
	 * Presses one or more actions/keys together, holds them for DurationSeconds, then releases them,
	 * completing once the release is done.
	 * @param Entries The actions/keys and values to hold. Each entry must set exactly one of Action or Key.
	 * @param DurationSeconds How long to hold before releasing, in seconds. Zero holds for a single
	 *        real tick, as a discrete tap. Must not be negative.
	 * @return An async result that completes once the hold ends and all entries have been released.
	 */
	UFUNCTION(meta = (AIRuntimeCallable, AIAccessMode = "Write"), Category = "InputToolset")
	UAsyncFunctionResultVoid* ApplyInput(const TArray<FInputToolsetEntry>& Entries, float DurationSeconds);

	/**
	 * Reports whether each entry is currently active. Only Key reads player input.
	 * @param Entries The actions/keys to query. Value is ignored.
	 * @return One entry per input Entries, in the same order.
	 */
	UFUNCTION(meta = (AIRuntimeCallable, AIAccessMode = "Read"), Category = "InputToolset")
	TArray<bool> IsInputApplied(const TArray<FInputToolsetEntry>& Entries);

private:
	// Watchers started by ApplyInput that haven't completed yet, so Deinitialize can cancel them
	// before this local player (and, if it's unloading, the module) goes away. Pruned of expired
	// entries opportunistically on each ApplyInput call.
	TArray<TWeakPtr<FDelayedHoldWatcher>> PendingWatchers;
};
