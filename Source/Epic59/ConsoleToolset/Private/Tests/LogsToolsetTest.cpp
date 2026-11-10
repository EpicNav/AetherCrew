// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Misc/AutomationTest.h"
#include "Misc/CString.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#include "LogsToolset59.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogLogsToolsetTest, Log, All);
// Compile-time maximum is Log, so Verbose and VeryVerbose cannot be set on it.
DEFINE_LOG_CATEGORY_STATIC(LogLogsToolsetCappedTest, Log, Log);

BEGIN_DEFINE_SPEC(FLogsToolsetSpec59, "AIAssistant.Toolsets.ConsoleToolset.LogsToolset",
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter
	| EAutomationTestFlags::CriticalPriority)

	bool TestContains(
		const FString& Context, const FString& ToSearch, const FString& ToFind,
		ESearchCase::Type Case = ESearchCase::CaseSensitive)
	{
		return TestTrue(
			FString::Printf(TEXT("%s: `%s` in `%s`"), *Context, *ToFind, *ToSearch),
			ToSearch.Contains(ToFind, Case));
	}

	/**
	 * Logs a uniquely marked line and returns its marker once the line is readable from the log
	 * file. GetLogEntries searches the file on disk, which the log writer reaches on its own
	 * schedule, so a search issued straight after UE_LOGF can run before the line it is looking
	 * for has landed. Every test that hunts for a line it just wrote goes through this.
	 *
	 * Tag names the calling test within the marker, so a line in the log says which test wrote it.
	 */
	FString LogMarkedLine(const TCHAR* Tag, const FString& Suffix = FString())
	{
		const FString Marker = FString::Printf(
			TEXT("LogsToolset%sMarker_%s"), Tag, *FGuid::NewGuid().ToString());
		UE_LOGF(LogLogsToolsetTest, Log, "%ls%ls", *Marker, *Suffix);
		GLog->Flush();

		const FString LogFilePath = ULogsToolset59::GetLogFilePathRaw();
		const double GiveUpAt = FPlatformTime::Seconds() + 5.0;
		while (FPlatformTime::Seconds() < GiveUpAt)
		{
			FString Content;
			if (FFileHelper::LoadFileToString(Content, *LogFilePath,
					FFileHelper::EHashOptions::None, FILEREAD_AllowWrite) &&
				Content.Contains(Marker))
			{
				return Marker;
			}

			FPlatformProcess::Sleep(0.01f);
		}

		AddError(FString::Printf(
			TEXT("Marked line never became readable from %s: %s"), *LogFilePath, *Marker));
		return Marker;
	}
END_DEFINE_SPEC(FLogsToolsetSpec59)

#define TestLogCategory LogLogsToolsetTest

void FLogsToolsetSpec59::Define()
{
	using namespace UE::ToolsetRegistry;

	Describe(TEXT("GetLogCategories"), [this]()
	{
		It(TEXT("Returns a non-empty sorted list"), [this]()
		{
			UE_LOGF(LogLogsToolsetTest, Log, "GetLogCategories probe");
			GLog->Flush();

			TArray<FString> Categories = ULogsToolset59::GetLogCategories();
			TestGreaterThan(TEXT("Categories is non-empty"), Categories.Num(), 0);

			TArray<FString> Sorted = Categories;
			Sorted.Sort();
			TestEqual(TEXT("Categories is sorted"), Categories, Sorted);
		});

		It(TEXT("Includes the test log category"), [this]()
		{
			UE_LOGF(LogLogsToolsetTest, Log, "Category presence probe");
			GLog->Flush();

			TArray<FString> Categories = ULogsToolset59::GetLogCategories();
			TestTrue(
				FString::Printf(
					TEXT("LogLogsToolsetTest is present in [%s]"),
					*FString::Join(Categories, TEXT(", "))),
				Categories.Contains(TEXT("LogLogsToolsetTest")));
		});

		It(TEXT("Filter narrows results to matching categories"), [this]()
		{
			UE_LOGF(LogLogsToolsetTest, Log, "Filter probe");
			GLog->Flush();

			TArray<FString> Filtered = ULogsToolset59::GetLogCategories(TEXT("LogLogsToolsetTest"));
			TestGreaterThan(TEXT("At least one result"), Filtered.Num(), 0);
			for (const FString& Category : Filtered)
			{
				TestContains(
					TEXT("Category contains filter string"),
					Category, TEXT("LogLogsToolsetTest"), ESearchCase::IgnoreCase);
			}
		});

		It(TEXT("GetLogCategories rejects a path outside the Saved folder"), [this]()
		{
			AddExpectedError(TEXT("location or format is not allowed"));
			FToolCallExceptionHandler ExceptionHandler;
			TArray<FString> Categories;
			ExceptionHandler.CaptureErrorsIn([&Categories]() -> void
				{
					Categories = ULogsToolset59::GetLogCategories(
						TEXT(""), FPaths::Combine(FPaths::EngineDir(), TEXT("OutsideSaved.log")));
				});
			TestEqual(TEXT("No categories for a path outside Saved"), Categories.Num(), 0);
			TestContains(
				TEXT("GetLogCategories rejects a path outside Saved"),
				ExceptionHandler.GetException(), TEXT("location or format"));
		});
	});

	Describe(TEXT("GetLogEntries"), [this]()
	{
		It(TEXT("Returns a non-empty list with no filters"), [this]()
		{
			TArray<FString> Entries = ULogsToolset59::GetLogEntries(TEXT(""), TEXT(""), 0);
			TestGreaterThan(TEXT("Entries is non-empty"), Entries.Num(), 0);
		});

		It(TEXT("Filters by category"), [this]()
		{
			UE_LOGF(LogLogsToolsetTest, Log, "Category filter probe");
			GLog->Flush();

			TArray<FString> Entries = ULogsToolset59::GetLogEntries(
				TEXT("LogLogsToolsetTest"), TEXT(""), 0);
			TestGreaterThan(TEXT("At least one entry"), Entries.Num(), 0);
			for (const FString& Entry : Entries)
			{
				TestContains(
					TEXT("Entry contains category prefix"), Entry, TEXT("]LogLogsToolsetTest:"));
			}
		});

		It(TEXT("Filters by pattern"), [this]()
		{
			const FString Marker = LogMarkedLine(TEXT("Pattern"));

			TArray<FString> Entries = ULogsToolset59::GetLogEntries(TEXT(""), *Marker, 0);
			if (TestEqual(TEXT("Exactly one entry matches"), Entries.Num(), 1))
			{
				TestContains(TEXT("Entry contains marker"), Entries[0], Marker);
			}
		});

		It(TEXT("Pattern matching is case-insensitive"), [this]()
		{
			const FString Marker = LogMarkedLine(TEXT("Case"));

			TArray<FString> Entries = ULogsToolset59::GetLogEntries(
				TEXT(""), *Marker.ToLower(), 0);
			TestEqual(TEXT("Case-insensitive match finds entry"), Entries.Num(), 1);
		});

		It(TEXT("Raises an error for an unknown category"), [this]()
		{
			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([this]() -> void
				{
					ULogsToolset59::GetLogEntries(
						TEXT("LogThisCategoryDoesNotExist_XYZ123"),
						TEXT(""),
						0);
				});
			TestContains(
				TEXT("GetLogEntries with invalid category raises error"),
				*ExceptionHandler.GetException(), TEXT("not found"));
		});

		It(TEXT("Respects MaxEntries limit"), [this]()
		{
			constexpr int32 Limit = 5;
			for (int32 i = 0; i < Limit * 2; ++i)
			{
				UE_LOGF(TestLogCategory, Verbose, "Log testing %d", i);
			}
			FString LogCategory = TestLogCategory.GetCategoryName().ToString();
			TArray<FString> All = ULogsToolset59::GetLogEntries(LogCategory, TEXT(""), 0);
			TArray<FString> Limited = ULogsToolset59::GetLogEntries(LogCategory, TEXT(""), Limit);
			TestLessEqual(TEXT("At most MaxEntries returned"), Limited.Num(), Limit);
			if (All.Num() >= Limit)
			{
				TestEqual(TEXT("Returns the most recent entries"),
					Limited, TArray<FString>(All.GetData() + All.Num() - Limit, Limit));
			}
		});

		It(TEXT("Truncates entries longer than MaxCharsPerEntry"), [this]()
		{
			constexpr int32 CharLimit = 64;
			const FString Marker = LogMarkedLine(TEXT("Long"), FString::ChrN(500, TEXT('x')));

			TArray<FString> Whole = ULogsToolset59::GetLogEntries(TEXT(""), *Marker, 0, TEXT(""), 0);
			TArray<FString> Truncated = ULogsToolset59::GetLogEntries(
				TEXT(""), *Marker, 0, TEXT(""), CharLimit);
			if (TestEqual(TEXT("Exactly one whole entry matches"), Whole.Num(), 1) &&
				TestEqual(TEXT("Exactly one truncated entry matches"), Truncated.Num(), 1))
			{
				const FString Expected = Whole[0].Left(CharLimit) + FString::Printf(
					TEXT(" [truncated, %d more chars]"), Whole[0].Len() - CharLimit);
				TestEqual(TEXT("Entry keeps its first MaxCharsPerEntry chars, then says how much "
					"was dropped"), Truncated[0], Expected);
			}
		});

		It(TEXT("Returns whole entries when MaxCharsPerEntry is zero"), [this]()
		{
			const FString Marker = LogMarkedLine(TEXT("Uncapped"), FString::ChrN(500, TEXT('x')));

			TArray<FString> Entries = ULogsToolset59::GetLogEntries(
				TEXT(""), *Marker, 0, TEXT(""), 0);
			if (TestEqual(TEXT("Exactly one entry matches"), Entries.Num(), 1))
			{
				TestGreaterThan(TEXT("Entry keeps its full length"), Entries[0].Len(), 500);
			}
		});

		It(TEXT("Combines category and pattern filters"), [this]()
		{
			const FString Marker = LogMarkedLine(TEXT("Combined"));

			TArray<FString> Entries = ULogsToolset59::GetLogEntries(
				TEXT("LogLogsToolsetTest"), *Marker, 0);
			TestEqual(TEXT("Exactly one combined match"), Entries.Num(), 1);
		});

		It(TEXT("GetLogEntries rejects a path outside the Saved folder"), [this]()
		{
			AddExpectedError(TEXT("location or format is not allowed"));
			FToolCallExceptionHandler ExceptionHandler;
			TArray<FString> Entries;
			ExceptionHandler.CaptureErrorsIn([&Entries]() -> void
				{
					Entries = ULogsToolset59::GetLogEntries(
						TEXT(""), TEXT(""), 0, FPaths::Combine(FPaths::EngineDir(), TEXT("OutsideSaved.log")));
				});
			TestEqual(TEXT("No entries for a path outside Saved"), Entries.Num(), 0);
			TestContains(
				TEXT("GetLogEntries rejects a path outside Saved"),
				ExceptionHandler.GetException(), TEXT("location or format"));
		});

		It(TEXT("GetLogEntries rejects a non-log file under the Saved folder"), [this]()
		{
			AddExpectedError(TEXT("location or format is not allowed"));
			const FString NonLogPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs/NotALog.txt"));
			FToolCallExceptionHandler ExceptionHandler;
			TArray<FString> Entries;
			ExceptionHandler.CaptureErrorsIn([&Entries, &NonLogPath]() -> void
				{
					Entries = ULogsToolset59::GetLogEntries(TEXT(""), TEXT(""), 0, NonLogPath);
				});
			TestEqual(TEXT("No entries for a non-.log file"), Entries.Num(), 0);
			TestContains(
				TEXT("GetLogEntries rejects a non-.log path"),
				ExceptionHandler.GetException(), TEXT("location or format"));
		});
	});

	Describe(TEXT("GetVerbosity"), [this]()
	{
		It(TEXT("Returns a verbosity string for a known category"), [this]()
		{
			UE_LOGF(LogLogsToolsetTest, Log, "GetVerbosity probe");
			const FString Verbosity = ULogsToolset59::GetVerbosity(TEXT("LogLogsToolsetTest"));
			TestNotEqual(TEXT("Verbosity is non-empty"), *Verbosity, TEXT(""));
		});

		It(TEXT("Returns empty string for an unknown category"), [this]()
		{
			const FString Verbosity =
				ULogsToolset59::GetVerbosity(TEXT("LogThisCategoryDoesNotExist_XYZ123"));
			TestEqual(TEXT("Returns empty string for unknown category"), Verbosity, TEXT(""));
		});

		It(TEXT("Raises an error for an unknown category"), [this]()
		{
			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([this]() -> void
				{
					ULogsToolset59::GetVerbosity(TEXT("LogThisCategoryDoesNotExist_XYZ123"));
				});
			TestContains(
				TEXT("GetVerbosity raises error with an unknown category"),
				ExceptionHandler.GetException(), TEXT("not found"));
		});

		It(TEXT("Raises an error for an empty category"), [this]()
		{
			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([this]() -> void
				{
					ULogsToolset59::GetVerbosity(TEXT(""));
				});
			TestContains(
				TEXT("GetVerbosity raises error with an empty category"),
				ExceptionHandler.GetException(), TEXT("Category cannot be empty"));
		});
	});

	Describe(TEXT("SetVerbosity"), [this]()
	{
		It(TEXT("Can round-trip a verbosity change"), [this]()
		{
			const FString Category = TEXT("LogLogsToolsetTest");
			const FString Original = ULogsToolset59::GetVerbosity(Category);
			if (!TestNotEqual(TEXT("Category verbosity is readable"), *Original, TEXT("")))
			{
				return;
			}

			ULogsToolset59::SetVerbosity(Category, TEXT("Warning"));
			TestEqual(TEXT("Verbosity set to Warning"),
				ULogsToolset59::GetVerbosity(Category), FString(TEXT("Warning")));

			ULogsToolset59::SetVerbosity(Category, Original);
			TestEqual(TEXT("Verbosity restored"),
				ULogsToolset59::GetVerbosity(Category), Original);
		});

		It(TEXT("Raises an error for an empty category"), [this]()
		{
			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([this]() -> void
				{
					ULogsToolset59::SetVerbosity(TEXT(""), TEXT("Display"));
				});
			TestContains(
				TEXT("SetVerbosity raises error with an empty category"),
				ExceptionHandler.GetException(), TEXT("Category cannot be empty"));
		});

		It(TEXT("Raises an error for an invalid verbosity"), [this]()
		{
			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([this]() -> void
				{
					ULogsToolset59::SetVerbosity(
						TestLogCategory.GetCategoryName().ToString(),
						TEXT("NotAVerbosity"));
				});
			TestContains(
				TEXT("SetVerbosity raises error with an invalid category"),
				ExceptionHandler.GetException(), TEXT("is not a valid verbosity level"));
		});

		It(TEXT("Raises an error for a category that does not exist"), [this]()
		{
			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([this]() -> void
				{
					ULogsToolset59::SetVerbosity(
						TEXT("LogThisCategoryDoesNotExist_XYZ123"), TEXT("Display"));
				});
			TestContains(
				TEXT("SetVerbosity raises error with an unknown category"),
				ExceptionHandler.GetException(), TEXT("not found"));
		});

		It(TEXT("Raises an error for a verbosity above the compile-time maximum"), [this]()
		{
			// UE 5.8: 5.8 has no FLogSuppressionInterface::FindCategory, so the ported SetVerbosity cannot read a category's
			// compile-time maximum (see LogsToolset.cpp); 5.8's FLogCategoryBase::SetVerbosity ensures and clamps instead.
			AddInfo(TEXT("UE 5.8's SetVerbosity cannot check the compile-time maximum; skipped."));
#if 0 // UE 5.8: skipped, see above
			const FString Category = LogLogsToolsetCappedTest.GetCategoryName().ToString();
			const FString Original = ULogsToolset59::GetVerbosity(Category);

			// Without validation this would trip an ensure in FLogCategoryBase::SetVerbosity.
			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([&Category]() -> void
				{
					ULogsToolset59::SetVerbosity(Category, TEXT("VeryVerbose"));
				});
			TestContains(
				TEXT("SetVerbosity raises error above the compile-time maximum"),
				ExceptionHandler.GetException(), TEXT("exceeds the compile-time maximum"));
			TestEqual(TEXT("Verbosity unchanged"),
				ULogsToolset59::GetVerbosity(Category), Original);
#endif
		});
	});
}

#endif  // WITH_DEV_AUTOMATION_TESTS
