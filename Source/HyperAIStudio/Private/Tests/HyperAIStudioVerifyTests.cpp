// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "HyperAIStudioVerify.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::Verify::Tests
{
	FHyperAIBotStep Make(const EHyperAIBotAction Action)
	{
		FHyperAIBotStep Step;
		Step.Action = Action;
		return Step;
	}

	FCase Case(const TCHAR* Name, TArray<FHyperAIBotStep> Steps, const float TimeoutSeconds = 20.f)
	{
		FCase Out;
		Out.Name = Name;
		Out.Steps = MoveTemp(Steps);
		Out.TimeoutSeconds = TimeoutSeconds;
		return Out;
	}

	/**
	 * The engine loop does not run inside a test, so the test plays the editor's part during a batch: a new frame number,
	 * which tick functions and timers need before they run again; the engine's viewport cleanup, which removes local
	 * players that have no screen; then the ticker the batch runs on. Like the editor during a batch, it does not give
	 * world-less tickables such as Enhanced Input a game tick, so input has to reach the copies without one.
	 */
	bool Pump(TFunctionRef<bool()> Done, const double MaxSeconds, const float SleepSeconds = 0.f)
	{
		const double Until = FPlatformTime::Seconds() + MaxSeconds;
		while (!Done() && FPlatformTime::Seconds() < Until)
		{
			++GFrameCounter;
			GEngine->CleanupGameViewport();
			FTSTicker::GetCoreTicker().Tick(FrameSeconds);
			if (SleepSeconds > 0.f)
			{
				FPlatformProcess::Sleep(SleepSeconds);
			}
		}
		return Done();
	}

	const FCaseOutcome* Find(const FBatchOutcome& Outcome, const TCHAR* Name)
	{
		return Outcome.Cases.FindByPredicate([Name](const FCaseOutcome& Case) { return Case.Name == Name; });
	}

	/** Batches and calibration write this machine's limit; a test leaves the file as it found it. */
	struct FKeepCalibration
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("VerifyCalibration.json"));
		FString Saved;
		bool bHadFile = FFileHelper::LoadFileToString(Saved, *Path);

		FKeepCalibration()
		{
			IFileManager::Get().Delete(*Path, false, true, true);
		}

		~FKeepCalibration()
		{
			if (bHadFile)
			{
				FFileHelper::SaveStringToFile(Saved, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			}
			else
			{
				IFileManager::Get().Delete(*Path, false, true, true);
			}
		}
	};

	bool EditorIsBack(UWorld* EditorWorld)
	{
		return !IsRunning() && GEditor->PlayWorld == nullptr && GWorld == EditorWorld && !GIsPlayInEditorWorld && GEditor->EditorWorld == nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioVerifyAdmissionTest,
	"HyperAIStudio.Verify.Admission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioVerifyAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Verify;
	using namespace HyperAIStudio::Verify::Tests;
	FHyperAIBotStep Wait = Make(EHyperAIBotAction::Wait);
	Wait.Seconds = 1.f;
	const FCase Good = Case(TEXT("good"), {Wait});
	FString Error;
	TestTrue(TEXT("A valid batch is admitted"), Admit({Good, Good}, 60.f, 4, Error));
	TestFalse(TEXT("An empty batch is refused"), Admit({}, 60.f, 4, Error));
	TestFalse(TEXT("A batch over the limit is refused"), Admit({Good, Good, Good}, 60.f, 2, Error));
	TestTrue(TEXT("and says how to go on"), Error.Contains(TEXT("hyper_verify_calibrate")));
	TestFalse(TEXT("A deadline over a minute is refused"), Admit({Good}, 61.f, 4, Error));
	TestFalse(TEXT("A deadline under 10 s is refused"), Admit({Good}, 5.f, 4, Error));
	TestFalse(TEXT("A case timeout that does not fit the deadline is refused"), Admit({Case(TEXT("long"), {Wait}, 56.f)}, 60.f, 4, Error));
	TestFalse(TEXT("A case without steps is refused"), Admit({Case(TEXT("empty"), {})}, 60.f, 4, Error));
	TestFalse(TEXT("A capture needs a screen the copies do not have"), Admit({Case(TEXT("shot"), {Make(EHyperAIBotAction::Capture)})}, 60.f, 4, Error));
	TestTrue(TEXT("and the refusal points at the playtest tool"), Error.Contains(TEXT("hyper_playtest_run")));
	TestFalse(TEXT("An invalid step is refused"), Admit({Case(TEXT("bad"), {Make(EHyperAIBotAction::Wait)})}, 60.f, 4, Error));

	TestEqual(TEXT("The largest batch within the budget"), PickLimit({{1, 3.0}, {2, 5.0}, {4, 9.0}, {6, 55.0}, {8, 70.0}}, 50.0), 4);
	TestEqual(TEXT("Every size within budget allows the most"), PickLimit({{8, 10.0}, {1, 3.0}, {10, 12.0}, {4, 6.0}}, 50.0), 10);
	TestEqual(TEXT("Even a slow single case leaves one"), PickLimit({{1, 60.0}}, 50.0), 1);
	TestEqual(TEXT("A size past the first over budget does not count"), PickLimit({{1, 3.0}, {2, 60.0}, {4, 9.0}}, 50.0), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioVerifyBatchTest,
	"HyperAIStudio.Verify.Batch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioVerifyBatchTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Verify;
	using namespace HyperAIStudio::Verify::Tests;
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || GEditor->PlayWorld || IsRunning())
	{
		AddError(TEXT("Needs the editor with a level open and no play session."));
		return false;
	}
	AddInfo(TEXT("Level: ") + EditorWorld->GetOutermost()->GetName());
	const FKeepCalibration KeepCalibration;

	// Three cases at once: a jump and a walk through the game's own move input pass; a missing actor fails.
	FHyperAIBotStep Walk = Make(EHyperAIBotAction::Hold);
	Walk.InputAction = TEXT("IA_Move");
	Walk.Value = TEXT("0,1");
	Walk.Seconds = 1.f;
	FHyperAIBotStep Moving = Make(EHyperAIBotAction::AssertProperty);
	Moving.Target = TEXT("Player");
	Moving.Property = TEXT("Speed");
	Moving.Compare = TEXT(">");
	Moving.Value = TEXT("100");
	FHyperAIBotStep Missing = Make(EHyperAIBotAction::AssertExists);
	Missing.Target = TEXT("NoSuchActor_HyperAIVerifyTest");
	TOptional<FBatchOutcome> Result;
	FString Error;
	const double Began = FPlatformTime::Seconds();
	TestTrue(TEXT("A batch starts"), Start({
		Case(TEXT("jump"), {Make(EHyperAIBotAction::Jump)}),
		Case(TEXT("walk"), {Walk, Moving}),
		Case(TEXT("missing"), {Missing})}, 60.f, [&Result](const FBatchOutcome& Outcome) { Result = Outcome; }, Error));
	TestFalse(TEXT("A second batch waits for the first"), Start({Case(TEXT("second"), {Make(EHyperAIBotAction::Jump)})}, 60.f, nullptr, Error));
	TestTrue(TEXT("The batch finishes"), Pump([&Result]() { return Result.IsSet(); }, 60.0));
	if (Result.IsSet())
	{
		AddInfo(FString::Printf(TEXT("Batch %s in %.2f s wall (%.2f s including the test's own ticking)"), *Result->Status, Result->WallSeconds, FPlatformTime::Seconds() - Began));
		for (const FCaseOutcome& Outcome : Result->Cases)
		{
			AddInfo(FString::Printf(TEXT("  %s: %s after %.1f simulated s. %s"), *Outcome.Name, *Outcome.Status, Outcome.SimulatedSeconds, *Outcome.Diagnostic));
		}
		TestEqual(TEXT("The batch completed"), Result->Status, FString(TEXT("completed")));
		const FCaseOutcome* Jump = Find(*Result, TEXT("jump"));
		const FCaseOutcome* Walked = Find(*Result, TEXT("walk"));
		const FCaseOutcome* Gone = Find(*Result, TEXT("missing"));
		TestTrue(TEXT("The player jumped and landed in its own copy"), Jump && Jump->Status == TEXT("passed"));
		TestTrue(TEXT("The player walked through the game's move input"), Walked && Walked->Status == TEXT("passed") && Walked->Steps.Num() == 2);
		TestTrue(TEXT("A missing actor fails its case with the reason"), Gone && Gone->Status == TEXT("failed") && Gone->FailedStep == 0
			&& Gone->Steps.Num() == 1 && Gone->Steps[0].Code == TEXT("not_found"));
	}
	TestTrue(TEXT("The editor is as it was"), EditorIsBack(EditorWorld));

	// A case that never ends is stopped at the batch deadline, however long its own timeout.
	FHyperAIBotStep Forever = Make(EHyperAIBotAction::WaitUntil);
	Forever.Until = EHyperAIBotAction::AssertGone;
	Forever.Target = TEXT("Player");
	Forever.TimeoutSeconds = 60.f;
	Result.Reset();
	TestTrue(TEXT("A short batch starts"), Start({Case(TEXT("forever"), {Forever}, 1000.f)}, 1.f,
		[&Result](const FBatchOutcome& Outcome) { Result = Outcome; }, Error));
	TestTrue(TEXT("and ends"), Pump([&Result]() { return Result.IsSet(); }, 20.0, 0.005f));
	if (Result.IsSet())
	{
		TestEqual(TEXT("at its deadline"), Result->Status, FString(TEXT("deadline")));
		TestTrue(TEXT("within a moment of it"), Result->WallSeconds < 3.0);
		TestTrue(TEXT("The running case reports timed out"), Result->Cases.Num() == 1 && Result->Cases[0].Status == TEXT("timed_out"));
		TestEqual(TEXT("A case that never ends at full speed does not lower the limit"), GetLimit(), DefaultLimit);
		AddInfo(FString::Printf(TEXT("Deadline batch: %s in %.2f s; %s"), *Result->Status, Result->WallSeconds,
			Result->Cases.IsEmpty() ? TEXT("") : *Result->Cases[0].Diagnostic));
	}
	TestTrue(TEXT("The editor is as it was again"), EditorIsBack(EditorWorld));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioVerifyToolsTest,
	"HyperAIStudio.Verify.Tools",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioVerifyToolsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Verify;
	using namespace HyperAIStudio::Verify::Tests;
	const FKeepCalibration KeepCalibration;

	FHyperAIVerifyRequest TooMany;
	FHyperAIVerifyCase One;
	One.Name = TEXT("wait");
	FHyperAIBotStep Wait = Make(EHyperAIBotAction::Wait);
	Wait.Seconds = 0.5f;
	One.Steps.Add(Wait);
	for (int32 Index = 0; Index <= DefaultLimit; ++Index)
	{
		TooMany.Cases.Add(One);
	}
	UHyperAIVerifyRunResult* Refused = UHyperAIStudioPIEPlaytestToolset::hyper_verify_run(TooMany);
	TestTrue(TEXT("Before calibration, a batch over the default limit is refused at once"),
		Refused->Value.Status == TEXT("refused") && Refused->Value.Limit == DefaultLimit);

	UHyperAIVerifyCalibrationResult* Known = UHyperAIStudioPIEPlaytestToolset::hyper_verify_calibrate(false);
	TestEqual(TEXT("Uncalibrated, the limit is reported as such"), Known->Value.Status, FString(TEXT("not_calibrated")));

	FHyperAIVerifyRequest Request;
	Request.Cases.Add(One);
	Request.DeadlineSeconds = 30.f;
	UHyperAIVerifyRunResult* Run = UHyperAIStudioPIEPlaytestToolset::hyper_verify_run(Request);
	TestTrue(TEXT("The tool answers once the batch has run"), Pump([Run]() { return Run->Value.bOk; }, 30.0));
	TestTrue(TEXT("with the case's result"), Run->Value.Status == TEXT("completed") && Run->Value.Passed == 1 && Run->Value.Cases.Num() == 1);

	// Calibration measures real batches; two sizes keep the test short.
	TOptional<FCalibration> Measured;
	FString Error;
	TestTrue(TEXT("Calibration starts"), Calibrate(2, [&Measured](const FCalibration& Calibration) { Measured = Calibration; }, Error));
	TestTrue(TEXT("and finishes"), Pump([&Measured]() { return Measured.IsSet(); }, 60.0));
	if (Measured.IsSet())
	{
		AddInfo(FString::Printf(TEXT("Calibration: %s, limit %d. %s"), *Measured->Status, Measured->Limit, *Measured->Diagnostic));
		TestEqual(TEXT("It measured"), Measured->Status, FString(TEXT("measured")));
		TestTrue(TEXT("both sizes"), Measured->SecondsByCount.Contains(1) && Measured->SecondsByCount.Contains(2));
		TestEqual(TEXT("and both finished well within the budget on this small level"), Measured->Limit, 2);
		TestEqual(TEXT("The level's limit is kept"), GetLimit(), 2);
		TestEqual(TEXT("and reported"), GetCalibration().Status, FString(TEXT("calibrated")));
	}
	return true;
}

#endif
