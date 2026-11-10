// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "ConsoleToolset.generated.h"

class FJsonObject;
class IConsoleObject;

/**
 * Drive engine console operations: read and write console variables (CVars), enumerate exec commands, dispatch exec commands.
 */
UCLASS(BlueprintType, MinimalAPI)
class UConsoleToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/*
	 * Finds all console variables that contain a given name.
	 * @param Name The partial or full name to search for.
	 * @return A JSON dict with info about matching cvars.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"), Category = "ConsoleToolset")
	static FString SearchCVars(const FString& Name);

	/*
	 * Returns exec commands whose name contains the search string (case-insensitive).
	 * @param Name Substring to match. Empty matches everything.
	 * @return Map from command name to its help text (may be empty).
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"), Category = "ConsoleToolset")
	static TMap<FString, FString> SearchConsoleCommands(const FString& Name);

	/*
	 * Reads the current value of a console variable as its canonical string
	 * representation (e.g. "true"/"false" for bools, "1.5" for floats).
	 * @param Name The name of the console variable.
	 * @return The current value as a string.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read"), Category = "ConsoleToolset")
	static FString GetCVarValue(const FString& Name);

	/*
	 * Sets the value of a console variable. The value is parsed according to
	 * the variable's declared type.
	 * @param Name The name of the console variable.
	 * @param Value The new value as a string.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Write"), Category = "ConsoleToolset")
	static void SetCVarValue(const FString& Name, const FString& Value);

	/*
	 * Executes an arbitrary UE exec command. Output is written to the session log.
	 * Python is refused: any command containing "py" as a whole word (the py command, including when wrapped by
	 * defer, redirecttofile, etc.) is rejected. Run Python with the Python scripting tool instead.
	 * @param Command The exec command line (as if typed at the console).
	 * @return Log category names that received output. An empty string in the list represents uncategorized output.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Execute"), Category = "ConsoleToolset")
	static TArray<FString> RunConsoleCommand(const FString& Command);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface

private:
	static TSharedPtr<FJsonObject> CVarToJson(IConsoleObject* CVar);
};
