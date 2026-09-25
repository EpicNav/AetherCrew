// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioAgentJobs.h"
#include "HyperAIStudioMatchJobs.h"
#include "HyperAIStudioPreviewPanel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

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
	return true;
}

#endif
