// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * Colour-grading LUTs as Unreal's post process takes them: 16 x 16 x 16 in sRGB, unwrapped to a 256 x 16 strip with blue
 * picking the 16-wide slice, red running across it and green down it. Fitted from an attempt to a reference, a LUT carries
 * the reference's grade, the part of a look a colourist adds after the light, which light alone cannot reach.
 */
namespace HyperAIStudio::Lighting::Lut
{
	constexpr int32 Size = 16;
	constexpr int32 StripWidth = Size * Size;
	constexpr int32 StripHeight = Size;

	/** Output colour by input colour, sRGB 0-1, red fastest: Table[(B * Size + G) * Size + R]. */
	using FTable = TArray<FVector3f>;

	FTable Identity();
	/**
	 * The table that takes Candidate's colours to Reference's. Aligned, pixel by pixel, for two images of the same size that
	 * frame the same view, where colours the attempt never shows follow an overall colour transform fitted alongside;
	 * otherwise to the look, a tone curve for lightness and colour by band of lightness.
	 */
	FTable Fit(TConstArrayView<FColor> Candidate, TConstArrayView<FColor> Reference, bool bAligned);
	FVector3f Sample(const FTable& Table, const FVector3f& Colour);
	/** First, then Second. */
	FTable Compose(const FTable& Second, const FTable& First);
	/** As the post process blends a LUT by its intensity: no change at 0. */
	FTable Weaken(const FTable& Table, float Intensity);
	TArray<FColor> Apply(const FTable& Table, TConstArrayView<FColor> Pixels);
	TArray<FColor> ToStrip(const FTable& Table);
	/** False unless Strip is 256 x 16. */
	bool FromStrip(TConstArrayView<FColor> Strip, int32 Width, int32 Height, FTable& OutTable);
}
