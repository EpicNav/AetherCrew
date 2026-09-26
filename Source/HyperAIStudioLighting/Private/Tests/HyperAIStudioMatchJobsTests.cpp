// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "Editor/Transactor.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentJobs.h"
#include "HyperAIStudioLightingGate.h"
#include "HyperAIStudioMatchJobs.h"
#include "HyperAIStudioPreviewPanel.h"
#include "HyperAIStudioWidgetShot.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::Lighting::MatchJobTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FHyperAIImageIteration Attempt(const int32 Index, const float Score)
	{
		FHyperAIImageIteration Iteration;
		Iteration.Index = Index;
		Iteration.Score = Score;
		return Iteration;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMatchJobsPromptTest,
	"HyperAIStudio.Lighting.MatchJobs.Prompts",
	HyperAIStudio::Lighting::MatchJobTests::Flags)

bool FHyperAIStudioMatchJobsPromptTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Jobs = HyperAIStudio::Lighting::MatchJobs;
	const FString Sky = TEXT(" This level's sky is Ultra Dynamic Sky ('UDS').");
	const FString Light = Jobs::BuildPrompt(Jobs::EGoal::Lighting, TEXT("Content/References/dusk.png"), TEXT("match_lighting_1"), 78.f, Sky);
	TestTrue(TEXT("the lighting prompt names the reference"), Light.Contains(TEXT("Content/References/dusk.png")));
	TestTrue(TEXT("and the session and target to score in"), Light.Contains(TEXT("sessionId match_lighting_1")) && Light.Contains(TEXT("targetScore 78")));
	TestTrue(TEXT("says HyperAI sends it back until the target"), Light.Contains(TEXT("until the best attempt reaches 78")));
	TestTrue(TEXT("locks exposure first"), Light.Contains(TEXT("lock_exposure")));
	TestTrue(TEXT("lights with every tool: stand-ins that let light through, clouds, softer shadows"), Light.Contains(TEXT("set_actor_shadow false"))
		&& Light.Contains(TEXT("set_clouds")) && Light.Contains(TEXT("ShadowAmount")));
	TestTrue(TEXT("and grades last with a fitted LUT"), Light.Contains(TEXT("bFitLut true")) && Light.Contains(TEXT("color_grading_lut")));
	TestTrue(TEXT("and carries the sky note"), Light.Contains(Sky));
	const FString Level = Jobs::BuildPrompt(Jobs::EGoal::Level, TEXT("Content/References/town.png"), TEXT("match_level_1"), 72.f, FString());
	TestTrue(TEXT("the level prompt builds like a level designer"), Level.Contains(TEXT("level designer")) && Level.Contains(TEXT("HyperAI_MatchCamera")));
	TestTrue(TEXT("at real scale"), Level.Contains(TEXT("180 cm")));
	TestTrue(TEXT("and saves when it matches"), Level.Contains(TEXT("Save the level when the score reaches 72")));
	TestFalse(TEXT("no sky note without a Blueprint sky"), Level.Contains(TEXT("sky is")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMatchJobsJudgeTest,
	"HyperAIStudio.Lighting.MatchJobs.Judge",
	HyperAIStudio::Lighting::MatchJobTests::Flags)

bool FHyperAIStudioMatchJobsJudgeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::MatchJobTests;
	namespace Jobs = HyperAIStudio::Lighting::MatchJobs;
	const TArray<FString> Weakest = {TEXT("top left: darker by 0.8 EV, cooler")};
	const TArray<FString> Suggestions = {TEXT("Brighten by 0.8 EV.")};

	// No attempt at all: one nudge, then the job stops.
	Jobs::FRoundState Empty;
	FHyperAIStudioAgentJobVerdict Verdict = Jobs::Judge(Empty, Jobs::EGoal::Lighting, TEXT("s"), 80.f, 2, {}, {}, {});
	TestTrue(TEXT("a round without an attempt is nudged to score one"), Verdict.NextMessage.Contains(TEXT("no attempt was scored")) && !Verdict.bSucceeded);
	Verdict = Jobs::Judge(Empty, Jobs::EGoal::Lighting, TEXT("s"), 80.f, 3, {}, {}, {});
	TestTrue(TEXT("two such rounds stop the job"), Verdict.NextMessage.IsEmpty() && Verdict.Status.Contains(TEXT("stopped")));

	// Progress below the target: back to work, with what differs and what to change.
	Jobs::FRoundState State;
	Verdict = Jobs::Judge(State, Jobs::EGoal::Lighting, TEXT("s"), 80.f, 2, {Attempt(0, 41.f), Attempt(1, 55.f), Attempt(2, 60.f)}, Weakest, Suggestions);
	TestFalse(TEXT("60 of 80 is not a match"), Verdict.bSucceeded);
	TestTrue(TEXT("the next round names the best so far"), Verdict.NextMessage.Contains(TEXT("round 2")) && Verdict.NextMessage.Contains(TEXT("60 of 80")));
	TestTrue(TEXT("and where it still differs"), Verdict.NextMessage.Contains(Weakest[0]) && Verdict.NextMessage.Contains(Suggestions[0]));
	TestTrue(TEXT("the status says how it stands"), Verdict.Status.Contains(TEXT("Best 60 of 80 after 3 attempts")));

	// A round that gains under a point asks for another kind of lever; a second one stops the job at its best.
	Verdict = Jobs::Judge(State, Jobs::EGoal::Level, TEXT("s"), 80.f, 3, {Attempt(0, 41.f), Attempt(1, 55.f), Attempt(2, 60.f), Attempt(3, 60.4f)}, Weakest, Suggestions);
	TestTrue(TEXT("a flat round asks to switch levers"), Verdict.NextMessage.Contains(TEXT("switch to another kind of lever")) && Verdict.NextMessage.Contains(TEXT("shapes still missing")));
	Verdict = Jobs::Judge(State, Jobs::EGoal::Level, TEXT("s"), 80.f, 4, {Attempt(0, 41.f), Attempt(1, 55.f), Attempt(2, 60.f), Attempt(3, 60.4f), Attempt(4, 60.1f)}, Weakest, Suggestions);
	TestTrue(TEXT("two flat rounds stop at the best"), Verdict.NextMessage.IsEmpty() && Verdict.Status.Contains(TEXT("stopped at its best")));

	// Reaching the target ends the job as a success.
	Jobs::FRoundState Reaching;
	Verdict = Jobs::Judge(Reaching, Jobs::EGoal::Lighting, TEXT("s"), 78.f, 2, {Attempt(0, 50.f), Attempt(1, 79.5f), Attempt(2, 76.f)}, {}, {});
	TestTrue(TEXT("the target is reached"), Verdict.bSucceeded && Verdict.NextMessage.IsEmpty());
	TestTrue(TEXT("at the best attempt, not the last"), Verdict.Status.Contains(TEXT("80 of 78 at attempt 1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMatchJobsActionsTest,
	"HyperAIStudio.Lighting.MatchJobs.MatchItActions",
	HyperAIStudio::Lighting::MatchJobTests::Flags)

bool FHyperAIStudioMatchJobsActionsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	for (const TCHAR* SourceId : {TEXT("lighting"), TEXT("image_match")})
	{
		const FHyperAIStudioPreviewPanel::FSource* Source = FHyperAIStudioPreviewPanel::FindSource(FName(SourceId));
		if (!TestNotNull(*FString::Printf(TEXT("the %s source is registered"), SourceId), Source))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s offers two agent actions"), SourceId), Source->AgentActions.Num(), 2);
		TestTrue(*FString::Printf(TEXT("%s offers Match the lighting and Build the level"), SourceId),
			Source->AgentActions.ContainsByPredicate([](const FHyperAIStudioPreviewPanel::FAgentAction& Action) { return Action.Id == FName(TEXT("match_lighting")); })
			&& Source->AgentActions.ContainsByPredicate([](const FHyperAIStudioPreviewPanel::FAgentAction& Action) { return Action.Id == FName(TEXT("match_level")); }));
	}
	TestTrue(TEXT("attempts refresh live only on the image match source"),
		FHyperAIStudioPreviewPanel::FindSource(FName(TEXT("image_match")))->bRefreshWhileAgentWorks
		&& !FHyperAIStudioPreviewPanel::FindSource(FName(TEXT("lighting")))->bRefreshWhileAgentWorks);
	FString Error;
	TestFalse(TEXT("an action nobody offers is refused"), FHyperAIStudioPreviewPanel::StartAgentAction(FName(TEXT("image_match")), FName(TEXT("nope")), FString(), Error));
	TestTrue(TEXT("with a reason"), Error.Contains(TEXT("nope")));
	TestFalse(TEXT("a reference outside the project is refused before any chat opens"),
		FHyperAIStudioPreviewPanel::StartAgentAction(FName(TEXT("image_match")), FName(TEXT("match_lighting")), TEXT("C:/Elsewhere/photo.png"), Error));
	TestTrue(TEXT("and says why"), Error.Contains(TEXT("project")));
	TestEqual(TEXT("no job was started"), FHyperAIStudioAgentJobs::FindByKey(TEXT("C:/Elsewhere/photo.png")).IsSet(), false);
	TestEqual(TEXT("a job needs a prompt"), FHyperAIStudioAgentJobs::Start(FHyperAIStudioAgentJobSpec(), Error), 0);
	TestTrue(TEXT("the image match source can take its view, for Relight this view"), static_cast<bool>(FHyperAIStudioPreviewPanel::FindSource(FName(TEXT("image_match")))->CaptureView));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMatchJobsSameViewTest,
	"HyperAIStudio.Lighting.MatchJobs.RelitReferenceKeepsItsView",
	HyperAIStudio::Lighting::MatchJobTests::Flags)

bool FHyperAIStudioMatchJobsSameViewTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Jobs = HyperAIStudio::Lighting::MatchJobs;
	// A relit reference as the image pack leaves one: the picture, a sidecar naming the capture it changed, and that capture's view.
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests") / (TEXT("SameView-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists=*/false, /*Tree=*/true); };
	const FString Reference = Folder / TEXT("relit_01.png");
	FString Input = Folder / TEXT("relit_input.png");
	FPaths::MakePathRelativeTo(Input, *FPaths::ProjectDir());
	FFileHelper::SaveStringToFile(FString::Printf(TEXT(R"({"image": "relit_01.png", "input_image": "%s"})"), *Input), *(Folder / TEXT("relit_01.json")));
	TestTrue(TEXT("Without the view beside the picture it changed, there is no note"), Jobs::SameViewNote(Reference).IsEmpty());
	FFileHelper::SaveStringToFile(TEXT(R"({"source": "camera", "camera_actor_label": "HyperAI_MatchCamera", "location": [1, 2, 3], "rotation": [-10, 90, 0], "fov_degrees": 75, "width": 1024, "height": 576})"),
		*(Folder / TEXT("capture.json")));
	const FString Camera = Jobs::SameViewNote(Reference);
	AddInfo(Camera);
	TestTrue(TEXT("A reference made from a camera's capture is matched through that camera, unmoved"),
		Camera.Contains(TEXT("source camera, camera_actor_label HyperAI_MatchCamera, width 1024 and height 576")) && Camera.Contains(TEXT("unmoved")));
	TestTrue(TEXT("naming the picture it was made from"), Camera.Contains(Input));
	// GPT image models draw 3:2 whatever the view's shape: the attempts are then captured at the reference's shape.
	HyperAIStudio::Tests::WriteScenePng(Reference, FIntPoint(1536, 1024));
	TestTrue(TEXT("A reference drawn at another shape is matched at its own"),
		Jobs::SameViewNote(Reference).Contains(TEXT("width 1024 and height 683, the reference's shape: the model drew it 1536 x 1024")));
	FFileHelper::SaveStringToFile(TEXT(R"({"source": "viewport", "location": [1, 2, 3], "rotation": [-10, 90, 0], "fov_degrees": 75, "width": 1024, "height": 576})"),
		*(Folder / TEXT("capture.json")));
	const FString Transform = Jobs::SameViewNote(Reference);
	TestTrue(TEXT("and one from the viewport's, by the transform it had"),
		Transform.Contains(TEXT("source transform, location (1.0, 2.0, 3.0), rotation (-10.0, 90.0, 0.0)")) && Transform.Contains(TEXT("fovDegrees 75.0")));
	FFileHelper::SaveStringToFile(TEXT(R"({"image": "relit_01.png"})"), *(Folder / TEXT("relit_01.json")));
	TestTrue(TEXT("A reference drawn from words has no note"), Jobs::SameViewNote(Reference).IsEmpty());

	const FString Light = Jobs::BuildPrompt(Jobs::EGoal::Lighting, TEXT("Content/References/Generated/relit/relit_01.png"), TEXT("s"), 78.f, FString(), Camera);
	TestTrue(TEXT("The lighting prompt frames the same view"), Light.Contains(TEXT("4. This reference is the level's own view")) && !Light.Contains(TEXT("Frame the view once")));
	const FString Level = Jobs::BuildPrompt(Jobs::EGoal::Level, TEXT("Content/References/Generated/relit/relit_01.png"), TEXT("s"), 72.f, FString(), Camera);
	TestTrue(TEXT("and so does the level prompt"), Level.Contains(TEXT("3. This reference is the level's own view")) && !Level.Contains(TEXT("Place a CineCameraActor")));

	// The view itself: HyperAI_MatchCamera, placed where the level viewport looks when the level has none.
	UWorld* World = HyperAIStudio::Lighting::Gate::GetEditorWorld();
	if (!FApp::CanEverRender() || !World || !GEditor)
	{
		AddWarning(TEXT("Needs a rendering editor with a level open to take the view."));
		return true;
	}
	const auto FindCamera = [World]() -> AActor*
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetActorLabel().Equals(Jobs::MatchCameraLabel, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	};
	// A camera the level already has is set aside for the test, so the one placed is this test's.
	AActor* Kept = FindCamera();
	if (Kept)
	{
		Kept->SetActorLabel(TEXT("HyperAI_MatchCamera_KeptByTest"), /*bMarkDirty=*/false);
	}
	ON_SCOPE_EXIT
	{
		if (Kept && IsValid(Kept))
		{
			Kept->SetActorLabel(Jobs::MatchCameraLabel, /*bMarkDirty=*/false);
		}
	};
	const bool bHadCamera = FindCamera() != nullptr;
	const bool bCanPlace = GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->IsPerspective();
	FString Path;
	FString Error;
	const bool bCaptured = Jobs::CaptureMatchView(FIntPoint(640, 360), Path, Error);
	AActor* Placed = bHadCamera ? nullptr : FindCamera();
	if (!bHadCamera && !bCanPlace)
	{
		TestFalse(TEXT("With no camera and no viewport to place one at, nothing is taken"), bCaptured);
		TestTrue(TEXT("and it says what to place"), Error.Contains(TEXT("HyperAI_MatchCamera")));
		return true;
	}
	if (!TestTrue(TEXT("The view is taken: ") + Error, bCaptured))
	{
		return false;
	}
	TestTrue(TEXT("as a picture inside the project"), IFileManager::Get().FileExists(*(FPaths::ProjectDir() / Path)));
	FString View;
	FFileHelper::LoadFileToString(View, *(FPaths::GetPath(FPaths::ProjectDir() / Path) / TEXT("capture.json")));
	TestTrue(TEXT("with the view it shows beside it"), View.Contains(TEXT("\"camera_actor_label\": \"HyperAI_MatchCamera\"")) && View.Contains(TEXT("640")));
	if (Placed)
	{
		TestTrue(TEXT("With none, HyperAI_MatchCamera is placed where the level viewport looks"),
			Placed->GetActorLocation().Equals(GCurrentLevelEditingViewportClient->GetViewLocation(), 1.0));
		// As the user would place it: one undo takes it away again.
		if (TestEqual(TEXT("in one undo step of its own"), GEditor->Trans->GetUndoContext().Title.ToString(), FString(TEXT("Place HyperAI_MatchCamera"))))
		{
			GEditor->UndoTransaction();
			TestNull(TEXT("which undo takes away"), FindCamera());
		}
		else if (IsValid(Placed))
		{
			Placed->Destroy();
		}
	}
	return true;
}

#endif
