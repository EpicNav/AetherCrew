// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * An agent's request limits fitted into what a tool takes, instead of refusing the call. A page size, a time budget or
 * an output limit just out of range is a guess, and refusing it costs the agent a whole round trip to learn the range.
 * Every change is noted, so the reply says what was used and the agent learns the range all the same.
 */
struct FHyperAIStudioRequestFit
{
	TArray<FString> Notes;

	/** Value moved to the nearest end of Min..Max, noted as "Name was -> now". */
	template <typename TValue>
	void Clamp(TValue& Value, const TValue Min, const TValue Max, const TCHAR* Name)
	{
		const TValue Fitted = FMath::Clamp(Value, Min, Max);
		if (Fitted != Value)
		{
			Note(FString::Printf(TEXT("%s %s -> %s"), Name, *Text(Value), *Text(Fitted)));
			Value = Fitted;
		}
	}

	/** Zero or less means the agent did not set it: the tool's default, without a note. Then Clamp. */
	template <typename TValue>
	void ClampOrDefault(TValue& Value, const TValue Min, const TValue Max, const TValue Default, const TCHAR* Name)
	{
		if (Value <= TValue(0))
		{
			Value = Default;
		}
		Clamp(Value, Min, Max, Name);
	}

	/** A dry run changes nothing, so the submission fields an agent filled in anyway are dropped before any lookup sees them. */
	void DryRun(const bool bDryRun, FString& OperationId, FString& ExpectedPlanHash)
	{
		if (bDryRun && (!OperationId.IsEmpty() || !ExpectedPlanHash.IsEmpty()))
		{
			Note(TEXT("operation_id and expected_plan_hash are unused on a dry run"));
			OperationId.Reset();
			ExpectedPlanHash.Reset();
		}
	}

	/**
	 * Build(Request) again under "<id>-2", "-3", ... while the agent's operation_id belongs to an earlier, different edit,
	 * as a descriptive id reused in a later session does. The same plan under the same id still replays: only a conflict
	 * retries, and each retry prepares afresh because the refused submission consumed its stage.
	 */
	template <typename TRequest, typename TBuild>
	auto FreshOperationId(TRequest& Request, TBuild&& Build, const TCHAR* ConflictStatus = TEXT("operation_id_conflict"))
	{
		auto Report = Build(Request);
		const FString Requested = Request.OperationId;
		for (int32 Suffix = 2; Suffix <= 9 && Report.Status == ConflictStatus; ++Suffix)
		{
			// Ids are at most 128 characters, so the suffix always fits.
			Request.OperationId = FString::Printf(TEXT("%s-%d"), *Requested.Left(125), Suffix);
			Report = Build(Request);
		}
		if (Request.OperationId != Requested && Report.Status != ConflictStatus)
		{
			Note(FString::Printf(TEXT("operation_id %s belongs to an earlier edit, so this one is %s; wait on that"),
				*Requested, *Request.OperationId));
		}
		return Report;
	}

	void Note(const FString& Change)
	{
		Notes.Add(Change);
	}

	/** "Adjusted to fit: a 30 -> 8; b ..." after Diagnostic, or Diagnostic as it was when nothing changed. */
	FString AppendTo(const FString& Diagnostic) const
	{
		if (Notes.IsEmpty())
		{
			return Diagnostic;
		}
		const FString Summary = FString::Printf(TEXT("Adjusted to fit: %s."), *FString::Join(Notes, TEXT("; ")));
		return Diagnostic.IsEmpty() ? Summary : Diagnostic + TEXT(" ") + Summary;
	}

	template <typename TValue>
	static FString Text(const TValue Value)
	{
		if constexpr (std::is_floating_point_v<TValue>)
		{
			return FString::SanitizeFloat(Value);
		}
		else
		{
			return LexToString(Value);
		}
	}
};
