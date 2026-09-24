// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioLightingToolset.h"

/**
 * hyper_image_compare: how close an attempt is to a reference, and where it differs. Measures are taken at a working
 * size with the candidate cropped to the reference's shape, so a reference of any size compares with a capture of any
 * size. Sessions live under Saved/HyperAIStudio/Match/<id>, one session.json and the images of each iteration.
 */
namespace HyperAIStudio::Lighting::Match
{
	FHyperAIImageCompareReport Compare(const FHyperAIImageCompareRequest& Request);
	/** Compares pixels already in memory; Compare loads the files and records the session around it. */
	FHyperAIImageCompareReport ComparePixels(const TArray<FColor>& Reference, int32 ReferenceWidth, int32 ReferenceHeight,
		const TArray<FColor>& Candidate, int32 CandidateWidth, int32 CandidateHeight, const FString& Mode, float TargetScore, int32 Grid);
	FString SessionsFolder();
	/** The newest session comparing against this reference, if any: its id, or empty. */
	FString FindSessionFor(const FString& ReferenceImagePath);
	/** A session's reference, mode, target and history as the last compare left them; false when there is none. */
	bool ReadSession(const FString& SessionId, FString& OutReference, FString& OutMode, TArray<FHyperAIImageIteration>& OutHistory, float& OutTargetScore);
}
