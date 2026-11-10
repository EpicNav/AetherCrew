// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"
#include "AsyncFunctionResult/AsyncFunctionResultString.h"
#include "EditorSubsystem.h"
#include "ToolsetCore/ToolsetProvider.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: ULiveCodingToolsetTools59 below

#include "LiveCodingToolset59.generated.h"

class FSubsystemCollectionBase;
struct LiveCodingToolsetTest59; // UE 5.8: renamed ...59; 5.8 registers a test of the 5.9 name

/**
 * Live Coding compile toolset.
 *
 * Exposes an MCP tool that triggers a Live Coding compile from the running
 * editor, returning the compile status together with any captured
 * LogLiveCoding output and UBT compiler diagnostics once the compile
 * finishes.
 *
 * Also an editor subsystem that registers itself with UToolsetRegistry for the lifetime of the
 * editor. Registration can be toggled at runtime via the LiveCodingToolset.Enable CVar.
 */
UCLASS(BlueprintType, MinimalAPI)
class ULiveCodingToolset59 : public UEditorSubsystem, public IToolsetProvider
{
	GENERATED_BODY()

public:

	LIVECODINGTOOLSET59_API virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	LIVECODINGTOOLSET59_API virtual void Deinitialize() override;

	/** Register or unregister the toolset with UToolsetRegistry at runtime. */
	void SetToolsetEnabled(bool bEnabled);

	/** Triggers a Live Coding compile and returns a pending result that resolves once the
	 * compile finishes. Live Coding must be enabled in Editor Preferences.
	 * @return An async result resolving to the compile status followed by any captured
	 * LogLiveCoding output and UBT compiler diagnostics. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "LiveCodingToolset")
	static LIVECODINGTOOLSET59_API UAsyncFunctionResultString* CompileLiveCoding();

	/** Poll before calling CompileLiveCoding to avoid blocking on the Broker's initial module load. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "LiveCodingToolset")
	static LIVECODINGTOOLSET59_API bool IsLiveCodingReady();

	/** Call before CompileLiveCoding on a session launched with Live Coding off; starts the Broker/Agent. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "LiveCodingToolset")
	static LIVECODINGTOOLSET59_API void EnableLiveCoding();

	//~ Begin IToolsetProvider Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End IToolsetProvider Interface

private:
	friend struct LiveCodingToolsetTest59;

	/** Resolves AsyncResult with a timeout error unless it has already completed. Returns true if it
	 *  timed out. */
	static LIVECODINGTOOLSET59_API bool TryTimeout(UAsyncFunctionResultString& AsyncResult);

	bool bToolsetRegistered = false;
};

/**
 * Live Coding compile toolset.
 *
 * UE 5.8: 5.8's registry registers UToolsetDefinition classes only, and ULiveCodingToolset59 is an editor
 * subsystem, so this class carries its tools (LiveCodingToolset59.LiveCodingToolsetTools59) and the subsystem
 * registers it.
 */
UCLASS(MinimalAPI)
class ULiveCodingToolsetTools59 : public UToolsetDefinition, public IToolsetProvider
{
	GENERATED_BODY()

public:
	/** Triggers a Live Coding compile and returns a pending result that resolves once the
	 * compile finishes. Live Coding must be enabled in Editor Preferences.
	 * @return An async result resolving to the compile status followed by any captured
	 * LogLiveCoding output and UBT compiler diagnostics. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "LiveCodingToolset")
	static UAsyncFunctionResultString* CompileLiveCoding() { return ULiveCodingToolset59::CompileLiveCoding(); }

	/** Poll before calling CompileLiveCoding to avoid blocking on the Broker's initial module load. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "LiveCodingToolset")
	static bool IsLiveCodingReady() { return ULiveCodingToolset59::IsLiveCodingReady(); }

	/** Call before CompileLiveCoding on a session launched with Live Coding off; starts the Broker/Agent. */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Execute"), Category = "LiveCodingToolset")
	static void EnableLiveCoding() { ULiveCodingToolset59::EnableLiveCoding(); }
};
