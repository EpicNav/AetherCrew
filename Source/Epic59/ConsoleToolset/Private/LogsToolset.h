// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "LogsToolset.generated.h"

/// Provides tools for reading the Unreal Engine output log and controlling
/// log category verbosity.
UCLASS(BlueprintType, MinimalAPI)
class ULogsToolset59 : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Returns log entries from a log file. Reads the current session's log file by default,
	 * but can read other log files such as crash logs.
	 * @param LogCategory If non-empty, only returns entries from this log category (e.g. "LogTemp").
	 * @param Pattern If non-empty, only returns entries whose text matches this regular
	 *   expression.
	 * @param MaxEntries Maximum number of entries to return, taken from the end of the log.
	 *   Pass 0 for no limit.
	 * @param LogFilePath Path to the log file to read. An empty string reads the current
	 *   session's log file.
	 * @param MaxCharsPerEntry Maximum length of a single entry, so that one very long line (such
	 *   as a full plugin listing) cannot dominate the result. Longer entries are cut to this
	 *   length and a short "[truncated, N more chars]" suffix is appended, so a returned entry
	 *   can be slightly longer than this value. Pass 0 for no limit.
	 * @return A list of matching log entries in chronological order.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read", PythonCallable), Category = "LogsToolset")
	static TArray<FString> GetLogEntries(
		const FString& LogCategory = TEXT(""), const FString& Pattern = TEXT(""), int32 MaxEntries = 1000,
		const FString& LogFilePath = TEXT(""), int32 MaxCharsPerEntry = 2000);

	/**
	 * Returns a sorted list of log categories.
	 * @param Filter If non-empty, only returns categories whose name contains this substring.
	 * @param LogFilePath Path to a log file to parse. When empty, the current session's registered
	 *   categories are used.
	 * @return A sorted list of log category names, e.g. ["LogBlueprint", "LogTemp"].
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read", PythonCallable), Category = "LogsToolset")
	static TArray<FString> GetLogCategories(
		const FString& Filter = TEXT(""), const FString& LogFilePath = TEXT(""));

	/**
	 * Returns the current verbosity level for a log category.
	 * @param LogCategory The log category name, e.g. "LogTemp".
	 * @return The verbosity level as a string: one of "NoLogging", "Fatal", "Error",
	 *   "Warning", "Display", "Log", "Verbose", or "VeryVerbose". Raises a script error
	 *   if the category is not found.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Read", PythonCallable), Category = "LogsToolset")
	static FString GetVerbosity(const FString& LogCategory);

	/**
	 * Sets the verbosity level for a log category.
	 * @param LogCategory The log category name, e.g. "LogTemp".
	 * @param Verbosity The verbosity level: one of "NoLogging", "Fatal", "Error",
	 *   "Warning", "Display", "Log", "Verbose", or "VeryVerbose".
	 *   Raises a script error if the category is not registered or the verbosity is above its
	 *   compile-time maximum.
	 */
	UFUNCTION(meta = (AICallable, AIRuntimeCallable, AIAccessMode = "Write", PythonCallable), Category = "LogsToolset")
	static void SetVerbosity(const FString& LogCategory, const FString& Verbosity);

private:
	/**
	 * Resolves the session log path without checking it exists, so it never raises.
	 * For comparing a caller's path against the session log.
	 */
	static FString GetLogFilePathRaw();
	/**
	 * Resolves the session log path and raises a script error if the file does not exist.
	 * For when the session log is about to be read.
	 */
	static FString GetLogFilePath();
	/**
	 * True when LogFilePath is a log file under an allowed directory.
	 */
	static bool IsAllowedExternalLogPath(const FString& LogFilePath);
	static TArray<FString> ReadLogLines(const FString& LogFilePath);

	friend class FLogsToolsetSpec;
};
