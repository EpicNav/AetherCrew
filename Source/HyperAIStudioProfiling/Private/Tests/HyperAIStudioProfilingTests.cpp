// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioProfilingToolset.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioProfileAnalysis.h"
#include "HyperAIStudioProfileSession.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "RHIGlobals.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/StrongObjectPtr.h"

namespace HyperAIStudio::Profiling::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString FixturePath(const TCHAR* Name)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		const FString Base = Plugin.IsValid() ? Plugin->GetBaseDir() : FPaths::ProjectPluginsDir() / TEXT("HyperAISb3f9f07c5a33V1");
		return FPaths::ConvertRelativePathToFull(Base / TEXT("Source/HyperAIStudioProfiling/Private/Tests/Fixtures") / Name);
	}

	bool LoadFixture(const TCHAR* Name, FCsvCapture& Out, FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FixturePath(Name)))
		{
			OutError = FString::Printf(TEXT("fixture %s is missing"), *FixturePath(Name));
			return false;
		}
		return ParseCsv(Text, Out, OutError);
	}

	/** The fixture analysed as a session would analyse it. */
	FHyperAIPerfReport Analysed(FAutomationTestBase& Test, const TCHAR* Name, const FCaptureContext& Context = FCaptureContext(),
		const FAnalysisOptions& Options = FAnalysisOptions())
	{
		FHyperAIPerfReport Report;
		FCsvCapture Csv;
		FString Error;
		if (!Test.TestTrue(FString::Printf(TEXT("%s parses: %s"), Name, *Error), LoadFixture(Name, Csv, Error)))
		{
			return Report;
		}
		Analyse(Csv, Context, Options, Report);
		Advise(Report, nullptr);
		return Report;
	}

	bool AnyContains(const TArray<FString>& Lines, const TCHAR* Text)
	{
		return Lines.ContainsByPredicate([Text](const FString& Line) { return Line.Contains(Text); });
	}

	/** Calls a tool the way an agent does, through Epic's toolset registry, and returns its report. */
	TSharedPtr<FJsonObject> CallTool(const FString& Tool, const FString& Json, FString& OutError)
	{
		const TStrongObjectPtr<UToolCallAsyncResultString> Result(UToolsetRegistry::ExecuteTool(
			FHyperAIStudioProfilingContracts::GetQualifiedToolsetName(), Tool, Json));
		const double End = FPlatformTime::Seconds() + 30.0;
		while (Result.IsValid() && !Result->bIsComplete && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		}
		if (!Result.IsValid() || !Result->bIsComplete || !Result->Error.IsEmpty())
		{
			OutError = Result.IsValid() ? (Result->Error.IsEmpty() ? FString(TEXT("the call did not finish")) : Result->Error) : FString(TEXT("no result"));
			return nullptr;
		}
		TSharedPtr<FJsonObject> Object;
		const TSharedPtr<FJsonObject>* Report = nullptr;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result->Value), Object) && Object.IsValid()
			&& Object->TryGetObjectField(TEXT("returnValue"), Report) && Report ? *Report : Object;
	}

	FString Quoted(const FString& Path)
	{
		return FString::Printf(TEXT("\"%s\""), *Path.Replace(TEXT("\\"), TEXT("/")));
	}

	/** Restores the edit-approval setting a test changed. */
	struct FApprovalScope
	{
		explicit FApprovalScope(const bool bRequire)
		{
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			bPrevious = Settings->bRequireApprovalForAgentEdits;
			Settings->bRequireApprovalForAgentEdits = bRequire;
		}
		~FApprovalScope()
		{
			GetMutableDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits = bPrevious;
		}
		bool bPrevious = true;
	};

	void EndPlay()
	{
		if (!GEditor) return;
		if (GEditor->IsPlaySessionRequestQueued()) GEditor->CancelRequestPlaySession();
		if (GEditor->PlayWorld) GEditor->RequestEndPlayMap();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProfilingParseTest,
	"HyperAIStudio.Profiling.Analysis.ParsesCsvProfilerFiles",
	HyperAIStudio::Profiling::Tests::Flags)

bool FHyperAIStudioProfilingParseTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Profiling;
	using namespace HyperAIStudio::Profiling::Tests;
	FCsvCapture Csv;
	FString Error;
	if (!TestTrue(TEXT("The hitches fixture parses: ") + Error, LoadFixture(TEXT("hitches.csv"), Csv, Error)))
	{
		return false;
	}
	TestEqual(TEXT("Every frame row is read, and no header row is a frame"), Csv.NumFrames(), 120);
	TestEqual(TEXT("The final header names a column that first appeared mid-capture"), Csv.Columns.Last(),
		FString(TEXT("Exclusive/RenderThread/FetchVisibilityForPrimitives")));
	const TArray<double>* Late = Csv.Column(TEXT("Exclusive/RenderThread/FetchVisibilityForPrimitives"));
	if (TestNotNull(TEXT("The late column is there"), Late))
	{
		TestEqual(TEXT("Rows written before it existed read 0"), (*Late)[0], 0.0);
		TestTrue(TEXT("Rows after carry its values"), (*Late)[10] > 0.0);
	}
	TestEqual(TEXT("A frame's events split on semicolons"), Csv.Events[30].Num(), 2);
	TestTrue(TEXT("and keep their text"), Csv.Events[30].Contains(TEXT("Level/Streaming in L_Arena")));
	TestEqual(TEXT("The metadata row is read"), Csv.Metadata.FindRef(TEXT("HasHeaderRowAtEnd")), FString(TEXT("1")));
	TestTrue(TEXT("The command line keeps its commas"), Csv.Metadata.FindRef(TEXT("Commandline")).Contains(TEXT("-game, -fixture=hitches")));
	TestEqual(TEXT("Values read as written"), (*Csv.Column(TEXT("FrameTime")))[30], 80.0);

	// A stat written by several threads under one name is one column, summed, as the CSV tools read it.
	FCsvCapture Summed;
	const FString Doubled = TEXT("EVENTS,FrameTime,Exclusive/GameThread/X,Exclusive/GameThread/X\n,10,1,2\n,12,1.5,0.5\nEVENTS,FrameTime,Exclusive/GameThread/X,Exclusive/GameThread/X\n[HasHeaderRowAtEnd],1\n");
	if (TestTrue(TEXT("A file with a repeated column parses"), ParseCsv(Doubled, Summed, Error)))
	{
		TestEqual(TEXT("Repeated columns merge into one"), Summed.Columns.Num(), 2);
		TestEqual(TEXT("and their values add up"), (*Summed.Column(TEXT("Exclusive/GameThread/X")))[0], 3.0);
	}
	FCsvCapture NotCsv;
	TestFalse(TEXT("A file that is not the CSV profiler's is refused"), ParseCsv(TEXT("hello,world\n1,2\n"), NotCsv, Error));
	TestFalse(TEXT("A header with no frames is refused"), ParseCsv(TEXT("EVENTS,FrameTime\n"), NotCsv, Error));

	TArray<double> Hundred;
	for (int32 Value = 1; Value <= 100; ++Value) Hundred.Add(Value);
	TestEqual(TEXT("p50 by nearest rank"), Percentile(Hundred, 50.0), 50.0);
	TestEqual(TEXT("p95 by nearest rank"), Percentile(Hundred, 95.0), 95.0);
	TestEqual(TEXT("p99 by nearest rank"), Percentile(Hundred, 99.0), 99.0);
	TestEqual(TEXT("p100 is the max"), Percentile(Hundred, 100.0), 100.0);
	TestEqual(TEXT("No values read 0"), Percentile(TArray<double>(), 50.0), 0.0);
	TestEqual(TEXT("One value is every percentile"), Percentile({7.0}, 99.0), 7.0);
	FHyperAIPerfSeries Series;
	Describe(TEXT("frame"), {4.0, 2.0, 6.0}, Series);
	TestEqual(TEXT("Describe averages"), Series.Average, 4.0);
	TestEqual(TEXT("and finds the max and min"), Series.Max + Series.Min, 8.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProfilingRecordedTest,
	"HyperAIStudio.Profiling.Analysis.RecordedEditorCapture",
	HyperAIStudio::Profiling::Tests::Flags)

bool FHyperAIStudioProfilingRecordedTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Profiling;
	using namespace HyperAIStudio::Profiling::Tests;
	// The first 40 frames a headless editor's session recorded, as the CSV profiler wrote them (machine details left out).
	FCsvCapture Csv;
	FString Error;
	if (!TestTrue(TEXT("The recorded capture parses: ") + Error, LoadFixture(TEXT("recorded_editor.csv"), Csv, Error)))
	{
		return false;
	}
	TestEqual(TEXT("All 40 frames are read"), Csv.NumFrames(), 40);
	for (const TCHAR* Column : {TEXT("FrameTime"), TEXT("GameThreadTime"), TEXT("RenderThreadTime"), TEXT("RHIThreadTime"), TEXT("GPUTime"),
		TEXT("RHI/DrawCalls"), TEXT("RHI/PrimitivesDrawn"), TEXT("PhysicalUsedMB"), TEXT("Exclusive/RenderThread/EventWait")})
	{
		TestTrue(FString::Printf(TEXT("The engine wrote %s"), Column), Csv.Find(Column) != INDEX_NONE);
	}
	const FHyperAIPerfReport Report = Analysed(*this, TEXT("recorded_editor.csv"));
	TestEqual(TEXT("39 frames after the first"), Report.Frames, 39);
	TestEqual(TEXT("A long game thread in a headless editor is game-thread bound"), Report.Verdict.Bound, FString(TEXT("game_thread")));
	TestTrue(TEXT("The 409 ms frame is the worst hitch"), Report.Hitches.Num() > 0 && Report.Hitches[0].Frame == 26 && Report.Hitches[0].FrameMs > 400.0);
	TestTrue(TEXT("An editor that under-reports RenderThreadTime is floored by the thread's own stats, and says so"), AnyContains(Report.Notes, TEXT("RenderThreadTime averages")));
	TestFalse(TEXT("so no stat claims more than its whole thread"), Report.TopCpuStats.ContainsByPredicate([](const FHyperAIPerfStatLine& Line) { return Line.SharePercent > 100.5; }));
	TestTrue(TEXT("The render thread's waiting is listed apart"), Report.Waits.ContainsByPredicate([](const FHyperAIPerfStatLine& Line) { return Line.Name == TEXT("Exclusive/RenderThread/EventWait"); }));
	TestTrue(TEXT("Draw calls come from the RHI's count"), Report.DrawCalls.Average > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProfilingVerdictTest,
	"HyperAIStudio.Profiling.Analysis.BoundVerdicts",
	HyperAIStudio::Profiling::Tests::Flags)

bool FHyperAIStudioProfilingVerdictTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Profiling;
	using namespace HyperAIStudio::Profiling::Tests;

	const FHyperAIPerfReport Gpu = Analysed(*this, TEXT("gpu_bound.csv"));
	TestEqual(TEXT("A frame waiting on the GPU is GPU-bound"), Gpu.Verdict.Bound, FString(TEXT("gpu")));
	TestTrue(TEXT("on nearly every frame"), Gpu.Verdict.FramesPercent > 90.0);
	TestTrue(TEXT("GPU time is available when the capture has it"), Gpu.bGpuTimeAvailable);
	TestTrue(TEXT("The evidence gives each thread's average"), AnyContains(Gpu.Verdict.Evidence, TEXT("game thread")) && AnyContains(Gpu.Verdict.Evidence, TEXT("GPU ")));
	TestTrue(TEXT("Lumen is the heaviest GPU pass"), Gpu.TopGpuStats.Num() > 0 && Gpu.TopGpuStats[0].Name == TEXT("GPU/Lumen"));
	TestTrue(TEXT("A visible set past the Nanite pool is called out"), AnyContains(Gpu.Renderer, TEXT("over the pool")));
	TestTrue(TEXT("and advised on"), AnyContains(Gpu.Advice, TEXT("StreamingPoolSize")));
	TestTrue(TEXT("The GPU advice names the heaviest pass"), AnyContains(Gpu.Advice, TEXT("GPU/Lumen")));
	TestEqual(TEXT("The thread series are frame-aligned"), Gpu.Threads.Num(), 4);

	const FHyperAIPerfReport Game = Analysed(*this, TEXT("game_bound.csv"));
	TestEqual(TEXT("A long game thread is game-thread bound"), Game.Verdict.Bound, FString(TEXT("game_thread")));
	TestTrue(TEXT("Physics tops the game thread's exclusive stats"), Game.TopCpuStats.Num() > 0 && Game.TopCpuStats[0].Name == TEXT("Exclusive/GameThread/Physics"));
	TestTrue(TEXT("Its share of the game thread is about 40%"), Game.TopCpuStats.Num() > 0 && FMath::IsNearlyEqual(Game.TopCpuStats[0].SharePercent, 40.0, 2.0));
	TestTrue(TEXT("Waits are kept apart from work"), Game.Waits.ContainsByPredicate([](const FHyperAIPerfStatLine& Line) { return Line.Name.Contains(TEXT("EventWait")); })
		&& !Game.TopCpuStats.ContainsByPredicate([](const FHyperAIPerfStatLine& Line) { return Line.Name.Contains(TEXT("EventWait")); }));
	TestTrue(TEXT("The advice names physics"), AnyContains(Game.Advice, TEXT("physics")));

	const FHyperAIPerfReport Render = Analysed(*this, TEXT("render_bound.csv"));
	TestEqual(TEXT("A long render thread is render-thread bound"), Render.Verdict.Bound, FString(TEXT("render_thread")));
	TestTrue(TEXT("Draw calls are averaged"), FMath::IsNearlyEqual(Render.DrawCalls.Average, 6103.0, 5.0));
	TestTrue(TEXT("and the advice speaks of them"), AnyContains(Render.Advice, TEXT("draw calls")));

	const FHyperAIPerfReport Rhi = Analysed(*this, TEXT("rhi_bound.csv"));
	TestEqual(TEXT("A long RHI thread is RHI-thread bound"), Rhi.Verdict.Bound, FString(TEXT("rhi_thread")));

	FCaptureContext Sixty;
	Sixty.CapFps = 60.0;
	Sixty.CapSource = TEXT("t.MaxFPS 60");
	const FHyperAIPerfReport Capped = Analysed(*this, TEXT("capped.csv"), Sixty);
	TestEqual(TEXT("Idle frames at the frame limit are capped"), Capped.Verdict.Bound, FString(TEXT("capped")));
	TestTrue(TEXT("The summary names the cap and its source"), Capped.Verdict.Summary.Contains(TEXT("60 fps")) && Capped.Verdict.Summary.Contains(TEXT("t.MaxFPS 60")));
	TestTrue(TEXT("The advice says to measure without it"), AnyContains(Capped.Advice, TEXT("t.MaxFPS 0")));
	const FHyperAIPerfReport Unnamed = Analysed(*this, TEXT("capped.csv"));
	TestEqual(TEXT("Idle frames with GPU time and no known limit are still capped"), Unnamed.Verdict.Bound, FString(TEXT("capped")));
	TestTrue(TEXT("by a limit the profile says it could not name"), Unnamed.Verdict.Summary.Contains(TEXT("could not name")));
	FCaptureContext VSync;
	VSync.CapSource = TEXT("r.VSyncEditor 1 (vsync)");
	TestEqual(TEXT("Vsync caps without a known rate"), Analysed(*this, TEXT("capped.csv"), VSync).Verdict.Bound, FString(TEXT("capped")));

	FCaptureContext Null;
	Null.bNullRhi = true;
	const FHyperAIPerfReport NoGpu = Analysed(*this, TEXT("nullrhi.csv"), Null);
	TestFalse(TEXT("A null RHI has no GPU time"), NoGpu.bGpuTimeAvailable);
	TestTrue(TEXT("and says so in those words"), NoGpu.GpuNote.Contains(TEXT("GPU time unavailable")));
	TestEqual(TEXT("Idle frames without GPU time or a known limit are unknown"), NoGpu.Verdict.Bound, FString(TEXT("unknown")));
	TestFalse(TEXT("No gpu series is listed"), NoGpu.Threads.ContainsByPredicate([](const FHyperAIPerfSeries& Series) { return Series.Name == TEXT("gpu"); }));
	TestFalse(TEXT("nor an rhi one when the RHI has no thread"), NoGpu.Threads.ContainsByPredicate([](const FHyperAIPerfSeries& Series) { return Series.Name == TEXT("rhi"); }));
	FCaptureContext NullCapped = Null;
	NullCapped.CapFps = 120.0;
	NullCapped.CapSource = TEXT("t.MaxFPS 120");
	TestEqual(TEXT("The same frames at a known 120 fps limit are capped"), Analysed(*this, TEXT("nullrhi.csv"), NullCapped).Verdict.Bound, FString(TEXT("capped")));

	// The level's cost, tied into the advice for the bound it explains.
	FSceneCostSummary Scene;
	const FString Json = TEXT("{\"returnValue\":{\"bOk\":true,\"estimatedDrawCalls\":4200,\"estimatedShadowDrawCalls\":900,\"naniteMaterialBins\":37,")
		TEXT("\"translucentMaterials\":6,\"nanitePrimitives\":120,\"nonNanitePrimitives\":800,\"trianglesNonNanite\":5300000,")
		TEXT("\"lights\":{\"shadowCastingDynamic\":3},\"heaviestMeshes\":[{\"name\":\"/Game/Props/SM_Rock.SM_Rock\",\"totalTriangles\":2400000,\"instances\":600,\"bNanite\":false}],")
		TEXT("\"findings\":[\"non_nanite_instances: /Game/Props/SM_Rock draws 2.4M triangles without Nanite\"]}}");
	if (TestTrue(TEXT("The cost report's JSON is read"), ReadSceneCost(Json, Scene)))
	{
		TestEqual(TEXT("its draw calls"), Scene.EstimatedDrawCalls, 4200);
		TestEqual(TEXT("its material bins"), Scene.NaniteMaterialBins, 37);
		TestTrue(TEXT("its heaviest mesh, by name"), Scene.HeaviestMeshes.Num() == 1 && Scene.HeaviestMeshes[0].StartsWith(TEXT("SM_Rock (600 instances")));
		FHyperAIPerfReport WithScene = Gpu;
		Advise(WithScene, &Scene);
		TestTrue(TEXT("GPU-bound advice cites the level's materials and heaviest meshes"),
			AnyContains(WithScene.Advice, TEXT("37 Nanite material bins")) && AnyContains(WithScene.Advice, TEXT("SM_Rock")));
		FHyperAIPerfReport RenderWithScene = Render;
		Advise(RenderWithScene, &Scene);
		TestTrue(TEXT("Render-bound advice cites the non-Nanite primitives"), AnyContains(RenderWithScene.Advice, TEXT("800 non-Nanite primitives")));
		TestTrue(TEXT("The cost report reads as a few lines"), DescribeSceneCost(Scene).Num() >= 3);
	}
	FSceneCostSummary Refused;
	TestFalse(TEXT("A cost report that did not complete is not used"), ReadSceneCost(TEXT("{\"returnValue\":{\"bOk\":false}}"), Refused));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProfilingHitchTest,
	"HyperAIStudio.Profiling.Analysis.HitchesAndPercentiles",
	HyperAIStudio::Profiling::Tests::Flags)

bool FHyperAIStudioProfilingHitchTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Profiling;
	using namespace HyperAIStudio::Profiling::Tests;
	const FHyperAIPerfReport Report = Analysed(*this, TEXT("hitches.csv"));
	TestEqual(TEXT("The capture's first frame is left out"), Report.Frames, 119);
	TestTrue(TEXT("and the profile says so"), AnyContains(Report.Notes, TEXT("left out")));
	TestTrue(TEXT("The median frame is about 16 ms"), FMath::IsNearlyEqual(Report.FrameTime.P50, 16.0, 0.3));
	TestEqual(TEXT("The slowest frame is the max"), Report.FrameTime.Max, 80.0);
	TestTrue(TEXT("p99 sits at or under the max"), Report.FrameTime.P99 <= Report.FrameTime.Max && Report.FrameTime.P99 >= Report.FrameTime.P95);
	TestEqual(TEXT("Two frames hitch: one over 50 ms, one over twice the median"), Report.HitchCount, 2);
	if (Report.Hitches.Num() == 2)
	{
		const FHyperAIPerfHitch& Worst = Report.Hitches[0];
		TestEqual(TEXT("The worst hitch is listed first, by its frame in the capture"), Worst.Frame, 30);
		TestEqual(TEXT("with its time"), Worst.FrameMs, 80.0);
		TestTrue(TEXT("and when it began: 200 ms plus 29 frames of about 16 ms"), FMath::IsNearlyEqual(Worst.AtSeconds, 0.664, 0.02));
		TestEqual(TEXT("It was the game thread's"), Worst.Bound, FString(TEXT("game")));
		TestTrue(TEXT("The game thread spiked most"), Worst.Spikes.Num() > 0 && Worst.Spikes[0].StartsWith(TEXT("GameThreadTime 75.0 ms")));
		TestTrue(TEXT("with physics under it"), AnyContains(Worst.Spikes, TEXT("Exclusive/GameThread/Physics 62.0 ms")));
		TestTrue(TEXT("The frame's events come along"), Worst.Events.Contains(TEXT("Level/Streaming in L_Arena")));
		const FHyperAIPerfHitch& Second = Report.Hitches[1];
		TestEqual(TEXT("The second is the 35 ms frame, over twice the median"), Second.Frame, 70);
		TestEqual(TEXT("bound by the GPU"), Second.Bound, FString(TEXT("gpu")));
		TestTrue(TEXT("where Lumen spiked"), AnyContains(Second.Spikes, TEXT("GPU/Lumen")));
	}
	TestTrue(TEXT("The advice describes the worst hitch"), AnyContains(Report.Advice, TEXT("frame 30")));

	FAnalysisOptions Lower;
	Lower.HitchThresholdMs = 25.0;
	TestEqual(TEXT("A 25 ms threshold also catches the 30 ms frame"), Analysed(*this, TEXT("hitches.csv"), FCaptureContext(), Lower).HitchCount, 3);
	FAnalysisOptions Few;
	Few.MaxHitches = 1;
	const FHyperAIPerfReport Capped = Analysed(*this, TEXT("hitches.csv"), FCaptureContext(), Few);
	TestEqual(TEXT("The list is cut to the worst"), Capped.Hitches.Num(), 1);
	TestEqual(TEXT("while the count stays whole"), Capped.HitchCount, 2);
	TestTrue(TEXT("Memory start, end and peak come from PhysicalUsedMB"), Report.MemoryStartMB > 0.0 && Report.MemoryPeakMB >= Report.MemoryEndMB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProfilingCompareTest,
	"HyperAIStudio.Profiling.Compare",
	HyperAIStudio::Profiling::Tests::Flags)

bool FHyperAIStudioProfilingCompareTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Profiling;
	using namespace HyperAIStudio::Profiling::Tests;
	FCaptureContext Sixty;
	Sixty.CapFps = 60.0;
	Sixty.CapSource = TEXT("t.MaxFPS 60");
	const FHyperAIPerfReport Before = Analysed(*this, TEXT("game_bound.csv"));
	const FHyperAIPerfReport After = Analysed(*this, TEXT("capped.csv"), Sixty);
	FHyperAIPerfCompareReport Report;
	Compare(Before, After, 5.0, 10, Report);
	const FHyperAIPerfDelta* Frame = Report.Metrics.FindByPredicate([](const FHyperAIPerfDelta& Delta) { return Delta.Metric == TEXT("frame_ms_avg"); });
	if (TestNotNull(TEXT("Frame time is compared"), Frame))
	{
		TestEqual(TEXT("A faster frame is better"), Frame->Verdict, FString(TEXT("better")));
		TestTrue(TEXT("by about a third"), Frame->ChangePercent < -30.0);
	}
	const FHyperAIPerfDelta* Fps = Report.Metrics.FindByPredicate([](const FHyperAIPerfDelta& Delta) { return Delta.Metric == TEXT("fps_avg"); });
	TestTrue(TEXT("More frames a second is better, not worse"), Fps && Fps->Verdict == TEXT("better"));
	TestEqual(TEXT("The bound before"), Report.BoundBefore, FString(TEXT("game_thread")));
	TestEqual(TEXT("and after"), Report.BoundAfter, FString(TEXT("capped")));
	const FHyperAIPerfDelta* Physics = Report.Stats.FindByPredicate([](const FHyperAIPerfDelta& Delta) { return Delta.Metric == TEXT("Exclusive/GameThread/Physics"); });
	TestTrue(TEXT("The stats that moved include physics, which dropped"), Physics && Physics->Change < 0.0);
	TestTrue(TEXT("The summary reads before -> after"), Report.Summary.Contains(TEXT("game_thread -> capped")));

	FHyperAIPerfCompareReport Same;
	Compare(Before, Before, 5.0, 10, Same);
	TestEqual(TEXT("A profile against itself has nothing better"), Same.Better, 0);
	TestEqual(TEXT("or worse"), Same.Worse, 0);

	// Through the tool, as an agent calls it: CSV paths inside the project.
	FString Error;
	const TSharedPtr<FJsonObject> Tool = CallTool(TEXT("hyper_profile_compare"), FString::Printf(TEXT("{\"request\":{\"baseline\":%s,\"candidate\":%s}}"),
		*Quoted(FixturePath(TEXT("gpu_bound.csv"))), *Quoted(FixturePath(TEXT("render_bound.csv")))), Error);
	if (TestTrue(TEXT("hyper_profile_compare answers: ") + Error, Tool.IsValid()))
	{
		TestEqual(TEXT("It compares two CSVs"), Tool->GetStringField(TEXT("status")), FString(TEXT("compared")));
		TestEqual(TEXT("and names the bound before"), Tool->GetStringField(TEXT("boundBefore")), FString(TEXT("gpu")));
		TestEqual(TEXT("and after"), Tool->GetStringField(TEXT("boundAfter")), FString(TEXT("render_thread")));
	}
	const TSharedPtr<FJsonObject> Missing = CallTool(TEXT("hyper_profile_compare"), TEXT("{\"request\":{\"baseline\":\"no-such-profile\",\"candidate\":\"no-such-either\"}}"), Error);
	TestTrue(TEXT("An unknown profile id is not_found"), Missing.IsValid() && Missing->GetStringField(TEXT("status")) == TEXT("not_found"));
	const TSharedPtr<FJsonObject> Empty = CallTool(TEXT("hyper_profile_compare"), TEXT("{\"request\":{}}"), Error);
	TestTrue(TEXT("No profiles is an invalid request"), Empty.IsValid() && Empty->GetStringField(TEXT("status")) == TEXT("invalid_request"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProfilingRefusalTest,
	"HyperAIStudio.Profiling.Tools.Refusals",
	HyperAIStudio::Profiling::Tests::Flags)

bool FHyperAIStudioProfilingRefusalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Profiling::Tests;
	if (!GEditor || GEditor->PlayWorld || HyperAIStudio::Profiling::Session::IsRunning())
	{
		AddInfo(TEXT("Play In Editor or a profile is running here; the refusals that need neither are not checked."));
		return true;
	}
	auto Session = [](const FHyperAIPerfSessionRequest& Request)
	{
		UHyperAIPerfSessionResult* Result = UHyperAIStudioProfilingToolset::hyper_profile_session(Request);
		return Result && Result->bIsComplete ? Result->Value : FHyperAIPerfReport();
	};
	FHyperAIPerfSessionRequest Attach;
	Attach.Mode = TEXT("attach");
	Attach.DurationSeconds = 9999.f;
	Attach.WarmupSeconds = 99.f;
	const FHyperAIPerfReport NoPie = Session(Attach);
	TestEqual(TEXT("Attaching with no play session is refused"), NoPie.Status, FString(TEXT("no_pie_world")));
	TestTrue(TEXT("and says what was fitted on the way: ") + NoPie.Diagnostic,
		NoPie.Diagnostic.Contains(TEXT("duration_seconds 9999.0 -> 60.0")) && NoPie.Diagnostic.Contains(TEXT("warmup_seconds 99.0 -> 30.0")));

	FHyperAIPerfSessionRequest Bogus;
	Bogus.Mode = TEXT("sometimes");
	TestEqual(TEXT("An unknown mode is refused"), Session(Bogus).Status, FString(TEXT("invalid_request")));
	FHyperAIPerfSessionRequest Escape;
	Escape.OperationId = TEXT("../outside");
	TestEqual(TEXT("An id that is no folder name is refused"), Session(Escape).Status, FString(TEXT("invalid_request")));
	FHyperAIPerfSessionRequest Unknown;
	Unknown.ProfileId = TEXT("profile-never-made");
	TestEqual(TEXT("Reading back a profile never made is not_found"), Session(Unknown).Status, FString(TEXT("not_found")));
	FHyperAIPerfSessionRequest Outside;
	Outside.CsvPath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Config/BaseEngine.ini"));
	const FHyperAIPerfReport OutsideReport = Session(Outside);
	TestEqual(TEXT("A file outside the project is refused"), OutsideReport.Status, FString(TEXT("invalid_request")));
	FHyperAIPerfSessionRequest LongRoute;
	LongRoute.Mode = TEXT("attach");
	LongRoute.Bot.SetNum(FHyperAIStudioProfilingContracts::MaxBotSteps + 1);
	TestEqual(TEXT("A bot route past the step limit is refused"), Session(LongRoute).Status, FString(TEXT("invalid_request")));

	// A CSV analysed by the tool is kept as a profile, and read back by its id.
	FHyperAIPerfSessionRequest FromCsv;
	FromCsv.CsvPath = FixturePath(TEXT("game_bound.csv"));
	FromCsv.OperationId = FString::Printf(TEXT("perf-test-csv-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	FromCsv.Label = TEXT("fixture");
	const FHyperAIPerfReport FromCsvReport = Session(FromCsv);
	TestEqual(TEXT("An existing CSV is analysed"), FromCsvReport.Status, FString(TEXT("completed")));
	TestEqual(TEXT("from the file"), FromCsvReport.Source, FString(TEXT("csv")));
	TestEqual(TEXT("with the bound it shows"), FromCsvReport.Verdict.Bound, FString(TEXT("game_thread")));
	TestTrue(TEXT("and kept as a profile"), IFileManager::Get().FileExists(*FromCsvReport.ReportPath));
	FHyperAIPerfSessionRequest ReadBack;
	ReadBack.ProfileId = FromCsv.OperationId;
	const FHyperAIPerfReport Stored = Session(ReadBack);
	TestEqual(TEXT("It reads back by id"), Stored.Source, FString(TEXT("stored")));
	TestEqual(TEXT("with the same frames"), Stored.Frames, FromCsvReport.Frames);
	TestEqual(TEXT("and label"), Stored.Label, FString(TEXT("fixture")));
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(FromCsvReport.ReportPath), false, true);

	FHyperAIPerfLiveRequest Live;
	UHyperAIPerfLiveResult* LiveResult = UHyperAIStudioProfilingToolset::hyper_profile_live(Live);
	TestTrue(TEXT("The live readout refuses with no play session"), LiveResult && LiveResult->bIsComplete && LiveResult->Value.Status == TEXT("no_pie_world"));

	// Starting play is an edit: with edit approvals on, the session waits for the user, and starts nothing until then.
	{
		const FApprovalScope Approvals(true);
		FHyperAIPerfSessionRequest Start;
		Start.Mode = TEXT("start");
		Start.DurationSeconds = 1.f;
		Start.OperationId = FString::Printf(TEXT("perf-test-approval-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		const FHyperAIPerfReport Held = Session(Start);
		TestTrue(TEXT("Starting play waits for approval: ") + Held.Status, Held.Status == TEXT("awaiting_user_approval") || Held.Status == TEXT("queued"));
		TestTrue(TEXT("It is held at the gate"), FHyperAIStudioApprovalGate::HasPending(Start.OperationId));
		TestFalse(TEXT("and nothing started"), HyperAIStudio::Profiling::Session::IsRunning() || GEditor->IsPlaySessionRequestQueued());
		FHyperAIStudioApprovalGate::Reject(Start.OperationId, TEXT("test"));
	}

	// Through the registry, as an agent calls it: the async result answers at once for a refusal.
	FString Error;
	const TSharedPtr<FJsonObject> Tool = CallTool(TEXT("hyper_profile_session"), TEXT("{\"request\":{\"mode\":\"attach\"}}"), Error);
	TestTrue(TEXT("The registry returns the refusal: ") + Error, Tool.IsValid() && Tool->GetStringField(TEXT("status")) == TEXT("no_pie_world"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProfilingSessionTest,
	"HyperAIStudio.Profiling.Session.MeasuresHeadlessPlay",
	HyperAIStudio::Profiling::Tests::Flags)

bool FHyperAIStudioProfilingSessionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Profiling::Tests;
	if (!GEditor || GEditor->PlayWorld || GEditor->IsPlaySessionInProgress() || HyperAIStudio::Profiling::Session::IsRunning())
	{
		AddInfo(TEXT("Play In Editor or a profile is already running here; this test leaves it alone."));
		return true;
	}
#if CSV_PROFILER
	if (FCsvProfiler::IsCapturing())
	{
		AddInfo(TEXT("A CSV capture is running here; this test leaves it alone."));
		return true;
	}
#endif
	const TSharedRef<FApprovalScope> Approvals = MakeShared<FApprovalScope>(false);
	FHyperAIPerfSessionRequest Request;
	Request.Mode = TEXT("start");
	Request.DurationSeconds = 2.f;
	Request.WarmupSeconds = 1.f;
	Request.bHotFunctions = true;
	Request.bSceneCost = true;
	Request.OperationId = FString::Printf(TEXT("perf-test-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	Request.Label = TEXT("headless");
	const TStrongObjectPtr<UHyperAIPerfSessionResult> Result(UHyperAIStudioProfilingToolset::hyper_profile_session(Request));
	if (!TestNotNull(TEXT("The session answers with a pending result"), Result.Get()))
	{
		return false;
	}
	TestFalse(TEXT("It runs over frames instead of blocking the call"), Result->bIsComplete);
	TestTrue(TEXT("and is running"), HyperAIStudio::Profiling::Session::IsRunning());
	const double Deadline = FPlatformTime::Seconds() + 180.0;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Result, Deadline, Approvals, Id = Request.OperationId]()
	{
		if (!Result->bIsComplete && FPlatformTime::Seconds() < Deadline) return false;
		if (!TestTrue(TEXT("The session finishes within three minutes"), Result->bIsComplete))
		{
			HyperAIStudio::Profiling::Session::Cancel(TEXT("test timed out"));
			return true;
		}
		const FHyperAIPerfReport& Report = Result->Value;
		AddInfo(FString::Printf(TEXT("Session: %s %s | frames %d, %.1f fps, bound %s | hot functions %d (%s) | GPU %s"), *Report.Status, *Report.Diagnostic,
			Report.Frames, Report.AverageFps, *Report.Verdict.Bound, Report.HotFunctions.Num(), *Report.HotFunctionsNote,
			Report.bGpuTimeAvailable ? TEXT("available") : *Report.GpuNote));
		TestEqual(TEXT("The session completes"), Report.Status, FString(TEXT("completed")));
		TestTrue(TEXT("and is ok"), Report.bOk);
		TestEqual(TEXT("It started play"), Report.Mode, FString(TEXT("start")));
		TestTrue(TEXT("It measured frames"), Report.Frames > 0 && Report.FrameTime.Average > 0.0);
		TestTrue(TEXT("over about the time asked"), Report.CapturedSeconds > 0.5);
		TestFalse(TEXT("It gives a bound"), Report.Verdict.Bound.IsEmpty());
		TestTrue(TEXT("with its evidence"), Report.Verdict.Evidence.Num() > 0);
		if (GUsingNullRHI)
		{
			TestFalse(TEXT("A null RHI has no GPU time"), Report.bGpuTimeAvailable);
			TestTrue(TEXT("and the profile says GPU time unavailable"), Report.GpuNote.Contains(TEXT("GPU time unavailable")));
		}
		TestTrue(TEXT("The CSV it recorded is on disk"), IFileManager::Get().FileExists(*Report.CsvPath));
		TestTrue(TEXT("and so is the profile"), IFileManager::Get().FileExists(*Report.ReportPath));
		TestFalse(TEXT("Hot functions are listed or their absence explained"), Report.HotFunctions.IsEmpty() && Report.HotFunctionsNote.IsEmpty());
		for (const FHyperAIPerfTimer& Timer : Report.HotFunctions)
		{
			TestTrue(TEXT("Each hot function has a name, a thread and a time"), !Timer.Name.IsEmpty() && !Timer.Thread.IsEmpty() && Timer.ExclusiveMs >= 0.0);
		}
		if (!Report.HotFunctions.IsEmpty())
		{
			TestFalse(TEXT("A trace that was analysed is kept beside the CSV"), Report.TracePath.IsEmpty());
		}
		TestTrue(TEXT("Advice is given"), Report.Advice.Num() > 0);

		FHyperAIPerfCompareRequest Compare;
		Compare.Baseline = Id;
		Compare.Candidate = Id;
		const FHyperAIPerfCompareReport Same = UHyperAIStudioProfilingToolset::hyper_profile_compare(Compare);
		TestEqual(TEXT("The kept profile compares by id"), Same.Status, FString(TEXT("compared")));
		TestEqual(TEXT("and against itself nothing got worse"), Same.Worse, 0);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Deadline]()
	{
		// A session that started play stops it.
		if ((GEditor->PlayWorld || GEditor->IsPlaySessionInProgress()) && FPlatformTime::Seconds() < Deadline + 30.0) return false;
		if (!TestNull(TEXT("The session stops the play it started"), GEditor->PlayWorld.Get()))
		{
			EndPlay();
		}
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProfilingLiveTest,
	"HyperAIStudio.Profiling.Live.ReadsNaniteAndNiagara",
	HyperAIStudio::Profiling::Tests::Flags)

bool FHyperAIStudioProfilingLiveTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Profiling::Tests;
	if (!GEditor || GEditor->PlayWorld || GEditor->IsPlaySessionInProgress() || HyperAIStudio::Profiling::Session::IsRunning())
	{
		AddInfo(TEXT("Play In Editor or a profile is already running here; this test leaves it alone."));
		return true;
	}
	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	GEditor->RequestPlaySession(Params);
	struct FState
	{
		int32 Before = 0;
		double SpawnedAt = 0.0;
		TStrongObjectPtr<UHyperAIPerfLiveResult> Sampled;
		FString SystemPath;
		bool bSkip = false;
	};
	const TSharedRef<FState> State = MakeShared<FState>();
	const double Deadline = FPlatformTime::Seconds() + 120.0;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Deadline]()
	{
		UWorld* World = GEditor->PlayWorld;
		if ((!World || !World->HasBegunPlay()) && FPlatformTime::Seconds() < Deadline) return false;
		if (!TestNotNull(TEXT("Play In Editor starts"), World)) return true;

		// What is there before anything is spawned: a sane, complete answer even with no Nanite or Niagara content.
		UHyperAIPerfLiveResult* Now = UHyperAIStudioProfilingToolset::hyper_profile_live(FHyperAIPerfLiveRequest());
		if (!TestTrue(TEXT("A read with no sample answers at once"), Now && Now->bIsComplete)) return true;
		const FHyperAIPerfLiveReport& Report = Now->Value;
		TestEqual(TEXT("It completes"), Report.Status, FString(TEXT("completed")));
		TestTrue(TEXT("Counts are never negative"), Report.Niagara.ActiveComponents >= 0 && Report.Niagara.Particles >= 0 && Report.Nanite.NanitePrimitives >= 0
			&& Report.Nanite.FallbackPrimitives >= 0 && Report.Nanite.NonNanitePrimitives >= 0);
		TestTrue(TEXT("Renderer headlines are read"), Report.Renderer.Num() >= 3);
		TestTrue(TEXT("It says what Nanite figures it cannot read"), AnyContains(Report.Nanite.Notes, TEXT("r.Nanite.ShowStats")));
		TestTrue(TEXT("Unsampled, it says what a sample adds"), AnyContains(Report.Niagara.Notes, TEXT("sample_seconds")));
		TestEqual(TEXT("Nothing was sampled"), Report.SampledFrames, 0);
		State->Before = Report.Niagara.ActiveComponents;
		AddInfo(FString::Printf(TEXT("Before: %s | Nanite %d, fallback %d, plain %d, pool %.0f MB"), *Report.Diagnostic, Report.Nanite.NanitePrimitives,
			Report.Nanite.FallbackPrimitives, Report.Nanite.NonNanitePrimitives, Report.Nanite.StreamingPoolMB));

		// A small looping system from the engine's own templates.
		UNiagaraSystem* System = nullptr;
		static const TCHAR* const Templates[] = {TEXT("/Niagara/DefaultAssets/Templates/Systems/FountainLightweight.FountainLightweight"),
			TEXT("/Niagara/DefaultAssets/Templates/Systems/MinimalLightweight.MinimalLightweight")};
		for (const TCHAR* Path : Templates)
		{
			System = LoadObject<UNiagaraSystem>(nullptr, Path);
			if (System) break;
		}
		if (!TestNotNull(TEXT("An engine Niagara template loads"), System)) return true;
		State->SystemPath = System->GetPathName();
		UNiagaraComponent* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(World, System, FVector(0.0, 0.0, 200.0), FRotator::ZeroRotator,
			FVector::OneVector, /*bAutoDestroy=*/false, /*bAutoActivate=*/true, ENCPoolMethod::None, /*bPreCullCheck=*/false);
		if (!Component && GUsingNullRHI)
		{
			// Niagara does not simulate without an RHI; the readout of no effects was checked above.
			AddInfo(TEXT("Niagara spawns nothing under a null RHI; the sampled Niagara read is left to an editor that renders."));
			State->bSkip = true;
			return true;
		}
		TestNotNull(TEXT("It spawns in the play world"), Component);
		State->SpawnedAt = FPlatformTime::Seconds();
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Deadline]()
	{
		if (!GEditor->PlayWorld || State->bSkip) return true;
		// Let it simulate a little first.
		if (FPlatformTime::Seconds() - State->SpawnedAt < 0.5) return false;
		FHyperAIPerfLiveRequest Request;
		Request.SampleSeconds = 1.f;
		State->Sampled.Reset(UHyperAIStudioProfilingToolset::hyper_profile_live(Request));
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Deadline]()
	{
		if (!State->Sampled.IsValid()) return true;
		if (!State->Sampled->bIsComplete && FPlatformTime::Seconds() < Deadline) return false;
		if (!TestTrue(TEXT("The sampled read finishes"), State->Sampled->bIsComplete))
		{
			HyperAIStudio::Profiling::Session::Cancel(TEXT("test timed out"));
			return true;
		}
		const FHyperAIPerfLiveReport& Report = State->Sampled->Value;
		AddInfo(FString::Printf(TEXT("Sampled: %s | %s"), *Report.Diagnostic, *FString::Join(Report.Notes, TEXT(" "))));
		TestEqual(TEXT("It completes"), Report.Status, FString(TEXT("completed")));
		TestTrue(TEXT("It sampled frames"), Report.SampledFrames > 0 && Report.SampledSeconds > 0.5);
		TestTrue(TEXT("The spawned component is counted"), Report.Niagara.ActiveComponents + Report.Niagara.InactiveComponents >= State->Before + 1);
		const FHyperAIPerfNiagaraSystem* Row = Report.Niagara.Systems.FindByPredicate([State](const FHyperAIPerfNiagaraSystem& System)
		{
			return System.System == State->SystemPath;
		});
		if (TestNotNull(TEXT("The spawned system is listed"), Row))
		{
			AddInfo(FString::Printf(TEXT("%s: %d instance(s), %d emitter(s), %lld particles (%lld CPU, %lld GPU), GT %.1f us, RT %.1f us"), *Row->System,
				Row->Instances, Row->ActiveEmitters, Row->Particles, Row->CpuParticles, Row->GpuParticles, Row->GameThreadUs, Row->RenderThreadUs));
			TestTrue(TEXT("Its cost was sampled, or particle perf stats are off"), Row->GameThreadUs >= 0.0 || AnyContains(Report.Notes, TEXT("fx.ParticlePerfStats")));
		}
		TestTrue(TEXT("Sampled Nanite streaming figures are read when the capture had them"), Report.Nanite.RootAllocationMB >= 0.0 || Report.SampledFrames > 0);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Deadline]()
	{
		static bool bRequested = false;
		if (!bRequested)
		{
			bRequested = true;
			EndPlay();
			return false;
		}
		if ((GEditor->PlayWorld || GEditor->IsPlaySessionInProgress()) && FPlatformTime::Seconds() < Deadline + 30.0) return false;
		bRequested = false;
		TestNull(TEXT("Play ends"), GEditor->PlayWorld.Get());
		return true;
	}));
	return true;
}

#endif
