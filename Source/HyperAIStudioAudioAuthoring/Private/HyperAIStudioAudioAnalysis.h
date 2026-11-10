// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAudioAuthoringToolset.h"

class UMetaSoundSource;
class USoundWave;

/** Listening without an audio device: a MetaSound rendered through its own generator, a SoundWave's samples, and what they measure. */
namespace HyperAIStudio::AudioAuthoring::Analysis
{
	/** Interleaved float samples. */
	struct FBuffer
	{
		TArray<float> Samples;
		int32 SampleRate = 48000;
		int32 Channels = 1;
		/** A MetaSound one-shot finished (On Finished) before the render's end. */
		bool bFinished = false;
	};

	/** Renders a fresh play of Source offline, up to MaxSeconds, with graph inputs set from Parameters. */
	bool RenderMetaSound(UMetaSoundSource& Source, int32 SampleRate, float MaxSeconds, const TArray<FHyperAIAudioSetting>& Parameters, FBuffer& Out, FString& OutError);

	/** A SoundWave's imported samples (editor data), as floats. */
	bool ReadWave(const USoundWave& Wave, FBuffer& Out, FString& OutError);

	/** Loudness (BS.1770), levels, onset, centroid, tail, clipping and the loop seam of a buffer. */
	FHyperAIAudioMeasure Measure(const FBuffer& Buffer, bool bEnvelope);

	FHyperAIAudioAnalyzeReport Analyze(const FHyperAIAudioAnalyzeRequest& Request);
}
