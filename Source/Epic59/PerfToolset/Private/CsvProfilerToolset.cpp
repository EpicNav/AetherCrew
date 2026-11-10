// Copyright Epic Games, Inc. All Rights Reserved.

#include "CsvProfilerToolset.h"

#include "CsvProfilerToolsetUtils.h"

#include "CSVProfilerUtils.h"

#include "Interfaces/IPluginManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h" // UE 5.8: DetectCsvFormat58
#include "Serialization/Archive.h" // UE 5.8: DetectCsvFormat58

namespace UE::CsvProfilerToolset::Private
{
	// UE 5.8: 5.9 CsvUtils' ECsvFormat and DetectCsvFormat (CSVProfilerUtils.cpp), copied: 5.8's CsvUtils lacks them.
	enum class ECsvFormat58 : uint8
	{
		Csv,
		CsvBin,
		Unknown
	};

	ECsvFormat58 DetectCsvFormat58(const FString& FilePath)
	{
		if (TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath)); Reader.IsValid())
		{
			constexpr uint8 Magic[] = { 0x06, 'C', 'S', 'V', 'B', 'I', 'N' };
			if (Reader->TotalSize() >= static_cast<int64>(sizeof(Magic)))
			{
				uint8 Header[UE_ARRAY_COUNT(Magic)] = {};
				Reader->Serialize(Header, sizeof(Header));
				if (Reader->IsError())
				{
					return ECsvFormat58::Unknown;
				}
				if (FMemory::Memcmp(Header, Magic, sizeof(Magic)) == 0)
				{
					return ECsvFormat58::CsvBin;
				}
			}
		}
		if (FilePath.EndsWith(TEXT(".csv.bin"), ESearchCase::IgnoreCase))
		{
			return ECsvFormat58::CsvBin;
		}
		if (FilePath.EndsWith(TEXT(".csv"), ESearchCase::IgnoreCase))
		{
			return ECsvFormat58::Csv;
		}
		return ECsvFormat58::Unknown;
	}

	using namespace UE::CsvProfilerToolset;

	// Resolves a virtual content path to a filesystem path. '/Saved/' has no real package mount, so it
	// is mapped by hand; every other mount goes through the engine. Returns false for an unmapped path.
	// Mirrors the AssetTools read_file/edit_file and DataFetchToolset download tools so every
	// file-taking toolset shares one path scheme.
	bool ResolveVirtualContentPath(const FString& VirtualPath, FString& OutFilename)
	{
		FString Normalized = VirtualPath;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

		const FString SavedRoot(TEXT("/Saved/"));
		if (Normalized.StartsWith(SavedRoot, ESearchCase::IgnoreCase))
		{
			TArray<FString> Parts;
			Normalized.RightChop(SavedRoot.Len()).ParseIntoArray(Parts, TEXT("/"), /*InCullEmpty=*/true);

			FString Result = FPaths::ProjectSavedDir();
			for (const FString& Part : Parts)
			{
				Result /= Part;
			}

			OutFilename = Result;
			return true;
		}

		return FPackageName::TryConvertLongPackageNameToFilename(Normalized, OutFilename);
	}

	// Roots a resolved capture may sit under: project Content, project Saved, and every enabled plugin's
	// Content dir. Same allow-list as the AssetTools/download tools; the engine's own mounts (e.g.
	// '/Engine/') resolve but are deliberately excluded.
	TArray<FString> AllowedCaptureRoots()
	{
		TArray<FString> Roots;
		Roots.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
		Roots.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));

		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPluginsWithContent())
		{
			Roots.Add(FPaths::ConvertRelativePathToFull(Plugin->GetContentDir()));
		}

		return Roots;
	}

	// Resolves a virtual content path and confirms it lands under an allowed root. Fills OutFullPath on
	// success; fills OutError and returns false on rejection. FullPath is compared collapsed, so a '..'
	// escape is checked at its resolved location rather than matched literally.
	bool ResolveAndValidateCapturePath(const FString& VirtualPath, FString& OutFullPath, FString& OutError)
	{
		FString Filename;
		if (!ResolveVirtualContentPath(VirtualPath, Filename))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a recognized virtual content path. Pass a path under '/Game/', '/Saved/', or an enabled plugin mount such as '/PluginName/'."),
				*VirtualPath);
			return false;
		}

		const FString FullPath = FPaths::ConvertRelativePathToFull(Filename);
		for (const FString& Root : AllowedCaptureRoots())
		{
			if (FPaths::IsUnderDirectory(FullPath, Root))
			{
				OutFullPath = FullPath;
				return true;
			}
		}

		OutError = FString::Printf(
			TEXT("'%s' resolved to '%s', which is outside the allowed roots (project Content, project Saved, or an enabled plugin's Content)."),
			*VirtualPath, *FullPath);
		return false;
	}

	// Resolves and gates Path (a virtual content path), then reads it into OutCapture, dispatching to the
	// text or binary engine reader by CsvUtils::DetectCsvFormat. On a rejected path, unknown format, or
	// reader failure, fills OutError and returns false.
	bool ReadCapture(const FString& Path, CsvUtils::FCsvProfilerCapture& OutCapture, FString& OutError)
	{
		FString FullPath;
		if (!ResolveAndValidateCapturePath(Path, FullPath, OutError))
		{
			return false;
		}

		const ECsvFormat58 Format = DetectCsvFormat58(FullPath); // UE 5.8: 5.8's CsvUtils has no DetectCsvFormat
		if (Format == ECsvFormat58::Unknown)
		{
			OutError = FString::Printf(TEXT("Could not determine the format of '%s'; expected a readable .csv or .csv.bin capture."), *Path);
			return false;
		}

		TArray<FText> ReadErrors;
		const bool bRead = (Format == ECsvFormat58::CsvBin)
			? CsvUtils::ReadFromCsvBin(OutCapture, *FullPath, &ReadErrors)
			: CsvUtils::ReadFromCsv(OutCapture, *FullPath, &ReadErrors);

		if (!bRead)
		{
			if (ReadErrors.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Failed to read CSV capture from '%s'."), *Path);
			}
			else
			{
				TArray<FString> Parts;
				Parts.Reserve(ReadErrors.Num());
				for (const FText& Err : ReadErrors)
				{
					Parts.Add(Err.ToString());
				}
				OutError = FString::Printf(TEXT("Failed to read CSV capture from '%s': %s"), *Path, *FString::Join(Parts, TEXT("; ")));
			}
			return false;
		}

		return true;
	}

	// Frame count is the value count of any sample (all samples share one row per frame); a capture
	// with no samples has zero frames.
	int32 FrameCountOf(const CsvUtils::FCsvProfilerCapture& Capture)
	{
		const auto It = Capture.Samples.CreateConstIterator();
		return It ? It->Value.Values.Num() : 0;
	}

	// Reduces one sample over [Start, End) into OutReduction. End is clamped to the sample's own value
	// count first, guarding a hypothetical short column (every stat is full-length today). Returns false
	// when the range holds no values for this stat, leaving OutReduction's aggregates unset.
	bool ReduceSample(const CsvUtils::FCsvProfilerSample& Sample, int32 Start, int32 End, FCsvProfilerStatReduction& OutReduction)
	{
		const int32 ClampedEnd = FMath::Min(End, Sample.Values.Num());
		if (ClampedEnd <= Start)
		{
			return false;
		}

		const TConstArrayView<float> Slice(Sample.Values.GetData() + Start, ClampedEnd - Start);
		ReduceValues(Slice, OutReduction.Average, OutReduction.Min, OutReduction.Max, OutReduction.Total);

		// Percentiles need the values in order, so sort a copy (NaN-safe); the capture's own data is const.
		TArray<float> Sorted(Slice.GetData(), Slice.Num());
		Sorted.Sort(RanksBeforeAscending);
		OutReduction.P50 = NearestRankPercentile(Sorted, 0.50);
		OutReduction.P90 = NearestRankPercentile(Sorted, 0.90);
		OutReduction.P95 = NearestRankPercentile(Sorted, 0.95);
		OutReduction.P99 = NearestRankPercentile(Sorted, 0.99);
		return true;
	}
}

FCsvProfilerInfoResult UCsvProfilerToolset::GetCsvInfo(
	const FString& Path,
	const FString& StatNameFilter,
	bool bIncludeAllMetadata,
	int32 GroupDepth)
{
	using namespace UE::CsvProfilerToolset;
	using namespace UE::CsvProfilerToolset::Private;

	FCsvProfilerInfoResult Result;

	CsvUtils::FCsvProfilerCapture Capture;
	FString Error;
	if (!ReadCapture(Path, Capture, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(Error);
		return {};
	}

	Result.FrameCount = FrameCountOf(Capture);
	Result.StatCount = Capture.Samples.Num();

	// Group histogram from every stat; list individual names only when a filter asks for them.
	// An agent could pass 0 or a negative depth; clamp so the histogram always groups by at least one level.
	const int32 EffectiveGroupDepth = FMath::Max(GroupDepth, 1);
	const bool bHasFilter = !StatNameFilter.IsEmpty();
	for (const TPair<FString, CsvUtils::FCsvProfilerSample>& Pair : Capture.Samples)
	{
		Result.StatGroups.FindOrAdd(StatGroupOf(Pair.Key, EffectiveGroupDepth))++;

		if (bHasFilter && StatNameMatches(Pair.Key, StatNameFilter))
		{
			Result.StatNames.Add(Pair.Key);
		}
	}

	// Events can repeat a name across frames; report each distinct name once.
	TSet<FString> SeenEvents;
	for (const CsvUtils::FCsvProfilerEvent& Event : Capture.Events)
	{
		bool bAlreadySeen = false;
		SeenEvents.Add(Event.Name, &bAlreadySeen);
		if (!bAlreadySeen)
		{
			Result.EventNames.Add(Event.Name);
		}
	}

	// Metadata defaults to the curated survey subset; the full block is opt-in.
	if (bIncludeAllMetadata)
	{
		Result.Metadata = Capture.Metadata;
	}
	else
	{
		for (const TPair<FString, FString>& Pair : Capture.Metadata)
		{
			if (IsCuratedMetadataKey(Pair.Key))
			{
				Result.Metadata.Add(Pair.Key, Pair.Value);
			}
		}
	}

	return Result;
}

FCsvProfilerReduceResult UCsvProfilerToolset::ReduceStat(
	const FString& Path,
	const TArray<FString>& StatNames,
	int32 FrameStart,
	int32 FrameEnd)
{
	using namespace UE::CsvProfilerToolset;
	using namespace UE::CsvProfilerToolset::Private;

	FCsvProfilerReduceResult Result;

	CsvUtils::FCsvProfilerCapture Capture;
	FString Error;
	if (!ReadCapture(Path, Capture, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(Error);
		return {};
	}

	const int32 FrameCount = FrameCountOf(Capture);

	int32 ResolvedStart = 0;
	int32 ResolvedEnd = 0;
	if (!ResolveFrameRange(FrameStart, FrameEnd, FrameCount, ResolvedStart, ResolvedEnd, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(Error);
		return {};
	}

	// Stats absent from the capture (or with no values in the range) are omitted; the caller sees which
	// requested names resolved by the keys present in the result.
	for (const FString& StatName : StatNames)
	{
		const CsvUtils::FCsvProfilerSample* Sample = Capture.Samples.Find(StatName);
		if (Sample == nullptr)
		{
			continue;
		}

		FCsvProfilerStatReduction Reduction;
		Reduction.StatName = StatName;
		if (ReduceSample(*Sample, ResolvedStart, ResolvedEnd, Reduction))
		{
			Result.Stats.Add(StatName, MoveTemp(Reduction));
		}
	}

	return Result;
}

FCsvProfilerStatValues UCsvProfilerToolset::ReadStatValues(
	const FString& Path,
	const FString& StatName,
	int32 FrameStart,
	int32 Count)
{
	using namespace UE::CsvProfilerToolset;
	using namespace UE::CsvProfilerToolset::Private;

	FCsvProfilerStatValues Result;
	Result.StatName = StatName;
	Result.FrameStart = FrameStart;

	CsvUtils::FCsvProfilerCapture Capture;
	FString Error;
	if (!ReadCapture(Path, Capture, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(Error);
		return {};
	}

	if (FrameStart < 0)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Frame start %d is negative."), FrameStart));
		return {};
	}
	if (Count == 0 || Count < -1)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Count %d is invalid; pass a positive frame count, or -1 to read to the end."), Count));
		return {};
	}

	const CsvUtils::FCsvProfilerSample* Sample = Capture.Samples.Find(StatName);
	if (Sample == nullptr)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Stat '%s' is not present in the capture."), *StatName));
		return {};
	}

	// Resolve Count into an exclusive end within this stat's own column: -1 reads to the end, and an
	// explicit Count is capped at the column length so an over-large request clamps rather than errors.
	// The sum is widened to int64 so a very large Count cannot overflow past the clamp.
	const int32 ColumnLength = Sample->Values.Num();
	const int64 RequestedEnd = (Count == -1) ? ColumnLength : static_cast<int64>(FrameStart) + Count;
	const int32 End = static_cast<int32>(FMath::Min<int64>(RequestedEnd, ColumnLength));

	// SliceStatValues copies [FrameStart, End) and returns the end actually covered, keeping the reported
	// window [FrameStart, FrameEnd) in step with the values returned (the struct's contract).
	const int32 CoveredEnd = SliceStatValues(Sample->Values, FrameStart, End, Result.Values);
	if (CoveredEnd <= FrameStart)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Stat '%s' has no values at or after frame %d."), *StatName, FrameStart));
		return {};
	}

	Result.FrameEnd = CoveredEnd;
	return Result;
}

FCsvProfilerEventList UCsvProfilerToolset::ListEvents(
	const FString& Path,
	int32 Count,
	int32 FrameStart,
	int32 FrameEnd)
{
	using namespace UE::CsvProfilerToolset;
	using namespace UE::CsvProfilerToolset::Private;

	FCsvProfilerEventList Result;

	CsvUtils::FCsvProfilerCapture Capture;
	FString Error;
	if (!ReadCapture(Path, Capture, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(Error);
		return {};
	}

	// Events carry their own frame indices and can fire in a capture with no stat columns, so the range
	// is bounded by MAX_int32 rather than the stat frame count: an open or wide end still reaches every
	// event.
	int32 ResolvedStart = 0;
	int32 ResolvedEnd = 0;
	if (!ResolveFrameRange(FrameStart, FrameEnd, MAX_int32, ResolvedStart, ResolvedEnd, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(Error);
		return {};
	}

	// All events for one frame are contiguous (events are stored in frame order), so collapse repeats
	// of a name within that run by keying on the name. Interleaved names on a busy frame (a sync load
	// and its FlushAsyncLoading alternate) mean a repeat is not always adjacent, so matching only the
	// previous entry misses them. First-seen order is preserved, keeping the Count head deterministic.
	TArray<FCsvProfilerEventEntry> Collapsed;
	int32 CurrentFrame = INDEX_NONE;
	TMap<FString, int32> NameToIndexThisFrame;
	for (const CsvUtils::FCsvProfilerEvent& Event : Capture.Events)
	{
		if (Event.Frame < ResolvedStart || Event.Frame >= ResolvedEnd)
		{
			continue;
		}

		if (Event.Frame != CurrentFrame)
		{
			CurrentFrame = Event.Frame;
			NameToIndexThisFrame.Reset();
		}

		if (const int32* ExistingIndex = NameToIndexThisFrame.Find(Event.Name))
		{
			Collapsed[*ExistingIndex].Count++;
			continue;
		}

		FCsvProfilerEventEntry Entry;
		Entry.Name = Event.Name;
		Entry.Frame = Event.Frame;
		NameToIndexThisFrame.Add(Event.Name, Collapsed.Num());
		Collapsed.Add(MoveTemp(Entry));
	}

	// Report the full matched count, then return only the requested head (up to Count) so the caller
	// controls how much crosses into context; a caller sees ConsideredCount exceed Events.Num() and
	// can raise Count or narrow with a range.
	Result.ConsideredCount = Collapsed.Num();

	const int32 ResultCount = FMath::Clamp(Count, 0, Collapsed.Num());
	Result.Events.Reserve(ResultCount);
	for (int32 Index = 0; Index < ResultCount; ++Index)
	{
		Result.Events.Add(MoveTemp(Collapsed[Index]));
	}

	return Result;
}

FCsvProfilerTopStatsResult UCsvProfilerToolset::TopStats(
	const FString& Path,
	int32 Count,
	ECsvProfilerStatRanking RankBy,
	const FString& StatNameFilter,
	int32 FrameStart,
	int32 FrameEnd)
{
	using namespace UE::CsvProfilerToolset;
	using namespace UE::CsvProfilerToolset::Private;

	FCsvProfilerTopStatsResult Result;

	CsvUtils::FCsvProfilerCapture Capture;
	FString Error;
	if (!ReadCapture(Path, Capture, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(Error);
		return {};
	}

	const int32 FrameCount = FrameCountOf(Capture);

	int32 ResolvedStart = 0;
	int32 ResolvedEnd = 0;
	if (!ResolveFrameRange(FrameStart, FrameEnd, FrameCount, ResolvedStart, ResolvedEnd, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(Error);
		return {};
	}

	// Reduce every stat passing the name filter; only the ranked head is returned.
	const bool bHasFilter = !StatNameFilter.IsEmpty();
	TArray<FCsvProfilerStatReduction> Reductions;
	for (const TPair<FString, CsvUtils::FCsvProfilerSample>& Pair : Capture.Samples)
	{
		if (bHasFilter && !StatNameMatches(Pair.Key, StatNameFilter))
		{
			continue;
		}

		FCsvProfilerStatReduction Reduction;
		Reduction.StatName = Pair.Key;
		if (ReduceSample(Pair.Value, ResolvedStart, ResolvedEnd, Reduction))
		{
			Reductions.Add(MoveTemp(Reduction));
		}
	}

	Result.ConsideredCount = Reductions.Num();

	Reductions.Sort([RankBy](const FCsvProfilerStatReduction& A, const FCsvProfilerStatReduction& B)
	{
		const auto RankKey = [RankBy](const FCsvProfilerStatReduction& R) -> double
		{
			switch (RankBy)
			{
			case ECsvProfilerStatRanking::Max:
				return static_cast<double>(R.Max);
			case ECsvProfilerStatRanking::Total:
				return R.Total;
			case ECsvProfilerStatRanking::P50:
				return static_cast<double>(R.P50);
			case ECsvProfilerStatRanking::P90:
				return static_cast<double>(R.P90);
			case ECsvProfilerStatRanking::P95:
				return static_cast<double>(R.P95);
			case ECsvProfilerStatRanking::P99:
				return static_cast<double>(R.P99);
			case ECsvProfilerStatRanking::Average:
			default:
				return R.Average;
			}
		};

		return RanksAheadDescending(RankKey(A), RankKey(B));
	});

	const int32 ResultCount = FMath::Clamp(Count, 0, Reductions.Num());
	Result.Stats.Reserve(ResultCount);
	for (int32 Index = 0; Index < ResultCount; ++Index)
	{
		Result.Stats.Add(MoveTemp(Reductions[Index]));
	}

	return Result;
}

bool UCsvProfilerToolset::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
