// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioNiagaraToolset.h"

class UWorld;

/**
 * hyper_niagara_live: what each Niagara system playing in a world is doing, per emitter, read on the game thread without
 * changing anything. hyper_profile_live measures what the systems cost; this says whether they work: particles, bounds,
 * spawning, sim target, state and the reasons an effect shows nothing.
 */
namespace HyperAIStudio::Niagara::Live
{
	/** Every Niagara component registered in World whose system, actor or component matches Filter (case aside). */
	void Read(UWorld& World, const FString& Filter, bool bIncludeInactive, FHyperAINiagaraLiveReport& Out);
	/**
	 * Takes the counters of Before (a read SampleSeconds earlier) into After: what each emitter spawned in between, and
	 * from that whether it is spawning. Emitters are matched by component path and emitter name.
	 */
	void ApplySample(const FHyperAINiagaraLiveReport& Before, double SampleSeconds, FHyperAINiagaraLiveReport& After);
	/** Issues that need the sample (active with nothing alive or spawning), the busiest TopN systems first, and Errors flattened. */
	void Finish(int32 TopN, FHyperAINiagaraLiveReport& Out);
	/** One read of the play world now, no sample window: what an adapter dispatch answers. */
	FHyperAINiagaraLiveReport ReadOnce(const FHyperAINiagaraLiveRequest& Request);
	/** "min (x,y,z) max (x,y,z)", or "empty". */
	FString BoundsText(const FBox& Box);
}
