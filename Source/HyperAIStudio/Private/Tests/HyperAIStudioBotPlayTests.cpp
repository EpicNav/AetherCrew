// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioVerify.h"
#include "HyperAIStudioWidgetShot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioPlaytestPanel.h"
#include "Tests/HyperAIStudioBotFixtures.h"

namespace HyperAIStudio::BotPlayTests
{
	using namespace HyperAIStudio::Verify;
	using HyperAIStudio::BotFixtures::SetUpCase;

	/** Batches write this machine's case limit; the test starts from the default and leaves the file as it found it. */
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

	FHyperAIBotStep Make(const EHyperAIBotAction Action)
	{
		FHyperAIBotStep Step;
		Step.Action = Action;
		return Step;
	}

	FHyperAIBotStep Play(const EHyperAIBotGoal Goal, const TCHAR* Target, const float Limit, const float Skill = 0.5f, const int32 Player = 0)
	{
		FHyperAIBotStep Step = Make(EHyperAIBotAction::Play);
		Step.Goal = Goal;
		Step.Target = Target;
		Step.Skill = Skill;
		Step.PlayerIndex = Player;
		Step.Seed = 7;
		const bool bTimed = Goal == EHyperAIBotGoal::Reach || Goal == EHyperAIBotGoal::Collect || Goal == EHyperAIBotGoal::Defeat;
		(bTimed ? Step.TimeoutSeconds : Step.Seconds) = Limit;
		return Step;
	}

	FHyperAIBotStep Wait(const float Seconds)
	{
		FHyperAIBotStep Step = Make(EHyperAIBotAction::Wait);
		Step.Seconds = Seconds;
		return Step;
	}

	FHyperAIBotStep Capture(const TCHAR* Name)
	{
		FHyperAIBotStep Step = Make(EHyperAIBotAction::Capture);
		Step.Text = Name;
		return Step;
	}

	FCase Case(const TCHAR* Name, TArray<FHyperAIBotStep> Steps, const float TimeoutSeconds)
	{
		FCase Out;
		Out.Name = Name;
		Out.Steps = MoveTemp(Steps);
		Out.TimeoutSeconds = TimeoutSeconds;
		return Out;
	}

	/** Plays a batch to the end, doing the editor loop's part as the verification tests do, and logs every step's evidence. */
	TOptional<FBatchOutcome> RunBatch(FAutomationTestBase& Test, TArray<FCase> Cases)
	{
		TOptional<FBatchOutcome> Result;
		FString Error;
		const bool bStarted = Start(MoveTemp(Cases), 60.f, [&Result](const FBatchOutcome& Outcome) { Result = Outcome; }, Error);
		if (!Test.TestTrue(TEXT("The batch starts: ") + Error, bStarted))
		{
			return Result;
		}
		const double Until = FPlatformTime::Seconds() + 120.0;
		while (!Result.IsSet() && FPlatformTime::Seconds() < Until)
		{
			++GFrameCounter;
			GEngine->CleanupGameViewport();
			FTSTicker::GetCoreTicker().Tick(FrameSeconds);
		}
		if (!Test.TestTrue(TEXT("and finishes"), Result.IsSet()))
		{
			return Result;
		}
		Test.AddInfo(FString::Printf(TEXT("Batch %s in %.1f s"), *Result->Status, Result->WallSeconds));
		for (const FCaseOutcome& Outcome : Result->Cases)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: %s after %.1f simulated s. %s"), *Outcome.Name, *Outcome.Status, Outcome.SimulatedSeconds, *Outcome.Diagnostic));
			for (const FStepOutcome& Step : Outcome.Steps)
			{
				Test.AddInfo(FString::Printf(TEXT("  %s %s (%s) at %.1f s: %s"), *Step.Action, *Step.Status, *Step.Code, Step.AtSeconds, *Step.Detail));
				if (!Step.File.IsEmpty())
				{
					Test.AddInfo(FString::Printf(TEXT("  Picture %s %s: %s"), *Outcome.Name, *Step.Action, *Step.File));
				}
			}
		}
		return Result;
	}

	const FCaseOutcome* Find(const FBatchOutcome& Outcome, const TCHAR* Name)
	{
		return Outcome.Cases.FindByPredicate([Name](const FCaseOutcome& Case) { return Case.Name == Name; });
	}

	/** The case's last step of the action, or null. */
	const FStepOutcome* Last(const FBatchOutcome& Outcome, const TCHAR* Name, const TCHAR* Action)
	{
		const FCaseOutcome* Case = Find(Outcome, Name);
		for (int32 Index = Case ? Case->Steps.Num() - 1 : -1; Index >= 0; --Index)
		{
			if (Case->Steps[Index].Action == Action)
			{
				return &Case->Steps[Index];
			}
		}
		return nullptr;
	}

	bool Passed(const FBatchOutcome& Outcome, const TCHAR* Name)
	{
		const FCaseOutcome* Case = Find(Outcome, Name);
		return Case && Case->Status == TEXT("passed");
	}

	/** The number after Label in Text, such as 15 in "took 15 damage"; -1 when it is not there. */
	double NumberAfter(const FString& Text, const TCHAR* Label)
	{
		const int32 At = Text.Find(Label);
		if (At == INDEX_NONE)
		{
			return -1.0;
		}
		const FString Rest = Text.Mid(At + FCString::Strlen(Label)).TrimStart();
		int32 End = 0;
		while (End < Rest.Len() && (FChar::IsDigit(Rest[End]) || Rest[End] == TEXT('.')))
		{
			++End;
		}
		return End > 0 ? FCString::Atod(*Rest.Left(End)) : -1.0;
	}

	bool HasFile(const FStepOutcome* Step)
	{
		return Step && !Step->File.IsEmpty() && IFileManager::Get().FileSize(*Step->File) > 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBotPlayTest,
	"HyperAIStudio.Bot.Play",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBotPlayTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BotPlayTests;
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || GEditor->PlayWorld || IsRunning())
	{
		AddError(TEXT("Needs the editor with a level open and no play session."));
		return false;
	}
	const APlayerStart* PlayerStart = nullptr;
	for (TActorIterator<APlayerStart> It(EditorWorld); It && !PlayerStart; ++It)
	{
		PlayerStart = *It;
	}
	if (!PlayerStart)
	{
		AddError(TEXT("The level needs a player start."));
		return false;
	}
	AddInfo(TEXT("Level: ") + EditorWorld->GetOutermost()->GetName());
	const FKeepCalibration KeepCalibration;
	SetCaseSetupForTests(&SetUpCase);
	ON_SCOPE_EXIT
	{
		SetCaseSetupForTests(nullptr);
	};

	// Moving like a player: round a wall, over a block too wide to go round, three coins, and a look round the level.
	FHyperAIBotStep Survey = Make(EHyperAIBotAction::Survey);
	const TOptional<FBatchOutcome> Moves = RunBatch(*this, {
		Case(TEXT("reach-around-wall"), {Wait(0.5f), Capture(TEXT("wall-ahead")), Play(EHyperAIBotGoal::Reach, TEXT("Goal"), 25.f)}, 30.f),
		Case(TEXT("reach-over-block"), {Play(EHyperAIBotGoal::Reach, TEXT("Goal"), 25.f)}, 30.f),
		Case(TEXT("collect-coins"), {Play(EHyperAIBotGoal::Collect, TEXT("Coin"), 30.f)}, 35.f),
		Case(TEXT("explore"), {Wait(0.3f), Survey, Play(EHyperAIBotGoal::Explore, TEXT(""), 20.f)}, 25.f)});
	if (Moves.IsSet())
	{
		const FStepOutcome* Around = Last(*Moves, TEXT("reach-around-wall"), TEXT("Play"));
		TestTrue(TEXT("It walks round a wall to the goal behind it"), Passed(*Moves, TEXT("reach-around-wall")) && Around && Around->Detail.Contains(TEXT("went round")));
		const FStepOutcome* Over = Last(*Moves, TEXT("reach-over-block"), TEXT("Play"));
		TestTrue(TEXT("It jumps a block too wide to go round"), Passed(*Moves, TEXT("reach-over-block")) && Over && Over->Detail.Contains(TEXT("jumps")));
		const FStepOutcome* Coins = Last(*Moves, TEXT("collect-coins"), TEXT("Play"));
		TestTrue(TEXT("It picks up all three coins"), Passed(*Moves, TEXT("collect-coins")) && Coins && Coins->Detail.Contains(TEXT("collected 3 of 3")));
		const FStepOutcome* Looked = Last(*Moves, TEXT("explore"), TEXT("Play"));
		TestTrue(TEXT("It explores new ground"), Passed(*Moves, TEXT("explore")) && Looked && NumberAfter(Looked->Detail, TEXT("visited ")) >= 6.0);
		const FStepOutcome* Read = Last(*Moves, TEXT("explore"), TEXT("Survey"));
		TestTrue(TEXT("Survey reads the controls as a player would"), Read && Read->Detail.Contains(TEXT("IA_Move (move)"))
			&& Read->Detail.Contains(TEXT("IA_Jump (jump)")) && Read->Detail.Contains(TEXT("IA_Fire (attack)")));
		TestTrue(TEXT("and the player's health"), Read && Read->Detail.Contains(TEXT("health Health.Health = 100")));
		TestTrue(TEXT("Every play writes a map of it"), HasFile(Around) && HasFile(Over) && HasFile(Coins) && HasFile(Looked));
		TestTrue(TEXT("A capture in a copy renders the player's camera"), HasFile(Last(*Moves, TEXT("reach-around-wall"), TEXT("Capture"))));
	}

	// Fighting like a player: two targets, a turret dodged and one stood in front of, and a duel between two players.
	const FVector Spot = PlayerStart->GetActorLocation() + FRotator(0.0, PlayerStart->GetActorRotation().Yaw, 0.0).Vector() * 1500.0;
	FHyperAIBotStep Join = Make(EHyperAIBotAction::AddPlayer);
	Join.Location = Spot;
	FHyperAIBotStep Rival = Play(EHyperAIBotGoal::Defeat, TEXT("Player0"), 40.f, 0.25f, 1);
	Rival.bBackground = true;
	Rival.Seed = 3;
	FHyperAIBotStep Standing = Make(EHyperAIBotAction::AssertProperty);
	Standing.Target = TEXT("Player");
	Standing.Property = TEXT("Health.Health");
	Standing.Compare = TEXT("<");
	Standing.Value = TEXT("100");
	const TOptional<FBatchOutcome> Fights = RunBatch(*this, {
		Case(TEXT("defeat-targets"), {Wait(0.5f), Capture(TEXT("targets")), Play(EHyperAIBotGoal::Defeat, TEXT("Enemy"), 30.f, 0.6f)}, 35.f),
		Case(TEXT("survive-evading"), {Play(EHyperAIBotGoal::Survive, TEXT("Turret"), 12.f)}, 20.f),
		Case(TEXT("survive-standing"), {Wait(12.f), Standing}, 20.f),
		Case(TEXT("duel"), {Join, Wait(0.5f), Capture(TEXT("duel-start")), Rival, Play(EHyperAIBotGoal::Defeat, TEXT("Player1"), 40.f, 0.9f, 0), Capture(TEXT("duel-end"))}, 50.f)});
	if (Fights.IsSet())
	{
		const FStepOutcome* Targets = Last(*Fights, TEXT("defeat-targets"), TEXT("Play"));
		TestTrue(TEXT("It aims, fires and defeats both targets"), Passed(*Fights, TEXT("defeat-targets")) && Targets && Targets->Detail.Contains(TEXT("defeated 2 of 2")));
		const FStepOutcome* Evading = Last(*Fights, TEXT("survive-evading"), TEXT("Play"));
		const FStepOutcome* Still = Last(*Fights, TEXT("survive-standing"), TEXT("AssertProperty"));
		const double Dodged = Evading ? NumberAfter(Evading->Detail, TEXT("took ")) : -1.0;
		const double StoodHealth = Still ? NumberAfter(Still->Detail, TEXT(" is ")) : -1.0;
		const double Stood = StoodHealth >= 0.0 ? 100.0 - StoodHealth : 100.0;
		AddInfo(FString::Printf(TEXT("The turret took %.0f health from the player who kept moving and %.0f from the one who stood still."), Dodged, Stood));
		TestTrue(TEXT("It survives a turret by moving"), Passed(*Fights, TEXT("survive-evading")) && Dodged >= 0.0);
		TestTrue(TEXT("The turret hits a player who stands still"), Passed(*Fights, TEXT("survive-standing")));
		TestTrue(TEXT("and moving takes less damage than standing still"), Dodged >= 0.0 && Dodged < Stood);
		const FCaseOutcome* Duel = Find(*Fights, TEXT("duel"));
		const FStepOutcome* Fight = Last(*Fights, TEXT("duel"), TEXT("Play"));
		TestTrue(TEXT("A second player joins"), Duel && Duel->Steps.Num() > 0 && Duel->Steps[0].Status == TEXT("passed"));
		TestTrue(TEXT("Two players fight it out to a winner"), Fight && (Fight->Code == TEXT("passed") || Fight->Code == TEXT("defeated")));
		TestTrue(TEXT("and the report tells both sides"), Fight && Fight->Detail.Contains(TEXT("meanwhile")) && Fight->Detail.Contains(TEXT("Player1")));
		TestTrue(TEXT("The more skilled player wins"), Duel && Duel->Status == TEXT("passed"));
		TestTrue(TEXT("Fights write maps too"), HasFile(Targets) && HasFile(Evading) && HasFile(Fight));
	}

	// With another player, and away from danger: follow a second player who explores, and run from the turret.
	FHyperAIBotStep Wanders = Play(EHyperAIBotGoal::Explore, TEXT(""), 16.f, 0.5f, 1);
	Wanders.bBackground = true;
	FHyperAIBotStep Follow = Play(EHyperAIBotGoal::Follow, TEXT("Player1"), 14.f);
	Follow.Radius = 500.f;
	FHyperAIBotStep Flee = Play(EHyperAIBotGoal::Flee, TEXT("Turret"), 10.f);
	Flee.Range = 2500.f;
	const TOptional<FBatchOutcome> Along = RunBatch(*this, {
		Case(TEXT("follow-player"), {Join, Wait(0.5f), Wanders, Follow}, 25.f),
		Case(TEXT("flee-turret"), {Flee}, 20.f)});
	if (Along.IsSet())
	{
		const FStepOutcome* Followed = Last(*Along, TEXT("follow-player"), TEXT("Play"));
		TestTrue(TEXT("It keeps up with a player who wanders"), Passed(*Along, TEXT("follow-player")) && Followed && Followed->Detail.Contains(TEXT("kept within 500 cm")));
		const FStepOutcome* Fled = Last(*Along, TEXT("flee-turret"), TEXT("Play"));
		TestTrue(TEXT("It runs until it is out of the turret's reach"), Passed(*Along, TEXT("flee-turret")) && Fled && NumberAfter(Fled->Detail, TEXT(" is ")) >= 2500.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensPlaytestPanelTest,
	"HyperAIStudio.Screens.PlaytestPanel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioScreensPlaytestPanelTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BotPlayTests;
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld || GEditor->PlayWorld || IsRunning())
	{
		AddError(TEXT("Needs the editor with a level open and no play session."));
		return false;
	}
	const APlayerStart* PlayerStart = nullptr;
	for (TActorIterator<APlayerStart> It(EditorWorld); It && !PlayerStart; ++It)
	{
		PlayerStart = *It;
	}
	if (!PlayerStart)
	{
		AddError(TEXT("The level needs a player start."));
		return false;
	}
	const FKeepCalibration KeepCalibration;
	SetCaseSetupForTests(&SetUpCase);
	ON_SCOPE_EXIT
	{
		SetCaseSetupForTests(nullptr);
	};

	const TSharedRef<SHyperAIStudioPlaytestPanel> Panel = SNew(SHyperAIStudioPlaytestPanel);
	TestTrue(TEXT("It opens with a Survey and an Explore case to start from"),
		Panel->GetCases().Contains(TEXT("\"Survey\"")) && Panel->GetCases().Contains(TEXT("\"Explore\"")));
	Panel->AddTemplate(TEXT("Duel"));
	TestTrue(TEXT("A template adds its case"), Panel->GetCases().Contains(TEXT("\"AddPlayer\"")) && Panel->GetCases().Contains(TEXT("\"duel\"")));
	Panel->AddTemplate(TEXT("Duel"));
	TestTrue(TEXT("with a name of its own"), Panel->GetCases().Contains(TEXT("\"duel-2\"")));

	// Three cases of the play test's game: two targets, three coins, and a duel.
	const FVector Spot = PlayerStart->GetActorLocation() + FRotator(0.0, PlayerStart->GetActorRotation().Yaw, 0.0).Vector() * 1500.0;
	Panel->SetCases(FString::Printf(TEXT(R"({"deadlineSeconds": 60, "cases": [
  {"name": "defeat-targets", "timeoutSeconds": 35, "steps": [{"action": "Wait", "seconds": 0.5}, {"action": "Capture", "text": "targets"},
    {"action": "Play", "goal": "Defeat", "target": "Enemy", "skill": 0.6, "timeoutSeconds": 30}]},
  {"name": "collect-coins", "timeoutSeconds": 35, "steps": [{"action": "Play", "goal": "Collect", "target": "Coin", "timeoutSeconds": 30}]},
  {"name": "duel", "timeoutSeconds": 50, "steps": [{"action": "AddPlayer", "location": {"x": %.0f, "y": %.0f, "z": %.0f}}, {"action": "Wait", "seconds": 0.5},
    {"action": "Play", "goal": "Defeat", "target": "Player0", "playerIndex": 1, "skill": 0.3, "bBackground": true, "timeoutSeconds": 40},
    {"action": "Play", "goal": "Defeat", "target": "Player1", "skill": 0.9, "timeoutSeconds": 40}, {"action": "Capture", "text": "duel-over"}]}]})"),
		Spot.X, Spot.Y, Spot.Z));
	FString Error;
	const bool bStarted = Panel->Run(Error);
	if (!TestTrue(TEXT("Play in copies starts the batch: ") + Error, bStarted))
	{
		return false;
	}
	const double Until = FPlatformTime::Seconds() + 120.0;
	while (Panel->IsRunning() && FPlatformTime::Seconds() < Until)
	{
		++GFrameCounter;
		GEngine->CleanupGameViewport();
		FTSTicker::GetCoreTicker().Tick(FrameSeconds);
	}
	const TOptional<FBatchOutcome>& Outcome = Panel->GetOutcome();
	if (!TestTrue(TEXT("and its results come back"), Outcome.IsSet()))
	{
		return false;
	}
	int32 Passed = 0;
	int32 Pictures = 0;
	for (const FCaseOutcome& Case : Outcome->Cases)
	{
		Passed += Case.Status == TEXT("passed");
		for (const FStepOutcome& Step : Case.Steps)
		{
			Pictures += !Step.File.IsEmpty();
		}
		AddInfo(FString::Printf(TEXT("%s: %s. %s"), *Case.Name, *Case.Status, *Case.Diagnostic));
	}
	TestEqual(TEXT("All three cases pass"), Passed, 3);
	TestTrue(FString::Printf(TEXT("and the results carry their maps and pictures (%d)"), Pictures), Pictures >= 5);
	const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(Panel, FIntPoint(1440, 1180), TEXT("step13-playtest-panel"));
	if (!Shot.IsEmpty())
	{
		AddInfo(TEXT("Screenshot: ") + Shot);
	}
	return true;
}

#endif
