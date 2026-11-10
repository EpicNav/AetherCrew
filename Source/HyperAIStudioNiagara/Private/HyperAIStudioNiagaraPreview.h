// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioNiagaraToolset.h"

/**
 * hyper_niagara_preview: a System simulated in its own FPreviewScene (never the level), seeked to each age with
 * AdvanceSimulation, and drawn by a scene capture with fixed exposure on a plain background. Every age becomes a tile
 * of one PNG contact sheet under Saved/HyperAIStudio/Captures/Niagara, measured as it is read back. Game thread only.
 */
namespace HyperAIStudio::Niagara::Preview
{
	FHyperAINiagaraPreviewReport Render(const FHyperAINiagaraPreviewRequest& Request);

	/** Coverage, mean colour, peak luminance and screen box of the pixels that differ from the empty Plate. */
	void Measure(const TArray<FColor>& Pixels, const TArray<FColor>& Plate, int32 Width, int32 Height, FHyperAINiagaraPreviewFrame& OutFrame);
}
