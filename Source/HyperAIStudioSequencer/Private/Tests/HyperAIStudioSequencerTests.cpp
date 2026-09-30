// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "HyperAIStudioSequencerToolset.h"
#include "HyperAIStudioSettings.h"
#include "LevelSequence.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "MovieScene.h"
#include "ObjectTools.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"

namespace HyperAIStudio::SequencerTests
{
	using C = FHyperAIStudioSequencerContracts;

	FHyperAISequenceOp Op(const TCHAR* Kind)
	{
		FHyperAISequenceOp Out;
		Out.Kind = Kind;
		return Out;
	}

	FHyperAISequenceOp Frame(const FString& Camera, const TArray<FString>& Subjects, const TCHAR* Framing, const float Time, const float Duration)
	{
		FHyperAISequenceOp Out = Op(TEXT("shot.frame"));
		Out.Camera = Camera;
		Out.Subjects = Subjects;
		Out.Framing = Framing;
		Out.Time = Time;
		Out.Duration = Duration;
		return Out;
	}

	FHyperAISequencePlanReport Run(const FString& Sequence, TArray<FHyperAISequenceOp> Ops, const bool bDryRun = false)
	{
		FHyperAISequencePlanRequest Plan;
		Plan.Sequence = Sequence;
		Plan.Operations = MoveTemp(Ops);
		Plan.bDryRun = bDryRun;
		return C::ApplyPlan(Plan);
	}

	AStaticMeshActor* Box(UWorld& World, const FString& Label, const FVector& Center, const FVector& Scale, const FRotator& Rotation = FRotator::ZeroRotator)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		AStaticMeshActor* Actor = Cube ? World.SpawnActor<AStaticMeshActor>(Center, Rotation, FActorSpawnParameters()) : nullptr;
		if (!Actor) return nullptr;
		Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Actor->SetActorScale3D(Scale);
		Actor->SetActorLabel(Label);
		return Actor;
	}

	ACineCameraActor* Camera(UWorld& World, const FString& Label)
	{
		for (TActorIterator<ACineCameraActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel() == Label) return *It;
		}
		return nullptr;
	}

	/** Where the sequence puts a bound actor at a second. */
	bool KeyedAt(ULevelSequence& Sequence, UWorld& World, AActor& Actor, const double Seconds, FVector& OutLocation, FRotator& OutRotation)
	{
		UMovieScene* Scene = Sequence.GetMovieScene();
		const FGuid Guid = Sequence.FindBindingFromObject(&Actor, &World);
		const UMovieScene3DTransformTrack* Track = Guid.IsValid() ? Scene->FindTrack<UMovieScene3DTransformTrack>(Guid) : nullptr;
		const UMovieScene3DTransformSection* Section = Track && Track->GetAllSections().Num() > 0 ? Cast<UMovieScene3DTransformSection>(Track->GetAllSections()[0]) : nullptr;
		if (!Section) return false;
		const auto Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		const FFrameTime Time = Seconds * Scene->GetTickResolution();
		double Values[6] = {};
		for (int32 Index = 0; Index < 6; ++Index)
		{
			if (!Channels[Index]->Evaluate(Time, Values[Index])) return false;
		}
		OutLocation = FVector(Values[0], Values[1], Values[2]);
		OutRotation = FRotator(Values[4], Values[5], Values[3]);
		return true;
	}

	bool Mentions(const TArray<FString>& Lines, const TCHAR* Text)
	{
		return Lines.ContainsByPredicate([Text](const FString& Line) { return Line.Contains(Text); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSequencerEndToEndTest,
	"HyperAIStudio.NativeTools.Cinematics.SequenceEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSequencerEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::SequencerTests;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		AddInfo(TEXT("Skipped: no editor world."));
		return true;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/Sequencer_") + Suffix;
	const FString Path = Folder + TEXT("/LS_Duel");
	const FString Hero = TEXT("HyperAI_TestHero_") + Suffix;
	const FString Rival = TEXT("HyperAI_TestRival_") + Suffix;
	const FString CamA = TEXT("Cam_A_") + Suffix;
	const FString CamB = TEXT("Cam_B_") + Suffix;
	const FString CamC = TEXT("Cam_C_") + Suffix;
	const FString CamD = TEXT("Cam_D_") + Suffix;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	// Far from the bench level: a floor, and two people five meters apart facing each other, both turned a little
	// toward +X, the audience's side of the line between them.
	const FVector Origin(700000.0, 700000.0, 30000.0);
	TArray<AActor*> Made;
	Made.Add(Box(*World, TEXT("HyperAI_TestStage_") + Suffix, Origin, FVector(30.0, 30.0, 0.2)));
	AStaticMeshActor* HeroActor = Box(*World, Hero, Origin + FVector(0.0, -250.0, 100.0), FVector(0.5, 0.5, 1.8), FRotator(0.0, 80.0, 0.0));
	AStaticMeshActor* RivalActor = Box(*World, Rival, Origin + FVector(0.0, 250.0, 100.0), FVector(0.5, 0.5, 1.8), FRotator(0.0, -80.0, 0.0));
	Made.Add(HeroActor);
	Made.Add(RivalActor);
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		for (const FString& Label : {CamA, CamB, CamC, CamD})
		{
			if (ACineCameraActor* Left = Camera(*World, Label)) Made.Add(Left);
		}
		for (AActor* Actor : Made)
		{
			if (IsValid(Actor)) World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Folder), Assets, /*bRecursive=*/true);
		TArray<UObject*> Objects;
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* Object = Asset.GetAsset()) Objects.Add(Object);
		}
		if (!Objects.IsEmpty()) ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
	};
	if (!TestTrue(TEXT("the stage is built"), !Made.Contains(nullptr))) return false;

	FHyperAISequenceOp Create = Op(TEXT("sequence.create"));
	Create.FrameRate = 24.f;
	Create.Duration = 8.f;
	// Shot/reverse-shot, both over the right shoulder, which puts the cameras on opposite sides of the line.
	FHyperAISequenceOp OverRival = Frame(CamA, {Hero}, TEXT("medium_close"), 0.f, 2.f);
	OverRival.Over = Rival;
	FHyperAISequenceOp OverHero = Frame(CamB, {Rival}, TEXT("medium_close"), 2.f, 2.f);
	OverHero.Over = Hero;
	FHyperAISequenceOp TwoShot = Frame(CamC, {Hero, Rival}, TEXT("wide"), 4.f, 2.f);
	TwoShot.Move = TEXT("push_in");
	FHyperAISequenceOp CloseUp = Frame(CamD, {Hero}, TEXT("close"), 6.f, 2.f);
	CloseUp.Side = TEXT("three_quarter_left");
	FHyperAISequenceOp Fade = Op(TEXT("fade.add"));
	FHyperAISequenceKey Black;
	Black.Time = 0.f;
	Black.Value = 1.f;
	FHyperAISequenceKey Clear;
	Clear.Time = 0.5f;
	Clear.Value = 0.f;
	Fade.Keys = {Black, Clear};
	FHyperAISequenceOp Audio = Op(TEXT("audio.add"));
	Audio.Asset = TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue");
	Audio.Time = 1.f;

	// Refused before anything changes.
	const FHyperAISequencePlanReport NoSequence = Run(Path, {CloseUp}, /*bDryRun=*/true);
	TestTrue(TEXT("a shot in a sequence that is not there is refused: ") + NoSequence.Diagnostic, !NoSequence.bOk && NoSequence.Diagnostic.Contains(TEXT("sequence.create")));
	FHyperAISequenceOp Giant = CloseUp;
	Giant.Framing = TEXT("giant");
	const FHyperAISequencePlanReport BadFraming = Run(Path, {Create, Giant}, /*bDryRun=*/true);
	TestTrue(TEXT("an unknown framing is refused with the framings listed: ") + BadFraming.Diagnostic, !BadFraming.bOk && BadFraming.Diagnostic.Contains(TEXT("medium_close")));
	FHyperAISequenceOp Nobody = CloseUp;
	Nobody.Subjects = {TEXT("NoSuchActor_") + Suffix};
	TestFalse(TEXT("a subject that is not there is refused"), Run(Path, {Create, Nobody}, /*bDryRun=*/true).bOk);
	TestTrue(TEXT("the whole plan checks"), Run(Path, {Create, OverRival, OverHero, TwoShot, CloseUp, Fade, Audio}, /*bDryRun=*/true).Status == TEXT("checked"));

	// The scene, cut: four shots, a fade from black, and a sound.
	const FHyperAISequencePlanReport Done = Run(Path, {Create, OverRival, OverHero, TwoShot, CloseUp, Fade, Audio});
	const FString Notes = FString::Join(Done.Notes, TEXT(" | "));
	if (!TestTrue(TEXT("the scene is cut: ") + Done.Diagnostic + TEXT(" | ") + Notes, Done.bOk && Done.Applied == 7)) return false;
	AddInfo(TEXT("Shots: ") + Notes);
	AddInfo(TEXT("Review: ") + FString::Join(Done.Review, TEXT(" | ")));
	ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *(Path + TEXT(".LS_Duel")));
	if (!TestNotNull(TEXT("the sequence is saved as an asset"), Sequence)) return false;
	TestTrue(TEXT("the review catches the reverse shot crossing the line"), Mentions(Done.Review, TEXT("crosses the line")));
	FHyperAISequenceInspectRequest Look;
	Look.Sequence = Path;
	const FHyperAISequenceInspectReport Seen = C::Inspect(Look);
	TestTrue(TEXT("inspect reads four cuts at 0, 2, 4 and 6 s: ") + FString::Join(Seen.Cuts, TEXT(" | ")), Seen.bOk && Seen.Cuts.Num() == 4
		&& Seen.Cuts[0].StartsWith(TEXT("0.00")) && Seen.Cuts[1].StartsWith(TEXT("2.00")) && Seen.Cuts[2].StartsWith(TEXT("4.00")) && Seen.Cuts[3].StartsWith(TEXT("6.00")));
	TestTrue(TEXT("at 24 fps and 8 s"), FMath::IsNearlyEqual(Seen.FrameRate, 24.f) && FMath::IsNearlyEqual(Seen.Duration, 8.f, 0.05f));

	// The close-up is framed as a close-up: an 85mm lens, three-quarter to the hero, aimed at the eyes' third.
	ACineCameraActor* Close = Camera(*World, CamD);
	FVector At;
	FRotator Turn;
	if (TestNotNull(TEXT("the close-up's camera is placed"), Close) && TestTrue(TEXT("and keyed"), KeyedAt(*Sequence, *World, *Close, 6.5, At, Turn)))
	{
		const float Lens = Close->GetCineCameraComponent()->CurrentFocalLength;
		const double Sensor = Close->GetCineCameraComponent()->Filmback.SensorHeight;
		// A close-up shows the top 21% of a 180 cm figure filling 80% of the frame.
		const double Expected = (0.21 * 180.0 / 0.8) * Lens / Sensor;
		const FVector Eye = HeroActor->GetActorLocation() + FVector(0.0, 0.0, 0.43 * 180.0);
		const double Distance = FVector::Dist(At, Eye);
		TestTrue(FString::Printf(TEXT("85mm (%.0f), %.0f cm from the eyes (%.0f expected)"), Lens, Distance, Expected), FMath::IsNearlyEqual(Lens, 85.f) && FMath::Abs(Distance - Expected) < Expected * 0.15);
		const FVector Toward = (At - HeroActor->GetActorLocation()).GetSafeNormal2D();
		const double Around = FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(Toward, HeroActor->GetActorForwardVector().GetSafeNormal2D())));
		TestTrue(FString::Printf(TEXT("three-quarter: %.0f degrees off the hero's facing"), Around), FMath::Abs(Around - 40.0) < 5.0);
		TestTrue(TEXT("looking at the hero"), FVector::DotProduct(Turn.Vector(), (Eye - At).GetSafeNormal()) > 0.98);
		TestTrue(TEXT("and focused on the hero"), Close->GetCineCameraComponent()->FocusSettings.FocusMethod == ECameraFocusMethod::Tracking
			&& Close->GetCineCameraComponent()->FocusSettings.TrackingFocusSettings.ActorToTrack.Get() == HeroActor);
	}
	// The two-shot pushes in: it starts farther out than it ends.
	ACineCameraActor* Wide = Camera(*World, CamC);
	FVector Opening;
	FVector Ending;
	FRotator Unused;
	if (TestNotNull(TEXT("the two-shot's camera is placed"), Wide) && KeyedAt(*Sequence, *World, *Wide, 4.0, Opening, Unused) && KeyedAt(*Sequence, *World, *Wide, 6.0, Ending, Unused))
	{
		const FVector Middle = (HeroActor->GetActorLocation() + RivalActor->GetActorLocation()) * 0.5;
		const double Ratio = FVector::Dist2D(Opening, Middle) / FMath::Max(FVector::Dist2D(Ending, Middle), 1.0);
		TestTrue(FString::Printf(TEXT("the push in closes by a third (%.2f)"), Ratio), Ratio > 1.25 && Ratio < 1.45);
	}
	UMovieSceneFadeTrack* FadeTrack = Sequence->GetMovieScene()->FindTrack<UMovieSceneFadeTrack>();
	const FMovieSceneFloatChannel* FadeChannel = FadeTrack && FadeTrack->GetAllSections().Num() > 0 ? FadeTrack->GetAllSections()[0]->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
	float Start = -1.f;
	float Half = -1.f;
	if (FadeChannel)
	{
		FadeChannel->Evaluate(FFrameTime(0), Start);
		FadeChannel->Evaluate(0.5 * Sequence->GetMovieScene()->GetTickResolution(), Half);
	}
	TestTrue(FString::Printf(TEXT("the scene fades up from black (%.2f, then %.2f)"), Start, Half), FMath::IsNearlyEqual(Start, 1.f) && FMath::IsNearlyEqual(Half, 0.f));
	TestTrue(TEXT("and the sound is on an audio track"), Mentions(Seen.Tracks, TEXT("Audio: CompileSuccess_Cue at 1.00")));

	// Reframing the reverse over the other shoulder keeps the line, and the cut is not doubled.
	FHyperAISequenceOp Reverse = OverHero;
	Reverse.Side = TEXT("left");
	const FHyperAISequencePlanReport Fixed = Run(Path, {Reverse});
	TestTrue(TEXT("the reverse is reframed: ") + FString::Join(Fixed.Review, TEXT(" | ")), Fixed.bOk && !Mentions(Fixed.Review, TEXT("crosses the line")));
	TestEqual(TEXT("still four cuts"), C::Inspect(Look).Cuts.Num(), 4);
	GEditor->UndoTransaction();
	TestTrue(TEXT("and undo brings the crossing back"), Mentions(C::Inspect(Look).Review, TEXT("crosses the line")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
