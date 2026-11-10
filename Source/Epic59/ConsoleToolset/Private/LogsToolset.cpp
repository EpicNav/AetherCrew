// Copyright Epic Games, Inc. All Rights Reserved.

#include "LogsToolset59.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Internationalization/Regex.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CoreMisc.h"
#include "Misc/FileHelper.h"
#include "Logging/LogCategory.h"
#include "Logging/LogSuppressionInterface.h"
#include "Logging/LogVerbosity.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/Paths.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LogsToolset59)

namespace
{
	// Matches a standard UE log line prefix and captures the category name.
	// Example: [2024.01.15-10.30.00:000][  0]LogTemp: Warning: Some message
	// NOTE: FRegexPattern must NOT be a static variable — it requires i18n to be initialized.
	const FString CategoryPatternStr = TEXT(R"regex(^\[[\d.\-:]+\]\[\s*\d+\](\w+):)regex");

	// Valid verbosity names accepted by the log console command, derived directly
	// from ELogVerbosity so the list stays in sync if new levels are ever added.
	const TArray<FString> ValidVerbosities = []()
	{
		TArray<FString> Result;
		for (int32 i = ELogVerbosity::NoLogging; i < ELogVerbosity::NumVerbosity; ++i)
		{
			Result.Add(ToString(static_cast<ELogVerbosity::Type>(i)));
		}
		return Result;
	}();

	// Output device that captures lines written to it into an array.
	//
	// GetLogCategories, GetVerbosity, and SetVerbosity all route through
	// FSelfRegisteringExec::StaticExec with the "log" command. This is the same
	// code path used by the editor's Output Log UI and UE's own developer tooling.
	// It is the only public way to enumerate the live FLogCategoryBase* registry
	// (FLogSuppressionImplementation::ReverseAssociations). FLogSuppressionInterface
	// offers only name-based lookup (FindCategory), not enumeration.
	struct FLineCapture : public FOutputDevice
	{
		TArray<FString> Lines;

		virtual void Serialize(
			const TCHAR* V, ELogVerbosity::Type, const FName&) override
		{
			Lines.Add(FString(V));
		}
	};
}

TArray<FString> ULogsToolset59::GetLogEntries(
	const FString& LogCategory, const FString& Pattern, int32 MaxEntries, const FString& LogFilePath,
	int32 MaxCharsPerEntry)
{
	if (!LogFilePath.IsEmpty() && !IsAllowedExternalLogPath(LogFilePath))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("Cannot read log file '%s': its location or format is not allowed."),
			*LogFilePath));
		return {};
	}

	// An empty path reads the current session's log.
	const FString ResolvedPath = LogFilePath.IsEmpty() ? GetLogFilePath() : LogFilePath;
	if (ResolvedPath.IsEmpty())
	{
		return {}; // GetLogFilePath already raised a script error for the missing session log.
	}
	if (!IFileManager::Get().FileExists(*ResolvedPath))
	{
		UKismetSystemLibrary::RaiseScriptError(
			FString::Printf(TEXT("Log file not found: %s"), *ResolvedPath));
		return {};
	}

	if (!LogCategory.IsEmpty())
	{
		const TArray<FString> Known = GetLogCategories(LogCategory, LogFilePath);
		const bool bExists = Known.ContainsByPredicate([&LogCategory](const FString& C)
		{
			return C.Equals(LogCategory, ESearchCase::IgnoreCase);
		});
		if (!bExists)
		{
			UKismetSystemLibrary::RaiseScriptError(
				FString::Printf(TEXT("Log category '%s' not found."), *LogCategory));
			return {};
		}
	}

	const FRegexPattern CategoryRegex(CategoryPatternStr);
	TOptional<FRegexPattern> PatternRegex;
	if (!Pattern.IsEmpty())
	{
		PatternRegex.Emplace(Pattern, ERegexPatternFlags::CaseInsensitive);
	}

	TArray<FString> Results;

	for (const FString& Line : ReadLogLines(ResolvedPath))
	{
		if (!LogCategory.IsEmpty())
		{
			FRegexMatcher Matcher(CategoryRegex, Line);
			if (!Matcher.FindNext() ||
				!Matcher.GetCaptureGroup(1).Equals(LogCategory, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		if (PatternRegex.IsSet())
		{
			FRegexMatcher Matcher(*PatternRegex, Line);
			if (!Matcher.FindNext())
			{
				continue;
			}
		}

		Results.Add(Line);
	}

	if (MaxEntries > 0 && Results.Num() > MaxEntries)
	{
		Results.RemoveAt(0, Results.Num() - MaxEntries);
	}

	if (MaxCharsPerEntry > 0)
	{
		for (FString& Entry : Results)
		{
			if (Entry.Len() > MaxCharsPerEntry)
			{
				const int32 DroppedChars = Entry.Len() - MaxCharsPerEntry;
				Entry.LeftInline(MaxCharsPerEntry);
				Entry.Append(FString::Printf(TEXT(" [truncated, %d more chars]"), DroppedChars));
			}
		}
	}

	return Results;
}

TArray<FString> ULogsToolset59::GetLogCategories(const FString& Filter, const FString& LogFilePath)
{
	// An empty or session-log path uses the live registry below, which lists even categories that have
	// registered but not yet logged a line. Any other file is parsed from its text instead.
	if (!LogFilePath.IsEmpty() && !FPaths::IsSamePath(LogFilePath, GetLogFilePathRaw()))
	{
		if (!IsAllowedExternalLogPath(LogFilePath))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("Cannot read log file '%s': its location or format is not allowed."),
				*LogFilePath));
			return {};
		}

		if (!IFileManager::Get().FileExists(*LogFilePath))
		{
			UKismetSystemLibrary::RaiseScriptError(
				FString::Printf(TEXT("Log file not found: %s"), *LogFilePath));
			return {};
		}

		const FRegexPattern CategoryRegex(CategoryPatternStr);
		TArray<FString> FileCategories;
		for (const FString& Line : ReadLogLines(LogFilePath))
		{
			FRegexMatcher Matcher(CategoryRegex, Line);
			if (Matcher.FindNext())
			{
				const FString Category = Matcher.GetCaptureGroup(1);
				if (Filter.IsEmpty() || Category.Contains(Filter, ESearchCase::IgnoreCase))
				{
					FileCategories.AddUnique(Category);
				}
			}
		}

		FileCategories.Sort();
		return FileCategories;
	}

	// "log list" asks FLogSuppressionImplementation to enumerate every FLogCategoryBase
	// that has registered itself via FLogSuppressionInterface::AssociateSuppress. Each
	// output line is formatted as "%-40s  %-12s  %s" (name, verbosity, debug-break flag).
	// Splitting on whitespace and taking the first token gives the category name.
	// "log list <filter>" narrows results to categories whose name contains the filter
	// string (case-insensitive).
	FLineCapture Capture;
	const FString Command = Filter.IsEmpty()
		? TEXT("log list")
		: FString::Printf(TEXT("log list %s"), *Filter);
	FSelfRegisteringExec::StaticExec(nullptr, *Command, Capture);

	TArray<FString> Categories;
	for (const FString& Line : Capture.Lines)
	{
		TArray<FString> Parts;
		Line.TrimEnd().ParseIntoArrayWS(Parts);
		if (Parts.Num() > 0)
		{
			Categories.Add(Parts[0]);
		}
	}

	Categories.Sort();
	return Categories;
}

FString ULogsToolset59::GetVerbosity(const FString& LogCategory)
{
	if (LogCategory.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Category cannot be empty."));
		return FString();
	}

	// "log list <filter>" filters by substring, so it may return multiple lines.
	// Pass the exact name as the filter to keep the output small, then match exactly.
	FLineCapture Capture;
	FSelfRegisteringExec::StaticExec(
		nullptr, *FString::Printf(TEXT("log list %s"), *LogCategory), Capture);

	for (const FString& Line : Capture.Lines)
	{
		TArray<FString> Parts;
		Line.TrimEnd().ParseIntoArrayWS(Parts);
		if (Parts.Num() >= 2 && Parts[0].Equals(LogCategory, ESearchCase::IgnoreCase))
		{
			return Parts[1];
		}
	}

	UKismetSystemLibrary::RaiseScriptError(
		FString::Printf(TEXT("Log category '%s' not found."), *LogCategory));
	return FString();
}

void ULogsToolset59::SetVerbosity(const FString& LogCategory, const FString& Verbosity)
{
	if (LogCategory.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Category cannot be empty."));
		return;
	}

	// ValidVerbosities is indexed by ELogVerbosity value.
	const int32 VerbosityIndex = ValidVerbosities.IndexOfByPredicate(
		[&Verbosity](const FString& V)
		{
			return V.Equals(Verbosity, ESearchCase::IgnoreCase);
		});

	if (VerbosityIndex == INDEX_NONE)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("'%s' is not a valid verbosity level. Use one of: %s"),
			*Verbosity, *FString::Join(ValidVerbosities, TEXT(", "))));
		return;
	}

	// The log command would silently ignore an unknown category, and would trip an ensure
	// for a verbosity above the category's compile-time maximum, so check both up front.
#if 0 // UE 5.8: FLogSuppressionInterface has no FindCategory, so the compile-time maximum cannot be checked.
	const FLogCategoryBase* Category =
		FLogSuppressionInterface::Get().FindCategory(FName(*LogCategory));
	if (!Category)
	{
		UKismetSystemLibrary::RaiseScriptError(
			FString::Printf(TEXT("Log category '%s' not found."), *LogCategory));
		return;
	}

	const ELogVerbosity::Type MaxVerbosity = Category->GetCompileTimeVerbosity();
	if (VerbosityIndex > static_cast<int32>(MaxVerbosity))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("'%s' exceeds the compile-time maximum verbosity '%s' of log category '%s'."),
			*Verbosity, ToString(MaxVerbosity), *LogCategory));
		return;
	}
#else
	// UE 5.8: the category must be registered; GetVerbosity raises the same "not found" error otherwise.
	if (GetVerbosity(LogCategory).IsEmpty())
	{
		return;
	}
#endif

	FOutputDeviceNull Null;
	FSelfRegisteringExec::StaticExec(
		nullptr, *FString::Printf(TEXT("log %s %s"), *LogCategory, *Verbosity), Null);
}

FString ULogsToolset59::GetLogFilePathRaw()
{
	return FPaths::ConvertRelativePathToFull(FPlatformOutputDevices::GetAbsoluteLogFilename());
}

bool ULogsToolset59::IsAllowedExternalLogPath(const FString& LogFilePath)
{
	return FPaths::GetExtension(LogFilePath).Equals(TEXT("log"), ESearchCase::IgnoreCase)
		&& FPaths::IsUnderDirectory(LogFilePath, FPaths::ProjectSavedDir());
}

FString ULogsToolset59::GetLogFilePath()
{
	const FString AbsLogPath = GetLogFilePathRaw();
	if (!AbsLogPath.IsEmpty() && IFileManager::Get().FileExists(*AbsLogPath))
	{
		return AbsLogPath;
	}

	UKismetSystemLibrary::RaiseScriptError(
		FString::Printf(TEXT("Log file not found: %s"), *AbsLogPath));
	return FString();
}

TArray<FString> ULogsToolset59::ReadLogLines(const FString& LogFilePath)
{
	check(!LogFilePath.IsEmpty());

	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *LogFilePath,
		FFileHelper::EHashOptions::None, FILEREAD_AllowWrite))
	{
		UKismetSystemLibrary::RaiseScriptError(
			FString::Printf(TEXT("Failed to read log file: %s"), *LogFilePath));
		return {};
	}

	TArray<FString> Lines;
	Content.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
	Lines.RemoveAll([](const FString& Line) { return Line.TrimEnd().IsEmpty(); });
	return Lines;
}
