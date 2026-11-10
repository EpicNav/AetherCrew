// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::PlaytestProofTests
{
	FHyperAIPIEPlaytestStep Wait(const TCHAR* StepId, const float Seconds, const int32 PieInstance = INDEX_NONE)
	{
		FHyperAIPIEPlaytestStep Step;
		Step.StepId = StepId;
		Step.Kind = EHyperAIPIEPlaytestStepKind::Bot;
		Step.Bot.Action = EHyperAIBotAction::Wait;
		Step.Bot.Seconds = Seconds;
		Step.PieInstance = PieInstance;
		return Step;
	}

	const FHyperAIPIEPlaytestEvidence* Find(const TArray<FHyperAIPIEPlaytestEvidence>& Evidence, const FString& Kind, const FString& Status)
	{
		return Evidence.FindByPredicate([&](const FHyperAIPIEPlaytestEvidence& Item) { return Item.Kind == Kind && Item.Status == Status; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlaytestPlayersBoundsTest,
	"HyperAIStudio.Playtest.PlayersAndVideoBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlaytestPlayersBoundsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PlaytestProofTests;
	FHyperAIPIEPlaytestRequest Request;
	Request.OperationId = TEXT("playtest-bounds-00001");
	Request.Mode = EHyperAIPIEPlaytestMode::StartInProcess;
	Request.NetMode = EHyperAIPIEPlaytestNetMode::ListenServer;
	Request.NumPlayers = 2;
	Request.bRecordVideo = true;
	Request.Steps = {Wait(TEXT("host"), 0.1f), Wait(TEXT("client"), 0.1f, 1)};
	FHyperAIStudioPIENormalizedPlan Plan;
	FString Code;
	FString Error;
	TestTrue(TEXT("a two-player recorded run is a valid plan: ") + Error, FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Request, Plan, Code, Error));
	const FString Hash = Plan.PlanHash;
	FHyperAIPIEPlaytestRequest Other = Request;
	Other.NumPlayers = 3;
	TestTrue(TEXT("three players are valid too"), FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Other, Plan, Code, Error));
	TestNotEqual(TEXT("and plan differently"), Plan.PlanHash, Hash);

	auto Refused = [&](FHyperAIPIEPlaytestRequest Bad, const TCHAR* ExpectedCode, const TCHAR* What)
	{
		TestFalse(What, FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Bad, Plan, Code, Error));
		TestEqual(FString(What) + TEXT(" with its reason"), Code, FString(ExpectedCode));
	};
	Other = Request;
	Other.NumPlayers = 9;
	Refused(Other, TEXT("invalid_players"), TEXT("nine players are refused"));
	Other = Request;
	Other.Mode = EHyperAIPIEPlaytestMode::AttachOnly;
	Other.Teardown = EHyperAIPIEPlaytestTeardown::LeaveSessionRunning;
	Refused(Other, TEXT("players_need_start"), TEXT("players on a session the run does not start are refused"));
	Other = Request;
	Other.VideoFps = 60;
	Refused(Other, TEXT("invalid_video_fps"), TEXT("a frame rate past 30 is refused"));
	Other = Request;
	Other.Steps[1].PieInstance = -2;
	Refused(Other, TEXT("invalid_pie_instance"), TEXT("a step's instance below -1 is refused"));
	Other = Request;
	Other.NetMode = EHyperAIPIEPlaytestNetMode::Standalone;
	Other.NumPlayers = 1;
	FHyperAIPIEPlaytestStep& Join = Other.Steps.AddDefaulted_GetRef();
	Join.StepId = TEXT("join");
	Join.Kind = EHyperAIPIEPlaytestStepKind::JoinPlayer;
	Refused(Other, TEXT("join_needs_network"), TEXT("a late join into a standalone game is refused"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPlaytestVideoAndPlayersTest,
	"HyperAIStudio.Playtest.VideoAndPlayers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPlaytestVideoAndPlayersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PlaytestProofTests;
	if (!GEditor || GEditor->IsPlaySessionInProgress())
	{
		AddInfo(TEXT("Skipped: a play session is already running."));
		return true;
	}
	// A listen server and its client play together, the client checks in, a third player joins late, and the
	// whole run is recorded with its steps drawn over the host's view.
	FHyperAIPIEPlaytestRequest Request;
	Request.OperationId = TEXT("playtest-proof-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
	Request.Mode = EHyperAIPIEPlaytestMode::StartInProcess;
	Request.Teardown = EHyperAIPIEPlaytestTeardown::StopOwnedSession;
	Request.NetMode = EHyperAIPIEPlaytestNetMode::ListenServer;
	Request.NumPlayers = 2;
	Request.bRecordVideo = true;
	Request.VideoFps = 10;
	Request.WholeTimeoutMs = 90000;
	FHyperAIPIEPlaytestStep Join;
	Join.StepId = TEXT("late-join");
	Join.Kind = EHyperAIPIEPlaytestStepKind::JoinPlayer;
	Join.TimeoutMs = 20000;
	Request.Steps = {Wait(TEXT("host-plays"), 1.0f), Wait(TEXT("client-plays"), 0.5f, 1), Join, Wait(TEXT("joiner-plays"), 0.5f, 2)};

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
		TestEqual(*FString::Printf(TEXT("the run completes (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));
		const FHyperAIPIEPlaytestEvidence* Joined = Find(Status.Evidence, TEXT("join_player"), TEXT("completed"));
		TestTrue(TEXT("a player joins late"), Joined != nullptr);
		if (Joined)
		{
			TestEqual(TEXT("making three worlds"), Joined->ItemCount, 3);
		}
		TestNotNull(TEXT("the joiner plays in its own world"), Status.Evidence.FindByPredicate([](const FHyperAIPIEPlaytestEvidence& Item)
		{
			return Item.StepId == TEXT("joiner-plays") && Item.Status == TEXT("completed");
		}));
		const FHyperAIPIEPlaytestEvidence* Video = Find(Status.Evidence, TEXT("video"), TEXT("completed"));
		if (TestNotNull(TEXT("the video is the run's evidence"), Video))
		{
			TArray<uint8> Bytes;
			TestTrue(TEXT("on disk: ") + Video->ObjectPath, FFileHelper::LoadFileToArray(Bytes, *Video->ObjectPath));
			TestTrue(TEXT("as an MP4"), Bytes.Num() > 8 && FMemory::Memcmp(Bytes.GetData() + 4, "ftyp", 4) == 0);
			FString Timeline;
			TestTrue(TEXT("with a timeline beside it"), FFileHelper::LoadFileToString(Timeline, *(FPaths::GetPath(Video->ObjectPath) / TEXT("timeline.json"))));
			TestTrue(TEXT("naming every step: ") + Timeline.Left(300),
				Timeline.Contains(TEXT("host-plays")) && Timeline.Contains(TEXT("late-join")) && Timeline.Contains(TEXT("joiner-plays")));
		}
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
