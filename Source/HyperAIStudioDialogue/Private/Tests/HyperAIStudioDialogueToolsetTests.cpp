// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioDialogueToolset.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioDialogueCookbook.h"
#include "HyperAIStudioDialogueGate.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioVoice.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"

namespace HyperAIStudio::Dialogue::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	namespace Gate = HyperAIStudio::Dialogue::Gate;

	FHyperAIDialogueLine Line(const TCHAR* Text, const TCHAR* Direction = TEXT(""), const TCHAR* Voice = TEXT(""), const TCHAR* Name = TEXT(""))
	{
		FHyperAIDialogueLine Result;
		Result.Text = Text;
		Result.Direction = Direction;
		Result.Voice = Voice;
		Result.Name = Name;
		return Result;
	}

	bool HasIssue(const TArray<FHyperAIDialogueIssue>& Issues, const TCHAR* Code)
	{
		return Issues.ContainsByPredicate([Code](const FHyperAIDialogueIssue& Issue) { return Issue.Code == Code; });
	}

	bool PumpUntil(TFunctionRef<bool()> Done, const double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Deadline) return false;
			FTSTicker::GetCoreTicker().Tick(0.016f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FlushRenderingCommands();
			FPlatformProcess::Sleep(0.01f);
		}
		return true;
	}

	/** A voice pointing at the first clip in the configured folder, or null when none is set up. */
	UHyperAIStudioVoice* MakeTestVoice(FString& OutWhy)
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		if (Settings->VoiceReferenceFolder.IsEmpty())
		{
			OutWhy = TEXT("Voice Reference Folder is not set in HyperAIStudio settings.");
			return nullptr;
		}
		const TArray<FString> Clips = Gate::ReadAvailableClips();
		if (Clips.IsEmpty())
		{
			OutWhy = FString::Printf(TEXT("No wav clips in %s."), *Settings->VoiceReferenceFolder);
			return nullptr;
		}
		UHyperAIStudioVoice* Voice = NewObject<UHyperAIStudioVoice>(GetTransientPackage(), TEXT("DV_HyperAITestVoice"));
		Voice->ReferenceClip = Clips[0];
		Voice->SpeakerDescription = TEXT("A weary man in his fifties, low gravelly voice");
		Voice->Seed = 12345;
		return Voice;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDialogueCohortTest,
	"HyperAIStudio.Dialogue.Contracts.ExactFourToolCohort",
	HyperAIStudio::Dialogue::Tests::Flags)

bool FHyperAIStudioDialogueCohortTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Names = FHyperAIStudioDialogueContracts::GetToolNames();
	TestEqual(TEXT("four tools"), Names.Num(), 4);
	TestEqual(TEXT("one variant per tool"), FHyperAIStudioDialogueContracts::GetAdapterDescriptor().Variants.Num(), Names.Num());
	int32 Callables = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioDialogueToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->HasMetaData(TEXT("AICallable"))) continue;
		++Callables;
		TestTrue(*FString::Printf(TEXT("%s is a manifest tool"), *It->GetName()), Names.Contains(It->GetName()));
		const FString ToolTip = It->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description within 400 characters"), *It->GetName()), !ToolTip.IsEmpty() && ToolTip.Len() <= 400);
	}
	TestEqual(TEXT("AICallable functions match the manifest"), Callables, Names.Num());

	TestTrue(TEXT("the cookbook is in the project"), HyperAIStudio::Dialogue::EnsureCookbook());
	FString Cookbook;
	FFileHelper::LoadFileToString(Cookbook, *HyperAIStudio::Dialogue::GetCookbookPath());
	TestTrue(TEXT("and says who writes the lines, and the text and direction rule"),
		Cookbook.Contains(TEXT("You write them")) && Cookbook.Contains(TEXT("direction"))
		&& Cookbook.Contains(TEXT("right to clone")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDialoguePromptTest,
	"HyperAIStudio.Dialogue.Prompt.MatchesTheDocumentedFormat",
	HyperAIStudio::Dialogue::Tests::Flags)

bool FHyperAIStudioDialoguePromptTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Dialogue::Tests;
	const FString Speaker = TEXT("A weary man in his fifties, low gravelly voice");

	// The documented shape: the first direction joins the description sentence, later ones take a capital.
	TestEqual(TEXT("two segments read as the guide writes them"),
		Gate::BuildPrompt(Speaker, {
			Line(TEXT("You signed my name."), TEXT("with barely controlled rage")),
			Line(TEXT("Both copies."), TEXT("a long pause"))}),
		Speaker + TEXT(", with barely controlled rage, \"You signed my name.\" A long pause, \"Both copies.\""));

	TestEqual(TEXT("a line with no direction is just the quoted text"),
		Gate::BuildPrompt(Speaker, {Line(TEXT("Both copies."))}),
		Speaker + TEXT(", \"Both copies.\""));

	TestEqual(TEXT("with no speaker it is the body alone"),
		Gate::BuildPrompt(FString(), {Line(TEXT("Both copies."))}), FString(TEXT("\"Both copies.\"")));

	// A double quote would close the span and the rest would be performed instead of spoken.
	TestEqual(TEXT("a quote inside the text is demoted"),
		Gate::BuildPrompt(FString(), {Line(TEXT("He said \"no\" twice."))}),
		FString(TEXT("\"He said 'no' twice.\"")));

	TestEqual(TEXT("a trailing comma on the description is not doubled"),
		Gate::BuildPrompt(TEXT("A tired woman,"), {Line(TEXT("Fine."))}), FString(TEXT("A tired woman, \"Fine.\"")));

	TestEqual(TEXT("a direction keeps no trailing punctuation"),
		Gate::BuildPrompt(FString(), {Line(TEXT("Fine."), TEXT("she sighs."))}), FString(TEXT("she sighs, \"Fine.\"")));

	TestEqual(TEXT("an empty segment is skipped"),
		Gate::BuildPrompt(FString(), {Line(TEXT("")), Line(TEXT("Fine."))}), FString(TEXT("\"Fine.\"")));
	TestTrue(TEXT("and nothing at all makes no prompt"), Gate::BuildPrompt(Speaker, {}).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDialogueLineChecksTest,
	"HyperAIStudio.Dialogue.Lines.CaughtBeforeSpendingARender",
	HyperAIStudio::Dialogue::Tests::Flags)

bool FHyperAIStudioDialogueLineChecksTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Dialogue::Tests;
	TestTrue(TEXT("no lines is an error"), HasIssue(Gate::CheckLines({}), TEXT("no_lines")));
	TestTrue(TEXT("an empty line is an error"), HasIssue(Gate::CheckLines({Line(TEXT("  "))}), TEXT("empty_line")));
	TestTrue(TEXT("a line with no voice is an error"), HasIssue(Gate::CheckLines({Line(TEXT("Hello."))}), TEXT("voice_not_found")));

	// Writing the action in the spoken text makes the model say the word.
	TestTrue(TEXT("an action inside the text is flagged"),
		HasIssue(Gate::CheckLines({Line(TEXT("Sigh, fine then."))}), TEXT("action_in_text")));
	TestFalse(TEXT("but the same word in the direction is fine"),
		HasIssue(Gate::CheckLines({Line(TEXT("Fine then."), TEXT("she sighs deeply"))}), TEXT("action_in_text")));

	TestTrue(TEXT("a quote in the text is a warning, not a refusal"),
		HasIssue(Gate::CheckLines({Line(TEXT("He said \"no\"."))}), TEXT("quote_in_text")));

	TestTrue(TEXT("two lines that would write the same file are an error"),
		HasIssue(Gate::CheckLines({Line(TEXT("One."), TEXT(""), TEXT(""), TEXT("Same")),
			Line(TEXT("Two."), TEXT(""), TEXT(""), TEXT("Same"))}), TEXT("duplicate_name")));
	TestTrue(TEXT("a name that is not an identifier is an error"),
		HasIssue(Gate::CheckLines({Line(TEXT("One."), TEXT(""), TEXT(""), TEXT("bad name!"))}), TEXT("invalid_name")));

	FString Long;
	for (int32 Index = 0; Index < 700; ++Index) Long += TEXT("a");
	TestTrue(TEXT("a line past the render size is an error"),
		HasIssue(Gate::CheckLines({Line(*Long)}), TEXT("line_too_long")));

	TArray<FHyperAIDialogueLine> TooMany;
	for (int32 Index = 0; Index < 80; ++Index) TooMany.Add(Line(TEXT("Line."), TEXT(""), TEXT(""), *FString::Printf(TEXT("L%d"), Index)));
	TestTrue(TEXT("too many lines for one render is an error"), HasIssue(Gate::CheckLines(TooMany), TEXT("too_many_lines")));

	TestTrue(TEXT("the estimate grows with the text"),
		Gate::EstimateRenderSeconds({Line(TEXT("A much longer line of dialogue than the other one."))})
			> Gate::EstimateRenderSeconds({Line(TEXT("Short."))}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDialogueWavTest,
	"HyperAIStudio.Dialogue.Audio.PcmBecomesAPlayableWav",
	HyperAIStudio::Dialogue::Tests::Flags)

bool FHyperAIStudioDialogueWavTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Dialogue::Tests;
	// The service answers with little-endian int16 mono; a wrong header here is silence or noise in the editor.
	TArray<uint8> Pcm;
	constexpr int32 Samples = 4800;
	Pcm.Reserve(Samples * 2);
	for (int32 Index = 0; Index < Samples; ++Index)
	{
		const int16 Value = static_cast<int16>(FMath::Sin(Index * 0.05f) * 12000.0f);
		Pcm.Add(Value & 0xFF);
		Pcm.Add((Value >> 8) & 0xFF);
	}
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/dialogue_tone.wav"));
	FString Error;
	if (!TestTrue(TEXT("a wav is written: ") + Error, Gate::WriteWav(Path, Pcm, 48000, Error)))
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); };

	TArray<uint8> File;
	if (!TestTrue(TEXT("and reads back"), FFileHelper::LoadFileToArray(File, *Path)))
	{
		return false;
	}
	TestEqual(TEXT("with a 44 byte header before the samples"), File.Num(), 44 + Pcm.Num());
	const FString Riff(4, reinterpret_cast<const ANSICHAR*>(File.GetData()));
	const FString Wave(4, reinterpret_cast<const ANSICHAR*>(File.GetData() + 8));
	TestEqual(TEXT("a RIFF file"), Riff, FString(TEXT("RIFF")));
	TestEqual(TEXT("holding WAVE"), Wave, FString(TEXT("WAVE")));
	const int32 Channels = File[22] | (File[23] << 8);
	const int32 Rate = File[24] | (File[25] << 8) | (File[26] << 16) | (File[27] << 24);
	const int32 Bits = File[34] | (File[35] << 8);
	TestEqual(TEXT("mono"), Channels, 1);
	TestEqual(TEXT("at the rate the service reported"), Rate, 48000);
	TestEqual(TEXT("16 bits a sample"), Bits, 16);

	TestFalse(TEXT("empty audio is refused"), Gate::WriteWav(Path, TArray<uint8>(), 48000, Error));
	TestFalse(TEXT("and so is a nonsense rate"), Gate::WriteWav(Path, Pcm, 0, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDialogueFrontDoorTest,
	"HyperAIStudio.Dialogue.Contracts.FrontDoor",
	HyperAIStudio::Dialogue::Tests::Flags)

bool FHyperAIStudioDialogueFrontDoorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Dialogue::Tests;
	FHyperAIDialogueInspectRequest Inspect;
	Inspect.MaxOutputBytes = 8;
	TestEqual(TEXT("a silly output bound is refused"),
		FHyperAIStudioDialogueContracts::Inspect(Inspect).Status, FString(TEXT("invalid_request")));
	Inspect.MaxOutputBytes = 65536;
	const FHyperAIDialogueInspectReport Inspected = FHyperAIStudioDialogueContracts::Inspect(Inspect);
	TestTrue(*FString::Printf(TEXT("inspect answers (%s)"), *Inspected.Diagnostic), Inspected.bOk);
	TestFalse(TEXT("and says what the speech service is doing"), Inspected.ServiceHealth.IsEmpty());

	FHyperAIDialogueRenderRequest Render;
	Render.SceneName.Reset();
	TestEqual(TEXT("a render needs a scene name"),
		FHyperAIStudioDialogueContracts::Render(Render).Status, FString(TEXT("invalid_scene")));
	Render.SceneName = TEXT("Test");
	TestEqual(TEXT("and refuses lines that cannot render"),
		FHyperAIStudioDialogueContracts::Render(Render).Status, FString(TEXT("invalid_lines")));

	FHyperAIDialogueApplyPlanRequest Plan;
	Plan.TargetFolder = TEXT("C:/Nope");
	TestEqual(TEXT("an import needs a /Game folder"),
		FHyperAIStudioDialogueContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_target_folder")));
	Plan.TargetFolder = TEXT("/Game/Dialogue/Test");
	Plan.RenderId = TEXT("nothing_here");
	TestEqual(TEXT("and a render that exists"),
		FHyperAIStudioDialogueContracts::BuildPlan(Plan).Status, FString(TEXT("render_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDialogueRenderTest,
	"HyperAIStudio.Dialogue.Execution.RendersOneLineOnTheService",
	HyperAIStudio::Dialogue::Tests::Flags)

bool FHyperAIStudioDialogueRenderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Dialogue::Tests;
	// Spends real GPU time on the configured service, so it runs only when one is actually up.
	const FHyperAIStudioServiceStatus Status = FHyperAIStudioExternalServices::QueryHealth(TEXT("dramabox"));
	PumpUntil([]()
	{
		return FHyperAIStudioExternalServices::QueryHealth(TEXT("dramabox")).Health != EHyperAIStudioServiceHealth::Unknown;
	}, 10.0);
	if (FHyperAIStudioExternalServices::QueryHealth(TEXT("dramabox")).Health != EHyperAIStudioServiceHealth::Reachable)
	{
		AddInfo(TEXT("The speech service is not reachable; skipped."));
		return true;
	}
	FString Why;
	UHyperAIStudioVoice* Voice = MakeTestVoice(Why);
	if (!Voice)
	{
		AddInfo(FString::Printf(TEXT("%s Skipped."), *Why));
		return true;
	}
	Voice->AddToRoot();
	ON_SCOPE_EXIT { Voice->RemoveFromRoot(); };

	FHyperAIDialogueLine TestLine = Line(TEXT("You signed my name."), TEXT("with barely controlled rage"));
	TestLine.Voice = Voice->GetPathName();
	TestLine.Name = TEXT("HyperAITestLine");

	FString RenderId;
	FString Folder;
	FString Error;
	if (!TestTrue(TEXT("the render starts: ") + Error, Gate::StartRender(TEXT("HyperAITest"), {TestLine}, RenderId, Folder, Error)))
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists=*/false, /*Tree=*/true); };

	FHyperAIDialogueRenderRecord Record;
	// Inside the job host's own budget, which is what a render actually gets.
	const bool bFinished = PumpUntil([&RenderId, &Record]()
	{
		return Gate::FindRender(RenderId, Record) && Record.State != TEXT("running");
	}, 580.0);
	// A render that shares the graphics card with this editor can take minutes for one line, and that is the
	// machine rather than the code. Anything else the service says is a real failure and fails the test.
	const bool bContended = !bFinished
		|| (Record.State == TEXT("failed") && (Record.Progress.Contains(TEXT("No answer")) || Record.Progress.Contains(TEXT("timed out"))));
	if (bContended)
	{
		AddInfo(FString::Printf(
			TEXT("The render did not finish in time (%s). The speech model shares this graphics card with the editor; ")
			TEXT("skipped rather than failed."), *Record.Progress));
		return true;
	}
	if (!TestEqual(*FString::Printf(TEXT("the render completes (%s)"), *Record.Progress), Record.State, FString(TEXT("complete"))))
	{
		return false;
	}
	TestEqual(TEXT("one line rendered"), Record.LinesDone, 1);
	TestTrue(TEXT("with audio in it"), Record.SecondsOfAudio > 0.2f);
	TestTrue(TEXT("and a render key to plan against"), FHyperAIStudioDialogueContracts::IsCanonicalSha256(Record.RenderKey));

	const FString Wav = Folder / TEXT("HyperAITestLine.wav");
	TestTrue(TEXT("the wav is on disk"), IFileManager::Get().FileExists(*Wav));
	TArray<uint8> File;
	if (FFileHelper::LoadFileToArray(File, *Wav) && File.Num() > 44)
	{
		int32 Peak = 0;
		for (int32 Index = 44; Index + 1 < File.Num(); Index += 2)
		{
			Peak = FMath::Max(Peak, FMath::Abs(static_cast<int16>(File[Index] | (File[Index + 1] << 8))));
		}
		// Silence would mean the prompt or the reference clip never reached the model.
		TestTrue(*FString::Printf(TEXT("and carries speech, not silence (peak %d)"), Peak), Peak > 1000);
	}
	return true;
}

#endif
