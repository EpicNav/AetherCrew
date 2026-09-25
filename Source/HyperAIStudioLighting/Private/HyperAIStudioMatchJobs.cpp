// Games by Hyper 2026.

#include "HyperAIStudioMatchJobs.h"

#include "HAL/IConsoleManager.h"
#include "HyperAIStudioImageMatch.h"
#include "HyperAIStudioLightingGate.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMatchJobs, Log, All);

namespace HyperAIStudio::Lighting::MatchJobs
{
namespace
{
	int32 Points(const float Score)
	{
		return FMath::RoundToInt(Score);
	}

	FString ProjectRelative(const FString& Full)
	{
		FString Relative = Full;
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		return FPaths::MakePathRelativeTo(Relative, *Root) ? Relative : Full;
	}
}

FString SkyNote(UWorld& World)
{
	TArray<FString> Skies;
	bool bUltra = false;
	bool bWeather = false;
	for (const FHyperAILightingActorRecord& Record : Gate::ReadActors(World))
	{
		if (Record.Kind != TEXT("sky_system"))
		{
			continue;
		}
		FString System;
		for (const FHyperAILightingProperty& Property : Record.Properties)
		{
			System = Property.Key == TEXT("system") ? Property.Value : System;
		}
		bUltra |= System == TEXT("Ultra Dynamic Sky");
		bWeather |= System == TEXT("Ultra Dynamic Weather");
		Skies.Add(FString::Printf(TEXT("%s ('%s')"), *System, *Record.Label));
	}
	if (Skies.IsEmpty())
	{
		return FString();
	}
	FString Note = FString::Printf(TEXT(" This level's sky is %s: light it with hyper_lighting_apply_plan's set_sky_system, by the variable names hyper_lighting_inspect lists"),
		*FString::Join(Skies, TEXT(" and ")));
	Note += bUltra
		? TEXT(": Time of Day (the sun's angle, 0 to 2400), Sun Light Intensity, Sun Light Color, Sky Light Intensity, Cloud Coverage, Fog, Dust Amount, Saturation, Contrast and the Exposure Bias for each time of day.")
		: TEXT(".");
	Note += TEXT(" Never add a directional light, sky light or height fog: the sky owns them, and those edits are refused. lock_exposure also turns off the sky's own exposure.");
	if (bWeather)
	{
		Note += TEXT(" Weather (Cloud Coverage, Fog, Rain, Snow) is set on Ultra Dynamic Weather.");
	}
	return Note;
}

FString BuildPrompt(const EGoal Goal, const FString& Reference, const FString& SessionId, const float Target, const FString& Sky)
{
	const FString Loop = FString::Printf(
		TEXT("This is a HyperAI Match job. After each of your turns HyperAI scores the session and, until the best attempt reaches %d, sends you back to work with what still differs. So work in rounds of up to 12 attempts, and end each turn with a one-line status.\n\n"),
		Points(Target));
	const FString Compare = FString::Printf(
		TEXT("call hyper_image_compare with referenceImagePath %s, sessionId %s, mode look, targetScore %d and a note saying what changed. Open its side-by-side and heat map before choosing the next change."),
		*Reference, *SessionId, Points(Target));
	if (Goal == EGoal::Lighting)
	{
		return FString::Printf(TEXT("Match the lighting of the open level to the reference image %s (attached).\n\n"), *Reference) + Loop
			+ TEXT("How to work (the image-match skill has the details):\n")
			+ TEXT("1. Look at the reference and say in one sentence what makes its look: time of day, key light direction and colour, palette, contrast, fog and mood.\n")
			+ TEXT("2. Call hyper_lighting_inspect to see how the level is lit.") + Sky + TEXT("\n")
			+ TEXT("3. Lock exposure first with lock_exposure, or every change is undone by the eye adapting. For lights in physical units that is about 15 EV100 for noon sun, 12 overcast, 8 a bright interior, 4 a dim one, -2 moonlight. A sky whose lights are not physical, such as Ultra Dynamic Sky, sits far lower, often -4 to 2: capture after locking and move the EV100 until the frame's luminanceP50 is near 0.18. A black or white capture means the EV100 is wrong, not the light.\n")
			+ TEXT("4. Frame the view once: a camera where the reference's camera would stand, captured with hyper_capture_view (source camera, the reference's aspect ratio). Use that same capture for every attempt.\n")
			+ TEXT("5. Each attempt: make one change, or one coherent group such as the sun's angle and colour together, capture, and ") + Compare + TEXT("\n")
			+ TEXT("6. Strongest levers first: the key light (angle, colour, intensity), then the sky and fog, then exposure and the post-process grade, with shadows and highlights graded apart for a painted look.\n")
			+ FString::Printf(TEXT("7. Keep edits unsaved while you iterate. When the score reaches %d, save with bSave true and say so.\n\n"), Points(Target))
			+ TEXT("A high score with an image that looks wrong is not a match: say what you see.");
	}
	return FString::Printf(TEXT("Build the open level so that it matches the reference image %s (attached): its layout, big shapes, materials and light, as a level designer and then a lighting artist would.\n\n"), *Reference) + Loop
		+ TEXT("How to work (the image-match skill has the details):\n")
		+ TEXT("1. Read the reference: where its camera stands (eye height, angle, lens), the big forms and where they sit (ground, walls, buildings, landmarks, water, foliage), their materials, the time of day and the light.\n")
		+ TEXT("2. Recall project memory (hyper_memory_recall) and look at the level (SceneTools get_current_level and find_actors). Search the project's content for meshes and materials that fit before you reach for engine shapes.\n")
		+ TEXT("3. Place a CineCameraActor labelled HyperAI_MatchCamera where the reference's camera would stand, and capture it with hyper_capture_view (source camera, camera_actor_label HyperAI_MatchCamera, the reference's aspect ratio). Use that capture for every attempt.\n")
		+ TEXT("4. Block out at real scale, biggest forms first, then mid-size forms, then detail (the level design pack's player metrics: a person is 180 cm, a door 220 cm high). Put things on the ground by tracing down, not by guessing heights, and keep the space playable.\n")
		+ TEXT("5. Materials next, and lighting last: lock exposure and light it to the reference as the image-match skill's lighting steps say.") + Sky + TEXT("\n")
		+ TEXT("6. Each attempt: change one group, capture, and ") + Compare + TEXT("\n")
		+ FString::Printf(TEXT("7. Save the level when the score reaches %d, and say what you built.\n\n"), Points(Target))
		+ TEXT("A high score with an image that looks wrong is not a match: say what you see.");
}

FHyperAIStudioAgentJobVerdict Judge(FRoundState& State, const EGoal Goal, const FString& SessionId, const float Target, const int32 NextRound,
	const TArray<FHyperAIImageIteration>& History, const TArray<FString>& Weakest, const TArray<FString>& Suggestions)
{
	FHyperAIStudioAgentJobVerdict Verdict;
	float Best = -1.f;
	int32 BestIndex = INDEX_NONE;
	for (const FHyperAIImageIteration& Iteration : History)
	{
		if (Iteration.Score > Best)
		{
			Best = Iteration.Score;
			BestIndex = Iteration.Index;
		}
	}
	if (History.Num() <= State.AttemptsBefore)
	{
		++State.EmptyRounds;
		Verdict.Status = History.IsEmpty() ? FString(TEXT("No attempt scored yet."))
			: FString::Printf(TEXT("No new attempt last round; the best is still %d of %d."), Points(Best), Points(Target));
		if (State.EmptyRounds >= 2)
		{
			Verdict.Status += TEXT(" Two rounds went by without an attempt, so the job stopped.");
			return Verdict;
		}
		Verdict.NextMessage = FString::Printf(
			TEXT("HyperAI Match, round %d: no attempt was scored last round. Capture the view and call hyper_image_compare with sessionId %s and targetScore %d after every change: that score is what ends this job. End the turn with a one-line status."),
			NextRound, *SessionId, Points(Target));
		return Verdict;
	}
	State.EmptyRounds = 0;
	const bool bFlat = State.BestBefore >= 0.f && Best - State.BestBefore < 1.f;
	State.FlatRounds = bFlat ? State.FlatRounds + 1 : 0;
	State.BestBefore = FMath::Max(State.BestBefore, Best);
	State.AttemptsBefore = History.Num();
	if (Best >= Target)
	{
		Verdict.bSucceeded = true;
		Verdict.Status = FString::Printf(TEXT("Matched: %d of %d at attempt %d, after %d attempts."), Points(Best), Points(Target), BestIndex, History.Num());
		return Verdict;
	}
	Verdict.Status = FString::Printf(TEXT("Best %d of %d after %d attempts."), Points(Best), Points(Target), History.Num());
	if (State.FlatRounds >= 2)
	{
		Verdict.Status += TEXT(" Two rounds gained under a point, so the job stopped at its best.");
		return Verdict;
	}
	FString Message = FString::Printf(
		TEXT("HyperAI Match, round %d: the best so far is %d of %d (attempt %d in session %s). Not there yet, so keep going with the same session and target.\n"),
		NextRound, Points(Best), Points(Target), BestIndex, *SessionId);
	if (!Weakest.IsEmpty())
	{
		Message += TEXT("\nWhere the best attempt still differs most:\n");
		for (int32 Index = 0; Index < Weakest.Num() && Index < 4; ++Index)
		{
			Message += TEXT("- ") + Weakest[Index] + TEXT("\n");
		}
	}
	if (!Suggestions.IsEmpty())
	{
		Message += TEXT("\nSuggested next changes:\n");
		for (int32 Index = 0; Index < Suggestions.Num() && Index < 3; ++Index)
		{
			Message += TEXT("- ") + Suggestions[Index] + TEXT("\n");
		}
	}
	if (State.FlatRounds == 1)
	{
		Message += Goal == EGoal::Level
			? TEXT("\nThe last round gained under a point, so switch to another kind of lever: add the shapes still missing, change materials, or move to the light.\n")
			: TEXT("\nThe last round gained under a point, so switch to another kind of lever: from the sun to the sky and fog, from colour to contrast, or from the light to the post-process grade.\n");
	}
	Message += TEXT("\nOpen each side-by-side before choosing the next change, and end the turn with a one-line status.");
	Verdict.NextMessage = Message;
	return Verdict;
}

bool Start(const EGoal Goal, const FString& ReferenceImagePath, FString& OutError)
{
	FString Why;
	if (!FHyperAIStudioPreviewPanel::IsSupportedProjectImage(ReferenceImagePath, Why))
	{
		OutError = Why;
		return false;
	}
	const FString Full = FPaths::ConvertRelativePathToFull(ReferenceImagePath);
	if (const TOptional<FHyperAIStudioAgentJobInfo> Running = FHyperAIStudioAgentJobs::FindByKey(Full); Running.IsSet() && Running->IsRunning())
	{
		OutError = FString::Printf(TEXT("Chat %d is already working on this reference; stop that job first."), Running->TabIndex);
		return false;
	}
	const FString SessionId = FString::Printf(TEXT("match_%s_%s"), Goal == EGoal::Lighting ? TEXT("lighting") : TEXT("level"),
		*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const float Target = Goal == EGoal::Lighting ? LightingTarget : LevelTarget;
	UWorld* World = Gate::GetEditorWorld();

	FHyperAIStudioAgentJobSpec Spec;
	Spec.Title = FString::Printf(TEXT("%s: %s"), Goal == EGoal::Lighting ? TEXT("Match the lighting") : TEXT("Build the level to match"), *FPaths::GetCleanFilename(Full));
	Spec.Category = Goal == EGoal::Lighting ? TEXT("Lighting") : TEXT("Level Design");
	Spec.Prompt = BuildPrompt(Goal, ProjectRelative(Full), SessionId, Target, World ? SkyNote(*World) : FString());
	Spec.Images = {Full};
	Spec.Key = Full;
	Spec.MaxFollowUps = MaxFollowUps;
	// Every attempt is an edit; a click on each would stall the loop the user started. They stay unsaved until it matches.
	Spec.AutoApprovePacks = Goal == EGoal::Lighting ? TArray<FString>{TEXT("lighting_lookdev")}
		: TArray<FString>{TEXT("lighting_lookdev"), TEXT("level_design"), TEXT("materials_dynamic_material")};
	// Once the agent is up, the Match page follows the job: each attempt appears in its strip as it is scored.
	Spec.OnStarted = [Full]() { FHyperAIStudioPreviewPanel::Open(FName(TEXT("image_match")), Full); };
	const TSharedRef<FRoundState> State = MakeShared<FRoundState>();
	Spec.OnTurnEnded = [State, Goal, SessionId, Target, Full](const int32 TurnsDone)
	{
		FString Reference;
		FString Mode;
		TArray<FHyperAIImageIteration> History;
		float SessionTarget = Target;
		Match::ReadSession(SessionId, Reference, Mode, History, SessionTarget);
		// What still differs, measured on the best attempt without adding it to the session again.
		TArray<FString> Weakest;
		TArray<FString> Suggestions;
		const FHyperAIImageIteration* Best = nullptr;
		for (const FHyperAIImageIteration& Iteration : History)
		{
			Best = !Best || Iteration.Score > Best->Score ? &Iteration : Best;
		}
		if (Best)
		{
			FHyperAIImageCompareRequest Request;
			Request.ReferenceImagePath = Full;
			Request.CandidateImagePath = Best->CandidateImagePath;
			Request.Mode = Mode.IsEmpty() ? FString(TEXT("look")) : Mode;
			Request.TargetScore = Target;
			const FHyperAIImageCompareReport Report = Match::Compare(Request);
			for (int32 Index = 0; Report.bOk && Index < Report.Regions.Num() && Index < 4; ++Index)
			{
				Weakest.Add(FString::Printf(TEXT("%s: %s"), *Report.Regions[Index].Name, *Report.Regions[Index].Difference));
			}
			for (int32 Index = 0; Report.bOk && Index < Report.Suggestions.Num() && Index < 3; ++Index)
			{
				Suggestions.Add(Report.Suggestions[Index]);
			}
		}
		return Judge(*State, Goal, SessionId, Target, TurnsDone + 1, History, Weakest, Suggestions);
	};
	const int32 Id = FHyperAIStudioAgentJobs::Start(MoveTemp(Spec), OutError);
	UE_LOG(LogHyperAIStudioMatchJobs, Display, TEXT("Match job %d for %s, session %s: %s"), Id, *Full, *SessionId, Id > 0 ? TEXT("started") : *OutError);
	return Id > 0;
}

TArray<FHyperAIStudioPreviewPanel::FAgentAction> Actions()
{
	FHyperAIStudioPreviewPanel::FAgentAction Light;
	Light.Id = FName(TEXT("match_lighting"));
	Light.Label = NSLOCTEXT("HyperAIStudioMatchJobs", "MatchLighting", "Match the lighting");
	Light.Tooltip = NSLOCTEXT("HyperAIStudioMatchJobs", "MatchLightingTip",
		"Your preferred agent relights the level until it scores 78 against the reference: sun, sky, fog, exposure and grade, or an Ultra Dynamic Sky's own settings. HyperAI checks after every round and sends it back to work until it matches. Its lighting edits are approved for you while it runs, and stay unsaved until it matches.");
	Light.Start = [](const FString& Reference, FString& OutError) { return Start(EGoal::Lighting, Reference, OutError); };

	FHyperAIStudioPreviewPanel::FAgentAction Level;
	Level.Id = FName(TEXT("match_level"));
	Level.Label = NSLOCTEXT("HyperAIStudioMatchJobs", "MatchLevel", "Build the level to match");
	Level.Tooltip = NSLOCTEXT("HyperAIStudioMatchJobs", "MatchLevelTip",
		"Your preferred agent builds the level toward the reference as a level designer would: a camera framed like it, the big shapes at real scale, then materials and light. HyperAI checks after every round and sends it back to work until it scores 72. Its level, lighting and material edits are approved for you while it runs.");
	Level.Start = [](const FString& Reference, FString& OutError) { return Start(EGoal::Level, Reference, OutError); };
	return {Light, Level};
}

namespace
{
	/** HyperAIStudio.Match <lighting|level|open> <reference image>: Match it from the console, or just the page on a reference. */
	void RunMatch(const TArray<FString>& Args)
	{
		if (Args.Num() < 2 || (Args[0] != TEXT("lighting") && Args[0] != TEXT("level") && Args[0] != TEXT("open")))
		{
			UE_LOG(LogHyperAIStudioMatchJobs, Warning, TEXT("HyperAIStudio.Match lighting|level|open <reference image inside the project>"));
			return;
		}
		FString Error;
		const FString Reference = FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" "));
		if (Args[0] == TEXT("open"))
		{
			FHyperAIStudioPreviewPanel::Open(FName(TEXT("lighting")), FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Reference) ? FPaths::ProjectDir() / Reference : Reference));
			return;
		}
		if (!FHyperAIStudioPreviewPanel::StartAgentAction(FName(TEXT("image_match")), FName(Args[0] == TEXT("lighting") ? TEXT("match_lighting") : TEXT("match_level")),
			FPaths::IsRelative(Reference) ? FPaths::ProjectDir() / Reference : Reference, Error))
		{
			UE_LOG(LogHyperAIStudioMatchJobs, Warning, TEXT("HyperAIStudio.Match: %s"), *Error);
		}
	}

	FAutoConsoleCommand MatchCommand(TEXT("HyperAIStudio.Match"),
		TEXT("Hand a reference image to your preferred agent, as the Match page's Match it does: HyperAIStudio.Match lighting|level <reference image>. open only shows it on the Match page."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunMatch));
}
}
