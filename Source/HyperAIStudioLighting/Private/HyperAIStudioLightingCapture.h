// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioLightingToolset.h"
#include "HyperAIStudioRequestFit.h"

/**
 * View captures for hyper_capture_view: final-colour frames from a camera, a transform, or the level viewport.
 *
 * Proof tools write only under Saved/HyperAIStudio/Captures, like the compare tool's own capture, so they need no
 * approval. A capture draws the world through its own scene-capture component, so nothing in the level moves.
 */
namespace HyperAIStudio::Lighting::Capture
{
	/** Starts a capture that may span editor ticks; the result completes when the last frame is written. */
	UHyperAICaptureViewResult* Start(const FHyperAICaptureViewRequest& Request);

	/** Captures without waiting between frames. A world-time capture of two or more frames needs Start instead. */
	FHyperAICaptureViewReport RunNow(const FHyperAICaptureViewRequest& Request);

	/** Completes every pending capture with an error; called when the pack unloads. */
	void CancelAll();

	/** A request's near misses fitted to what a capture takes, each change noted in Out; Start and RunNow do this first. */
	void Fit(FHyperAICaptureViewRequest& Request, FHyperAIStudioRequestFit& Out);

	/** Mean absolute RGB change (0-1) and share of pixels whose largest channel change exceeds Threshold. */
	void Diff(TConstArrayView<FColor> A, TConstArrayView<FColor> B, float Threshold, float& OutMeanAbs, float& OutChangedFraction);

	/** Starts noting edits to the level, which captures wait to show; the module calls these. */
	void Startup();
	void Shutdown();

	/**
	 * Whether a capture may shoot: the level's latest edit is old enough to have shown (SecondsSinceEdit is negative
	 * when none is known) and the last two draws matched, or settling ran out of time (bOutTimedOut).
	 */
	bool IsSettled(double SecondsSettling, double SecondsSinceEdit, uint64 FramesSinceEdit, float LastChange, bool& bOutTimedOut);
}
