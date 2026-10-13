// Games by Hyper 2026.

#include "HyperAIStudioProfileAnalysis.h"

#include "Dom/JsonObject.h"
#include "HyperAIStudioProfilingToolset.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace HyperAIStudio::Profiling
{
	namespace
	{
		const FString FrameColumn(TEXT("FrameTime"));
		const FString GameColumn(TEXT("GameThreadTime"));
		const FString RenderColumn(TEXT("RenderThreadTime"));
		const FString RhiColumn(TEXT("RHIThreadTime"));
		const FString GpuColumn(TEXT("GPUTime"));
		const FString DrawCallsColumn(TEXT("RHI/DrawCalls"));
		const FString PrimitivesColumn(TEXT("RHI/PrimitivesDrawn"));
		const FString MemoryColumn(TEXT("PhysicalUsedMB"));

		/** The per-frame classes a frame falls in, in the order ties go. */
		enum class EFrameBound : uint8 { Game, Render, Rhi, Gpu, Wait, Count };

		const TCHAR* FrameBoundName(const EFrameBound Bound)
		{
			switch (Bound)
			{
			case EFrameBound::Game: return TEXT("game");
			case EFrameBound::Render: return TEXT("render");
			case EFrameBound::Rhi: return TEXT("rhi");
			case EFrameBound::Gpu: return TEXT("gpu");
			default: return TEXT("wait");
			}
		}

		const TCHAR* VerdictName(const EFrameBound Bound)
		{
			switch (Bound)
			{
			case EFrameBound::Game: return TEXT("game_thread");
			case EFrameBound::Render: return TEXT("render_thread");
			case EFrameBound::Rhi: return TEXT("rhi_thread");
			case EFrameBound::Gpu: return TEXT("gpu");
			default: return TEXT("capped");
			}
		}

		const TCHAR* ThreadWords(const EFrameBound Bound)
		{
			switch (Bound)
			{
			case EFrameBound::Game: return TEXT("the game thread");
			case EFrameBound::Render: return TEXT("the render thread");
			case EFrameBound::Rhi: return TEXT("the RHI thread");
			case EFrameBound::Gpu: return TEXT("the GPU");
			default: return TEXT("waiting");
			}
		}

		FString Ms(const double Value)
		{
			return FString::Printf(TEXT("%.1f"), Value);
		}

		/** Frames from Skip on of a column, or none. */
		TArray<double> Slice(const FCsvCapture& Csv, const FString& Name, const int32 Skip)
		{
			TArray<double> Out;
			if (const TArray<double>* Column = Csv.Column(Name))
			{
				for (int32 Index = Skip; Index < Column->Num(); ++Index)
				{
					Out.Add((*Column)[Index]);
				}
			}
			return Out;
		}

		double Average(const TArray<double>& Values)
		{
			double Sum = 0.0;
			for (const double Value : Values) Sum += Value;
			return Values.IsEmpty() ? 0.0 : Sum / Values.Num();
		}

		bool AnyPositive(const TArray<double>& Values)
		{
			return Values.ContainsByPredicate([](const double Value) { return Value > 0.0; });
		}

		/** Stats whose time is a thread waiting, not working: they explain a bound but are no work to cut. */
		bool IsWaitStat(const FString& Leaf)
		{
			return Leaf.Contains(TEXT("Wait")) || Leaf.Contains(TEXT("Idle")) || Leaf.Contains(TEXT("Sleep")) || Leaf.Contains(TEXT("Stall"))
				|| Leaf.Contains(TEXT("Flush"));
		}

		/** A column timed in milliseconds: the thread totals, exclusive CPU stats and GPU passes. */
		bool IsTimingColumn(const FString& Name)
		{
			return Name == GameColumn || Name == RenderColumn || Name == RhiColumn || Name == GpuColumn
				|| Name.StartsWith(TEXT("Exclusive/")) || Name.StartsWith(TEXT("GPU/"));
		}

		/** A short hint for what a stat or pass usually means, by its name. Empty when there is none worth saying. */
		FString HintFor(const FString& Name)
		{
			struct FHint { const TCHAR* Key; const TCHAR* Hint; };
			static const FHint Hints[] = {
				{TEXT("Lumen"), TEXT("Lumen: lower its scalability (sg.GlobalIlluminationQuality, r.Lumen.*) or use it where it pays")},
				{TEXT("Shadow"), TEXT("shadows: fewer movable shadow-casting lights, smaller shadow distances, or cached virtual shadow maps")},
				{TEXT("Translucen"), TEXT("translucency: less overlapping translucent surface on screen, or cheaper translucent materials")},
				{TEXT("BasePass"), TEXT("base pass: material complexity and overdraw; non-Nanite triangles")},
				{TEXT("Nanite"), TEXT("Nanite: material bins, masked or WPO materials, and overdraw")},
				{TEXT("PostProcess"), TEXT("post processing: screen percentage and expensive post effects")},
				{TEXT("TSR"), TEXT("TSR: screen percentage and TSR quality")},
				{TEXT("Niagara"), TEXT("Niagara: fewer or cheaper systems, effect type caps, GPU simulation")},
				{TEXT("Physic"), TEXT("physics: simpler collision, fewer simulating bodies, sleep thresholds")},
				{TEXT("Chaos"), TEXT("physics: simpler collision, fewer simulating bodies, sleep thresholds")},
				{TEXT("Anim"), TEXT("animation: update rate optimisation, animation budgeting, fewer evaluated bones")},
				{TEXT("GarbageCollect"), TEXT("garbage collection: fewer UObjects churned per second, incremental GC settings")},
				{TEXT("Tick"), TEXT("ticking: tick intervals, disabling ticks that do nothing, or events instead of Tick")},
				{TEXT("Navigation"), TEXT("navigation: navmesh rebuilds at runtime and path queries")},
				{TEXT("Blueprint"), TEXT("Blueprint VM: move hot graph loops to C++ or run them less often")},
				{TEXT("Slate"), TEXT("UI: widget invalidation and fewer ticking widgets")},
				{TEXT("UMG"), TEXT("UI: widget invalidation and fewer ticking widgets")},
				{TEXT("Streaming"), TEXT("streaming: what streams in during play, and its pool sizes")},
				{TEXT("Load"), TEXT("loading during play: preload or stream what is loaded synchronously")},
			};
			for (const FHint& Hint : Hints)
			{
				if (Name.Contains(Hint.Key)) return Hint.Hint;
			}
			return FString();
		}

		void AddDelta(TArray<FHyperAIPerfDelta>& Out, const FString& Metric, const double Before, const double After, const double ThresholdPercent,
			const bool bHigherIsBetter = false)
		{
			FHyperAIPerfDelta& Delta = Out.AddDefaulted_GetRef();
			Delta.Metric = Metric;
			Delta.Before = Before;
			Delta.After = After;
			Delta.Change = After - Before;
			Delta.ChangePercent = FMath::Abs(Before) > UE_DOUBLE_SMALL_NUMBER ? Delta.Change / FMath::Abs(Before) * 100.0 : (Delta.Change == 0.0 ? 0.0 : 100.0);
			if (FMath::Abs(Delta.ChangePercent) < ThresholdPercent)
			{
				Delta.Verdict = TEXT("same");
			}
			else
			{
				const bool bLower = Delta.Change < 0.0;
				Delta.Verdict = (bLower != bHigherIsBetter) ? TEXT("better") : TEXT("worse");
			}
		}
	}

	const TArray<double>* FCsvCapture::Column(const FString& Name) const
	{
		const int32 Index = Find(Name);
		return Index == INDEX_NONE ? nullptr : &Values[Index];
	}

	bool ParseCsv(const FString& Text, FCsvCapture& Out, FString& OutError)
	{
		Out = FCsvCapture();
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*CullEmpty=*/true);
		if (Lines.Num() < 2)
		{
			OutError = TEXT("The file has no frames: a CSV profiler file has a header row and a row per frame.");
			return false;
		}
		TArray<FString> FirstHeader;
		Lines[0].ParseIntoArray(FirstHeader, TEXT(","), /*CullEmpty=*/false);
		if (FirstHeader.IsEmpty() || FirstHeader[0].TrimStartAndEnd() != TEXT("EVENTS"))
		{
			OutError = TEXT("This is not a CSV profiler file: its first column is not EVENTS.");
			return false;
		}

		// The metadata row ([Key],Value pairs) ends the file; the row before it names every column.
		int32 MetadataLine = INDEX_NONE;
		for (int32 Index = Lines.Num() - 1; Index >= 1; --Index)
		{
			if (Lines[Index].StartsWith(TEXT("[")))
			{
				MetadataLine = Index;
				break;
			}
		}
		if (MetadataLine != INDEX_NONE)
		{
			TArray<FString> Cells;
			Lines[MetadataLine].ParseIntoArray(Cells, TEXT(","), /*CullEmpty=*/false);
			for (int32 Cell = 0; Cell + 1 < Cells.Num(); ++Cell)
			{
				const FString& Key = Cells[Cell];
				if (!Key.StartsWith(TEXT("[")) || !Key.EndsWith(TEXT("]"))) continue;
				const FString Name = Key.Mid(1, Key.Len() - 2);
				if (Name == TEXT("Commandline"))
				{
					// The command line is written last, commas and all.
					TArray<FString> Rest;
					for (int32 More = Cell + 1; More < Cells.Num(); ++More) Rest.Add(Cells[More]);
					Out.Metadata.Add(Name, FString::Join(Rest, TEXT(",")));
					break;
				}
				Out.Metadata.Add(Name, Cells[Cell + 1]);
				++Cell;
			}
		}
		const bool bHeaderAtEnd = MetadataLine != INDEX_NONE && MetadataLine >= 2
			&& Out.Metadata.FindRef(TEXT("HasHeaderRowAtEnd")) == TEXT("1") && Lines[MetadataLine - 1].StartsWith(TEXT("EVENTS"));
		const int32 HeaderLine = bHeaderAtEnd ? MetadataLine - 1 : 0;
		const int32 LastRow = bHeaderAtEnd ? MetadataLine - 2 : (MetadataLine != INDEX_NONE ? MetadataLine - 1 : Lines.Num() - 1);

		TArray<FString> Header;
		Lines[HeaderLine].ParseIntoArray(Header, TEXT(","), /*CullEmpty=*/false);
		// A stat written for several threads under one name is one column, summed, as the CSV tools read it.
		TArray<int32> ColumnOfCell;
		ColumnOfCell.Init(INDEX_NONE, Header.Num());
		for (int32 Cell = 1; Cell < Header.Num(); ++Cell)
		{
			const FString Name = Header[Cell].TrimStartAndEnd();
			if (Name.IsEmpty()) continue;
			int32 Column = Out.Columns.IndexOfByKey(Name);
			if (Column == INDEX_NONE)
			{
				Column = Out.Columns.Add(Name);
				Out.Values.AddDefaulted();
			}
			ColumnOfCell[Cell] = Column;
		}
		if (Out.Columns.IsEmpty())
		{
			OutError = TEXT("The header names no stats.");
			return false;
		}

		TArray<FString> Cells;
		for (int32 Line = 1; Line <= LastRow; ++Line)
		{
			// A header row can repeat mid-file when columns were added; it is not a frame.
			if (Lines[Line].StartsWith(TEXT("EVENTS,"))) continue;
			Cells.Reset();
			Lines[Line].ParseIntoArray(Cells, TEXT(","), /*CullEmpty=*/false);
			if (Cells.IsEmpty()) continue;
			TArray<FString>& Events = Out.Events.AddDefaulted_GetRef();
			if (!Cells[0].IsEmpty())
			{
				Cells[0].ParseIntoArray(Events, TEXT(";"), /*CullEmpty=*/true);
				// With csv.EventTimestamps each event ends in ##<seconds into the frame>.
				for (FString& Event : Events)
				{
					int32 Mark = INDEX_NONE;
					if (Event.FindLastChar(TEXT('#'), Mark) && Mark > 0 && Event[Mark - 1] == TEXT('#')) Event.LeftInline(Mark - 1);
				}
			}
			const int32 Frame = Out.Events.Num() - 1;
			for (TArray<double>& Column : Out.Values)
			{
				Column.Add(0.0);
			}
			for (int32 Cell = 1; Cell < Cells.Num() && Cell < ColumnOfCell.Num(); ++Cell)
			{
				const int32 Column = ColumnOfCell[Cell];
				if (Column != INDEX_NONE && !Cells[Cell].IsEmpty())
				{
					Out.Values[Column][Frame] += FCString::Atod(*Cells[Cell]);
				}
			}
		}
		if (Out.NumFrames() == 0)
		{
			OutError = TEXT("The file has a header but no frames.");
			return false;
		}
		return true;
	}

	double Percentile(TArray<double> Values, const double P)
	{
		if (Values.IsEmpty()) return 0.0;
		Values.Sort();
		// Nearest rank: the smallest value with at least P% of values at or below it.
		const int32 Rank = FMath::Clamp(FMath::CeilToInt(P / 100.0 * Values.Num()), 1, Values.Num());
		return Values[Rank - 1];
	}

	void Describe(const FString& Name, const TArray<double>& Values, FHyperAIPerfSeries& Out)
	{
		Out = FHyperAIPerfSeries();
		Out.Name = Name;
		if (Values.IsEmpty()) return;
		Out.Average = Average(Values);
		Out.P50 = Percentile(Values, 50.0);
		Out.P95 = Percentile(Values, 95.0);
		Out.P99 = Percentile(Values, 99.0);
		Out.Max = FMath::Max(Values);
		Out.Min = FMath::Min(Values);
	}

	void Analyse(const FCsvCapture& Csv, const FCaptureContext& Context, const FAnalysisOptions& Options, FHyperAIPerfReport& Out)
	{
		const int32 Skip = FMath::Clamp(Context.SkipFrames, 0, FMath::Max(0, Csv.NumFrames() - 2));
		const TArray<double> Frame = Slice(Csv, FrameColumn, Skip);
		if (Frame.IsEmpty())
		{
			Out.Notes.Add(TEXT("The capture has no FrameTime column, so nothing could be timed."));
			return;
		}
		const int32 N = Frame.Num();
		TArray<double> Game = Slice(Csv, GameColumn, Skip);
		TArray<double> Render = Slice(Csv, RenderColumn, Skip);
		TArray<double> Rhi = Slice(Csv, RhiColumn, Skip);
		const TArray<double> Gpu = Slice(Csv, GpuColumn, Skip);

		// A thread's own column can miss most of its work: RenderThreadTime is measured between window presents, so an editor
		// that presents several windows a frame, or none, under-reports it. The thread's exclusive stats, waits left out,
		// are a floor under it.
		auto Floor = [&](TArray<double>& Column, const TCHAR* Thread, const TCHAR* ColumnName)
		{
			const FString Prefix = FString::Printf(TEXT("Exclusive/%s/"), Thread);
			TArray<double> Busy;
			Busy.Init(0.0, N);
			bool bAny = false;
			for (int32 Index = 0; Index < Csv.Columns.Num(); ++Index)
			{
				const FString& Name = Csv.Columns[Index];
				if (!Name.StartsWith(Prefix) || IsWaitStat(Name.RightChop(Prefix.Len()))) continue;
				bAny = true;
				for (int32 Frame = 0; Frame < N; ++Frame) Busy[Frame] += Csv.Values[Index][Frame + Skip];
			}
			const double BusyAverage = Average(Busy);
			if (!bAny || BusyAverage < 0.5 || Average(Column) >= 0.5 * BusyAverage) return;
			Out.Notes.Add(FString::Printf(TEXT("%s averages %s ms, well under the %s ms the thread's own exclusive stats add up to (waits left out), as when an editor presents several windows a frame or none; the profile uses the larger."),
				ColumnName, *Ms(Average(Column)), *Ms(BusyAverage)));
			Column.SetNumZeroed(N);
			for (int32 Frame = 0; Frame < N; ++Frame) Column[Frame] = FMath::Max(Column[Frame], Busy[Frame]);
		};
		Floor(Game, TEXT("GameThread"), *GameColumn);
		Floor(Render, TEXT("RenderThread"), *RenderColumn);
		Floor(Rhi, TEXT("RHIThread"), *RhiColumn);
		const bool bRhi = AnyPositive(Rhi);
		Out.bGpuTimeAvailable = !Context.bNullRhi && AnyPositive(Gpu);
		if (!Out.bGpuTimeAvailable)
		{
			Out.GpuNote = Context.bNullRhi ? TEXT("GPU time unavailable (null RHI: this editor renders nothing).")
				: Csv.Find(GpuColumn) == INDEX_NONE ? TEXT("GPU time unavailable (the capture has no GPUTime column).")
				: TEXT("GPU time unavailable (the RHI reported none: no GPU timing on this RHI, or nothing rendered).");
		}

		Out.Frames = N;
		Out.CapturedSeconds = 0.0;
		for (const double Value : Frame) Out.CapturedSeconds += Value / 1000.0;
		Out.AverageFps = Out.CapturedSeconds > 0.0 ? N / Out.CapturedSeconds : 0.0;
		Describe(TEXT("frame"), Frame, Out.FrameTime);
		Out.Threads.Reset();
		Describe(TEXT("game"), Game, Out.Threads.AddDefaulted_GetRef());
		Describe(TEXT("render"), Render, Out.Threads.AddDefaulted_GetRef());
		if (bRhi) Describe(TEXT("rhi"), Rhi, Out.Threads.AddDefaulted_GetRef());
		if (Out.bGpuTimeAvailable) Describe(TEXT("gpu"), Gpu, Out.Threads.AddDefaulted_GetRef());

		auto At = [](const TArray<double>& Values, const int32 Index) { return Values.IsValidIndex(Index) ? Values[Index] : 0.0; };

		// Each frame is bound by whatever was busiest in it, or by waiting when nothing was busy for most of it.
		TArray<EFrameBound> Bounds;
		Bounds.SetNum(N);
		int32 Counts[static_cast<int32>(EFrameBound::Count)] = {};
		for (int32 Index = 0; Index < N; ++Index)
		{
			const double Busy[] = {At(Game, Index), At(Render, Index), bRhi ? At(Rhi, Index) : 0.0, Out.bGpuTimeAvailable ? At(Gpu, Index) : 0.0};
			int32 Top = 0;
			for (int32 Which = 1; Which < 4; ++Which)
			{
				if (Busy[Which] > Busy[Top]) Top = Which;
			}
			EFrameBound Bound = static_cast<EFrameBound>(Top);
			if (Frame[Index] <= 0.0 || Busy[Top] < Options.BusyShare * Frame[Index])
			{
				Bound = EFrameBound::Wait;
			}
			Bounds[Index] = Bound;
			++Counts[static_cast<int32>(Bound)];
		}
		int32 Winner = 0;
		for (int32 Which = 1; Which < static_cast<int32>(EFrameBound::Count); ++Which)
		{
			if (Counts[Which] > Counts[Winner]) Winner = Which;
		}
		const EFrameBound Majority = static_cast<EFrameBound>(Winner);

		const double AvgFrame = Out.FrameTime.Average;
		const double AvgGame = Average(Game);
		const double AvgRender = Average(Render);
		const double AvgRhi = bRhi ? Average(Rhi) : 0.0;
		const double AvgGpu = Out.bGpuTimeAvailable ? Average(Gpu) : 0.0;
		const double Averages[] = {AvgGame, AvgRender, AvgRhi, AvgGpu};

		FHyperAIPerfVerdict& Verdict = Out.Verdict;
		Verdict = FHyperAIPerfVerdict();
		Verdict.CapFps = Context.CapFps;
		Verdict.CapSource = Context.CapSource;
		Verdict.FramesPercent = 100.0 * Counts[Winner] / N;
		for (int32 Which = 0; Which < static_cast<int32>(EFrameBound::Count); ++Which)
		{
			if (Counts[Which] > 0)
			{
				Verdict.Breakdown.Add(FString::Printf(TEXT("%s %.0f%%"), VerdictName(static_cast<EFrameBound>(Which)), 100.0 * Counts[Which] / N));
			}
		}
		Verdict.Evidence.Add(FString::Printf(TEXT("Frames average %s ms (%.1f fps); game thread %s ms, render thread %s ms%s, %s."),
			*Ms(AvgFrame), Out.AverageFps, *Ms(AvgGame), *Ms(AvgRender), bRhi ? *FString::Printf(TEXT(", RHI thread %s ms"), *Ms(AvgRhi)) : TEXT(""),
			Out.bGpuTimeAvailable ? *FString::Printf(TEXT("GPU %s ms"), *Ms(AvgGpu)) : *Out.GpuNote.LeftChop(1)));

		// The busiest on average, for capped frames and near ties.
		int32 Busiest = 0;
		int32 Second = INDEX_NONE;
		for (int32 Which = 1; Which < 4; ++Which)
		{
			if (Averages[Which] > Averages[Busiest]) Busiest = Which;
		}
		for (int32 Which = 0; Which < 4; ++Which)
		{
			if (Which != Busiest && (Second == INDEX_NONE || Averages[Which] > Averages[Second])) Second = Which;
		}

		if (Majority != EFrameBound::Wait)
		{
			Verdict.Bound = VerdictName(Majority);
			const double Mine = Averages[static_cast<int32>(Majority)];
			Verdict.Summary = FString::Printf(TEXT("%s-bound: %s is the busiest on %.0f%% of frames, averaging %s ms of a %s ms frame."),
				Majority == EFrameBound::Gpu ? TEXT("GPU") : Majority == EFrameBound::Game ? TEXT("Game-thread")
				: Majority == EFrameBound::Render ? TEXT("Render-thread") : TEXT("RHI-thread"),
				ThreadWords(Majority), Verdict.FramesPercent, *Ms(Mine), *Ms(AvgFrame));
			Verdict.Evidence.Add(FString::Printf(TEXT("%s takes %.0f%% of the frame time on average."), ThreadWords(Majority),
				AvgFrame > 0.0 ? 100.0 * Mine / AvgFrame : 0.0));
			if (Counts[static_cast<int32>(EFrameBound::Wait)] > 0)
			{
				Verdict.Evidence.Add(FString::Printf(TEXT("On %.0f%% of frames nothing was busy for %.0f%% of the frame (waiting on a limit, vsync or something unmeasured)."),
					100.0 * Counts[static_cast<int32>(EFrameBound::Wait)] / N, Options.BusyShare * 100.0));
			}
		}
		else
		{
			const double CapMs = Context.CapFps > 0.0 ? 1000.0 / Context.CapFps : 0.0;
			const bool bMatchesCap = CapMs > 0.0 && FMath::Abs(AvgFrame - CapMs) <= 0.15 * CapMs;
			const bool bVsync = Context.CapSource.Contains(TEXT("VSync"));
			const double BusiestMs = Averages[Busiest];
			const FString BusiestWords = FString::Printf(TEXT("%s (%s ms, %.0f%% of the frame)"), ThreadWords(static_cast<EFrameBound>(Busiest)),
				*Ms(BusiestMs), AvgFrame > 0.0 ? 100.0 * BusiestMs / AvgFrame : 0.0);
			if (bMatchesCap || bVsync)
			{
				Verdict.Bound = TEXT("capped");
				Verdict.Summary = bMatchesCap
					? FString::Printf(TEXT("Capped at %.0f fps by %s: the busiest is %s, so each frame waits about %s ms."),
						Context.CapFps, *Context.CapSource, *BusiestWords, *Ms(FMath::Max(0.0, AvgFrame - BusiestMs)))
					: FString::Printf(TEXT("Capped by %s at about %.0f fps: the busiest is %s, so each frame waits about %s ms."),
						*Context.CapSource, Out.AverageFps, *BusiestWords, *Ms(FMath::Max(0.0, AvgFrame - BusiestMs)));
				Verdict.Evidence.Add(FString::Printf(TEXT("On %.0f%% of frames no thread%s was busy for %.0f%% of the frame."), Verdict.FramesPercent,
					Out.bGpuTimeAvailable ? TEXT(" nor the GPU") : TEXT(""), Options.BusyShare * 100.0));
				if (!Out.bGpuTimeAvailable)
				{
					Verdict.Evidence.Add(Out.GpuNote + TEXT(" The GPU's share of the frame is unknown."));
				}
			}
			else if (!Out.bGpuTimeAvailable)
			{
				Verdict.Bound = TEXT("unknown");
				Verdict.Summary = FString::Printf(TEXT("Unknown: GPU time unavailable, and on %.0f%% of frames no CPU thread was busy for %.0f%% of the frame; a GPU bound or an unread frame limit would both explain it. The busiest CPU thread is %s."),
					Verdict.FramesPercent, Options.BusyShare * 100.0, *BusiestWords);
				Verdict.Evidence.Add(Out.GpuNote);
			}
			else
			{
				Verdict.Bound = TEXT("capped");
				Verdict.Summary = FString::Printf(TEXT("Capped at about %.0f fps by a limit the profile could not name (vsync or a frame limit): neither the CPU threads nor the GPU were busy for %.0f%% of %.0f%% of frames; the busiest is %s."),
					Out.AverageFps, Options.BusyShare * 100.0, Verdict.FramesPercent, *BusiestWords);
			}
			if (CapMs > 0.0)
			{
				Verdict.Evidence.Add(FString::Printf(TEXT("The frame limit in effect is %.0f fps (%s ms a frame) from %s."), Context.CapFps, *Ms(CapMs), *Context.CapSource));
			}
		}
		if (Second != INDEX_NONE && Averages[Busiest] > 0.0 && Averages[Second] >= 0.9 * Averages[Busiest] && Averages[Second] > 0.0)
		{
			Verdict.Evidence.Add(FString::Printf(TEXT("%s (%s ms) is within 10%% of %s (%s ms): relieving one soon leaves the frame bound by the other."),
				ThreadWords(static_cast<EFrameBound>(Second)), *Ms(Averages[Second]), ThreadWords(static_cast<EFrameBound>(Busiest)), *Ms(Averages[Busiest])));
		}

		// Hitches: over the threshold, or over twice the median and clearly above it.
		const double Median = Out.FrameTime.P50;
		Out.HitchThresholdMs = Options.HitchThresholdMs;
		TArray<int32> HitchFrames;
		for (int32 Index = 0; Index < N; ++Index)
		{
			if (Frame[Index] > Options.HitchThresholdMs || (Frame[Index] > 2.0 * Median && Frame[Index] - Median >= Options.HitchMinOverMedianMs))
			{
				HitchFrames.Add(Index);
			}
		}
		Out.HitchCount = HitchFrames.Num();
		HitchFrames.Sort([&Frame](const int32 A, const int32 B) { return Frame[A] > Frame[B]; });
		if (HitchFrames.Num() > Options.MaxHitches) HitchFrames.SetNum(Options.MaxHitches);

		TArray<int32> TimingColumns;
		TMap<int32, double> Medians;
		if (!HitchFrames.IsEmpty())
		{
			for (int32 Column = 0; Column < Csv.Columns.Num(); ++Column)
			{
				if (IsTimingColumn(Csv.Columns[Column]))
				{
					TimingColumns.Add(Column);
					Medians.Add(Column, Percentile(Slice(Csv, Csv.Columns[Column], Skip), 50.0));
				}
			}
		}
		const TArray<double>& AllFrames = *Csv.Column(FrameColumn);
		for (const int32 Index : HitchFrames)
		{
			FHyperAIPerfHitch& Hitch = Out.Hitches.AddDefaulted_GetRef();
			const int32 CaptureFrame = Index + Skip;
			Hitch.Frame = CaptureFrame;
			for (int32 Before = 0; Before < CaptureFrame; ++Before) Hitch.AtSeconds += AllFrames[Before] / 1000.0;
			Hitch.FrameMs = Frame[Index];
			Hitch.Bound = FrameBoundName(Bounds[Index]);
			TArray<TPair<double, FString>> Spikes;
			for (const int32 Column : TimingColumns)
			{
				const double Value = Csv.Values[Column][CaptureFrame];
				const double ColumnMedian = Medians.FindRef(Column);
				if (Value - ColumnMedian >= 1.0)
				{
					Spikes.Emplace(Value - ColumnMedian, FString::Printf(TEXT("%s %s ms (median %s)"), *Csv.Columns[Column], *Ms(Value), *Ms(ColumnMedian)));
				}
			}
			Spikes.Sort([](const TPair<double, FString>& A, const TPair<double, FString>& B) { return A.Key > B.Key; });
			for (int32 Spike = 0; Spike < Spikes.Num() && Spike < 5; ++Spike)
			{
				Hitch.Spikes.Add(Spikes[Spike].Value);
			}
			if (Csv.Events.IsValidIndex(CaptureFrame))
			{
				for (const FString& Event : Csv.Events[CaptureFrame])
				{
					if (Hitch.Events.Num() < 8) Hitch.Events.Add(Event.Left(200));
				}
			}
		}

		// Top stats: exclusive CPU time per thread, GPU passes, and what threads spent waiting.
		auto ThreadAverage = [&](const FString& Thread)
		{
			return Thread == TEXT("GameThread") ? AvgGame : Thread == TEXT("RenderThread") ? AvgRender : Thread == TEXT("RHIThread") ? AvgRhi : 0.0;
		};
		TArray<FHyperAIPerfStatLine> Cpu;
		TArray<FHyperAIPerfStatLine> GpuPasses;
		TArray<FHyperAIPerfStatLine> Waits;
		for (int32 Column = 0; Column < Csv.Columns.Num(); ++Column)
		{
			const FString& Name = Csv.Columns[Column];
			const bool bExclusive = Name.StartsWith(TEXT("Exclusive/"));
			const bool bGpuPass = Name.StartsWith(TEXT("GPU/"));
			if (!bExclusive && !bGpuPass) continue;
			const TArray<double> Values = Slice(Csv, Name, Skip);
			FHyperAIPerfStatLine Line;
			Line.Name = Name;
			Line.AverageMs = Average(Values);
			Line.MaxMs = Values.IsEmpty() ? 0.0 : FMath::Max(Values);
			if (Line.AverageMs < 0.01) continue;
			if (bGpuPass)
			{
				Line.Thread = TEXT("GPU");
				Line.SharePercent = AvgGpu > 0.0 ? 100.0 * Line.AverageMs / AvgGpu : 0.0;
				GpuPasses.Add(MoveTemp(Line));
				continue;
			}
			TArray<FString> Parts;
			Name.ParseIntoArray(Parts, TEXT("/"));
			Line.Thread = Parts.Num() >= 3 ? Parts[1] : FString();
			const double ThreadAvg = ThreadAverage(Line.Thread);
			Line.SharePercent = ThreadAvg > 0.0 ? 100.0 * Line.AverageMs / ThreadAvg : 0.0;
			if (IsWaitStat(Parts.Last()))
			{
				Waits.Add(MoveTemp(Line));
			}
			else
			{
				Cpu.Add(MoveTemp(Line));
			}
		}
		auto TopOf = [&Options](TArray<FHyperAIPerfStatLine>& Lines)
		{
			Lines.Sort([](const FHyperAIPerfStatLine& A, const FHyperAIPerfStatLine& B) { return A.AverageMs > B.AverageMs; });
			if (Lines.Num() > Options.TopN) Lines.SetNum(Options.TopN);
		};
		TopOf(Cpu);
		TopOf(GpuPasses);
		TopOf(Waits);
		Out.TopCpuStats = MoveTemp(Cpu);
		Out.TopGpuStats = MoveTemp(GpuPasses);
		Out.Waits = MoveTemp(Waits);
		if (Out.bGpuTimeAvailable && Out.TopGpuStats.IsEmpty())
		{
			Out.Notes.Add(TEXT("The capture has no per-pass GPU timings: the CSV gets them only with r.GPUCsvStatsEnabled 1, which a session turns on while it measures."));
		}
		if (Out.TopCpuStats.IsEmpty())
		{
			Out.Notes.Add(TEXT("The capture has no Exclusive CPU stats (the CSV profiler's Exclusive category was off or nothing recorded)."));
		}

		const TArray<double> Draws = Slice(Csv, DrawCallsColumn, Skip);
		const TArray<double> Prims = Slice(Csv, PrimitivesColumn, Skip);
		if (!Draws.IsEmpty()) Describe(TEXT("draw_calls"), Draws, Out.DrawCalls);
		if (!Prims.IsEmpty()) Describe(TEXT("primitives"), Prims, Out.Primitives);
		const TArray<double> Memory = Slice(Csv, MemoryColumn, Skip);
		if (!Memory.IsEmpty())
		{
			Out.MemoryStartMB = Memory[0];
			Out.MemoryEndMB = Memory.Last();
			Out.MemoryPeakMB = FMath::Max(Memory);
		}

		// Renderer figures the CSV profiler records when its categories are on.
		auto Has = [&Csv](const TCHAR* Name) { return Csv.Find(Name) != INDEX_NONE; };
		if (Has(TEXT("NaniteStreaming/RootAllocationMB")))
		{
			Out.Renderer.Add(FString::Printf(TEXT("Nanite root pages: %s MB."), *Ms(Average(Slice(Csv, TEXT("NaniteStreaming/RootAllocationMB"), Skip)))));
		}
		if (Has(TEXT("NaniteStreamingDetail/VisibleStreamingDataSizeMB")))
		{
			const TArray<double> Visible = Slice(Csv, TEXT("NaniteStreamingDetail/VisibleStreamingDataSizeMB"), Skip);
			const TArray<double> Pool = Slice(Csv, TEXT("NaniteStreamingDetail/StreamingPoolSizeMB"), Skip);
			const double PoolMB = Pool.IsEmpty() ? 0.0 : FMath::Max(Pool);
			const double PeakVisible = Visible.IsEmpty() ? 0.0 : FMath::Max(Visible);
			Out.Renderer.Add(FString::Printf(TEXT("Nanite visible streaming data: %s MB on average, %s MB at peak, of a %s MB streaming pool%s."),
				*Ms(Average(Visible)), *Ms(PeakVisible), *Ms(PoolMB),
				PoolMB > 0.0 && PeakVisible > PoolMB ? TEXT(" (over the pool: pages are evicted and requested again)") : TEXT("")));
		}
		if (Has(TEXT("NaniteStreamingDetail/NewStreamingDataSizeMB")))
		{
			double Requested = 0.0;
			for (const double Value : Slice(Csv, TEXT("NaniteStreamingDetail/NewStreamingDataSizeMB"), Skip)) Requested += Value;
			Out.Renderer.Add(FString::Printf(TEXT("Nanite page data requested over the capture: %s MB."), *Ms(Requested)));
		}
		// Free pages come back from the GPU only while shadow stats are fed back; a column of zeros says nothing.
		if (Has(TEXT("VSM/FreePages")) && Slice(Csv, TEXT("VSM/FreePages"), Skip).ContainsByPredicate([](const double Value) { return Value != 0.0; }))
		{
			const TArray<double> Free = Slice(Csv, TEXT("VSM/FreePages"), Skip);
			const double Least = Free.IsEmpty() ? 0.0 : FMath::Min(Free);
			Out.Renderer.Add(FString::Printf(TEXT("Virtual shadow maps: at least %.0f free physical pages%s."), Least,
				Least < 0.0 ? TEXT(" (negative: the page pool overflowed; raise r.Shadow.Virtual.MaxPhysicalPages or lower shadow resolution)") : TEXT("")));
		}
		if (Has(TEXT("RenderTargetPoolSize")))
		{
			Out.Renderer.Add(FString::Printf(TEXT("Render target pool: %s MB, %s MB in use."), *Ms(Average(Slice(Csv, TEXT("RenderTargetPoolSize"), Skip))),
				*Ms(Average(Slice(Csv, TEXT("RenderTargetPoolUsed"), Skip)))));
		}
		if (Skip > 0)
		{
			Out.Notes.Add(FString::Printf(TEXT("The first %d frame(s) of the capture are left out: the frame a capture starts in carries its own setup."), Skip));
		}
	}

	bool ReadSceneCost(const FString& Json, FSceneCostSummary& Out)
	{
		Out = FSceneCostSummary();
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()) return false;
		const TSharedPtr<FJsonObject>* Wrapped = nullptr;
		const TSharedPtr<FJsonObject> Report = Root->TryGetObjectField(TEXT("returnValue"), Wrapped) && Wrapped ? *Wrapped : Root;
		bool bOk = false;
		if (!Report->TryGetBoolField(TEXT("bOk"), bOk) || !bOk) return false;
		Out.bValid = true;
		Report->TryGetNumberField(TEXT("estimatedDrawCalls"), Out.EstimatedDrawCalls);
		Report->TryGetNumberField(TEXT("estimatedShadowDrawCalls"), Out.EstimatedShadowDrawCalls);
		Report->TryGetNumberField(TEXT("naniteMaterialBins"), Out.NaniteMaterialBins);
		Report->TryGetNumberField(TEXT("translucentMaterials"), Out.TranslucentMaterials);
		Report->TryGetNumberField(TEXT("nanitePrimitives"), Out.NanitePrimitives);
		Report->TryGetNumberField(TEXT("nonNanitePrimitives"), Out.NonNanitePrimitives);
		Report->TryGetNumberField(TEXT("trianglesNonNanite"), Out.TrianglesNonNanite);
		const TSharedPtr<FJsonObject>* Lights = nullptr;
		if (Report->TryGetObjectField(TEXT("lights"), Lights) && Lights)
		{
			(*Lights)->TryGetNumberField(TEXT("shadowCastingDynamic"), Out.ShadowCastingDynamicLights);
		}
		const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
		if (Report->TryGetArrayField(TEXT("heaviestMeshes"), Meshes) && Meshes)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Meshes)
			{
				const TSharedPtr<FJsonObject>* Mesh = nullptr;
				if (Out.HeaviestMeshes.Num() >= 3 || !Value->TryGetObject(Mesh) || !Mesh) continue;
				FString Name;
				int64 Total = 0;
				int32 Instances = 0;
				bool bNanite = false;
				(*Mesh)->TryGetStringField(TEXT("name"), Name);
				(*Mesh)->TryGetNumberField(TEXT("totalTriangles"), Total);
				(*Mesh)->TryGetNumberField(TEXT("instances"), Instances);
				(*Mesh)->TryGetBoolField(TEXT("bNanite"), bNanite);
				Out.HeaviestMeshes.Add(FString::Printf(TEXT("%s (%d instances, %.2fM triangles%s)"), *FPaths::GetBaseFilename(Name), Instances,
					Total / 1000000.0, bNanite ? TEXT(", Nanite") : TEXT(", not Nanite")));
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
		if (Report->TryGetArrayField(TEXT("findings"), Findings) && Findings)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Findings)
			{
				if (Out.Findings.Num() < 5) Out.Findings.Add(Value->AsString().Left(300));
			}
		}
		return true;
	}

	TArray<FString> DescribeSceneCost(const FSceneCostSummary& Scene)
	{
		TArray<FString> Lines;
		if (!Scene.bValid) return Lines;
		Lines.Add(FString::Printf(TEXT("About %d draw calls and %d shadow draw calls estimated; %d Nanite and %d non-Nanite primitives; %.2fM non-Nanite triangles at LOD0."),
			Scene.EstimatedDrawCalls, Scene.EstimatedShadowDrawCalls, Scene.NanitePrimitives, Scene.NonNanitePrimitives, Scene.TrianglesNonNanite / 1000000.0));
		Lines.Add(FString::Printf(TEXT("%d Nanite material bins, %d translucent materials, %d dynamic shadow-casting lights."),
			Scene.NaniteMaterialBins, Scene.TranslucentMaterials, Scene.ShadowCastingDynamicLights));
		if (!Scene.HeaviestMeshes.IsEmpty())
		{
			Lines.Add(TEXT("Heaviest meshes: ") + FString::Join(Scene.HeaviestMeshes, TEXT("; ")) + TEXT("."));
		}
		for (const FString& Finding : Scene.Findings)
		{
			Lines.Add(Finding);
		}
		return Lines;
	}

	void Advise(FHyperAIPerfReport& Report, const FSceneCostSummary* Scene)
	{
		TArray<FString>& Advice = Report.Advice;
		Advice.Reset();
		const FHyperAIPerfVerdict& Verdict = Report.Verdict;
		const bool bScene = Scene && Scene->bValid;
		auto Names = [](const TArray<FHyperAIPerfStatLine>& Lines, const TCHAR* Thread, const int32 Most)
		{
			TArray<FString> Out;
			for (const FHyperAIPerfStatLine& Line : Lines)
			{
				if (Thread && Line.Thread != Thread) continue;
				FString Short = Line.Name;
				Short.RemoveFromStart(TEXT("Exclusive/"));
				Out.Add(FString::Printf(TEXT("%s %s ms"), *Short, *Ms(Line.AverageMs)));
				if (Out.Num() >= Most) break;
			}
			return Out;
		};
		auto Hints = [](const TArray<FHyperAIPerfStatLine>& Lines, const TCHAR* Thread)
		{
			TArray<FString> Out;
			for (const FHyperAIPerfStatLine& Line : Lines)
			{
				if (Thread && Line.Thread != Thread) continue;
				const FString Hint = HintFor(Line.Name);
				if (!Hint.IsEmpty()) Out.AddUnique(Hint);
				if (Out.Num() >= 3) break;
			}
			return Out;
		};
		auto Timers = [](const TArray<FHyperAIPerfTimer>& Lines, const TCHAR* Thread, const int32 Most)
		{
			TArray<FString> Out;
			for (const FHyperAIPerfTimer& Timer : Lines)
			{
				if (Thread && !Timer.Thread.StartsWith(Thread)) continue;
				Out.Add(FString::Printf(TEXT("%s %s ms/frame"), *Timer.Name, *FString::Printf(TEXT("%.2f"), Timer.ExclusivePerFrameMs)));
				if (Out.Num() >= Most) break;
			}
			return Out;
		};

		if (Verdict.Bound == TEXT("gpu"))
		{
			FString Line = Verdict.Summary;
			const TArray<FString> Passes = Names(Report.TopGpuStats, nullptr, 4);
			if (!Passes.IsEmpty()) Line += TEXT(" Heaviest GPU passes: ") + FString::Join(Passes, TEXT(", ")) + TEXT(".");
			if (bScene)
			{
				Line += FString::Printf(TEXT(" The level draws about %d draw calls, %d Nanite material bins and %d translucent materials"),
					Scene->EstimatedDrawCalls, Scene->NaniteMaterialBins, Scene->TranslucentMaterials);
				Line += Scene->HeaviestMeshes.IsEmpty() ? FString(TEXT(".")) : FString::Printf(TEXT("; heaviest meshes %s."), *FString::Join(Scene->HeaviestMeshes, TEXT(", ")));
			}
			Advice.Add(Line);
			const TArray<FString> Try = Hints(Report.TopGpuStats, nullptr);
			Advice.Add(Try.IsEmpty() ? FString(TEXT("Try a lower screen percentage first to confirm the GPU bound (frame time should drop), then cut the heaviest pass."))
				: TEXT("Try: ") + FString::Join(Try, TEXT("; ")) + TEXT(". A lower screen percentage confirms the GPU bound when frame time drops with it."));
		}
		else if (Verdict.Bound == TEXT("game_thread"))
		{
			FString Line = Verdict.Summary;
			const TArray<FString> Stats = Names(Report.TopCpuStats, TEXT("GameThread"), 4);
			if (!Stats.IsEmpty()) Line += TEXT(" Top game-thread stats: ") + FString::Join(Stats, TEXT(", ")) + TEXT(".");
			const TArray<FString> Hot = Timers(Report.HotFunctions, TEXT("GameThread"), 4);
			if (!Hot.IsEmpty()) Line += TEXT(" Hottest game-thread timers: ") + FString::Join(Hot, TEXT(", ")) + TEXT(".");
			Advice.Add(Line);
			const TArray<FString> Try = Hints(Report.TopCpuStats, TEXT("GameThread"));
			if (!Try.IsEmpty()) Advice.Add(TEXT("Try: ") + FString::Join(Try, TEXT("; ")) + TEXT("."));
		}
		else if (Verdict.Bound == TEXT("render_thread") || Verdict.Bound == TEXT("rhi_thread"))
		{
			FString Line = Verdict.Summary;
			if (Report.DrawCalls.Average > 0.0)
			{
				Line += FString::Printf(TEXT(" The RHI submitted %.0f draw calls a frame on average (%.0f at most)."), Report.DrawCalls.Average, Report.DrawCalls.Max);
			}
			const TCHAR* Thread = Verdict.Bound == TEXT("render_thread") ? TEXT("RenderThread") : TEXT("RHIThread");
			const TArray<FString> Stats = Names(Report.TopCpuStats, Thread, 4);
			if (!Stats.IsEmpty()) Line += FString::Printf(TEXT(" Top %s stats: "), Thread) + FString::Join(Stats, TEXT(", ")) + TEXT(".");
			if (bScene)
			{
				Line += FString::Printf(TEXT(" The level has %d non-Nanite primitives (about %d draw calls and %d shadow draw calls estimated) and %d dynamic shadow-casting lights."),
					Scene->NonNanitePrimitives, Scene->EstimatedDrawCalls, Scene->EstimatedShadowDrawCalls, Scene->ShadowCastingDynamicLights);
			}
			Advice.Add(Line);
			Advice.Add(TEXT("Try: fewer draw calls (Nanite or instancing for repeated meshes, merged actors, cull distances), and fewer movable shadow-casting lights, each of which draws the scene again."));
		}
		else if (Verdict.Bound == TEXT("capped"))
		{
			Advice.Add(Verdict.Summary);
			Advice.Add(Verdict.CapSource.Contains(TEXT("throttl"))
				? FString(TEXT("The editor was throttled in the background, so the frame rate says nothing about the game: keep the editor in front, or turn off Editor Preferences > Use Less CPU when in Background, and measure again."))
				: FString(TEXT("The real bound is hidden under the cap: measure again with it off (t.MaxFPS 0, r.VSync 0) to see what limits the frame.")));
		}
		else if (Verdict.Bound == TEXT("unknown"))
		{
			Advice.Add(Verdict.Summary);
			Advice.Add(TEXT("Measure in an editor that renders (not -nullrhi) so GPU time is known, and with any frame limit off (t.MaxFPS 0)."));
		}

		if (Report.HitchCount > 0 && !Report.Hitches.IsEmpty())
		{
			const FHyperAIPerfHitch& Worst = Report.Hitches[0];
			FString Line = FString::Printf(TEXT("%d hitch(es) over %s ms or twice the median frame; the worst is frame %d at %.1f s, %s ms, bound by %s"),
				Report.HitchCount, *Ms(Report.HitchThresholdMs), Worst.Frame, Worst.AtSeconds, *Ms(Worst.FrameMs), *Worst.Bound);
			if (!Worst.Spikes.IsEmpty()) Line += TEXT("; it spiked in ") + FString::Join(Worst.Spikes, TEXT(", "));
			if (!Worst.Events.IsEmpty()) Line += TEXT("; events: ") + FString::Join(Worst.Events, TEXT(", "));
			Line += TEXT(".");
			Advice.Add(Line);
			if (!Report.HotFunctionsInWorstFrame.IsEmpty())
			{
				TArray<FString> Hot;
				for (const FHyperAIPerfTimer& Timer : Report.HotFunctionsInWorstFrame)
				{
					Hot.Add(FString::Printf(TEXT("%s %s ms"), *Timer.Name, *Ms(Timer.ExclusiveMs)));
					if (Hot.Num() >= 4) break;
				}
				Advice.Add(FString::Printf(TEXT("Inside the trace's slowest frame (%s ms) the most time went to %s."), *Ms(Report.WorstTraceFrameMs), *FString::Join(Hot, TEXT(", "))));
			}
		}
		if (Report.MemoryEndMB - Report.MemoryStartMB > 100.0)
		{
			Advice.Add(FString::Printf(TEXT("Physical memory grew %.0f MB over the capture (%.0f to %.0f MB): look for something loading or leaking during play."),
				Report.MemoryEndMB - Report.MemoryStartMB, Report.MemoryStartMB, Report.MemoryEndMB));
		}
		for (const FString& Line : Report.Renderer)
		{
			if (Line.Contains(TEXT("over the pool")))
			{
				Advice.Add(TEXT("Nanite's visible set outgrows its streaming pool: raise r.Nanite.Streaming.StreamingPoolSize, or the streamer keeps evicting pages it needs."));
			}
			if (Line.Contains(TEXT("overflowed")))
			{
				Advice.Add(Line);
			}
		}
		if (!Report.FxSystems.IsEmpty() && Report.FxSystems[0].GameThreadUs >= 500.0)
		{
			Advice.Add(FString::Printf(TEXT("Niagara: %s costs %.0f us a frame on the game thread (%.1f instances on average)."),
				*FPaths::GetBaseFilename(Report.FxSystems[0].System), Report.FxSystems[0].GameThreadUs, Report.FxSystems[0].Instances));
		}
	}

	void Compare(const FHyperAIPerfReport& Before, const FHyperAIPerfReport& After, const double ThresholdPercent, const int32 TopN,
		FHyperAIPerfCompareReport& Out)
	{
		Out.Metrics.Reset();
		Out.Stats.Reset();
		Out.BoundBefore = Before.Verdict.Bound;
		Out.BoundAfter = After.Verdict.Bound;
		Out.BaselineLabel = Before.Label;
		Out.CandidateLabel = After.Label;
		AddDelta(Out.Metrics, TEXT("frame_ms_avg"), Before.FrameTime.Average, After.FrameTime.Average, ThresholdPercent);
		AddDelta(Out.Metrics, TEXT("frame_ms_p50"), Before.FrameTime.P50, After.FrameTime.P50, ThresholdPercent);
		AddDelta(Out.Metrics, TEXT("frame_ms_p95"), Before.FrameTime.P95, After.FrameTime.P95, ThresholdPercent);
		AddDelta(Out.Metrics, TEXT("frame_ms_p99"), Before.FrameTime.P99, After.FrameTime.P99, ThresholdPercent);
		AddDelta(Out.Metrics, TEXT("frame_ms_max"), Before.FrameTime.Max, After.FrameTime.Max, ThresholdPercent);
		AddDelta(Out.Metrics, TEXT("fps_avg"), Before.AverageFps, After.AverageFps, ThresholdPercent, /*bHigherIsBetter=*/true);
		for (const FHyperAIPerfSeries& Series : Before.Threads)
		{
			const FHyperAIPerfSeries* Match = After.Threads.FindByPredicate([&Series](const FHyperAIPerfSeries& Other) { return Other.Name == Series.Name; });
			if (Match)
			{
				AddDelta(Out.Metrics, Series.Name + TEXT("_ms_avg"), Series.Average, Match->Average, ThresholdPercent);
				AddDelta(Out.Metrics, Series.Name + TEXT("_ms_p95"), Series.P95, Match->P95, ThresholdPercent);
			}
			else
			{
				Out.Notes.Add(FString::Printf(TEXT("Only the baseline has %s time."), *Series.Name));
			}
		}
		if (Before.bGpuTimeAvailable != After.bGpuTimeAvailable)
		{
			Out.Notes.Add(TEXT("GPU time is available in only one of the two profiles, so the GPU is not compared."));
		}
		if (!Before.DrawCalls.Name.IsEmpty() && !After.DrawCalls.Name.IsEmpty())
		{
			AddDelta(Out.Metrics, TEXT("draw_calls_avg"), Before.DrawCalls.Average, After.DrawCalls.Average, ThresholdPercent);
		}
		if (!Before.Primitives.Name.IsEmpty() && !After.Primitives.Name.IsEmpty())
		{
			AddDelta(Out.Metrics, TEXT("primitives_avg"), Before.Primitives.Average, After.Primitives.Average, ThresholdPercent);
		}
		const double BeforeHitchRate = Before.CapturedSeconds > 0.0 ? Before.HitchCount / Before.CapturedSeconds * 60.0 : 0.0;
		const double AfterHitchRate = After.CapturedSeconds > 0.0 ? After.HitchCount / After.CapturedSeconds * 60.0 : 0.0;
		AddDelta(Out.Metrics, TEXT("hitches_per_minute"), BeforeHitchRate, AfterHitchRate, ThresholdPercent);
		if (Before.MemoryPeakMB > 0.0 && After.MemoryPeakMB > 0.0)
		{
			AddDelta(Out.Metrics, TEXT("memory_peak_mb"), Before.MemoryPeakMB, After.MemoryPeakMB, ThresholdPercent);
		}
		if (FMath::Abs(Before.HitchThresholdMs - After.HitchThresholdMs) > 0.01)
		{
			Out.Notes.Add(FString::Printf(TEXT("The profiles counted hitches over different thresholds (%s and %s ms)."), *Ms(Before.HitchThresholdMs), *Ms(After.HitchThresholdMs)));
		}

		// Stats from either profile's top lists; one missing from a list was below that profile's top, so it reads 0 there.
		TMap<FString, TPair<double, double>> Stats;
		auto Collect = [&Stats](const TArray<FHyperAIPerfStatLine>& Lines, const bool bAfter)
		{
			for (const FHyperAIPerfStatLine& Line : Lines)
			{
				TPair<double, double>& Pair = Stats.FindOrAdd(Line.Name, TPair<double, double>(0.0, 0.0));
				(bAfter ? Pair.Value : Pair.Key) = Line.AverageMs;
			}
		};
		Collect(Before.TopCpuStats, false);
		Collect(Before.TopGpuStats, false);
		Collect(After.TopCpuStats, true);
		Collect(After.TopGpuStats, true);
		for (const TPair<FString, TPair<double, double>>& Pair : Stats)
		{
			AddDelta(Out.Stats, Pair.Key, Pair.Value.Key, Pair.Value.Value, ThresholdPercent);
		}
		Out.Stats.Sort([](const FHyperAIPerfDelta& A, const FHyperAIPerfDelta& B) { return FMath::Abs(A.Change) > FMath::Abs(B.Change); });
		if (Out.Stats.Num() > TopN) Out.Stats.SetNum(TopN);

		Out.Better = 0;
		Out.Worse = 0;
		for (const FHyperAIPerfDelta& Delta : Out.Metrics)
		{
			Out.Better += Delta.Verdict == TEXT("better") ? 1 : 0;
			Out.Worse += Delta.Verdict == TEXT("worse") ? 1 : 0;
		}
		const FHyperAIPerfDelta& Frame = Out.Metrics[0];
		Out.Summary = FString::Printf(TEXT("Frame time %s -> %s ms (%+.1f%%, %s); p99 %s -> %s ms; bound %s -> %s; %d metric(s) better, %d worse."),
			*Ms(Frame.Before), *Ms(Frame.After), Frame.ChangePercent, *Frame.Verdict, *Ms(Before.FrameTime.P99), *Ms(After.FrameTime.P99),
			Out.BoundBefore.IsEmpty() ? TEXT("?") : *Out.BoundBefore, Out.BoundAfter.IsEmpty() ? TEXT("?") : *Out.BoundAfter, Out.Better, Out.Worse);
	}
}
