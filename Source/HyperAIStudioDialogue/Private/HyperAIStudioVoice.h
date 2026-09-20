// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "HyperAIStudioVoice.generated.h"

/**
 * One character's voice: the clip a speech model clones the timbre from, and the description that pins the
 * register. Both are needed. The clip alone gives a voice that sounds right and performs flat; the description
 * alone gives a performance in nobody's voice. Keeping them on an asset is what makes a character sound like
 * themselves across a hundred lines, and what lets the engine underneath change without the timbre moving.
 */
UCLASS(BlueprintType)
class UHyperAIStudioVoice final : public UDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * File name of the reference clip, inside the Voice Reference Folder in settings. At least ten seconds of
	 * clean speech from one speaker. Use a clip you have the right to clone.
	 */
	UPROPERTY(EditAnywhere, Category = "Voice")
	FString ReferenceClip;

	/**
	 * Who is speaking, in a sentence, as a director would say it: age, build of the voice, and its habitual
	 * register. "A weary man in his fifties, low gravelly voice". This opens every prompt for the character.
	 */
	UPROPERTY(EditAnywhere, Category = "Voice", meta = (MultiLine = "true"))
	FString SpeakerDescription;

	/**
	 * Fixed per voice, so a re-render of the same line comes back the same. Change it to draw a different
	 * reading of the whole character; leave it alone to keep one.
	 */
	UPROPERTY(EditAnywhere, Category = "Voice")
	int32 Seed = 1;

	/** How closely the model follows the prompt. Higher is more literal and less natural. */
	UPROPERTY(EditAnywhere, Category = "Performance", meta = (ClampMin = "1.0", ClampMax = "10.0"))
	float CfgScale = 3.0f;

	/** Spatio-temporal guidance. Higher steadies the delivery; too high flattens it. */
	UPROPERTY(EditAnywhere, Category = "Performance", meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float StgScale = 1.0f;

	/** Stretches or compresses the reading. Below one is faster. */
	UPROPERTY(EditAnywhere, Category = "Performance", meta = (ClampMin = "0.5", ClampMax = "2.0"))
	float DurationMultiplier = 1.0f;

	/** How much the voice breathes between phrases. */
	UPROPERTY(EditAnywhere, Category = "Performance", meta = (ClampMin = "0.5", ClampMax = "2.0"))
	float BreathingFactor = 1.1f;

	/** Seconds of the reference clip the model listens to. */
	UPROPERTY(EditAnywhere, Category = "Performance", meta = (ClampMin = "1.0", ClampMax = "30.0"))
	float ReferenceWindow = 10.0f;

	/** True when this voice has everything a render needs. */
	bool IsRenderable(FString& OutReason) const
	{
		if (ReferenceClip.IsEmpty())
		{
			OutReason = TEXT("This voice has no reference clip.");
			return false;
		}
		if (SpeakerDescription.IsEmpty())
		{
			OutReason = TEXT("This voice has no speaker description, so every line would be performed in no particular register.");
			return false;
		}
		return true;
	}
};
