// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class USoundWave;

/**
 * Players for a set of SoundWaves, which the audio generation pack wraps its clips in. Each makes a new asset at Package
 * (refusing one already there), saves it and returns its object path; empty, with OutError, when it could not.
 * Game thread only.
 */
struct HYPERAISTUDIOAUDIOAUTHORING_API FHyperAIStudioAudioWrappers
{
	/**
	 * A MetaSound Source. Mode one_shot plays the first wave once, loop loops it, random plays one of the waves at random
	 * on each play, never the same one twice in a row. PitchSemitones and Volume are the ranges each play is picked from:
	 * (0, 0) and (1, 1) leave them fixed.
	 */
	static FString MakeMetaSoundPlayer(const FString& Package, const TArray<USoundWave*>& Waves, const FString& Mode,
		FVector2D PitchSemitones, FVector2D Volume, FString& OutError);

	/** A Sound Cue: a Random node over the waves (no repeats), under a Modulator with the pitch and volume ranges, looping when bLoop. */
	static FString MakeRandomCue(const FString& Package, const TArray<USoundWave*>& Waves, FVector2D PitchSemitones, FVector2D Volume,
		bool bLoop, FString& OutError);
};
