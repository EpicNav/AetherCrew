// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioBot.h"
#include "HyperAIStudioProfileAnalysis.h"
#include "HyperAIStudioProfileTrace.h"
#include "HyperAIStudioProfilingToolset.h"

/**
 * One measured stretch of Play In Editor: attach to it or start it, wait out a warm-up, then record the CSV profiler (and
 * optionally an Insights trace, Niagara's per-system cost and a bot route) for a set time, and hand back what was
 * recorded. Game thread only; one run at a time, since the CSV profiler and the trace are each one per process.
 */
namespace HyperAIStudio::Profiling::Session
{
	struct FRunOptions
	{
		/** Names the run: its folder, its job in Activity, and the operation hyper_operation_wait follows. */
		FString Id;
		bool bStartPie = false;
		double WarmupSeconds = 0.0;
		double DurationSeconds = 10.0;
		bool bTrace = false;
		bool bFx = true;
		bool bSceneCost = false;
		/** Read Nanite, Niagara and renderer figures at the end, while play still runs. */
		bool bLive = false;
		int32 TopN = 10;
		int32 LiveTopN = 10;
		TArray<FHyperAIBotStep> Bot;
		/** Where the run's files go. */
		FString Directory;
		/** CSV profiler categories turned on for the run (and back off after): NaniteStreamingDetail, VSM. */
		TArray<FString> CsvCategories;
		/** The tool that asked, for Activity. */
		FString ToolName;
	};

	struct FRunOutcome
	{
		/** completed, stopped, capture_failed, no_pie_world. */
		FString Status;
		FString Diagnostic;
		FString Level;
		/** attach or start. */
		FString Mode;
		FString CsvPath;
		FString TracePath;
		bool bCsvParsed = false;
		FCsvCapture Csv;
		FCaptureContext Context;
		bool bTraced = false;
		Trace::FHotFunctions Hot;
		TArray<FHyperAIPerfFxCost> Fx;
		bool bFxGathered = false;
		/** hyper_scene_cost_report's answer (JSON), when asked for and it answered. */
		FString SceneCostJson;
		FString Bot;
		double CapturedSeconds = 0.0;
		bool bLiveRead = false;
		FHyperAIPerfNaniteLive Nanite;
		FHyperAIPerfNiagaraLive Niagara;
		TArray<FString> Renderer;
		TArray<FString> Notes;
	};

	using FOnDone = TFunction<void(FRunOutcome& Outcome)>;

	/** Whether a run like Options could start now. Status is busy, no_pie_world, play_in_editor_running or csv_unavailable. */
	bool CanStart(const FRunOptions& Options, FString& OutStatus, FString& OutWhy);
	/** Starts the run; OnDone runs on the game thread once everything is recorded and read. */
	bool Start(FRunOptions Options, FOnDone OnDone, FString& OutStatus, FString& OutWhy);
	bool IsRunning();
	/** Stops a running run now; its OnDone still runs, with status stopped. */
	void Cancel(const FString& Reason);
	void Shutdown();
}
