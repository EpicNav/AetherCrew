// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioBot.h"
#include "HyperAIStudioNativeReadToolset.h"
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "Misc/AutomationTest.h"
#include "Tests/HyperAIStudioBotFixtures.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::BotTests
{
	/** A game world ticked by hand, with a floating player pawn at the origin and a tagged target 600 cm ahead. */
	struct FTestGame
	{
		UWorld* World = nullptr;
		ADefaultPawn* Pawn = nullptr;
		APlayerController* Controller = nullptr;
		ATargetPoint* Coin = nullptr;
		FString Output = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("BotTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));

		FTestGame()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			// Without a game mode nothing begins play, and actors that never begin play never tick.
			World->GetWorldSettings()->NotifyBeginPlay();
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Pawn = World->SpawnActor<ADefaultPawn>(FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, Params);
			Controller = World->SpawnActor<APlayerController>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
			if (Controller && Pawn)
			{
				Controller->Possess(Pawn);
			}
			Coin = World->SpawnActor<ATargetPoint>(FVector(600.0, 0.0, 100.0), FRotator::ZeroRotator, Params);
			if (Coin)
			{
				Coin->Tags.Add(TEXT("Coin"));
			}
		}

		~FTestGame()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
			IFileManager::Get().DeleteDirectory(*Output, /*RequireExists=*/false, /*Tree=*/true);
		}

		void Tick()
		{
			World->Tick(LEVELTICK_All, 1.f / 30.f);
		}

		/** Runs a step to the end, a frame at a time; Frames says how many it took. */
		FHyperAIBotResult Run(FHyperAIBotExecutor& Bot, const FHyperAIBotStep& Step, int32* Frames = nullptr, const TFunction<void(int32)>& EachFrame = nullptr)
		{
			FHyperAIBotResult Result = Bot.Begin(Step);
			int32 Frame = 0;
			for (; Frame < 900 && !Result.IsDone(); ++Frame)
			{
				if (EachFrame)
				{
					EachFrame(Frame);
				}
				Tick();
				Result = Bot.Tick();
			}
			if (Frames)
			{
				*Frames = Frame;
			}
			return Result;
		}
	};

	FHyperAIBotStep Make(const EHyperAIBotAction Action)
	{
		FHyperAIBotStep Step;
		Step.Action = Action;
		return Step;
	}

	bool Passed(const FHyperAIBotResult& Result)
	{
		return Result.State == FHyperAIBotResult::EState::Passed;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBotStepsTest,
	"HyperAIStudio.Bot.Steps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBotStepsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BotTests;
	FString Error;
	FHyperAIBotStep Invalid = Make(EHyperAIBotAction::Wait);
	TestFalse(TEXT("Wait needs seconds"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::Press);
	TestFalse(TEXT("Press needs an input action"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::WaitUntil);
	Invalid.Until = EHyperAIBotAction::Jump;
	TestFalse(TEXT("WaitUntil waits only for an assertion"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::AssertProperty);
	Invalid.Property = TEXT("Score");
	Invalid.Compare = TEXT("~=");
	TestFalse(TEXT("Comparisons are a closed set"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::MoveTo);
	Invalid.TimeoutSeconds = 3600.f;
	TestFalse(TEXT("Timeouts are bounded"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::Play);
	Invalid.Goal = EHyperAIBotGoal::Defeat;
	TestFalse(TEXT("Play Defeat needs a target"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid.Goal = EHyperAIBotGoal::Explore;
	TestFalse(TEXT("Play Explore needs seconds"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid.Seconds = 10.f;
	TestTrue(TEXT("and with them it is valid"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid.Skill = 2.f;
	TestFalse(TEXT("Skill is 0 to 1"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::Wait);
	Invalid.Seconds = 1.f;
	Invalid.bBackground = true;
	TestFalse(TEXT("Only Play, MoveTo and Hold run in the background"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid.bBackground = false;
	Invalid.PlayerIndex = 8;
	TestFalse(TEXT("Players are 0 to 7"), FHyperAIBotExecutor::Validate(Invalid, Error));

	FTestGame Game;
	if (!TestTrue(TEXT("The test world has a possessed pawn and a target"), Game.Pawn && Game.Controller && Game.Coin && Game.Controller->GetPawn() == Game.Pawn))
	{
		return false;
	}
	FHyperAIBotExecutor Bot(*Game.World, Game.Output);

	int32 Frames = 0;
	FHyperAIBotStep Wait = Make(EHyperAIBotAction::Wait);
	Wait.Seconds = 0.5f;
	TestTrue(TEXT("Wait passes"), Passed(Game.Run(Bot, Wait, &Frames)));
	TestTrue(TEXT("after about half a second of frames"), Frames >= 14 && Frames <= 17);

	const FHyperAIBotResult Survey = Bot.Begin(Make(EHyperAIBotAction::Survey));
	TestTrue(TEXT("Survey reports the player and a census"), Passed(Survey) && Survey.Detail.Contains(TEXT("player")) && Survey.Detail.Contains(TEXT("actors")));

	FHyperAIBotStep Exists = Make(EHyperAIBotAction::AssertExists);
	Exists.Target = TEXT("Coin");
	TestTrue(TEXT("A target is found by its tag"), Passed(Bot.Begin(Exists)));
	FHyperAIBotStep Property = Make(EHyperAIBotAction::AssertProperty);
	Property.Property = TEXT("BaseEyeHeight");
	Property.Compare = TEXT(">=");
	Property.Value = FString::SanitizeFloat(Game.Pawn->BaseEyeHeight);
	const FHyperAIBotResult Compared = Bot.Begin(Property);
	TestTrue(FString::Printf(TEXT("A property on the player compares as a number: %s"), *Compared.Detail), Passed(Compared));
	Property.Property = TEXT("NoSuchProperty");
	const FHyperAIBotResult Unreadable = Bot.Begin(Property);
	TestEqual(TEXT("An unknown property says so"), Unreadable.Code, FString(TEXT("property_unreadable")));
	Property.Property = TEXT("PrimaryActorTick.TickInterval");
	Property.Compare = TEXT("==");
	Property.Value = TEXT("0");
	const FHyperAIBotResult InStruct = Bot.Begin(Property);
	TestTrue(FString::Printf(TEXT("A path goes into structs: %s"), *InStruct.Detail), Passed(InStruct));
	Property.Property = TEXT("GetActorTimeDilation");
	Property.Value = TEXT("1");
	const FHyperAIBotResult Called = Bot.Begin(Property);
	TestTrue(FString::Printf(TEXT("and can end in a const function that only reads: %s"), *Called.Detail), Passed(Called));
	Property.Property = TEXT("K2_DestroyActor");
	TestEqual(TEXT("but never one that changes anything"), Bot.Begin(Property).Code, FString(TEXT("property_unreadable")));
	UHyperAIStudioTestHealth* Health = NewObject<UHyperAIStudioTestHealth>(Game.Pawn, TEXT("Health"));
	Game.Pawn->AddInstanceComponent(Health);
	Health->RegisterComponent();
	const FHyperAIBotResult Read = Bot.Begin(Make(EHyperAIBotAction::Survey));
	TestTrue(FString::Printf(TEXT("Survey finds the player's health as a player reads it: %s"), *Read.Detail), Read.Detail.Contains(TEXT("health Health.Health = 100")));
	TestEqual(TEXT("A world without a game has no one to join it"), Bot.Begin(Make(EHyperAIBotAction::AddPlayer)).Code, FString(TEXT("no_game_instance")));
	FHyperAIBotStep Other = Make(EHyperAIBotAction::AssertExists);
	Other.Target = TEXT("Player1");
	TestEqual(TEXT("Player1 is a second player's pawn, and there is none"), Bot.Begin(Other).Code, FString(TEXT("not_found")));
	Other.Target = TEXT("Player0");
	TestTrue(TEXT("Player0 is the first"), Passed(Bot.Begin(Other)));
	FHyperAIBotStep Near = Make(EHyperAIBotAction::AssertNear);
	Near.Other = TEXT("Coin");
	Near.Radius = 100.f;
	TestEqual(TEXT("The coin starts out of reach"), Bot.Begin(Near).Code, FString(TEXT("too_far")));

	FHyperAIBotStep Move = Make(EHyperAIBotAction::MoveTo);
	Move.Target = TEXT("Coin");
	Move.Radius = 50.f;
	// This bare world's player controller has no local player, so the pawn's movement ignores input, as it would
	// for a remote player. That checks the bot's own part: steering, arrival and noticing it is stuck. Real
	// movement is checked in PIE.
	TestFalse(TEXT("MoveTo keeps going until it arrives"), Bot.Begin(Move).IsDone());
	TestTrue(TEXT("It steers the player toward the target"), Game.Pawn->GetPendingMovementInputVector().GetSafeNormal().X > 0.99);
	Game.Pawn->SetActorLocation(FVector(570.0, 0.0, 100.0));
	Game.Tick();
	const FHyperAIBotResult Arrived = Bot.Tick();
	TestTrue(FString::Printf(TEXT("and arrives within its radius: %s"), *Arrived.Detail), Passed(Arrived));
	TestTrue(TEXT("The player is now near the coin"), Passed(Bot.Begin(Near)));
	FHyperAIBotStep Far = Make(EHyperAIBotAction::MoveTo);
	Far.Location = FVector(5000.0, 0.0, 100.0);
	int32 StuckFrames = 0;
	const FHyperAIBotResult Stuck = Game.Run(Bot, Far, &StuckFrames);
	TestEqual(TEXT("A player that does not move is reported stuck"), Stuck.Code, FString(TEXT("stuck")));
	TestTrue(TEXT("after about three seconds, not the whole timeout"), StuckFrames >= 85 && StuckFrames <= 100);

	FHyperAIBotStep Look = Make(EHyperAIBotAction::LookAt);
	Look.Location = FVector(0.0, 600.0, 100.0);
	TestTrue(TEXT("LookAt passes"), Passed(Bot.Begin(Look)));
	const FRotator Expected = (Look.Location - Game.Pawn->GetPawnViewLocation()).Rotation();
	TestTrue(TEXT("and turns the view to the point"), FMath::IsNearlyEqual(
		FRotator::NormalizeAxis(Game.Controller->GetControlRotation().Yaw), FRotator::NormalizeAxis(Expected.Yaw), 1.0));

	FHyperAIBotStep Gone = Make(EHyperAIBotAction::WaitUntil);
	Gone.Until = EHyperAIBotAction::AssertGone;
	Gone.Target = TEXT("Coin");
	TestTrue(TEXT("WaitUntil sees the coin go"), Passed(Game.Run(Bot, Gone, nullptr, [&Game](const int32 Frame)
	{
		if (Frame == 3 && Game.Coin)
		{
			Game.Coin->Destroy();
		}
	})));
	FHyperAIBotStep Never = Make(EHyperAIBotAction::WaitUntil);
	Never.Until = EHyperAIBotAction::AssertExists;
	Never.Target = TEXT("Nobody");
	Never.TimeoutSeconds = 0.3f;
	const FHyperAIBotResult TimedOut = Game.Run(Bot, Never);
	TestEqual(TEXT("WaitUntil gives up at its timeout"), TimedOut.Code, FString(TEXT("timed_out")));
	TestTrue(TEXT("with the last reading"), TimedOut.Detail.Contains(TEXT("Nobody")));

	FHyperAIBotStep Sample = Make(EHyperAIBotAction::Sample);
	Sample.Seconds = 0.3f;
	const FHyperAIBotResult Sampled = Game.Run(Bot, Sample);
	TestTrue(TEXT("Sample reports the player's speed"), Passed(Sampled) && Sampled.Detail.Contains(TEXT("speed")));

	TestEqual(TEXT("Only a character jumps"), Bot.Begin(Make(EHyperAIBotAction::Jump)).Code, FString(TEXT("not_a_character")));
	FHyperAIBotStep Press = Make(EHyperAIBotAction::Press);
	Press.InputAction = TEXT("IA_Jump");
	TestEqual(TEXT("Input needs a local player"), Bot.Begin(Press).Code, FString(TEXT("no_local_player")));
	const FHyperAIBotResult Shot = Bot.Begin(Make(EHyperAIBotAction::Capture));
	TestTrue(FString::Printf(TEXT("Without a viewport, a capture renders the player's camera: %s"), *Shot.Detail),
		Passed(Shot) && IFileManager::Get().FileSize(*Shot.File) > 0);
	FHyperAIBotStep Click = Make(EHyperAIBotAction::UiClick);
	Click.Widget = TEXT("WBP_Menu.PlayButton");
	TestEqual(TEXT("A missing widget says so"), Bot.Begin(Click).Code, FString(TEXT("widget_not_found")));

	FHyperAIBotStep Record = Make(EHyperAIBotAction::RecordStart);
	Record.Text = TEXT("walk");
	TestTrue(TEXT("RecordStart starts"), Passed(Bot.Begin(Record)));
	TestEqual(TEXT("A second recording is refused"), Bot.Begin(Record).Code, FString(TEXT("already_recording")));
	for (int32 Frame = 0; Frame < 3; ++Frame)
	{
		FTSTicker::GetCoreTicker().Tick(0.6f);
	}
	const FHyperAIBotResult Stopped = Bot.Begin(Make(EHyperAIBotAction::RecordStop));
	TestTrue(FString::Printf(TEXT("RecordStop reports the frames it rendered: %s"), *Stopped.Detail), Passed(Stopped) && !Stopped.Detail.StartsWith(TEXT("0 frames")));
	TestEqual(TEXT("There is nothing left to stop"), Bot.Begin(Make(EHyperAIBotAction::RecordStop)).Code, FString(TEXT("not_recording")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPIEPlaytestBotStepsTest,
	"HyperAIStudio.NativeTools.PIEPlaytest.BotSteps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPIEPlaytestBotStepsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BotTests;
	FHyperAIPIEPlaytestRequest Request;
	Request.OperationId = TEXT("bot-steps-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
	Request.Mode = EHyperAIPIEPlaytestMode::AttachOnly;
	Request.Teardown = EHyperAIPIEPlaytestTeardown::LeaveSessionRunning;
	FHyperAIPIEPlaytestStep& Step = Request.Steps.AddDefaulted_GetRef();
	Step.StepId = TEXT("walk");
	Step.Kind = EHyperAIPIEPlaytestStepKind::Bot;
	Step.Bot.Action = EHyperAIBotAction::MoveTo;
	Step.Bot.Target = TEXT("Coin");

	FHyperAIStudioPIENormalizedPlan Plan;
	FString Code;
	FString Error;
	TestTrue(TEXT("A bot step is a valid plan, with no token"), FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Request, Plan, Code, Error));
	TestTrue(TEXT("and one with effects"), Plan.bHasEffects);
	const FString FirstHash = Plan.PlanHash;
	FHyperAIPIEPlaytestRequest Other = Request;
	Other.Steps[0].Bot.Target = TEXT("Door");
	FHyperAIStudioPIENormalizedPlan OtherPlan;
	TestTrue(TEXT("Another target is valid too"), FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Other, OtherPlan, Code, Error));
	TestNotEqual(TEXT("and hashes differently"), OtherPlan.PlanHash, FirstHash);

	FHyperAIPIEPlaytestRequest BadBot = Request;
	BadBot.Steps[0].Bot = FHyperAIBotStep();
	BadBot.Steps[0].Bot.Action = EHyperAIBotAction::Press;
	TestFalse(TEXT("A bot step that cannot run is refused"), FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(BadBot, Plan, Code, Error));
	TestEqual(TEXT("with the bot's reason"), Code, FString(TEXT("invalid_bot_step")));
	FHyperAIPIEPlaytestRequest Stray = Request;
	Stray.Steps[0].Kind = EHyperAIPIEPlaytestStepKind::QueryActors;
	TestFalse(TEXT("Bot fields on another kind are refused"), FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Stray, Plan, Code, Error));
	TestEqual(TEXT("as irrelevant"), Code, FString(TEXT("irrelevant_field")));

	// Operation status and wait see playtests. With no PIE session this one waits for a world until cancelled.
	FHyperAIStudioPIEPlaytestService::Startup();
	const FHyperAIPIEPlaytestRunResult Submitted = FHyperAIStudioPIEPlaytestService::Submit(Request);
	TestTrue(TEXT("The production gate accepts a plan with effects"), Submitted.bAccepted);
	const FHyperAIOperationStatus Running = UHyperAIStudioNativeReadToolset::hyper_operation_status(Request.OperationId);
	TestTrue(TEXT("Operation status finds the playtest, still running"), Running.bFound && !Running.bTerminal);
	TestEqual(TEXT("from the playtest service"), Running.JournalAccessMode, FString(TEXT("playtest_service")));
	TestTrue(TEXT("It can be cancelled"), FHyperAIStudioPIEPlaytestService::Cancel(Request.OperationId, TEXT("test over"), Error));
	for (int32 Tick = 0; Tick < 20 && !UHyperAIStudioNativeReadToolset::hyper_operation_status(Request.OperationId).bTerminal; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.f);
	}
	const FHyperAIOperationStatus Ended = UHyperAIStudioNativeReadToolset::hyper_operation_status(Request.OperationId);
	TestTrue(TEXT("and then reads as terminal"), Ended.bTerminal);
	TestEqual(TEXT("with its reason as the status"), Ended.Status, FString(TEXT("cancelled")));
	return true;
}

#endif
