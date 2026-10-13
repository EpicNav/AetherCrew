// Games by Hyper 2026.

#include "HyperAIStudioProfileSession.h"

#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioProfileLive.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "RHIGlobals.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tasks/Task.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioProfileSession, Log, All);

namespace HyperAIStudio::Profiling::Session
{
	namespace
	{
		/** Play In Editor has this long to begin play after the session asks for it. */
		constexpr double StartTimeoutSeconds = 90.0;
		/** The CSV file, the trace and the cost report have this long to finish after the capture ends. */
		constexpr double EndTimeoutSeconds = 30.0;
		/** Past this the trace is left for Unreal Insights: a session of a busy editor can be large. */
		constexpr double TraceAnalysisTimeoutSeconds = 120.0;

		enum class EPhase : uint8 { WaitingForWorld, Warmup, Capturing, Ending, Analysing, Done };

		/** What the Activity job shows, shared with its poll. */
		struct FJobState
		{
			FString Progress;
			FString Diagnostic;
			bool bDone = false;
			bool bFailed = false;
		};

		UWorld* PlayWorld()
		{
			return GEditor ? GEditor->PlayWorld.Get() : nullptr;
		}

		FString LevelOf(const UWorld* World)
		{
			if (!World) return FString();
			FString Name = World->GetOutermost()->GetName();
			// A play world's package carries the PIE prefix; the level is the same.
			Name.ReplaceInline(*FString::Printf(TEXT("UEDPIE_%d_"), World->GetOutermost()->GetPIEInstanceID()), TEXT(""));
			return Name;
		}

		/** The frame-rate limits the capture runs under, as far as the profile can read them. */
		FCaptureContext ReadContext()
		{
			FCaptureContext Context;
			Context.bNullRhi = GUsingNullRHI;
			const IConsoleVariable* MaxFps = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
			const IConsoleVariable* VSync = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"));
			const IConsoleVariable* VSyncEditor = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSyncEditor"));
			if (MaxFps && MaxFps->GetFloat() > 0.f)
			{
				Context.CapFps = MaxFps->GetFloat();
				Context.CapSource = FString::Printf(TEXT("t.MaxFPS %.0f"), Context.CapFps);
			}
			else if (GEngine && GEngine->bUseFixedFrameRate && GEngine->FixedFrameRate > 0.f)
			{
				Context.CapFps = GEngine->FixedFrameRate;
				Context.CapSource = FString::Printf(TEXT("the engine's fixed frame rate %.0f"), Context.CapFps);
			}
			else if (GEngine && GEngine->bSmoothFrameRate && GEngine->SmoothedFrameRateRange.HasUpperBound())
			{
				Context.CapFps = GEngine->SmoothedFrameRateRange.GetUpperBoundValue();
				Context.CapSource = FString::Printf(TEXT("the engine's smoothed frame rate range (at most %.0f)"), Context.CapFps);
			}
			if (Context.CapSource.IsEmpty() && ((VSync && VSync->GetInt() != 0) || (VSyncEditor && VSyncEditor->GetInt() != 0)))
			{
				Context.CapSource = VSyncEditor && VSyncEditor->GetInt() != 0 ? TEXT("r.VSyncEditor 1 (vsync)") : TEXT("r.VSync 1 (vsync)");
			}
			return Context;
		}

		class FRun
		{
		public:
			FRun(FRunOptions&& InOptions, FOnDone&& InOnDone)
				: Options(MoveTemp(InOptions))
				, OnDone(MoveTemp(InOnDone))
				, Outcome(MakeShared<FRunOutcome, ESPMode::ThreadSafe>())
				, Job(MakeShared<FJobState, ESPMode::ThreadSafe>())
			{
			}

			~FRun()
			{
				if (Ticker.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
				}
				ReleaseEditorHooks();
			}

			bool Begin(FString& OutStatus, FString& OutWhy)
			{
				Outcome->Mode = Options.bStartPie ? TEXT("start") : TEXT("attach");
				IFileManager::Get().MakeDirectory(*Options.Directory, /*Tree=*/true);
				if (Options.bStartPie)
				{
					ULevelEditorPlaySettings* Settings = DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
					if (!Settings)
					{
						OutStatus = TEXT("capture_failed");
						OutWhy = TEXT("Could not make play settings for the session.");
						return false;
					}
					// One player in this editor process, whatever the user's own play settings say.
					Settings->SetPlayNetMode(PIE_Standalone);
					Settings->SetRunUnderOneProcess(true);
					Settings->SetPlayNumberOfClients(1);
					PlaySettings.Reset(Settings);
					FRequestPlaySessionParams Params;
					Params.SessionDestination = EPlaySessionDestinationType::InProcess;
					Params.WorldType = EPlaySessionWorldType::PlayInEditor;
					Params.EditorPlaySettings = Settings;
					Params.bAllowOnlineSubsystem = false;
					GEditor->RequestPlaySession(Params);
					bOwnsPie = true;
				}
				// A capture of an editor throttled in the background measures the throttle, not the game.
				if (GEditor)
				{
					UEditorEngine::FShouldDisableCPUThrottling Throttle = UEditorEngine::FShouldDisableCPUThrottling::CreateLambda([]() { return true; });
					ThrottleHandle = Throttle.GetHandle();
					GEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Throttle));
				}
				PhaseStart = FPlatformTime::Seconds();
				Phase = EPhase::WaitingForWorld;
				Job->Progress = Options.bStartPie ? TEXT("starting Play In Editor") : TEXT("attaching to Play In Editor");
				Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FRun::Tick), 0.f);

				FHyperAIStudioAsyncJobRequest Request;
				Request.PackId = FHyperAIStudioProfilingContracts::PackId;
				Request.ToolName = Options.ToolName;
				Request.Target = TEXT("Play In Editor");
				Request.OperationId = Options.Id;
				Request.DeadlineMs = FMath::Min(FHyperAIStudioAsyncJobHost::MaxDeadlineMs,
					static_cast<int32>((Options.WarmupSeconds + Options.DurationSeconds + StartTimeoutSeconds + 180.0) * 1000.0));
				Request.PollIntervalMs = 250;
				Request.OnCancel = []() { Cancel(TEXT("Cancelled from the Activity panel.")); };
				const TSharedRef<FJobState, ESPMode::ThreadSafe> State = Job;
				FString JobId;
				FString JobError;
				if (!FHyperAIStudioAsyncJobHost::Start(Request, [State](FString& OutProgress, FString& OutDiagnostic)
				{
					OutProgress = State->Progress;
					OutDiagnostic = State->Diagnostic;
					return !State->bDone ? EHyperAIStudioAsyncJobPoll::Running
						: State->bFailed ? EHyperAIStudioAsyncJobPoll::Failed : EHyperAIStudioAsyncJobPoll::Completed;
				}, JobId, JobError))
				{
					Outcome->Notes.Add(TEXT("The session is not listed in Activity: ") + JobError);
				}
				return true;
			}

			/** Stops whatever runs and reports Status, unless the run is past measuring. */
			void Stop(const FString& Reason)
			{
				if (Phase == EPhase::Done || Phase == EPhase::Analysing || Phase == EPhase::Ending) return;
				Outcome->Notes.Add(Reason);
				if (Phase == EPhase::Capturing)
				{
					EndCapture(TEXT("stopped"), Reason);
				}
				else
				{
					Fail(TEXT("stopped"), Reason);
				}
			}

			void ForceFinishForShutdown()
			{
				if (Phase == EPhase::Capturing)
				{
#if CSV_PROFILER
					FCsvProfiler::Get()->EndCapture();
#endif
					Trace::Stop();
					RestoreCategories();
				}
				Fx.Reset();
				Bot.Reset();
				ReleaseEditorHooks();
				Phase = EPhase::Done;
			}

			bool IsDone() const { return Phase == EPhase::Done; }

			const FRunOptions& GetOptions() const { return Options; }

		private:
			bool Tick(float)
			{
				switch (Phase)
				{
				case EPhase::WaitingForWorld: TickWaiting(); break;
				case EPhase::Warmup: TickWarmup(); break;
				case EPhase::Capturing: TickCapturing(); break;
				case EPhase::Ending: TickEnding(); break;
				case EPhase::Analysing: TickAnalysing(); break;
				default: break;
				}
				return Phase != EPhase::Done;
			}

			void TickWaiting()
			{
				UWorld* World = PlayWorld();
				if (World && World->HasBegunPlay())
				{
					Outcome->Level = LevelOf(World);
					Phase = EPhase::Warmup;
					PhaseStart = FPlatformTime::Seconds();
					Job->Progress = FString::Printf(TEXT("warming up %.0f s"), Options.WarmupSeconds);
					return;
				}
				if (FPlatformTime::Seconds() - PhaseStart > StartTimeoutSeconds)
				{
					Fail(TEXT("no_pie_world"), FString::Printf(TEXT("Play In Editor did not begin play within %.0f s."), StartTimeoutSeconds));
				}
			}

			void TickWarmup()
			{
				if (!PlayWorld())
				{
					Fail(TEXT("stopped"), TEXT("Play In Editor ended before the capture began."));
					return;
				}
				if (FPlatformTime::Seconds() - PhaseStart >= Options.WarmupSeconds)
				{
					BeginCapture();
				}
			}

			void BeginCapture()
			{
#if CSV_PROFILER
				FCsvProfiler* Csv = FCsvProfiler::Get();
				if (FCsvProfiler::IsCapturing())
				{
					Fail(TEXT("busy"), TEXT("Another CSV capture started while the session warmed up; only one runs at a time."));
					return;
				}
				for (const FString& Category : Options.CsvCategories)
				{
					const int32 Index = FCsvProfiler::GetCategoryIndex(Category);
					if (Index != INDEX_NONE && !Csv->IsCategoryEnabled(Index))
					{
						Csv->EnableCategoryByIndex(Index, true);
						CategoriesTurnedOn.Add(Index);
					}
				}
				// Per-pass GPU timings reach the CSV only with r.GPUCsvStatsEnabled on; it goes back off when the capture ends.
				if (IConsoleVariable* GpuStats = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUCsvStatsEnabled")))
				{
					if (GpuStats->GetInt() == 0)
					{
						GpuStats->Set(TEXT("1"), static_cast<EConsoleVariableFlags>(GpuStats->GetFlags() & ECVF_SetByMask));
						bGpuCsvStatsTurnedOn = GpuStats->GetInt() != 0;
					}
				}
				Csv->BeginCapture(-1, Options.Directory, TEXT("profile"));
#endif
				Outcome->Context = ReadContext();
				if (Options.bTrace)
				{
					const FString TracePath = Options.Directory / TEXT("profile.utrace");
					FString Why;
					if (Trace::Start(TracePath, Why))
					{
						bTraceStarted = true;
						Outcome->TracePath = TracePath;
					}
					else
					{
						Outcome->Hot.Note = TEXT("No hot functions: ") + Why + TEXT(".");
					}
				}
				if (Options.bFx)
				{
					Fx = MakeUnique<Live::FFxCostSampler>();
				}
				if (!Options.Bot.IsEmpty())
				{
					Bot = MakeUnique<FHyperAIBotExecutor>(*PlayWorld(), Options.Directory / TEXT("bot"));
					BeginBotStep();
				}
				Phase = EPhase::Capturing;
				PhaseStart = FPlatformTime::Seconds();
				Job->Progress = FString::Printf(TEXT("measuring %.0f s"), Options.DurationSeconds);
			}

			void BeginBotStep()
			{
				while (Bot && NextBotStep < Options.Bot.Num())
				{
					const FHyperAIBotResult Result = Bot->Begin(Options.Bot[NextBotStep]);
					if (!Result.IsDone()) return;
					if (!RecordBotResult(Result)) return;
				}
			}

			/** False when the route ended (a step failed). */
			bool RecordBotResult(const FHyperAIBotResult& Result)
			{
				const FString Action = FHyperAIBotExecutor::ActionName(Options.Bot[NextBotStep].Action);
				++NextBotStep;
				if (Result.State == FHyperAIBotResult::EState::Failed)
				{
					BotSummary = FString::Printf(TEXT("Step %d (%s) failed: %s %s; the route stopped there and the capture went on."),
						NextBotStep, *Action, *Result.Code, *Result.Detail.Left(300));
					Bot->Stop();
					bBotEnded = true;
					return false;
				}
				if (NextBotStep >= Options.Bot.Num())
				{
					BotSummary = FString::Printf(TEXT("All %d step(s) passed, %.1f s into the capture."), Options.Bot.Num(), FPlatformTime::Seconds() - PhaseStart);
					bBotEnded = true;
				}
				return true;
			}

			void TickCapturing()
			{
				const double Elapsed = FPlatformTime::Seconds() - PhaseStart;
				if (!PlayWorld())
				{
					const FString Why = FString::Printf(TEXT("Play In Editor ended %.1f s into the capture."), Elapsed);
					Outcome->Notes.Add(Why);
					EndCapture(Elapsed >= 1.0 ? TEXT("completed") : TEXT("stopped"), Why);
					return;
				}
				if (GEngine && GEngine->ShouldThrottleCPUUsage())
				{
					++ThrottledTicks;
				}
				++CapturedTicks;
				if (Bot && !bBotEnded)
				{
					const FHyperAIBotResult Result = Bot->Tick();
					if (Result.IsDone() && RecordBotResult(Result))
					{
						BeginBotStep();
					}
				}
				if (Elapsed >= Options.DurationSeconds)
				{
					EndCapture(TEXT("completed"), FString());
				}
			}

			void EndCapture(const FString& Status, const FString& Why)
			{
				Outcome->Status = Status;
				Outcome->Diagnostic = Why;
				Outcome->CapturedSeconds = FPlatformTime::Seconds() - PhaseStart;
				if (CapturedTicks > 0 && ThrottledTicks * 2 > CapturedTicks)
				{
					Outcome->Context.CapFps = 3.0;
					Outcome->Context.CapSource = TEXT("the editor's background throttling");
				}
				if (Bot)
				{
					if (!bBotEnded)
					{
						BotSummary = FString::Printf(TEXT("%d of %d step(s) done when the capture ended; the rest were stopped."), NextBotStep, Options.Bot.Num());
					}
					Bot->Stop();
					Bot.Reset();
					Outcome->Bot = BotSummary;
				}
				if (Fx)
				{
					Outcome->bFxGathered = Fx->IsGathering();
					Outcome->Fx = Fx->Read(Options.LiveTopN);
					if (!Outcome->bFxGathered)
					{
						Outcome->Notes.Add(TEXT("Niagara costs were not measured: particle perf stats are off (fx.ParticlePerfStats.Enabled 0)."));
					}
					Fx.Reset();
				}
				UWorld* World = PlayWorld();
				if (Options.bLive && World)
				{
					Live::ReadNanite(*World, Options.LiveTopN, Outcome->Nanite);
					Live::ReadNiagara(*World, Options.LiveTopN, Outcome->Niagara);
					Outcome->Renderer = Live::ReadRenderer();
					Outcome->bLiveRead = true;
				}
				if (Options.bSceneCost && World && UToolsetRegistry::IsAvailable())
				{
					if (UToolsetRegistry::IsToolsetRegistered(TEXT("HyperAIStudioSceneSpatial.HyperAIStudioSceneSpatialToolset")))
					{
						SceneCost.Reset(UToolsetRegistry::ExecuteTool(TEXT("HyperAIStudioSceneSpatial.HyperAIStudioSceneSpatialToolset"),
							TEXT("hyper_scene_cost_report"), TEXT("{\"request\":{\"world\":\"pie\",\"topN\":5}}")));
					}
					else
					{
						Outcome->Notes.Add(TEXT("The level's cost report was not read: hyper_scene_cost_report (level_design pack) is not loaded."));
					}
				}
#if CSV_PROFILER
				CsvFile = FCsvProfiler::Get()->EndCapture();
#endif
				if (bTraceStarted)
				{
					Trace::Stop();
				}
				ReleaseEditorHooks();
				if (bOwnsPie && GEditor)
				{
					if (GEditor->IsPlaySessionRequestQueued()) GEditor->CancelRequestPlaySession();
					if (GEditor->PlayWorld) GEditor->RequestEndPlayMap();
				}
				Phase = EPhase::Ending;
				PhaseStart = FPlatformTime::Seconds();
				Job->Progress = TEXT("writing the capture");
			}

			void TickEnding()
			{
				const bool bTimedOut = FPlatformTime::Seconds() - PhaseStart > EndTimeoutSeconds;
				const bool bCsvReady = !CsvFile.IsValid() || CsvFile.IsReady();
				const bool bCostReady = !SceneCost.IsValid() || SceneCost->bIsComplete;
				bool bTraceReady = true;
				if (bTraceStarted)
				{
					// The trace's writer closes the file a moment after Stop; it is ready once it stops growing.
					const int64 Size = IFileManager::Get().FileSize(*Outcome->TracePath);
					const double Now = FPlatformTime::Seconds();
					if (Size != LastTraceSize)
					{
						LastTraceSize = Size;
						TraceSizeSince = Now;
					}
					bTraceReady = !Trace::IsWriting() && Size > 0 && Now - TraceSizeSince >= 0.5;
				}
				if (!bTimedOut && !(bCsvReady && bCostReady && bTraceReady)) return;

				RestoreCategories();
				if (SceneCost.IsValid())
				{
					if (SceneCost->bIsComplete && SceneCost->Error.IsEmpty())
					{
						Outcome->SceneCostJson = SceneCost->Value;
					}
					else
					{
						Outcome->Notes.Add(TEXT("The level's cost report did not answer: ") + (SceneCost->Error.IsEmpty() ? FString(TEXT("timed out")) : SceneCost->Error));
					}
					SceneCost.Reset();
				}
				if (CsvFile.IsValid() && CsvFile.IsReady())
				{
					Outcome->CsvPath = CsvFile.Get();
				}
				else if (CsvFile.IsValid())
				{
					Outcome->Notes.Add(TEXT("The CSV profiler had not finished writing its file after 30 s."));
				}
				if (bTraceStarted && !bTraceReady)
				{
					Outcome->Hot.Note = TEXT("No hot functions: the trace file was still being written after 30 s.");
					bTraceStarted = false;
				}
				Outcome->bTraced = bTraceStarted;

				// Reading the CSV takes a moment and touches nothing in the engine, so it runs off the game thread.
				const TSharedRef<FRunOutcome, ESPMode::ThreadSafe> Shared = Outcome;
				CsvTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Shared]()
				{
					if (Shared->CsvPath.IsEmpty()) return;
					FString Text;
					FString Error;
					if (Shared->CsvPath.EndsWith(TEXT(".gz")))
					{
						Shared->Notes.Add(TEXT("The CSV profiler wrote a compressed file (csv.CompressionMode 1), which this tool does not read; set csv.CompressionMode 0."));
					}
					else if (!FFileHelper::LoadFileToString(Text, *Shared->CsvPath))
					{
						Shared->Notes.Add(TEXT("The CSV file could not be read: ") + Shared->CsvPath);
					}
					else if (ParseCsv(Text, Shared->Csv, Error))
					{
						Shared->bCsvParsed = true;
					}
					else
					{
						Shared->Notes.Add(TEXT("The CSV file could not be parsed: ") + Error);
					}
				});
				// The trace's analysis starts here, on the game thread, and runs on TraceServices' own thread.
				if (Outcome->bTraced)
				{
					TraceSession = Trace::BeginAnalysis(Outcome->TracePath);
					if (!TraceSession.IsValid())
					{
						Outcome->Hot.Note = TEXT("No hot functions: TraceServices could not open the trace.");
						Outcome->bTraced = false;
					}
				}
				Phase = EPhase::Analysing;
				PhaseStart = FPlatformTime::Seconds();
				Job->Progress = Outcome->bTraced ? TEXT("analysing the capture and the trace") : TEXT("analysing the capture");
			}

			void TickAnalysing()
			{
				if (TraceSession.IsValid() && !bTraceSummarised)
				{
					if (TraceSession->IsAnalysisComplete())
					{
						// Summing the timers reads only the finished session, so it runs off the game thread too.
						const TSharedRef<FRunOutcome, ESPMode::ThreadSafe> Shared = Outcome;
						const TSharedPtr<const TraceServices::IAnalysisSession> Session = TraceSession;
						const int32 TopN = Options.TopN;
						TraceTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Shared, Session, TopN]()
						{
							Shared->Hot = Trace::Summarise(*Session, TopN);
						});
						bTraceSummarised = true;
					}
					else if (FPlatformTime::Seconds() - PhaseStart > TraceAnalysisTimeoutSeconds)
					{
						TraceSession->Stop(/*bAndWait=*/true);
						TraceSession.Reset();
						Outcome->Hot.Note = FString::Printf(TEXT("No hot functions: analysing the trace took over %.0f s; open %s in Unreal Insights instead."),
							TraceAnalysisTimeoutSeconds, *Outcome->TracePath);
					}
					return;
				}
				if (!CsvTask.IsCompleted() || !TraceTask.IsCompleted()) return;
				TraceSession.Reset();
				if (Outcome->Status.IsEmpty()) Outcome->Status = TEXT("completed");
				if (Outcome->Status == TEXT("completed") && !Outcome->bCsvParsed)
				{
					Outcome->Status = TEXT("capture_failed");
					Outcome->Diagnostic = TEXT("The CSV profiler wrote nothing this tool could read.");
				}
				Finish();
			}

			void Fail(const FString& Status, const FString& Why)
			{
				Outcome->Status = Status;
				Outcome->Diagnostic = Why;
				Fx.Reset();
				Bot.Reset();
				ReleaseEditorHooks();
				if (bOwnsPie && GEditor)
				{
					if (GEditor->IsPlaySessionRequestQueued()) GEditor->CancelRequestPlaySession();
					if (GEditor->PlayWorld) GEditor->RequestEndPlayMap();
				}
				Finish();
			}

			void Finish()
			{
				Phase = EPhase::Done;
				Job->bFailed = Outcome->Status != TEXT("completed");
				Job->Progress = Outcome->Status;
				if (OnDone)
				{
					FOnDone Done = MoveTemp(OnDone);
					Done(*Outcome);
				}
				// Done may have set the diagnostic the Activity job shows.
				Job->Diagnostic = Outcome->Diagnostic;
				Job->bDone = true;
			}

			void RestoreCategories()
			{
#if CSV_PROFILER
				for (const int32 Index : CategoriesTurnedOn)
				{
					FCsvProfiler::Get()->EnableCategoryByIndex(Index, false);
				}
#endif
				CategoriesTurnedOn.Reset();
				if (bGpuCsvStatsTurnedOn)
				{
					if (IConsoleVariable* GpuStats = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUCsvStatsEnabled")))
					{
						GpuStats->Set(TEXT("0"), static_cast<EConsoleVariableFlags>(GpuStats->GetFlags() & ECVF_SetByMask));
					}
					bGpuCsvStatsTurnedOn = false;
				}
			}

			void ReleaseEditorHooks()
			{
				if (ThrottleHandle.IsValid() && GEditor)
				{
					const FDelegateHandle Handle = ThrottleHandle;
					GEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll(
						[Handle](const UEditorEngine::FShouldDisableCPUThrottling& Delegate) { return Delegate.GetHandle() == Handle; });
				}
				ThrottleHandle.Reset();
				PlaySettings.Reset();
			}

			FRunOptions Options;
			FOnDone OnDone;
			TSharedRef<FRunOutcome, ESPMode::ThreadSafe> Outcome;
			TSharedRef<FJobState, ESPMode::ThreadSafe> Job;
			EPhase Phase = EPhase::WaitingForWorld;
			double PhaseStart = 0.0;
			bool bOwnsPie = false;
			bool bTraceStarted = false;
			int32 CapturedTicks = 0;
			int32 ThrottledTicks = 0;
			int64 LastTraceSize = -1;
			double TraceSizeSince = 0.0;
			TArray<int32> CategoriesTurnedOn;
			bool bGpuCsvStatsTurnedOn = false;
			TSharedFuture<FString> CsvFile;
			TStrongObjectPtr<ULevelEditorPlaySettings> PlaySettings;
			TStrongObjectPtr<UToolCallAsyncResultString> SceneCost;
			TUniquePtr<Live::FFxCostSampler> Fx;
			TUniquePtr<FHyperAIBotExecutor> Bot;
			int32 NextBotStep = 0;
			bool bBotEnded = false;
			FString BotSummary;
			FDelegateHandle ThrottleHandle;
			FTSTicker::FDelegateHandle Ticker;
			UE::Tasks::FTask CsvTask;
			UE::Tasks::FTask TraceTask;
			TSharedPtr<const TraceServices::IAnalysisSession> TraceSession;
			bool bTraceSummarised = false;
		};

		TUniquePtr<FRun>& Current()
		{
			static TUniquePtr<FRun> Run;
			return Run;
		}

		/** Drops a finished run on a later tick: it cannot delete itself from inside its own ticker. */
		void ReapLater()
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				if (Current() && Current()->IsDone()) Current().Reset();
				return false;
			}), 0.f);
		}
	}

	bool CanStart(const FRunOptions& Options, FString& OutStatus, FString& OutWhy)
	{
#if !CSV_PROFILER
		OutStatus = TEXT("csv_unavailable");
		OutWhy = TEXT("This build has no CSV profiler (CSV_PROFILER is 0).");
		return false;
#else
		if (!IsInGameThread() || !GEditor)
		{
			OutStatus = TEXT("invalid_request");
			OutWhy = TEXT("Profiling runs on the editor's game thread.");
			return false;
		}
		if (IsRunning())
		{
			OutStatus = TEXT("busy");
			OutWhy = FString::Printf(TEXT("Profile %s is still running; one runs at a time. Wait for it with hyper_operation_wait."), *Current()->GetOptions().Id);
			return false;
		}
		if (FCsvProfiler::IsCapturing())
		{
			OutStatus = TEXT("busy");
			OutWhy = TEXT("A CSV capture is already running (csvprofile start, or another tool); only one runs at a time.");
			return false;
		}
		if (Options.bStartPie && (GEditor->IsPlaySessionInProgress() || GEditor->IsPlaySessionRequestQueued() || GEditor->PlayWorld))
		{
			OutStatus = TEXT("play_in_editor_running");
			OutWhy = TEXT("Play In Editor is already running: attach to it (mode attach) instead of starting another.");
			return false;
		}
		if (!Options.bStartPie && !GEditor->PlayWorld)
		{
			OutStatus = TEXT("no_pie_world");
			OutWhy = TEXT("Play In Editor is not running: start it, or use mode start (or auto) to have the session start it.");
			return false;
		}
		return true;
#endif
	}

	bool Start(FRunOptions Options, FOnDone OnDone, FString& OutStatus, FString& OutWhy)
	{
		if (!CanStart(Options, OutStatus, OutWhy)) return false;
		TUniquePtr<FRun> Run = MakeUnique<FRun>(MoveTemp(Options), [OnDone = MoveTemp(OnDone)](FRunOutcome& Outcome)
		{
			if (OnDone) OnDone(Outcome);
			ReapLater();
		});
		if (!Run->Begin(OutStatus, OutWhy)) return false;
		Current() = MoveTemp(Run);
		return true;
	}

	bool IsRunning()
	{
		return Current().IsValid() && !Current()->IsDone();
	}

	void Cancel(const FString& Reason)
	{
		if (IsRunning()) Current()->Stop(Reason);
	}

	void Shutdown()
	{
		if (Current())
		{
			Current()->ForceFinishForShutdown();
			Current().Reset();
		}
	}
}
