// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FHyperAIPerfTimer;

namespace TraceServices
{
	class IAnalysisSession;
}

/**
 * The hottest CPU timers of a play session, from an Unreal Insights trace recorded to a file while it ran and analysed in
 * this editor with TraceServices' timing provider.
 */
namespace HyperAIStudio::Profiling::Trace
{
	/**
	 * Starts recording the cpu and frame channels to Path. False, with Why, when a trace is already being recorded (by
	 * Unreal Insights, -trace or another tool): a second one cannot start, and that one is left alone.
	 */
	bool Start(const FString& Path, FString& OutWhy);
	/** Stops the recording Start began and turns off the channels it turned on. */
	void Stop();
	/** True while the trace Start began is still being written. */
	bool IsWriting();

	struct FHotFunctions
	{
		bool bOk = false;
		FString Note;
		/** Game, render and RHI thread timers over the whole trace, by exclusive time. */
		TArray<FHyperAIPerfTimer> Session;
		/** Game and render thread timers inside the slowest game frame. */
		TArray<FHyperAIPerfTimer> WorstFrame;
		double WorstFrameMs = 0.0;
		int64 GameFrames = 0;
	};

	/**
	 * Starts analysing the trace at Path. Game thread only: TraceServices hands a new analysis to every trace module
	 * (Chaos Visual Debugger, Audio Insights...), and those insist on the game thread. Null when the file cannot be opened.
	 */
	TSharedPtr<const TraceServices::IAnalysisSession> BeginAnalysis(const FString& Path);
	/** The TopN timers of a finished analysis. Reads only the session, so it may run off the game thread. */
	FHotFunctions Summarise(const TraceServices::IAnalysisSession& Session, int32 TopN);
}
