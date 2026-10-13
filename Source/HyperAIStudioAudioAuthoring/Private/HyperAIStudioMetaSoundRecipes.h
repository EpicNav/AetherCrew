// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAudioAuthoringToolset.h"

class USoundWave;

/** Procedural MetaSound Sources built from recipes, and MetaSound players for a set of SoundWaves. */
namespace HyperAIStudio::AudioAuthoring::MetaSounds
{
	/** Every recipe, or the one named; empty when the name is unknown. */
	TArray<FHyperAIMetaSoundRecipeInfo> Recipes(const FString& Only = FString());

	/** Checks the recipe's input overrides against its inputs: known names, numbers in range. */
	bool CheckInputs(const FString& Recipe, const TArray<FHyperAIAudioSetting>& Inputs, FString& OutError);

	/** Builds the recipe into a new MetaSound Source at Package and saves it; its object path, or empty with OutError. */
	FString BuildRecipe(const FString& Recipe, const FString& Package, const TArray<FHyperAIAudioSetting>& Inputs, int32& OutNodeCount, FString& OutError);

	/** A MetaSound Source playing Waves: one_shot, loop or random (see FHyperAIStudioAudioWrappers). */
	FString BuildPlayer(const FString& Package, const TArray<USoundWave*>& Waves, const FString& Mode, FVector2D PitchSemitones, FVector2D Volume,
		FString& OutError);

	/** A MetaSound Source's graph inputs, as Name (Type). */
	TArray<FString> DescribeInputs(const UObject& MetaSoundSource);
}
