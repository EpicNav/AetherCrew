// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/UnrealString.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AssertionMacros.h"

namespace UE::CsvProfilerToolset
{
	// Validates a half-open [Start, End) range against a capture of FrameCount frames. Rejects (never
	// clamps) an out-of-bounds or empty range, filling OutError and returning false.
	inline bool ValidateFrameRange(int32 Start, int32 End, int32 FrameCount, FString& OutError)
	{
		if (Start < 0)
		{
			OutError = FString::Printf(TEXT("Frame range start %d is negative."), Start);
			return false;
		}
		if (Start >= FrameCount)
		{
			OutError = FString::Printf(TEXT("Frame range start %d is at or past the frame count %d."), Start, FrameCount);
			return false;
		}
		if (End <= Start)
		{
			OutError = FString::Printf(TEXT("Frame range end %d must be greater than start %d."), End, Start);
			return false;
		}
		if (End > FrameCount)
		{
			OutError = FString::Printf(TEXT("Frame range end %d is past the frame count %d."), End, FrameCount);
			return false;
		}
		return true;
	}

	// Resolves the shared (FrameStart, FrameEnd) pair into a concrete [OutStart, OutEnd) window. FrameEnd
	// -1 runs to the end (OutEnd -> RangeCeiling); FrameStart has no open sentinel, since a range always
	// starts at a known frame. The default whole-run request on a zero-frame capture yields an empty
	// result rather than a range error. A bound is validated against RangeCeiling, the exclusive upper
	// bound the range lives within (the stat frame count, or MAX_int32 for events). Returns false only
	// for an invalid range.
	inline bool ResolveFrameRange(int32 FrameStart, int32 FrameEnd, int32 RangeCeiling, int32& OutStart, int32& OutEnd, FString& OutError)
	{
		OutStart = FrameStart;
		OutEnd = (FrameEnd == -1) ? RangeCeiling : FrameEnd;

		// A capture with no frames (RangeCeiling 0) has no range to validate against; the default
		// whole-run request [0, end) resolves to an empty window instead of raising a range error.
		if (RangeCeiling == 0 && FrameStart == 0 && FrameEnd == -1)
		{
			return true;
		}

		return ValidateFrameRange(OutStart, OutEnd, RangeCeiling, OutError);
	}

	// Derives all four aggregates in one pass over the per-frame slice. Min/Max/Total come
	// straight from Values (never from the engine's precomputed Average/Total fields, which are
	// zero for text CSV); Total is defined as the sum over this slice and Average as Total/N.
	// Slice must be non-empty; callers guarantee that via ValidateFrameRange (width >= 1).
	inline void ReduceValues(TConstArrayView<float> Slice, double& OutAverage, float& OutMin, float& OutMax, double& OutTotal)
	{
		check(!Slice.IsEmpty());

		float Min = Slice[0];
		float Max = Slice[0];
		double Total = 0.0;

		for (const float Value : Slice)
		{
			Min = FMath::Min(Min, Value);
			Max = FMath::Max(Max, Value);
			Total += static_cast<double>(Value);
		}

		OutMin = Min;
		OutMax = Max;
		OutTotal = Total;
		OutAverage = Total / static_cast<double>(Slice.Num());
	}

	// Copies the half-open [FrameStart, End) slice out of a stat's own value array, clamping End down to
	// the array length so a short column can never read past its data. Returns the exclusive end actually
	// covered, so returned-end == FrameStart + OutValues.Num() whenever anything is copied; that keeps a
	// caller's reported window in step with the values returned. A FrameStart at or past the column copies
	// nothing and returns an end <= FrameStart. Callers guarantee FrameStart >= 0.
	inline int32 SliceStatValues(TConstArrayView<float> Values, int32 FrameStart, int32 End, TArray<float>& OutValues)
	{
		const int32 ClampedEnd = FMath::Min(End, Values.Num());
		if (ClampedEnd > FrameStart)
		{
			OutValues.Append(Values.GetData() + FrameStart, ClampedEnd - FrameStart);
		}
		return ClampedEnd;
	}

	// Strict-weak-ordering predicate for ranking stats highest-first that stays valid when a key is NaN.
	// A raw A > B is not a valid sort predicate once either operand is NaN (NaN compares false both ways),
	// which can drive TArray::Sort's introsort past the end of the range; this orders finite keys
	// descending and sinks NaN to the bottom, treating NaN-vs-NaN (and equal keys) as unordered.
	inline bool RanksAheadDescending(double A, double B)
	{
		const bool bANaN = FMath::IsNaN(A);
		const bool bBNaN = FMath::IsNaN(B);
		if (bANaN || bBNaN)
		{
			return !bANaN && bBNaN;
		}
		return A > B;
	}

	// Ascending counterpart of RanksAheadDescending: orders finite keys ascending and sinks NaN to the end
	// so sorting a slice that may contain NaN stays a valid strict-weak-ordering (a raw A < B is not, since
	// NaN compares false both ways and can drive introsort past the range).
	inline bool RanksBeforeAscending(float A, float B)
	{
		const bool bANaN = FMath::IsNaN(A);
		const bool bBNaN = FMath::IsNaN(B);
		if (bANaN || bBNaN)
		{
			return !bANaN && bBNaN;
		}
		return A < B;
	}

	// Nearest-rank percentile of an ascending-sorted, non-empty slice: the value at 1-based rank
	// ceil(Fraction * N), the smallest value at or above Fraction of the data. Returns an actual sample
	// (no interpolation); Fraction is clamped so 0 maps to the minimum and 1 to the maximum.
	inline float NearestRankPercentile(TConstArrayView<float> SortedAscending, double Fraction)
	{
		check(!SortedAscending.IsEmpty());

		const int32 Num = SortedAscending.Num();
		const int32 Rank = FMath::Clamp(static_cast<int32>(FMath::CeilToDouble(Fraction * Num)), 1, Num);
		return SortedAscending[Rank - 1];
	}

	// Group label for a stat name: its ancestor path, capped at Depth '/'-delimited levels. The final
	// segment is the leaf metric and is never a group, so "Exclusive/GameThread/X" groups under
	// "Exclusive" at depth 1 and "Exclusive/GameThread" at depth 2 or deeper. A name with fewer than
	// Depth levels groups under its full parent path (everything before the last '/'); a name with no
	// '/' is a bare leaf and buckets under "(ungrouped)". Depth is assumed >= 1.
	inline FString StatGroupOf(const FString& StatName, int32 Depth = 1)
	{
		int32 FirstSlash = INDEX_NONE;
		if (!StatName.FindChar(TEXT('/'), FirstSlash))
		{
			return TEXT("(ungrouped)");
		}

		// Walk to the Depth-th '/': the group key is everything before it. With fewer than Depth
		// slashes, fall back to the last '/' so the leaf metric is never returned as its own group.
		int32 SlashesSeen = 0;
		int32 LastSlash = FirstSlash;
		for (int32 CharIndex = 0; CharIndex < StatName.Len(); ++CharIndex)
		{
			if (StatName[CharIndex] == TEXT('/'))
			{
				LastSlash = CharIndex;
				if (++SlashesSeen == Depth)
				{
					return StatName.Left(CharIndex);
				}
			}
		}

		return StatName.Left(LastSlash);
	}

	// True when StatName matches the wildcard Filter. '*' matches any sequence, '?' a single char.
	// Empty-filter semantics differ per caller (list nothing vs. match all), so callers test for an
	// empty filter themselves before calling this.
	inline bool StatNameMatches(const FString& StatName, const FString& Filter)
	{
		return StatName.MatchesWildcard(Filter, ESearchCase::IgnoreCase);
	}

	// Metadata keys GetCsvInfo returns when full metadata is not requested. A capture carries ~150
	// metadata fields, some very large (the command line); this fixed high-value subset keeps the
	// default response small. Matched case-insensitively against the capture's keys.
	inline const TCHAR* const CuratedMetadataKeys[] =
	{
		TEXT("platform"),
		TEXT("deviceprofile"),
		TEXT("buildversion"),
		TEXT("engineversion"),
		TEXT("changelist"),
		TEXT("config"),
		TEXT("targetframerate"),
		TEXT("captureduration"),
	};

	// True when Key is one of the curated survey keys (case-insensitive).
	inline bool IsCuratedMetadataKey(const FString& Key)
	{
		for (const TCHAR* Curated : CuratedMetadataKeys)
		{
			if (Key.Equals(Curated, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
}
