// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class FHyperAIFxCostListener;
class UWorld;
struct FHyperAIPerfFxCost;
struct FHyperAIPerfNaniteLive;
struct FHyperAIPerfNiagaraLive;

namespace HyperAIStudio::Profiling
{
	struct FCsvCapture;
}

/**
 * What the running game draws, read on the game thread without changing anything: Nanite's streaming state and which
 * meshes draw with it, the Niagara systems playing and their effect types' caps, and the renderer's headline settings.
 */
namespace HyperAIStudio::Profiling::Live
{
	/** Nanite on this platform, its streaming pool, and the play world's Nanite, fallback and plain static meshes. */
	void ReadNanite(UWorld& World, int32 TopN, FHyperAIPerfNaniteLive& Out);
	/** Sampled Nanite streaming figures from a short CSV capture (NaniteStreaming and NaniteStreamingDetail categories). */
	void ApplyNaniteSample(const FCsvCapture& Csv, FHyperAIPerfNaniteLive& Out);
	/** The play world's Niagara components: systems, emitters, CPU and GPU particles, effect types and the FX budget. */
	void ReadNiagara(UWorld& World, int32 TopN, FHyperAIPerfNiagaraLive& Out);
	/** Puts sampled per-system costs on the systems listed, and lists systems over budget. */
	void ApplyFxCosts(const TArray<FHyperAIPerfFxCost>& Costs, int32 TopN, FHyperAIPerfNiagaraLive& Out);
	/** Lumen, virtual shadow maps, MegaLights, screen percentage, vsync and frame limit, and the last frame's figures. */
	TArray<FString> ReadRenderer();
	/** Sampled renderer figures: shadow pages, render target pool. */
	void ApplyRendererSample(const FCsvCapture& Csv, TArray<FString>& Out);

	/**
	 * Measures each Niagara system's game-thread, render-thread and GPU cost while it is alive, with the engine's particle
	 * perf stats (the listener fx.ParticlePerfStats uses). Game thread only.
	 */
	class FFxCostSampler
	{
	public:
		FFxCostSampler();
		~FFxCostSampler();
		/** Costs per frame since the sampler began, busiest on the game thread first. */
		TArray<FHyperAIPerfFxCost> Read(int32 TopN);
		/** False when particle perf stats are off (fx.ParticlePerfStats.Enabled 0). */
		bool IsGathering() const { return bGathering; }

	private:
		TSharedPtr<::FHyperAIFxCostListener, ESPMode::ThreadSafe> Listener;
		bool bGathering = false;
	};
}
