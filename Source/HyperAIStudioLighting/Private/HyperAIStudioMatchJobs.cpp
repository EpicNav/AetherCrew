// Games by Hyper 2026.

#include "HyperAIStudioMatchJobs.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HyperAIStudioImageMatch.h"
#include "HyperAIStudioLightingCapture.h"
#include "HyperAIStudioLightingGate.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

	TSharedPtr<FJsonObject> ReadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) ? Object : nullptr;
	}

	FString Triple(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
		{
			return FString();
		}
		return FString::Printf(TEXT("(%.1f, %.1f, %.1f)"), (*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	}
}

FString SameViewNote(const FString& ReferenceImagePath)
{
	// The reference's sidecar names the picture it changed, and the capture.json beside that picture names its view.
	const FString Reference = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(ReferenceImagePath) ? FPaths::ProjectDir() / ReferenceImagePath : ReferenceImagePath);
	const TSharedPtr<FJsonObject> Sidecar = ReadJson(FPaths::ChangeExtension(Reference, TEXT("json")));
	FString Input;
	if (!Sidecar.IsValid() || !Sidecar->TryGetStringField(TEXT("input_image"), Input) || Input.IsEmpty())
	{
		return FString();
	}
	const FString InputFull = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Input) ? FPaths::ProjectDir() / Input : Input);
	const TSharedPtr<FJsonObject> View = ReadJson(FPaths::GetPath(InputFull) / TEXT("capture.json"));
	if (!View.IsValid())
	{
		return FString();
	}
	FString Source;
	FString Camera;
	int32 Width = 0;
	int32 Height = 0;
	double Fov = 0.0;
	View->TryGetStringField(TEXT("source"), Source);
	View->TryGetStringField(TEXT("camera_actor_label"), Camera);
	View->TryGetNumberField(TEXT("width"), Width);
	View->TryGetNumberField(TEXT("height"), Height);
	View->TryGetNumberField(TEXT("fov_degrees"), Fov);
	// A model may draw another shape than the view it was given, as GPT image models draw 3:2; captured at the reference's shape,
	// the same camera frames what the model framed, and the compare crops nothing away.
	FString Shape;
	FImage Drawn;
	if (Width > 0 && FImageUtils::LoadImage(*Reference, Drawn) && Drawn.SizeX > 0 && Drawn.SizeY > 0)
	{
		const double Aspect = static_cast<double>(Drawn.SizeX) / Drawn.SizeY;
		if (FMath::Abs(Aspect - static_cast<double>(Width) / FMath::Max(1, Height)) > 0.02 * Aspect)
		{
			Height = FMath::RoundToInt(Width / Aspect);
			Shape = FString::Printf(TEXT(", the reference's shape: the model drew it %d x %d"), Drawn.SizeX, Drawn.SizeY);
		}
	}
	const FString Location = Triple(View, TEXT("location"));
	const FString Rotation = Triple(View, TEXT("rotation"));
	const FString How = Source == TEXT("camera") && !Camera.IsEmpty()
		? FString::Printf(TEXT("source camera, camera_actor_label %s, width %d and height %d%s"), *Camera, Width, Height, *Shape)
		: FString::Printf(TEXT("source transform, location %s, rotation %s as pitch, yaw and roll, fovDegrees %.1f, width %d and height %d%s"),
			*Location, *Rotation, Fov, Width, Height, *Shape);
	return FString::Printf(TEXT("This reference is the level's own view, relit by an image model from a capture of it (%s): the same camera, framing and objects. ")
		TEXT("Capture every attempt from that same view, unmoved: hyper_capture_view with %s. Match the light only, and where the picture drew an object a little differently, leave the object alone."),
		*ProjectRelative(InputFull), *How);
}

bool CaptureMatchView(const FIntPoint& Size, FString& OutImagePath, FString& OutError)
{
	UWorld* World = Gate::GetEditorWorld();
	if (!World)
	{
		OutError = TEXT("No level is open in the editor.");
		return false;
	}
	bool bPlaced = false;
	for (TActorIterator<AActor> It(World); It && !bPlaced; ++It)
	{
		bPlaced = It->GetActorLabel().Equals(MatchCameraLabel, ESearchCase::IgnoreCase);
	}
	if (!bPlaced)
	{
		const FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
		if (!Client || !Client->IsPerspective())
		{
			OutError = FString::Printf(TEXT("The level has no %s, and there is no perspective level viewport to place one at. Place a camera labelled %s where the view should be."),
				MatchCameraLabel, MatchCameraLabel);
			return false;
		}
		// As the user would place it: one undo step, and a camera they can see and move to relight another view.
		const FScopedTransaction Transaction(NSLOCTEXT("HyperAIStudioMatchJobs", "PlaceMatchCamera", "Place HyperAI_MatchCamera"));
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transactional;
		ACameraActor* Camera = World->SpawnActor<ACameraActor>(Client->GetViewLocation(), Client->GetViewRotation(), Params);
		if (!Camera)
		{
			OutError = FString::Printf(TEXT("Could not place %s."), MatchCameraLabel);
			return false;
		}
		Camera->SetActorLabel(MatchCameraLabel);
		Camera->GetCameraComponent()->SetFieldOfView(Client->ViewFOV);
		UE_LOG(LogHyperAIStudioMatchJobs, Display, TEXT("Placed %s where the level viewport looks."), MatchCameraLabel);
	}
	FHyperAICaptureViewRequest Request;
	Request.Source = TEXT("camera");
	Request.CameraActorLabel = MatchCameraLabel;
	Request.Width = FMath::Clamp(Size.X, 64, 1920);
	Request.Height = FMath::Clamp(Size.Y, 64, 1080);
	Request.Label = TEXT("relight");
	const FHyperAICaptureViewReport Report = Capture::RunNow(Request);
	if (!Report.bOk || Report.Frames.IsEmpty())
	{
		OutError = Report.Diagnostic.IsEmpty() ? FString(TEXT("The view could not be captured.")) : Report.Diagnostic;
		return false;
	}
	OutImagePath = Report.Frames[0].ImagePath;
	return true;
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

FString BuildPrompt(const EGoal Goal, const FString& Reference, const FString& SessionId, const float Target, const FString& Sky, const FString& SameView,
	const FString& Lessons)
{
	const FString Loop = FString::Printf(
		TEXT("This is a HyperAI Match job. After each of your turns HyperAI scores the session and, until the best attempt reaches %d, sends you back to work with what still differs. So work in rounds of up to 12 attempts, and end each turn with a one-line status.\n\n"),
		Points(Target));
	const FString Compare = FString::Printf(
		TEXT("call hyper_image_compare with referenceImagePath %s, sessionId %s, mode look, targetScore %d and a note naming what changed and its new values, such as \"sun pitch -12, 3800 K\": HyperAI learns from those notes which changes helped. Open its side-by-side and heat map before choosing the next change."),
		*Reference, *SessionId, Points(Target));
	if (Goal == EGoal::Lighting)
	{
		return FString::Printf(TEXT("Match the lighting of the open level to the reference image %s (attached).\n\n"), *Reference) + Loop
			+ TEXT("How to work (the image-match skill has the details):\n")
			+ TEXT("1. Look at the reference and say in one sentence what makes its look: time of day, key light direction and colour, palette, contrast, fog and mood.\n")
			+ TEXT("2. Call hyper_lighting_inspect to see how the level is lit, which materials the view shows, and how the project renders light (its rendering list: GI and reflection method, tracing).") + Sky + TEXT("\n")
			+ TEXT("3. Lock exposure first with lock_exposure, or every change is undone by the eye adapting. For lights in physical units that is about 15 EV100 for noon sun, 12 overcast, 8 a bright interior, 4 a dim one, -2 moonlight. A sky whose lights are not physical, such as Ultra Dynamic Sky, sits far lower, often -3 to 3: capture after locking and move the EV100 until the frame's luminanceP50 is near 0.18. A black or white capture means the EV100 is wrong, not the light.\n")
			+ (SameView.IsEmpty()
				? FString(TEXT("4. Frame the view once: a camera where the reference's camera would stand, captured with hyper_capture_view (source camera, the reference's aspect ratio). Use that same capture for every attempt.\n"))
				: TEXT("4. ") + SameView + TEXT("\n"))
			+ TEXT("5. Each attempt: make one change, or one coherent group such as the sun's angle and colour together, capture, and ") + Compare + TEXT("\n")
			+ TEXT("6. Light it as a lighting artist does, with every tool rather than one:\n")
			+ TEXT("   - The key light first: the sun's pitch, yaw, temperature and intensity, and source_angle for how soft its shadows fall.\n")
			+ TEXT("   - Tell assets from light. The reference may show finished assets where the level has blockout shapes: foliage lets light through where a solid sphere blocks it, textures break light up, and bounce light fills real shadows. Do not chase an asset difference with light: set_actor_shadow false on a shape that stands for foliage, cloth or glass, and leave surface detail to the materials.\n")
			+ TEXT("   - Surfaces take the light. Where the reference shows a surface glossier, wetter, rougher, brighter, darker or glowing where the level's is not, change its material, not the light. hyper_lighting_inspect lists the materials in view with their parameters and notes on what keeps each from taking light as the real surface would. set_material_parameter changes one wherever the level shows it (Roughness, Specular, Metallic, a base colour or tint, an emissive strength) on an instance the level owns, never the project's material; set_actor_material_parameter changes one actor's alone. Under Lumen, a roughness above lumen_max_roughness_to_trace reflects only a blur, and a non-metal's base colour belongs between 0.02 and 0.81. A small glowing mesh that should light its surroundings needs set_actor_lighting emissive_light_source true. Reflection captures help only where reflection_method is not lumen: then add_reflection_capture covers each room or street with shiny surfaces.\n")
			+ TEXT("   - The atmosphere: set_sky_atmosphere (rayleigh_scale, mie_scale, MieAnisotropy for the glow around the sun); set_fog DirectionalInscatteringExponent and DirectionalInscatteringLuminance for haze the sun lights, SecondFogData for low mist, volumetric_fog with the sun's VolumetricScatteringIntensity for shafts; set_sun light_shaft_bloom with BloomScale and BloomThreshold for god rays; set_clouds with material:Cloud_GlobalCoverage, material:Cloud_GlobalDensity and LayerBottomAltitude for the sky.\n")
			+ TEXT("   - Shadows darker than the reference: soften them with several tools together, not one: the sun's source_angle, and its ShadowAmount below 1, which lets part of its light into every shadow; set_sky_light intensity with MinOcclusion and OcclusionTint, IndirectLightingIntensity on the sun and in post process, fills that cast no shadow, set_post_process LocalExposureShadowContrastScale below 1 and a lower AmbientOcclusionIntensity, then the grade's shadows_gain.\n")
			+ TEXT("   - Interiors, night scenes and practicals run on local lights: hyper_lighting_inspect lists the point, spot and rect lights lighting the camera's view first. set_light changes one by label (intensity in its intensity_units, color, temperature, attenuation_radius, source_radius, cone angles, source_width and source_height, cast_shadows, volumetric_scattering, location, aim_at), add_light places a new key, fill, rim or practical, and enabled false turns one off: nothing is deleted.\n")
			+ TEXT("   - Any setting the Details panel shows on the sun, sky light, fog, sky atmosphere, clouds or post process can be set by name, such as set_sun BloomScale or set_post_process FilmToe; hyper_lighting_inspect lists what differs from the defaults.\n")
			+ TEXT("7. Grade last, as a colourist would. Once the light is close, compare with bFitLut true: it fits a colour-grading LUT to the reference and says what the attempt would score through it. If that is higher, apply it with set_post_process color_grading_lut <lutPath>, keep refining the light, and fit again after a big change of light. Bloom, vignette and white balance finish it.\n")
			+ FString::Printf(TEXT("8. Keep edits unsaved while you iterate. When the score reaches %d, save with bSave true and say so.\n\n"), Points(Target))
			+ Lessons
			+ TEXT("A high score with an image that looks wrong is not a match: say what you see.");
	}
	return FString::Printf(TEXT("Build the open level so that it matches the reference image %s (attached): its layout, big shapes, materials and light, as a level designer and then a lighting artist would.\n\n"), *Reference) + Loop
		+ TEXT("How to work (the image-match skill has the details):\n")
		+ TEXT("1. Read the reference: where its camera stands (eye height, angle, lens), the big forms and where they sit (ground, walls, buildings, landmarks, water, foliage), their materials, the time of day and the light.\n")
		+ TEXT("2. Recall project memory (hyper_memory_recall) and look at the level (SceneTools get_current_level and find_actors). Search the project's content for meshes and materials that fit before you reach for engine shapes.\n")
		+ (SameView.IsEmpty()
			? FString(TEXT("3. Place a CineCameraActor labelled HyperAI_MatchCamera where the reference's camera would stand, and capture it with hyper_capture_view (source camera, camera_actor_label HyperAI_MatchCamera, the reference's aspect ratio). Use that capture for every attempt.\n"))
			: TEXT("3. ") + SameView + TEXT("\n"))
		+ TEXT("4. Block out at real scale, biggest forms first, then mid-size forms, then detail (the level design pack's player metrics: a person is 180 cm, a door 220 cm high). Put things on the ground by tracing down, not by guessing heights, and keep the space playable.\n")
		+ TEXT("5. Materials next, and lighting last: lock exposure and light it to the reference as the image-match skill's lighting steps say, and where a surface takes the light differently from the reference's (glossier, rougher, glowing), tune it with set_material_parameter. Lamps, windows and fires in the reference are local lights: add_light places them and set_light shapes them.") + Sky + TEXT("\n")
		+ TEXT("6. Each attempt: change one group, capture, and ") + Compare + TEXT("\n")
		+ FString::Printf(TEXT("7. Save the level when the score reaches %d, and say what you built.\n\n"), Points(Target))
		+ Lessons
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
	// What this session's own attempts taught, from the notes they were scored with: build on one, do not repeat the other.
	TArray<FString> Helped;
	TArray<FString> Hurt;
	Studies::Learn(History, Helped, Hurt);
	if (!Hurt.IsEmpty())
	{
		Message += TEXT("\nWhat lowered the score this session, so do not do it again:\n");
		for (int32 Index = 0; Index < Hurt.Num() && Index < 3; ++Index)
		{
			Message += TEXT("- ") + Hurt[Index] + TEXT("\n");
		}
	}
	if (!Helped.IsEmpty())
	{
		Message += TEXT("\nWhat raised it, to build on:\n");
		for (int32 Index = 0; Index < Helped.Num() && Index < 3; ++Index)
		{
			Message += TEXT("- ") + Helped[Index] + TEXT("\n");
		}
	}
	if (State.FlatRounds == 1)
	{
		Message += Goal == EGoal::Level
			? TEXT("\nThe last round gained under a point, so switch to another kind of lever: add the shapes still missing, change materials, or move to the light.\n")
			: TEXT("\nThe last round gained under a point, so switch to another kind of lever: from the sun to the atmosphere, fog and clouds; from darker shadows to softer ones (sky light occlusion, indirect light, local exposure); from a blockout shape's shadow to set_actor_shadow; from the light to the surfaces, with set_material_parameter where the reference's are glossier, rougher or glowing; or from the light to the grade, with a LUT fitted by hyper_image_compare's bFitLut.\n");
	}
	Message += TEXT("\nOpen each side-by-side before choosing the next change, and end the turn with a one-line status.");
	Verdict.NextMessage = Message;
	return Verdict;
}

TOptional<Studies::FStudy> StudyOf(const EGoal Goal, const FString& SessionId, const FString& ReferenceProjectPath, const FString& Level, const float Target,
	const FHyperAIStudioAgentJobInfo& Job, const TArray<FHyperAIImageIteration>& History, const TArray<FString>& Weakest, const TArray<FString>& Suggestions)
{
	if (History.IsEmpty())
	{
		return {};
	}
	Studies::FStudy Study;
	Study.Goal = Goal == EGoal::Lighting ? TEXT("lighting") : TEXT("level");
	Study.Reference = ReferenceProjectPath;
	Study.Level = Level;
	Study.Session = SessionId;
	Study.Result = Job.State == EHyperAIStudioAgentJobState::Succeeded ? TEXT("matched") : Job.State == EHyperAIStudioAgentJobState::Failed ? TEXT("failed") : TEXT("stopped");
	Study.Status = Job.Status;
	Study.Target = Target;
	Study.Attempts = History.Num();
	for (const FHyperAIImageIteration& Iteration : History)
	{
		Study.Best = FMath::Max(Study.Best, Iteration.Score);
	}
	Study.Date = FDateTime::Now().ToString(TEXT("%Y-%m-%d"));
	Studies::Learn(History, Study.Helped, Study.Hurt);
	Study.StillDiffered = Weakest;
	Study.NextSteps = Suggestions;
	return Study;
}

namespace
{
	/** What a job leaves for the next one: its study, from the session it scored in. */
	void WriteStudy(const EGoal Goal, const FString& SessionId, const FString& Reference, const FString& Level, const float Target, const FHyperAIStudioAgentJobInfo& Job,
		const TArray<FString>& Weakest, const TArray<FString>& Suggestions, const FString& ProjectDir)
	{
		FString SessionReference;
		FString Mode;
		TArray<FHyperAIImageIteration> History;
		float SessionTarget = Target;
		Match::ReadSession(SessionId, SessionReference, Mode, History, SessionTarget);
		const TOptional<Studies::FStudy> Study = StudyOf(Goal, SessionId, Reference, Level, Target, Job, History, Weakest, Suggestions);
		if (!Study.IsSet())
		{
			UE_LOG(LogHyperAIStudioMatchJobs, Display, TEXT("Match job %d scored no attempt, so it leaves no study."), Job.Id);
			return;
		}
		FString Path;
		FString Error;
		if (Studies::Write(ProjectDir, *Study, Path, Error))
		{
			UE_LOG(LogHyperAIStudioMatchJobs, Display, TEXT("Match job %d left its study at %s."), Job.Id, *Path);
		}
		else
		{
			UE_LOG(LogHyperAIStudioMatchJobs, Warning, TEXT("Match job %d's study was not written: %s"), Job.Id, *Error);
		}
	}

	/** Follows a job from the moment it starts, and writes its study once it ends, however it ends. */
	struct FEnding
	{
		int32 JobId = 0;
		FDelegateHandle Handle;
		bool bWritten = false;
		TArray<FString> Weakest;
		TArray<FString> Suggestions;
	};
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
	const FString Relative = ProjectRelative(Full);
	const FString Level = World ? World->GetOutermost()->GetName() : FString();
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	// The studies earlier jobs left: this level's first.
	const FString Lessons = Studies::Lessons(Studies::Load(ProjectDir, 60), Goal == EGoal::Lighting ? TEXT("lighting") : TEXT("level"), Level);

	FHyperAIStudioAgentJobSpec Spec;
	Spec.Title = FString::Printf(TEXT("%s: %s"), Goal == EGoal::Lighting ? TEXT("Match the lighting") : TEXT("Build the level to match"), *FPaths::GetCleanFilename(Full));
	Spec.Category = Goal == EGoal::Lighting ? TEXT("Lighting") : TEXT("Level Design");
	Spec.Prompt = BuildPrompt(Goal, Relative, SessionId, Target, World ? SkyNote(*World) : FString(), SameViewNote(Full), Lessons);
	Spec.Images = {Full};
	Spec.Key = Full;
	Spec.MaxFollowUps = MaxFollowUps;
	// Every attempt is an edit; a click on each would stall the loop the user started. They stay unsaved until it matches.
	Spec.AutoApprovePacks = Goal == EGoal::Lighting ? TArray<FString>{TEXT("lighting_lookdev")}
		: TArray<FString>{TEXT("lighting_lookdev"), TEXT("level_design"), TEXT("materials_dynamic_material")};
	// Once the agent is up, the Match page follows the job: each attempt appears in its strip as it is scored.
	Spec.OnStarted = [Full]() { FHyperAIStudioPreviewPanel::Open(FName(TEXT("image_match")), Full); };
	const TSharedRef<FRoundState> State = MakeShared<FRoundState>();
	const TSharedRef<FEnding> Ending = MakeShared<FEnding>();
	Spec.OnTurnEnded = [State, Ending, Goal, SessionId, Target, Full](const int32 TurnsDone)
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
		Ending->Weakest = Weakest;
		Ending->Suggestions = Suggestions;
		return Judge(*State, Goal, SessionId, Target, TurnsDone + 1, History, Weakest, Suggestions);
	};
	const int32 Id = FHyperAIStudioAgentJobs::Start(MoveTemp(Spec), OutError);
	UE_LOG(LogHyperAIStudioMatchJobs, Display, TEXT("Match job %d for %s, session %s: %s"), Id, *Full, *SessionId, Id > 0 ? TEXT("started") : *OutError);
	if (Id > 0)
	{
		// However it ends, matched, stopped by its owner or by the user, or failed, the job leaves a study for the next one.
		Ending->JobId = Id;
		Ending->Handle = FHyperAIStudioAgentJobs::OnChanged().AddLambda([Ending, Goal, SessionId, Target, Relative, Level, ProjectDir]()
		{
			if (Ending->bWritten)
			{
				return;
			}
			for (const FHyperAIStudioAgentJobInfo& Job : FHyperAIStudioAgentJobs::List())
			{
				if (Job.Id != Ending->JobId || Job.IsRunning()) continue;
				Ending->bWritten = true;
				WriteStudy(Goal, SessionId, Relative, Level, Target, Job, Ending->Weakest, Ending->Suggestions, ProjectDir);
				// Let go on the next tick: removing a handler while the change is still being announced to it would free it mid-call.
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Handle = Ending->Handle](float)
				{
					FHyperAIStudioAgentJobs::OnChanged().Remove(Handle);
					return false;
				}));
				return;
			}
		});
	}
	return Id > 0;
}

TArray<FHyperAIStudioPreviewPanel::FAgentAction> Actions()
{
	FHyperAIStudioPreviewPanel::FAgentAction Light;
	Light.Id = FName(TEXT("match_lighting"));
	Light.Label = NSLOCTEXT("HyperAIStudioMatchJobs", "MatchLighting", "Match the lighting");
	Light.Tooltip = NSLOCTEXT("HyperAIStudioMatchJobs", "MatchLightingTip",
		"Your preferred agent relights the level until it scores 78 against the reference: sun, sky, fog, exposure and grade, or an Ultra Dynamic Sky's own settings, and how its surfaces take the light. HyperAI checks after every round and sends it back to work until it matches, and keeps a study of each job so the next one starts from it. Its lighting edits are approved for you while it runs, and stay unsaved until it matches.");
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
