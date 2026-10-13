// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ProgressBar.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioBot.h"
#include "HyperAIStudioBotSurvey.h"
#include "HyperAIStudioLevelSurvey.h"
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "HyperAIStudioPhonePlaytest.h"
#include "HyperAIStudioPixelStreamingBackend.h"
#include "HyperAIStudioQrCode.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/HyperAIStudioBotFixtures.h"
#include <limits>

namespace HyperAIStudio::PlaytestScaleTests
{
	/** A game world ticked by hand: a floating player at Start over a floor, and whatever the test adds. */
	struct FWorld
	{
		UWorld* World = nullptr;
		ADefaultPawn* Pawn = nullptr;
		APlayerController* Controller = nullptr;
		FString Output = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()), TEXT("PlaytestScale-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));

		explicit FWorld(const FVector& Start = FVector(0.0, 0.0, 120.0))
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->BeginPlay();
			World->GetWorldSettings()->NotifyBeginPlay();
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Pawn = World->SpawnActor<ADefaultPawn>(Start, FRotator::ZeroRotator, Params);
			Controller = World->SpawnActor<APlayerController>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
			if (Controller && Pawn)
			{
				Controller->Possess(Pawn);
			}
		}

		~FWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
			IFileManager::Get().DeleteDirectory(*Output, /*RequireExists=*/false, /*Tree=*/true);
		}

		/** A box of Size cm with its top face centred at Top. */
		AStaticMeshActor* Box(const FVector& Top, const FVector& Size)
		{
			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Top - FVector(0.0, 0.0, Size.Z * 0.5), FRotator::ZeroRotator, Params);
			if (Actor && Cube)
			{
				Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
				Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
				Actor->SetActorScale3D(Size / 100.0);
				Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				Actor->GetStaticMeshComponent()->SetCollisionResponseToAllChannels(ECR_Block);
			}
			return Actor;
		}

		void Tick()
		{
			World->Tick(LEVELTICK_All, 1.f / 30.f);
		}

		FHyperAIBotResult Run(FHyperAIBotExecutor& Bot, const FHyperAIBotStep& Step, const int32 MaxFrames = 2400)
		{
			FHyperAIBotResult Result = Bot.Begin(Step);
			for (int32 Frame = 0; Frame < MaxFrames && !Result.IsDone(); ++Frame)
			{
				Tick();
				Result = Bot.Tick();
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

	FHyperAIPIEPlaytestRequest Players(const int32 Count, const EHyperAIPIEPlaytestNetMode NetMode, const int32 Joins)
	{
		FHyperAIPIEPlaytestRequest Request;
		Request.OperationId = TEXT("players-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
		Request.Mode = EHyperAIPIEPlaytestMode::StartInProcess;
		Request.NetMode = NetMode;
		Request.NumPlayers = Count;
		for (int32 Index = 0; Index < Joins; ++Index)
		{
			FHyperAIPIEPlaytestStep& Join = Request.Steps.AddDefaulted_GetRef();
			Join.StepId = FString::Printf(TEXT("join-%d"), Index);
			Join.Kind = EHyperAIPIEPlaytestStepKind::JoinPlayer;
		}
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlaytestEightPlayersTest,
	"HyperAIStudio.Playtest.EightPlayers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlaytestEightPlayersTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlaytestScaleTests;
	using C = FHyperAIStudioPIEPlaytestContracts;
	FHyperAIStudioPIENormalizedPlan Plan;
	FString Code;
	FString Error;
	TestEqual(TEXT("eight players at most"), C::MaxPlayers, 8);
	TestTrue(TEXT("8 players on a listen server"), C::NormalizeRequest(Players(8, EHyperAIPIEPlaytestNetMode::ListenServer, 0), Plan, Code, Error));
	TestTrue(TEXT("8 clients of a dedicated server"), C::NormalizeRequest(Players(8, EHyperAIPIEPlaytestNetMode::Client, 0), Plan, Code, Error));
	TestTrue(TEXT("6 players and 2 late joiners"), C::NormalizeRequest(Players(6, EHyperAIPIEPlaytestNetMode::ListenServer, 2), Plan, Code, Error));
	TestFalse(TEXT("9 players are refused"), C::NormalizeRequest(Players(9, EHyperAIPIEPlaytestNetMode::ListenServer, 0), Plan, Code, Error));
	TestEqual(TEXT("as invalid_players"), Code, FString(TEXT("invalid_players")));
	TestFalse(TEXT("7 players and 2 late joiners make 9"), C::NormalizeRequest(Players(7, EHyperAIPIEPlaytestNetMode::ListenServer, 2), Plan, Code, Error));
	TestTrue(TEXT("saying how many that makes"), Error.Contains(TEXT("make 9 players")));

	// Memory: 512 MB a player after 1.5 GB kept for the editor; a dedicated server counts as one more.
	constexpr int64 MB = 1024ll * 1024;
	TestEqual(TEXT("nothing fits in 1 GB"), C::PlayersThatFit(1024 * MB, false), 0);
	TestEqual(TEXT("4 players fit in 3.5 GB"), C::PlayersThatFit(3584 * MB, false), 4);
	TestEqual(TEXT("3 clients and a dedicated server fit in 3.5 GB"), C::PlayersThatFit(3584 * MB, true), 3);
	TestEqual(TEXT("8 at most however much is free"), C::PlayersThatFit(65536 * MB, false), 8);
	TestEqual(TEXT("8 players need 5.5 GB"), C::PlayersThatFit(5632 * MB, false), 8);
	TestEqual(TEXT("and 7 fit in a little less"), C::PlayersThatFit(5600 * MB, false), 7);
	TestEqual(TEXT("a world-ready wait of 30 s for one world"), C::WorldReadyTimeoutMs(1), 30000);
	TestEqual(TEXT("82.5 s for a dedicated server and 7 clients"), C::WorldReadyTimeoutMs(8), 82500);
	TestEqual(TEXT("90 s at most"), C::WorldReadyTimeoutMs(9), 90000);

	// A bot acts as any of 8 players: Player0 to Player7.
	FHyperAIBotStep Step = Make(EHyperAIBotAction::AssertExists);
	Step.Target = TEXT("Player7");
	Step.PlayerIndex = 7;
	TestTrue(TEXT("player index 7 is valid"), FHyperAIBotExecutor::Validate(Step, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlaytestEightPlayersInPlayTest,
	"HyperAIStudio.Playtest.EightPlayersInPlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlaytestEightPlayersInPlayTest::RunTest(const FString& Parameters)
{
	using C = FHyperAIStudioPIEPlaytestContracts;
	if (!GEditor || GEditor->IsPlaySessionInProgress())
	{
		AddInfo(TEXT("Skipped: a play session is already running."));
		return true;
	}
	// As many of 8 players as this machine has the memory for: a listen server and its clients, the last two late joiners,
	// each checked by a bot of its own in its own world.
	const int32 Fit = C::PlayersThatFit(static_cast<int64>(FPlatformMemory::GetStats().AvailablePhysical), false);
	const int32 Total = FMath::Min(C::MaxPlayers, FMath::Max(Fit, C::UncheckedPlayers));
	if (Total < 3)
	{
		AddInfo(FString::Printf(TEXT("Skipped: this machine has the memory for %d players."), Total));
		return true;
	}
	AddInfo(FString::Printf(TEXT("Playing %d players (%d at the start, 2 late joiners)."), Total, Total - 2));
	FHyperAIPIEPlaytestRequest Request;
	Request.OperationId = TEXT("eight-players-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
	Request.Mode = EHyperAIPIEPlaytestMode::StartInProcess;
	Request.Teardown = EHyperAIPIEPlaytestTeardown::StopOwnedSession;
	Request.NetMode = EHyperAIPIEPlaytestNetMode::ListenServer;
	Request.NumPlayers = Total - 2;
	Request.WholeTimeoutMs = 240000;
	Request.MaxEvidenceItems = 64;
	auto Check = [](const int32 Instance)
	{
		FHyperAIPIEPlaytestStep Step;
		Step.StepId = FString::Printf(TEXT("player-%d"), Instance);
		Step.Kind = EHyperAIPIEPlaytestStepKind::Bot;
		Step.PieInstance = Instance;
		Step.Bot.Action = EHyperAIBotAction::AssertExists;
		Step.Bot.Target = TEXT("PlayerController");
		return Step;
	};
	for (int32 Instance = 0; Instance < Total - 2; ++Instance)
	{
		Request.Steps.Add(Check(Instance));
	}
	for (int32 Join = 0; Join < 2; ++Join)
	{
		FHyperAIPIEPlaytestStep Step;
		Step.StepId = FString::Printf(TEXT("late-join-%d"), Join);
		Step.Kind = EHyperAIPIEPlaytestStepKind::JoinPlayer;
		Step.TimeoutMs = 30000;
		Request.Steps.Add(Step);
		Request.Steps.Add(Check(Total - 2 + Join));
	}
	FHyperAIStudioPIEPlaytestService::Startup();
	const FHyperAIPIEPlaytestRunResult Submitted = FHyperAIStudioPIEPlaytestService::Submit(Request);
	if (!TestTrue(TEXT("the run is accepted: ") + Submitted.Diagnostic, Submitted.bAccepted))
	{
		return true;
	}
	const FString OperationId = Request.OperationId;
	const double Started = FPlatformTime::Seconds();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, OperationId, Started, Total]()
	{
		const FHyperAIPIEPlaytestStatusResult Status = FHyperAIStudioPIEPlaytestService::GetStatus(OperationId, 64, FString());
		if (!Status.bTerminal && FPlatformTime::Seconds() < Started + 250.0)
		{
			return false;
		}
		TestEqual(*FString::Printf(TEXT("the %d-player run completes (%s)"), Total, *Status.Diagnostic), Status.Status, FString(TEXT("completed")));
		int32 Checked = 0;
		int32 Joined = 0;
		for (const FHyperAIPIEPlaytestEvidence& Item : Status.Evidence)
		{
			Checked += Item.StepId.StartsWith(TEXT("player-")) && Item.Status == TEXT("completed");
			if (Item.Kind == TEXT("join_player") && Item.Status == TEXT("completed"))
			{
				++Joined;
				AddInfo(Item.ValuePreview);
			}
		}
		TestEqual(TEXT("every player's bot checked its own world"), Checked, Total);
		TestEqual(TEXT("both late joiners came in"), Joined, 2);
		AddInfo(FString::Printf(TEXT("%d players in %.1f s"), Total, FPlatformTime::Seconds() - Started));
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBotActorWidgetAssertsTest,
	"HyperAIStudio.Bot.ActorAndWidgetAsserts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBotActorWidgetAssertsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlaytestScaleTests;
	FString Error;
	FHyperAIBotStep Invalid = Make(EHyperAIBotAction::AssertCount);
	Invalid.Value = TEXT("3");
	TestFalse(TEXT("AssertCount needs a target"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid.Target = TEXT("Coin");
	Invalid.Compare = TEXT("contains");
	TestFalse(TEXT("AssertCount compares numbers"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::AssertDistance);
	Invalid.Value = TEXT("100");
	TestFalse(TEXT("AssertDistance needs something to measure to"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::AssertAbility);
	Invalid.Property = TEXT("GA_Dash");
	Invalid.Value = TEXT("ready");
	TestFalse(TEXT("AssertAbility knows its states"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::AssertTag);
	TestFalse(TEXT("AssertTag needs the tag"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::UiAssert);
	Invalid.Widget = TEXT("HUD.Score");
	Invalid.Property = TEXT("Colour");
	TestFalse(TEXT("UiAssert knows what it checks"), FHyperAIBotExecutor::Validate(Invalid, Error));
	Invalid = Make(EHyperAIBotAction::SurveyLevel);
	Invalid.Value = TEXT("99");
	TestFalse(TEXT("SurveyLevel's grid is 4 to 48 a side"), FHyperAIBotExecutor::Validate(Invalid, Error));

	FWorld Game;
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ATargetPoint* Coin = Game.World->SpawnActor<ATargetPoint>(FVector(600.0 + Index * 100.0, 0.0, 120.0), FRotator::ZeroRotator, Params);
		Coin->Tags.Add(TEXT("Coin"));
	}
	FHyperAIBotExecutor Bot(*Game.World, Game.Output);
	FHyperAIBotStep Count = Make(EHyperAIBotAction::AssertCount);
	Count.Target = TEXT("Coin");
	Count.Value = TEXT("3");
	FHyperAIBotResult Result = Bot.Begin(Count);
	TestTrue(FString::Printf(TEXT("three coins: %s"), *Result.Detail), Passed(Result));
	Count.Compare = TEXT(">");
	Result = Bot.Begin(Count);
	TestEqual(TEXT("not more than three"), Result.Code, FString(TEXT("assertion_failed")));
	Count.Target = TEXT("TargetPoint");
	Count.Compare = TEXT(">=");
	Result = Bot.Begin(Count);
	TestTrue(TEXT("counted by class too"), Passed(Result));

	FHyperAIBotStep Distance = Make(EHyperAIBotAction::AssertDistance);
	Distance.Target = TEXT("Player");
	Distance.Location = FVector(600.0, 0.0, 120.0);
	Distance.Compare = TEXT("<");
	Distance.Value = TEXT("650");
	Result = Bot.Begin(Distance);
	TestTrue(FString::Printf(TEXT("600 cm is under 650: %s"), *Result.Detail), Passed(Result));
	Distance.Compare = TEXT(">=");
	Distance.Value = TEXT("1000");
	Result = Bot.Begin(Distance);
	TestEqual(TEXT("and not 1000 or more"), Result.Code, FString(TEXT("assertion_failed")));

	FHyperAIBotStep Tag = Make(EHyperAIBotAction::AssertTag);
	Tag.Target = TEXT("Coin");
	Tag.Property = TEXT("Some.Tag");
	Result = Bot.Begin(Tag);
	TestEqual(TEXT("an actor without tags of either kind"), Result.Code, FString(TEXT("no_tags")));

	// A HUD that is not on the screen: found by its name and by its widgets' class, and seen as hidden.
	UUserWidget* Hud = CreateWidget<UUserWidget>(Game.World, UHyperAIStudioTestHud::StaticClass(), TEXT("WBP_TestHUD"));
	if (TestNotNull(TEXT("a HUD widget"), Hud))
	{
		Hud->WidgetTree = NewObject<UWidgetTree>(Hud, TEXT("WidgetTree"));
		UVerticalBox* Root = Hud->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
		Hud->WidgetTree->RootWidget = Root;
		UTextBlock* Score = Hud->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ScoreText"));
		Score->SetText(FText::FromString(TEXT("Score 40")));
		UProgressBar* Health = Hud->WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("HealthBar"));
		Health->SetPercent(0.25f);
		Root->AddChild(Score);
		Root->AddChild(Health);
		FHyperAIBotStep Ui = Make(EHyperAIBotAction::UiAssert);
		Ui.Widget = TEXT("WBP_TestHUD.ScoreText");
		Result = Bot.Begin(Ui);
		TestEqual(FString::Printf(TEXT("a HUD not added to the viewport is not on screen: %s"), *Result.Detail), Result.Code, FString(TEXT("not_visible")));
		Ui.Property = TEXT("Hidden");
		Result = Bot.Begin(Ui);
		TestTrue(TEXT("and Hidden says so"), Passed(Result));
		Ui.Widget = TEXT("WBP_TestHUD.ProgressBar");
		Result = Bot.Begin(Ui);
		TestTrue(FString::Printf(TEXT("found by its class: %s"), *Result.Detail), Passed(Result) && Result.Detail.Contains(TEXT("not on screen")));
		Ui.Widget = TEXT("WBP_TestHUD.Nothing");
		Result = Bot.Begin(Ui);
		TestEqual(TEXT("a widget it does not have"), Result.Code, FString(TEXT("widget_not_found")));
	}
	Bot.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLevelSurveyGridTest,
	"HyperAIStudio.LevelSurvey.Grid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLevelSurveyGridTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Bot::Survey;
	const FGrid Grid = FGrid::Over(FBox(FVector(-1000.0, -500.0, 0.0), FVector(1000.0, 500.0, 300.0)), FVector::ZeroVector, 0.0, 4);
	TestEqual(TEXT("a square over the longer side"), Grid.Size(), 2000.0);
	TestEqual(TEXT("500 cm cells"), Grid.CellSize, 500.0);
	TestTrue(TEXT("from the low corner"), Grid.Origin.Equals(FVector2D(-1000.0, -1000.0)));
	TestEqual(TEXT("the cell holding a point"), Grid.IndexAt(FVector2D(-900.0, 100.0)), Grid.IndexOf(0, 2));
	TestEqual(TEXT("outside is none"), Grid.IndexAt(FVector2D(5000.0, 0.0)), INDEX_NONE);
	TestTrue(TEXT("a cell's middle"), Grid.Center(Grid.IndexOf(1, 3)).Equals(FVector2D(-250.0, 750.0)));
	const FGrid Near = FGrid::Over(FBox(FVector(-1.0e5), FVector(1.0e5)), FVector(100.0, 200.0, 0.0), 1000.0, 8);
	TestEqual(TEXT("Range keeps the grid round the player"), Near.Size(), 2000.0);

	// Ground in every cell but a hole column; one cell up a wall's height; one way blocked.
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	TArray<double> Ground;
	Ground.Init(0.0, Grid.Num());
	for (int32 Y = 0; Y < 4; ++Y)
	{
		Ground[Grid.IndexOf(2, Y)] = NaN;
	}
	Ground[Grid.IndexOf(3, 3)] = 0.0;
	Ground[Grid.IndexOf(1, 3)] = 900.0;
	const TArray<ECell> Cells = Classify(Grid, Ground, Grid.IndexOf(0, 0), 350.0,
		[&Grid](const int32 From, const int32 To) { return !(From == Grid.IndexOf(0, 1) && To == Grid.IndexOf(1, 1)) && !(From == Grid.IndexOf(1, 1) && To == Grid.IndexOf(0, 1)); });
	TestTrue(TEXT("the start is reachable"), Cells[Grid.IndexOf(0, 0)] == ECell::Reachable);
	TestTrue(TEXT("its neighbour too"), Cells[Grid.IndexOf(1, 0)] == ECell::Reachable);
	TestTrue(TEXT("a hole has no ground"), Cells[Grid.IndexOf(2, 1)] == ECell::NoGround);
	TestTrue(TEXT("ground past the hole is unreachable"), Cells[Grid.IndexOf(3, 0)] == ECell::Unreachable);
	TestTrue(TEXT("a ledge 900 cm up is unreachable"), Cells[Grid.IndexOf(1, 3)] == ECell::Unreachable);
	TestTrue(TEXT("a blocked way is reached round the side"), Cells[Grid.IndexOf(1, 1)] == ECell::Reachable);
	TestEqual(TEXT("reachable share of the ground"), FMath::RoundToInt32(ReachablePercent(Cells)), 58);

	const TArray<FVector2D> Points = {FVector2D(0.0, 0.0), FVector2D(10.0, 0.0), FVector2D(1000.0, 0.0), FVector2D(0.0, 1000.0), FVector2D(990.0, 0.0)};
	const TArray<int32> Picks = Spread(Points, 3);
	TestEqual(TEXT("three picks"), Picks.Num(), 3);
	TestEqual(TEXT("the first point first"), Picks[0], 0);
	TestTrue(TEXT("then the farthest ones"), Picks.Contains(2) || Picks.Contains(3));
	TestFalse(TEXT("not a point next to one picked"), Picks.Contains(1));

	const FVector2D Pixel = WorldToPixel(Grid, 1024, FVector2D(-500.0, 250.0));
	const FVector2D Back = PixelToWorld(Grid, 1024, Pixel.X, Pixel.Y);
	TestTrue(TEXT("pixels and the world map both ways"), Back.Equals(FVector2D(-500.0, 250.0), 0.01));
	TestTrue(TEXT("image up is +X"), WorldToPixel(Grid, 1024, FVector2D(900.0, 0.0)).Y < WorldToPixel(Grid, 1024, FVector2D(-900.0, 0.0)).Y);

	TArray<FColor> Pixels;
	TArray<bool> Walked;
	Walked.Init(false, Grid.Num());
	PaintOverview(Pixels, 64, Grid, Cells, Walked, Grid.IndexOf(0, 0), {}, {{FVector(750.0, 750.0, 0.0), TEXT("fell")}});
	TestEqual(TEXT("the overview is square"), Pixels.Num(), 64 * 64);
	const FVector2D Reach = WorldToPixel(Grid, 64, Grid.Center(Grid.IndexOf(1, 0)));
	const FColor ReachColor = Pixels[FMath::FloorToInt(Reach.Y) * 64 + FMath::FloorToInt(Reach.X)];
	TestTrue(TEXT("reachable ground is tinted green"), ReachColor.G > ReachColor.R && ReachColor.G > ReachColor.B);
	const FVector2D Unreach = WorldToPixel(Grid, 64, Grid.Center(Grid.IndexOf(3, 0)));
	const FColor UnreachColor = Pixels[FMath::FloorToInt(Unreach.Y) * 64 + FMath::FloorToInt(Unreach.X)];
	TestTrue(TEXT("unreachable ground is tinted red"), UnreachColor.R > UnreachColor.G);
	const FVector2D Fall = WorldToPixel(Grid, 64, FVector2D(750.0, 750.0));
	TestEqual(TEXT("a fall is marked magenta"), Pixels[FMath::FloorToInt(Fall.Y) * 64 + FMath::FloorToInt(Fall.X)], FColor(255, 0, 255));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLevelSurveyPlayTest,
	"HyperAIStudio.LevelSurvey.Play",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLevelSurveyPlayTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PlaytestScaleTests;
	FWorld Game(FVector(-800.0, 0.0, 120.0));
	// A floor the player stands on, a wall across part of it, and an island with a gap of nothing between.
	Game.Box(FVector(-500.0, 0.0, 0.0), FVector(1400.0, 1400.0, 40.0));
	Game.Box(FVector(-500.0, 300.0, 300.0), FVector(80.0, 800.0, 300.0));
	Game.Box(FVector(1200.0, 0.0, 0.0), FVector(600.0, 1400.0, 40.0));
	FHyperAIBotExecutor Bot(*Game.World, Game.Output);
	FHyperAIBotStep Survey = Make(EHyperAIBotAction::SurveyLevel);
	Survey.Value = TEXT("12");
	Survey.Seconds = 3.f;
	Survey.TimeoutSeconds = 60.f;
	Survey.Text = TEXT("test-survey");
	const FVector Start = Game.Pawn->GetActorLocation();
	FHyperAIBotResult Result = Game.Run(Bot, Survey);
	if (!TestTrue(FString::Printf(TEXT("the survey passes: %s %s"), *Result.Code, *Result.Detail), Passed(Result)))
	{
		return false;
	}
	TestTrue(TEXT("without a navmesh it follows the ground"), Result.Detail.Contains(TEXT("ground connectivity")));
	TestTrue(TEXT("the player is put back where it started"), Game.Pawn->GetActorLocation().Equals(Start, 1.0));
	const FString Coverage = FPaths::Combine(Result.File, TEXT("coverage.json"));
	FString Text;
	TSharedPtr<FJsonObject> Json;
	if (!TestTrue(TEXT("coverage.json is written"), FFileHelper::LoadFileToString(Text, *Coverage))
		|| !TestTrue(TEXT("as JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("its schema"), Json->GetStringField(TEXT("schema")), FString(TEXT("hyperai.level-survey.v1")));
	TestTrue(TEXT("some ground is reachable"), Json->GetNumberField(TEXT("reachable")) > 0);
	TestTrue(TEXT("the island is not"), Json->GetNumberField(TEXT("unreachable")) > 0);
	TestTrue(TEXT("the gap and the outside have no ground"), Json->GetNumberField(TEXT("noGround")) > 0);
	TestTrue(TEXT("the map marks the start"), Json->GetStringField(TEXT("map")).Contains(TEXT("S")));
	TestTrue(TEXT("the overview image is written"), IFileManager::Get().FileExists(*FPaths::Combine(Result.File, TEXT("overview.png"))));
	const int32 Frames = static_cast<int32>(Json->GetNumberField(TEXT("frames")));
	AddInfo(FString::Printf(TEXT("Survey: %s"), *Result.Detail.Left(600)));
	IFileManager::Get().Copy(*FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()), TEXT("LevelSurvey-overview.png")),
		*FPaths::Combine(Result.File, TEXT("overview.png")));
	const FHyperAIBotResult WithVideo = HyperAIStudio::LevelSurvey::WithVideo(EHyperAIBotAction::SurveyLevel, Result);
	if (Frames > 0)
	{
		TestTrue(FString::Printf(TEXT("its %d frames become survey.mp4: %s"), Frames, *WithVideo.Detail),
			IFileManager::Get().FileSize(*FPaths::Combine(Result.File, TEXT("survey.mp4"))) > 0);
	}
	else
	{
		AddInfo(TEXT("No frames rendered in this world (no scene); the video step was not exercised here."));
	}
	TestEqual(TEXT("other steps are left alone"), HyperAIStudio::LevelSurvey::WithVideo(EHyperAIBotAction::Wait, Result).Detail, Result.Detail);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLevelSurveyInPlayTest,
	"HyperAIStudio.LevelSurvey.InPlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLevelSurveyInPlayTest::RunTest(const FString& Parameters)
{
	if (!GEditor || GEditor->IsPlaySessionInProgress())
	{
		AddInfo(TEXT("Skipped: a play session is already running."));
		return true;
	}
	// The open level surveyed in Play In Editor, where the game's own character walks, falls and is filmed.
	FHyperAIPIEPlaytestRequest Request;
	Request.OperationId = TEXT("survey-play-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
	Request.Mode = EHyperAIPIEPlaytestMode::StartInProcess;
	Request.Teardown = EHyperAIPIEPlaytestTeardown::StopOwnedSession;
	Request.WholeTimeoutMs = 90000;
	Request.bRecordVideo = true;
	FHyperAIPIEPlaytestStep Settle;
	Settle.StepId = TEXT("settle");
	Settle.Kind = EHyperAIPIEPlaytestStepKind::Bot;
	Settle.Bot.Action = EHyperAIBotAction::Wait;
	Settle.Bot.Seconds = 0.5f;
	FHyperAIPIEPlaytestStep Survey;
	Survey.StepId = TEXT("survey");
	Survey.Kind = EHyperAIPIEPlaytestStepKind::Bot;
	Survey.TimeoutMs = 30000;
	Survey.Bot.Action = EHyperAIBotAction::SurveyLevel;
	Survey.Bot.Value = TEXT("16");
	Survey.Bot.Seconds = 8.f;
	Survey.Bot.TimeoutSeconds = 30.f;
	Survey.Bot.Text = TEXT("pie-survey");
	Request.Steps = {Settle, Survey};
	FHyperAIStudioPIEPlaytestService::Startup();
	const FHyperAIPIEPlaytestRunResult Submitted = FHyperAIStudioPIEPlaytestService::Submit(Request);
	if (!TestTrue(TEXT("the run is accepted: ") + Submitted.Diagnostic, Submitted.bAccepted))
	{
		return true;
	}
	const FString OperationId = Request.OperationId;
	const double Deadline = FPlatformTime::Seconds() + 100.0;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, OperationId, Deadline]()
	{
		const FHyperAIPIEPlaytestStatusResult Status = FHyperAIStudioPIEPlaytestService::GetStatus(OperationId, 64, FString());
		if (!Status.bTerminal && FPlatformTime::Seconds() < Deadline)
		{
			return false;
		}
		TestEqual(*FString::Printf(TEXT("the survey run completes (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));
		const FHyperAIPIEPlaytestEvidence* Survey = Status.Evidence.FindByPredicate([](const FHyperAIPIEPlaytestEvidence& Item)
		{
			return Item.StepId == TEXT("survey") && Item.Status == TEXT("completed");
		});
		if (!TestNotNull(TEXT("the survey step completed"), Survey))
		{
			return true;
		}
		AddInfo(TEXT("Survey in play: ") + Survey->ValuePreview);
		TestTrue(TEXT("coverage.json"), IFileManager::Get().FileExists(*FPaths::Combine(Survey->ObjectPath, TEXT("coverage.json"))));
		TestTrue(TEXT("the overview"), IFileManager::Get().FileExists(*FPaths::Combine(Survey->ObjectPath, TEXT("overview.png"))));
		TestTrue(TEXT("and the video of its frames"), IFileManager::Get().FileSize(*FPaths::Combine(Survey->ObjectPath, TEXT("survey.mp4"))) > 0);
		TestTrue(TEXT("which the step reports"), Survey->ValuePreview.Contains(TEXT("survey.mp4")));
		const FString Keep = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()), TEXT("LevelSurvey-InPlay"));
		for (const TCHAR* Name : {TEXT("overview.png"), TEXT("frame_0001.png"), TEXT("frame_0020.png"), TEXT("coverage.json"), TEXT("survey.mp4")})
		{
			IFileManager::Get().Copy(*FPaths::Combine(Keep, Name), *FPaths::Combine(Survey->ObjectPath, Name));
		}
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioQrCodeTest,
	"HyperAIStudio.PhonePlaytest.QrCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioQrCodeTest::RunTest(const FString& Parameters)
{
	// ISO/IEC 18004 Annex I: "01234567" at 1-M, its 16 data codewords and the 10 error-correction codewords that follow.
	const TArray<uint8> Data = {0x10, 0x20, 0x0C, 0x56, 0x61, 0x80, 0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11};
	const TArray<uint8> Expected = {0xA5, 0x24, 0xD4, 0xC1, 0xED, 0x36, 0xC7, 0x87, 0x2C, 0x55};
	TestTrue(TEXT("Reed-Solomon codewords as the standard gives them"), FHyperAIStudioQrCode::ReedSolomonRemainder(Data, 10) == Expected);
	TestEqual(TEXT("format bits M, mask 0"), static_cast<int32>(FHyperAIStudioQrCode::FormatBits(0, 0)), 0x5412);
	TestEqual(TEXT("format bits L, mask 0"), static_cast<int32>(FHyperAIStudioQrCode::FormatBits(1, 0)), 0x77C4);
	TestEqual(TEXT("format bits H, mask 7"), static_cast<int32>(FHyperAIStudioQrCode::FormatBits(2, 7)), 0x083B);
	TestEqual(TEXT("version 7's bits"), static_cast<int32>(FHyperAIStudioQrCode::VersionBits(7)), 0x07C94);
	TestEqual(TEXT("1-M holds 16 data codewords"), FHyperAIStudioQrCode::DataCodewords(1), 16);
	TestEqual(TEXT("10-M holds 216"), FHyperAIStudioQrCode::DataCodewords(10), 216);

	FHyperAIStudioQrCode Code;
	FString Error;
	const FString Url = HyperAIStudio::PhonePlaytest::PlayerUrl(TEXT("192.168.1.20"), 8080, TEXT("DefaultStreamer"));
	if (!TestTrue(TEXT("a player URL encodes"), FHyperAIStudioQrCode::Encode(Url, Code, Error)))
	{
		return false;
	}
	TestEqual(TEXT("its size follows its version"), Code.Size, 17 + 4 * Code.Version);
	TestTrue(TEXT("in version 10 or less"), Code.Version >= 1 && Code.Version <= 10);
	for (const FIntPoint& Corner : {FIntPoint(0, 0), FIntPoint(Code.Size - 7, 0), FIntPoint(0, Code.Size - 7)})
	{
		TestTrue(TEXT("finder patterns: a dark ring"), Code.IsDark(Corner.X, Corner.Y) && Code.IsDark(Corner.X + 6, Corner.Y + 6));
		TestFalse(TEXT("round a light ring"), Code.IsDark(Corner.X + 1, Corner.Y + 1));
		TestTrue(TEXT("round a dark middle"), Code.IsDark(Corner.X + 3, Corner.Y + 3));
	}
	TestTrue(TEXT("the dark module"), Code.IsDark(8, Code.Size - 8));
	for (int32 Index = 8; Index < Code.Size - 8; ++Index)
	{
		TestTrue(TEXT("timing patterns alternate"), Code.IsDark(Index, 6) == (Index % 2 == 0));
	}
	FHyperAIStudioQrCode Small;
	TestTrue(TEXT("a short text"), FHyperAIStudioQrCode::Encode(TEXT("hello"), Small, Error));
	TestEqual(TEXT("fits version 1"), Small.Version, 1);
	FHyperAIStudioQrCode TooLong;
	TestFalse(TEXT("300 bytes do not fit"), FHyperAIStudioQrCode::Encode(FString::ChrN(300, TEXT('a')), TooLong, Error));
	const FString Png = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("qr-test.png"));
	TestTrue(TEXT("written as a PNG"), Code.SavePng(Png, 4, Error) && IFileManager::Get().FileSize(*Png) > 0);
	IFileManager::Get().Delete(*Png);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPhonePlaytestContractsTest,
	"HyperAIStudio.PhonePlaytest.Contracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPhonePlaytestContractsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PhonePlaytest;
	TestTrue(TEXT("192.168 is private"), ClassifyIpv4(TEXT("192.168.1.20")) == EAddressKind::Private);
	TestTrue(TEXT("10/8 is private"), ClassifyIpv4(TEXT("10.0.0.5")) == EAddressKind::Private);
	TestTrue(TEXT("172.16-31 is private"), ClassifyIpv4(TEXT("172.20.1.1")) == EAddressKind::Private);
	TestTrue(TEXT("172.32 is not"), ClassifyIpv4(TEXT("172.32.1.1")) == EAddressKind::Public);
	TestTrue(TEXT("loopback"), ClassifyIpv4(TEXT("127.0.0.1")) == EAddressKind::Loopback);
	TestTrue(TEXT("link-local"), ClassifyIpv4(TEXT("169.254.3.4")) == EAddressKind::LinkLocal);
	TestTrue(TEXT("a carrier's NAT"), ClassifyIpv4(TEXT("100.70.1.2")) == EAddressKind::CarrierNat);
	TestTrue(TEXT("a public address"), ClassifyIpv4(TEXT("203.0.113.5")) == EAddressKind::Public);
	TestTrue(TEXT("not an address"), ClassifyIpv4(TEXT("192.168.1")) == EAddressKind::Invalid);
	TestTrue(TEXT("nor this"), ClassifyIpv4(TEXT("192.168.1.300")) == EAddressKind::Invalid);

	FString Error;
	TestEqual(TEXT("192.168 before 10"), PickLanAddress({TEXT("127.0.0.1"), TEXT("10.0.0.5"), TEXT("192.168.1.20")}, FString(), Error), FString(TEXT("192.168.1.20")));
	TestEqual(TEXT("a requested address of this PC"), PickLanAddress({TEXT("10.0.0.5"), TEXT("192.168.1.20")}, TEXT("10.0.0.5"), Error), FString(TEXT("10.0.0.5")));
	TestTrue(TEXT("but not one it lacks"), PickLanAddress({TEXT("10.0.0.5")}, TEXT("10.0.0.6"), Error).IsEmpty());
	TestTrue(TEXT("nor a public one"), PickLanAddress({TEXT("10.0.0.5")}, TEXT("8.8.8.8"), Error).IsEmpty());
	TestTrue(TEXT("no LAN, no address"), PickLanAddress({TEXT("127.0.0.1")}, FString(), Error).IsEmpty());

	TestEqual(TEXT("origins: the LAN page, this PC, and the game's stream"), AllowedOrigins(TEXT("192.168.1.20"), 8080),
		FString(TEXT("http://192.168.1.20:8080,http://127.0.0.1:8080,http://localhost:8080,127.0.0.1")));
	const FString Url = PlayerUrl(TEXT("192.168.1.20"), 8080, TEXT("HyperAIPlaytest"));
	TestTrue(TEXT("the URL a phone opens"), Url.StartsWith(TEXT("http://192.168.1.20:8080/?StreamerId=HyperAIPlaytest&AutoConnect=true")));
	TestTrue(TEXT("with touch input"), Url.Contains(TEXT("TouchInput=true")));

	FHyperAIPhonePlaytestRequest Request;
	Request.Target = EHyperAIPhonePlaytestTarget::Standalone;
	Request.MaxViewers = 6;
	Request.bAllViewersControl = false;
	Request.bOffscreen = true;
	const FString Args = GameArguments(Request, TEXT("/Game/Maps/Arena"), TEXT("C:/Logs/game.log"));
	TestTrue(TEXT("the level, as a game"), Args.StartsWith(TEXT("/Game/Maps/Arena -game ")));
	TestTrue(TEXT("streaming to the local server"), Args.Contains(TEXT("-PixelStreamingConnectionURL=ws://127.0.0.1:8888")));
	TestTrue(TEXT("as HyperAIPlaytest"), Args.Contains(TEXT("-PixelStreamingID=HyperAIPlaytest")));
	TestTrue(TEXT("the first viewer in control"), Args.Contains(TEXT("-PixelStreamingInputController=Host")));
	TestTrue(TEXT("six encoder sessions"), Args.Contains(TEXT("-PixelStreamingEncoderMaxSessions=6")));
	TestTrue(TEXT("off screen"), Args.Contains(TEXT("-RenderOffScreen")) && !Args.Contains(TEXT("-windowed")));
	TestTrue(TEXT("logging beside it"), Args.Contains(TEXT("-abslog=\"C:/Logs/game.log\"")));

	FHyperAIPhonePlaytestRequest Bad;
	Bad.HttpPort = 80;
	TestFalse(TEXT("ports under 1024 are refused"), Validate(Bad, Error));
	Bad = FHyperAIPhonePlaytestRequest();
	Bad.StreamerPort = Bad.HttpPort;
	TestFalse(TEXT("one port for both"), Validate(Bad, Error));
	Bad = FHyperAIPhonePlaytestRequest();
	Bad.MaxViewers = 9;
	TestFalse(TEXT("nine viewers"), Validate(Bad, Error));
	Bad = FHyperAIPhonePlaytestRequest();
	Bad.Map = TEXT("/Game/Maps/Arena");
	TestFalse(TEXT("a map for Play In Editor"), Validate(Bad, Error));
	TestTrue(TEXT("the defaults are valid"), Validate(FHyperAIPhonePlaytestRequest(), Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPhonePlaytestStartTest,
	"HyperAIStudio.PhonePlaytest.Start",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPhonePlaytestStartTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PhonePlaytest;
	ON_SCOPE_EXIT
	{
		SetAddressesForTests({});
		Stop();
	};
	Stop();
	FHyperAIPhonePlaytestRequest Request;
	Request.bDryRun = true;
	Request.HttpPort = 18471;
	Request.StreamerPort = 18472;
	Request.MaxViewers = 8;
	const bool bPlugin = IPluginManager::Get().FindEnabledPlugin(TEXT("PixelStreaming2")).IsValid();
	SetAddressesForTests({TEXT("192.168.77.20")});
	FHyperAIPhonePlaytestReport Report = Start(Request);
	if (!bPlugin)
	{
		TestEqual(TEXT("refused without the plugin"), Report.Status, FString(TEXT("refused")));
		TestTrue(TEXT("saying to enable it"), Report.Diagnostic.Contains(TEXT("Pixel Streaming 2 plugin")));
		AddInfo(TEXT("Pixel Streaming 2 is not enabled in this project: the rest of the start path was not exercised."));
		return true;
	}
	if (!FHyperAIStudioPixelStreamingBackends::Get())
	{
		AddError(TEXT("The plugin is enabled but HyperAI's Pixel Streaming backend did not register."));
		return false;
	}
	TestTrue(TEXT("with the plugin, the PIE auto-stream that downloads is off"),
		IConsoleManager::Get().FindConsoleVariable(TEXT("PixelStreaming2.Editor.AutoStreamPIE")) && !IConsoleManager::Get().FindConsoleVariable(TEXT("PixelStreaming2.Editor.AutoStreamPIE"))->GetBool());
	if (Report.Status == TEXT("refused") && Report.Diagnostic.Contains(TEXT("player page is missing")))
	{
		AddInfo(TEXT("The Pixel Streaming player page is not on this machine: start was refused as designed. ") + Report.Diagnostic);
		return true;
	}
	if (!TestEqual(FString::Printf(TEXT("a dry run is checked: %s"), *Report.Diagnostic), Report.Status, FString(TEXT("checked"))))
	{
		return false;
	}
	TestTrue(TEXT("its URL is on the LAN address"), Report.Url.StartsWith(TEXT("http://192.168.77.20:18471/?StreamerId=")));
	TestTrue(TEXT("with a QR code to scan"), !Report.QrCodePng.IsEmpty() && IFileManager::Get().FileExists(*Report.QrCodePng));
	TestTrue(TEXT("and says what it exposes"), Report.Exposure.Contains(TEXT("every network interface")) && Report.Exposure.Contains(TEXT("no public")));
	TestEqual(TEXT("nothing runs after a dry run"), Status().Status, FString(TEXT("idle")));

	SetAddressesForTests({TEXT("192.168.77.20"), TEXT("203.0.113.5")});
	Report = Start(Request);
	TestEqual(TEXT("refused with a public address"), Report.Status, FString(TEXT("refused")));
	TestTrue(TEXT("naming it"), Report.Diagnostic.Contains(TEXT("203.0.113.5")));
	SetAddressesForTests({TEXT("127.0.0.1")});
	Report = Start(Request);
	TestTrue(TEXT("refused with no LAN address"), Report.Status == TEXT("refused") && Report.Diagnostic.Contains(TEXT("no private LAN address")));

	// For real: it asks first, and nothing starts until the user answers. Stopping withdraws the question.
	SetAddressesForTests({TEXT("192.168.77.20")});
	Request.bDryRun = false;
	Request.OperationId = TEXT("phone-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Report = Start(Request);
	if (!TestEqual(FString::Printf(TEXT("it waits for approval: %s"), *Report.Diagnostic), Report.Status, FString(TEXT("awaiting_user_approval"))))
	{
		return false;
	}
	TestTrue(TEXT("as an external effect the user must approve"), FHyperAIStudioApprovalGate::HasPending(Request.OperationId));
	TestEqual(TEXT("status says so"), Status().Status, FString(TEXT("awaiting_user_approval")));
	TestEqual(TEXT("a second start is refused meanwhile"), Start(Request).Status, FString(TEXT("busy")));
	TestFalse(TEXT("no server before approval"), FHyperAIStudioPixelStreamingBackends::Get()->IsSignallingReady());
	Report = Stop();
	TestEqual(TEXT("stopped"), Report.Status, FString(TEXT("stopped")));
	TestFalse(TEXT("the question is withdrawn"), FHyperAIStudioApprovalGate::HasPending(Request.OperationId));
	TestEqual(TEXT("and nothing runs"), Status().Status, FString(TEXT("idle")));
	return true;
}

#endif
