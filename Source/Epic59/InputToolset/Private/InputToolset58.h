// Games by Hyper 2026.
// UE 5.8: HyperAI's static front for Epic's UInputToolset. 5.9 dispatches InputToolset's tools to the
// local player's UInputToolset subsystem (IToolsetProvider instance dispatch); 5.8's registry calls static
// tools on UToolsetDefinition classes only, so these statics resolve the same target and forward to it.

#pragma once

#include "CoreMinimal.h"
#include "InputToolset.h"
#include "ToolsetRegistry/ToolsetDefinition59.h"

#include "InputToolset58.generated.h"

class UAsyncFunctionResultVoid;

/**
 * Drives input on the local player of the running PIE or game session: presses, holds, and releases
 * Enhanced Input actions and raw legacy keys, for mappings still on the legacy key/axis system.
 */
UCLASS(MinimalAPI)
class UInputToolset58 : public UToolsetDefinition, public IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Presses one or more actions/keys together, holds them for DurationSeconds, then releases them,
	 * completing once the release is done.
	 * @param Entries The actions/keys and values to hold. Each entry must set exactly one of Action or Key.
	 * @param DurationSeconds How long to hold before releasing, in seconds. Zero holds for a single
	 *        real tick, as a discrete tap. Must not be negative.
	 * @return An async result that completes once the hold ends and all entries have been released.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Write"), Category = "InputToolset")
	static UAsyncFunctionResultVoid* ApplyInput(const TArray<FInputToolsetEntry>& Entries, float DurationSeconds);

	/**
	 * Reports whether each entry is currently active. Only Key reads player input.
	 * @param Entries The actions/keys to query. Value is ignored.
	 * @return One entry per input Entries, in the same order.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"), Category = "InputToolset")
	static TArray<bool> IsInputApplied(const TArray<FInputToolsetEntry>& Entries);

	/** The local player's UInputToolset that 5.9's registry would dispatch to, or null when no session has one. */
	static UInputToolset* ResolveTarget();
};
