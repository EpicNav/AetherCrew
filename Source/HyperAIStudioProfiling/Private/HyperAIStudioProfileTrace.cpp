// Games by Hyper 2026.

#include "HyperAIStudioProfileTrace.h"

#include "HAL/FileManager.h"
#include "HyperAIStudioProfilingToolset.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Trace/Detail/Channel.h"
#include "Trace/Trace.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Containers/Tables.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace HyperAIStudio::Profiling::Trace
{
	namespace
	{
		/** What Start changed, so Stop puts it back. */
		struct FRecording
		{
			bool bActive = false;
			FString Path;
			TArray<FString> ChannelsTurnedOn;
		};

		FRecording& Recording()
		{
			static FRecording Value;
			return Value;
		}

		bool IsChannelOn(const TCHAR* Name)
		{
			const UE::Trace::FChannel* Channel = UE::Trace::FindChannel(Name);
			return Channel && Channel->IsEnabled();
		}

		/** Threads whose timers are listed, by the names the engine gives them. */
		bool IsCriticalThread(const FString& Name)
		{
			return Name.StartsWith(TEXT("GameThread")) || Name.StartsWith(TEXT("RenderThread")) || Name.StartsWith(TEXT("RHIThread"));
		}

		/** A timer that is a thread waiting rather than working. */
		bool IsWaitTimer(const FString& Name)
		{
			return Name.Contains(TEXT("Wait")) || Name.Contains(TEXT("Idle")) || Name.Contains(TEXT("Sleep")) || Name.Contains(TEXT("Stall"));
		}

		/** Aggregates one thread over [Start, End] into Out, by exclusive time. */
		void AggregateThread(const TraceServices::ITimingProfilerProvider& Timing, const uint32 ThreadId, const FString& ThreadName, const double Start,
			const double End, const int64 GameFrames, TArray<FHyperAIPerfTimer>& Out)
		{
			TraceServices::FCreateAggregationParams Params;
			Params.IntervalStart = Start;
			Params.IntervalEnd = End;
			Params.GpuQueueFilter = [](uint32) { return false; };
			Params.CpuThreadFilter = [ThreadId](const uint32 Id) { return Id == ThreadId; };
			const TUniquePtr<TraceServices::ITable<TraceServices::FTimingProfilerAggregatedStats>> Table(Timing.CreateAggregation(Params));
			if (!Table) return;
			const TUniquePtr<TraceServices::ITableReader<TraceServices::FTimingProfilerAggregatedStats>> Reader(Table->CreateReader());
			for (; Reader && Reader->IsValid(); Reader->NextRow())
			{
				const TraceServices::FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
				if (!Row || !Row->Timer || !Row->Timer->Name || Row->InstanceCount == 0) continue;
				const FString Name(Row->Timer->Name);
				if (IsWaitTimer(Name)) continue;
				FHyperAIPerfTimer& Timer = Out.AddDefaulted_GetRef();
				Timer.Name = Name.Left(200);
				Timer.Thread = ThreadName;
				Timer.ExclusiveMs = Row->TotalExclusiveTime * 1000.0;
				Timer.InclusiveMs = Row->TotalInclusiveTime * 1000.0;
				Timer.ExclusivePerFrameMs = GameFrames > 0 ? Timer.ExclusiveMs / GameFrames : 0.0;
				Timer.Calls = static_cast<int64>(Row->InstanceCount);
			}
		}

		void KeepTop(TArray<FHyperAIPerfTimer>& Timers, const int32 TopN)
		{
			Timers.Sort([](const FHyperAIPerfTimer& A, const FHyperAIPerfTimer& B) { return A.ExclusiveMs > B.ExclusiveMs; });
			if (Timers.Num() > TopN) Timers.SetNum(TopN);
		}
	}

	bool Start(const FString& Path, FString& OutWhy)
	{
		FRecording& State = Recording();
		if (State.bActive)
		{
			OutWhy = TEXT("a trace this tool started is still being written");
			return false;
		}
		if (FTraceAuxiliary::IsConnected())
		{
			OutWhy = FString::Printf(TEXT("a trace is already being recorded (%s), and only one can run; it was left alone"),
				*FTraceAuxiliary::GetTraceDestinationString());
			return false;
		}
		TArray<FString> TurnedOn;
		static const TCHAR* const Channels[] = {TEXT("Cpu"), TEXT("Frame")};
		for (const TCHAR* Channel : Channels)
		{
			if (!IsChannelOn(Channel)) TurnedOn.Add(Channel);
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		FTraceAuxiliary::FOptions Options;
		Options.bTruncateFile = true;
		// What was traced before the capture is not part of it.
		Options.bExcludeTail = true;
		if (!FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *Path, TEXT("cpu,frame"), &Options))
		{
			OutWhy = TEXT("the trace file could not be opened for writing");
			for (const FString& Channel : TurnedOn) UE::Trace::ToggleChannel(*Channel, false);
			return false;
		}
		State.bActive = true;
		State.Path = Path;
		State.ChannelsTurnedOn = MoveTemp(TurnedOn);
		return true;
	}

	void Stop()
	{
		FRecording& State = Recording();
		if (!State.bActive) return;
		FTraceAuxiliary::Stop();
		for (const FString& Channel : State.ChannelsTurnedOn)
		{
			UE::Trace::ToggleChannel(*Channel, false);
		}
		State = FRecording();
	}

	bool IsWriting()
	{
		return Recording().bActive || FTraceAuxiliary::IsConnected();
	}

	TSharedPtr<const TraceServices::IAnalysisSession> BeginAnalysis(const FString& Path)
	{
		check(IsInGameThread());
		ITraceServicesModule& Module = FModuleManager::LoadModuleChecked<ITraceServicesModule>(TEXT("TraceServices"));
		const TSharedPtr<TraceServices::IAnalysisService> Service = Module.GetAnalysisService();
		return Service.IsValid() ? Service->StartAnalysis(*Path) : nullptr;
	}

	FHotFunctions Summarise(const TraceServices::IAnalysisSession& Session, const int32 TopN)
	{
		FHotFunctions Result;
		TraceServices::FAnalysisSessionReadScope ReadScope(Session);
		const TraceServices::ITimingProfilerProvider* Timing = TraceServices::ReadTimingProfilerProvider(Session);
		if (!Timing)
		{
			Result.Note = TEXT("The trace has no timing data (its cpu channel recorded nothing).");
			return Result;
		}
		// Session time runs from the traced process's clock origin, so the span measured is the game frames'.
		const TraceServices::IFrameProvider& Frames = TraceServices::ReadFrameProvider(Session);
		const uint64 FrameCount = Frames.GetFrameCount(TraceFrameType_Game);
		double First = TNumericLimits<double>::Max();
		double Last = 0.0;
		double WorstStart = 0.0;
		double WorstEnd = 0.0;
		Frames.EnumerateFrames(TraceFrameType_Game, uint64(0), FrameCount, [&](const TraceServices::FFrame& Frame)
		{
			// The frame still open when the trace stopped has no end.
			if (!FMath::IsFinite(Frame.StartTime) || !FMath::IsFinite(Frame.EndTime) || Frame.EndTime <= Frame.StartTime) return;
			++Result.GameFrames;
			First = FMath::Min(First, Frame.StartTime);
			Last = FMath::Max(Last, Frame.EndTime);
			if (Frame.EndTime - Frame.StartTime > WorstEnd - WorstStart)
			{
				WorstStart = Frame.StartTime;
				WorstEnd = Frame.EndTime;
			}
		});
		const double SpanStart = Result.GameFrames > 0 ? First : 0.0;
		const double SpanEnd = Result.GameFrames > 0 ? Last : Session.GetDurationSeconds();

		TArray<TPair<uint32, FString>> Threads;
		TraceServices::ReadThreadProvider(Session).EnumerateThreads([&Threads](const TraceServices::FThreadInfo& Info)
		{
			const FString Name = Info.Name ? FString(Info.Name) : FString();
			if (IsCriticalThread(Name)) Threads.Emplace(Info.Id, Name);
		});
		if (Threads.IsEmpty())
		{
			Result.Note = TEXT("The trace names no game, render or RHI thread.");
			return Result;
		}
		for (const TPair<uint32, FString>& Thread : Threads)
		{
			AggregateThread(*Timing, Thread.Key, Thread.Value, SpanStart, SpanEnd, Result.GameFrames, Result.Session);
		}
		KeepTop(Result.Session, TopN);

		// What the game and render threads did inside the slowest game frame the trace saw.
		if (WorstEnd > WorstStart)
		{
			Result.WorstFrameMs = (WorstEnd - WorstStart) * 1000.0;
			for (const TPair<uint32, FString>& Thread : Threads)
			{
				if (Thread.Value.StartsWith(TEXT("RHIThread"))) continue;
				AggregateThread(*Timing, Thread.Key, Thread.Value, WorstStart, WorstEnd, 1, Result.WorstFrame);
			}
			KeepTop(Result.WorstFrame, TopN);
		}
		Result.bOk = !Result.Session.IsEmpty();
		Result.Note = Result.bOk
			? FString::Printf(TEXT("From an Unreal Insights trace (cpu and frame channels) of %lld game frames over %.1f s, analysed in the editor with TraceServices' timing provider; timers that only wait are left out."),
				Result.GameFrames, SpanEnd - SpanStart)
			: TEXT("The trace recorded no CPU timers on the game, render or RHI thread.");
		return Result;
	}
}
