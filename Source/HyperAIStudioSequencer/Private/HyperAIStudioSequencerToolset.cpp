// Games by Hyper 2026.

#include "HyperAIStudioSequencerToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "CineCameraSettings.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "CollisionQueryParams.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSourceControl.h"
#include "LevelSequence.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneTimeHelpers.h"
#include "ScopedTransaction.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Sound/SoundBase.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Tracks/MovieSceneDoubleTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioSequencer, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioSequencer"

FString FHyperAIStudioSequencerContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioSequencer.HyperAIStudioSequencerToolset");
}

TArray<FString> FHyperAIStudioSequencerContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioSequencerContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::SequencerTools::Private
{
	using C = FHyperAIStudioSequencerContracts;

	/** Tags a framed camera carries, so the review knows what each shot is of. */
	const TCHAR* const SubjectTag = TEXT("HyperAISubject:");
	const TCHAR* const OverTag = TEXT("HyperAIOver:");
	const TCHAR* const SizeTag = TEXT("HyperAIFraming:");

	FString KindOf(const FHyperAISequenceOp& Op)
	{
		return Op.Kind.TrimStartAndEnd().ToLower();
	}

	FString Lower(const FString& Text)
	{
		return Text.TrimStartAndEnd().ToLower().Replace(TEXT(" "), TEXT("_")).Replace(TEXT("-"), TEXT("_"));
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	template <typename T>
	T* Load(const FString& Path)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("/"))) return nullptr;
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	FString PackageOf(const FString& Reference)
	{
		FString Path = Reference.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
		return Path;
	}

	AActor* FindActor(UWorld& World, const FString& Label)
	{
		const FString Wanted = Label.TrimStartAndEnd();
		if (Wanted.IsEmpty()) return nullptr;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel().Equals(Wanted, ESearchCase::IgnoreCase)) return *It;
		}
		return nullptr;
	}

	ACineCameraActor* FindCamera(UWorld& World, const FString& Label)
	{
		return Cast<ACineCameraActor>(FindActor(World, Label));
	}

	// --- Time -----------------------------------------------------------------------------------------------------

	FFrameNumber ToFrame(const UMovieScene& Scene, const double Seconds)
	{
		return (Seconds * Scene.GetTickResolution()).RoundToFrame();
	}

	double ToSeconds(const UMovieScene& Scene, const FFrameNumber Frame)
	{
		return Scene.GetTickResolution().AsSeconds(Frame);
	}

	bool ParseFrameRate(const float Fps, FFrameRate& Out)
	{
		if (FMath::IsNearlyEqual(Fps, 23.976f, 0.01f)) Out = FFrameRate(24000, 1001);
		else if (FMath::IsNearlyEqual(Fps, 29.97f, 0.01f)) Out = FFrameRate(30000, 1001);
		else if (FMath::IsNearlyEqual(Fps, 59.94f, 0.01f)) Out = FFrameRate(60000, 1001);
		else if (Fps >= 1.f && Fps <= 240.f && FMath::IsNearlyEqual(Fps, FMath::RoundToFloat(Fps))) Out = FFrameRate(FMath::RoundToInt32(Fps), 1);
		else return false;
		return true;
	}

	/** Makes sure the playback range reaches the given second. */
	void ReachTo(UMovieScene& Scene, const double Seconds)
	{
		const TRange<FFrameNumber> Range = Scene.GetPlaybackRange();
		const FFrameNumber Start = UE::MovieScene::DiscreteInclusiveLower(Range);
		const FFrameNumber End = UE::MovieScene::DiscreteExclusiveUpper(Range);
		const FFrameNumber Wanted = ToFrame(Scene, Seconds);
		if (Wanted > End) Scene.SetPlaybackRange(Start, (Wanted - Start).Value);
	}

	// --- Bindings -------------------------------------------------------------------------------------------------

	/** The actor's binding in the sequence, made when it has none. */
	FGuid BindActor(ULevelSequence& Sequence, AActor& Actor, UWorld& World)
	{
		UMovieScene* Scene = Sequence.GetMovieScene();
		const FGuid Existing = Sequence.FindBindingFromObject(&Actor, &World);
		if (Existing.IsValid()) return Existing;
		const FGuid Guid = Scene->AddPossessable(Actor.GetActorLabel(), Actor.GetClass());
		Sequence.BindPossessableObject(Guid, Actor, &World);
		return Guid;
	}

	/** A component's binding, under its actor's, made when it has none. */
	FGuid BindComponent(ULevelSequence& Sequence, AActor& Actor, UActorComponent& Component, UWorld& World)
	{
		UMovieScene* Scene = Sequence.GetMovieScene();
		const FGuid Parent = BindActor(Sequence, Actor, World);
		for (int32 Index = 0; Index < Scene->GetPossessableCount(); ++Index)
		{
			const FMovieScenePossessable& Possessable = Scene->GetPossessable(Index);
			if (Possessable.GetParent() == Parent && Possessable.GetName() == Component.GetName()) return Possessable.GetGuid();
		}
		const FGuid Guid = Scene->AddPossessable(Component.GetName(), Component.GetClass());
		if (FMovieScenePossessable* Possessable = Scene->FindPossessable(Guid)) Possessable->SetParent(Parent, Scene);
		Sequence.BindPossessableObject(Guid, Component, &Actor);
		return Guid;
	}

	FString BindingName(const UMovieScene& Scene, const FGuid& Guid)
	{
		for (int32 Index = 0; Index < Scene.GetPossessableCount(); ++Index)
		{
			const FMovieScenePossessable& Possessable = const_cast<UMovieScene&>(Scene).GetPossessable(Index);
			if (Possessable.GetGuid() == Guid) return Possessable.GetName();
		}
		for (int32 Index = 0; Index < Scene.GetSpawnableCount(); ++Index)
		{
			const FMovieSceneSpawnable& Spawnable = const_cast<UMovieScene&>(Scene).GetSpawnable(Index);
			if (Spawnable.GetGuid() == Guid) return Spawnable.GetName();
		}
		return Guid.ToString();
	}

	/** A cut to the camera at the frame; a cut already there is turned to this camera rather than doubled. */
	void CutTo(UMovieScene& Scene, const FGuid& Camera, const FFrameNumber At)
	{
		UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(Scene.GetCameraCutTrack());
		if (!Cuts) Cuts = Cast<UMovieSceneCameraCutTrack>(Scene.AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
		if (!Cuts) return;
		Cuts->Modify();
		for (UMovieSceneSection* Section : Cuts->GetAllSections())
		{
			UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
			if (Cut && Cut->HasStartFrame() && Cut->GetInclusiveStartFrame() == At)
			{
				Cut->Modify();
				Cut->SetCameraGuid(Camera);
				return;
			}
		}
		Cuts->AddNewCameraCut(UE::MovieScene::FRelativeObjectBindingID(Camera), At);
	}

	// --- Keys -----------------------------------------------------------------------------------------------------

	enum class EInterp : uint8 { Auto, Linear, Constant };

	bool ParseInterp(const FString& Text, EInterp& Out)
	{
		const FString Value = Lower(Text);
		if (Value.IsEmpty() || Value == TEXT("auto") || Value == TEXT("cubic") || Value == TEXT("smooth")) Out = EInterp::Auto;
		else if (Value == TEXT("linear")) Out = EInterp::Linear;
		else if (Value == TEXT("constant") || Value == TEXT("hold") || Value == TEXT("step")) Out = EInterp::Constant;
		else return false;
		return true;
	}

	template <typename ChannelType, typename ValueType>
	void AddKey(ChannelType& Channel, const FFrameNumber Frame, const ValueType Value, const EInterp Interp)
	{
		switch (Interp)
		{
		case EInterp::Linear: Channel.AddLinearKey(Frame, Value); break;
		case EInterp::Constant: Channel.AddConstantKey(Frame, Value); break;
		default: Channel.AddCubicKey(Frame, Value, RCTM_Auto); break;
		}
	}

	/** Takes out a channel's keys from From to To, so new keys replace them rather than stack on them. */
	template <typename ChannelType>
	void ClearKeys(ChannelType& Channel, const FFrameNumber From, const FFrameNumber To)
	{
		auto Data = Channel.GetData();
		// The times are read afresh each step: removing a key may move the array.
		for (int32 Index = Data.GetTimes().Num() - 1; Index >= 0; --Index)
		{
			const FFrameNumber Time = Data.GetTimes()[Index];
			if (Time >= From && Time <= To) Data.RemoveKey(Index);
		}
	}

	template <typename SectionType>
	SectionType* InfiniteSection(UMovieSceneTrack& Track)
	{
		for (UMovieSceneSection* Existing : Track.GetAllSections())
		{
			if (SectionType* Typed = Cast<SectionType>(Existing)) return Typed;
		}
		SectionType* Section = Cast<SectionType>(Track.CreateNewSection());
		if (!Section) return nullptr;
		Section->SetRange(TRange<FFrameNumber>::All());
		Track.AddSection(*Section);
		return Section;
	}

	UMovieScene3DTransformSection* TransformSection(UMovieScene& Scene, const FGuid& Guid)
	{
		UMovieScene3DTransformTrack* Track = Scene.FindTrack<UMovieScene3DTransformTrack>(Guid);
		if (!Track) Track = Scene.AddTrack<UMovieScene3DTransformTrack>(Guid);
		return Track ? InfiniteSection<UMovieScene3DTransformSection>(*Track) : nullptr;
	}

	/** Location and rotation keys: channels 0-2 are location X, Y, Z; 3-5 rotation roll, pitch, yaw. */
	void KeyTransform(UMovieScene3DTransformSection& Section, const FFrameNumber Frame, const FVector& Location, const FRotator& Rotation, const EInterp Interp)
	{
		const auto Channels = Section.GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		if (Channels.Num() < 6) return;
		const double Values[6] = {Location.X, Location.Y, Location.Z, Rotation.Roll, Rotation.Pitch, Rotation.Yaw};
		for (int32 Index = 0; Index < 6; ++Index)
		{
			AddKey(*Channels[Index], Frame, Values[Index], Interp);
		}
	}

	void ClearTransformKeys(UMovieScene3DTransformSection& Section, const FFrameNumber From, const FFrameNumber To)
	{
		for (FMovieSceneDoubleChannel* Channel : Section.GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>())
		{
			if (Channel) ClearKeys(*Channel, From, To);
		}
	}

	bool EvaluateTransform(const UMovieScene3DTransformSection& Section, const FFrameNumber Frame, FVector& OutLocation, FRotator& OutRotation)
	{
		const auto Channels = Section.GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
		if (Channels.Num() < 6) return false;
		double Values[6] = {};
		for (int32 Index = 0; Index < 6; ++Index)
		{
			if (!Channels[Index]->Evaluate(FFrameTime(Frame), Values[Index])) return false;
		}
		OutLocation = FVector(Values[0], Values[1], Values[2]);
		OutRotation = FRotator(Values[4], Values[5], Values[3]);
		return true;
	}

	/** A property path on the actor or one of its components: the object, and the property's name and full path. */
	struct FPropertyTarget
	{
		UObject* Object = nullptr;
		UActorComponent* Component = nullptr;
		FString Path;
		FName Leaf;
		const FProperty* Property = nullptr;
	};

	bool ResolveProperty(AActor& Actor, const FString& Text, FPropertyTarget& Out, FString& OutError)
	{
		TArray<FString> Parts;
		Text.TrimStartAndEnd().ParseIntoArray(Parts, TEXT("."));
		if (Parts.IsEmpty())
		{
			OutError = TEXT("Name the Property, as Component.Property or Property.");
			return false;
		}
		UObject* Object = &Actor;
		int32 First = 0;
		for (UActorComponent* Component : Actor.GetComponents())
		{
			if (Component && Parts.Num() > 1 && (Component->GetName().Equals(Parts[0], ESearchCase::IgnoreCase)
				|| Component->GetClass()->GetName().Equals(Parts[0], ESearchCase::IgnoreCase)
				|| Component->GetClass()->GetName().Equals(Parts[0] + TEXT("Component"), ESearchCase::IgnoreCase)))
			{
				Object = Component;
				Out.Component = Component;
				First = 1;
				break;
			}
		}
		const UStruct* Owner = Object->GetClass();
		const FProperty* Property = nullptr;
		TArray<FString> Path;
		for (int32 Index = First; Index < Parts.Num(); ++Index)
		{
			Property = Owner ? Owner->FindPropertyByName(FName(*Parts[Index])) : nullptr;
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no property %s."), *Object->GetClass()->GetName(), *FString::Join(TArrayView<const FString>(Parts).RightChop(First), TEXT(".")));
				return false;
			}
			Path.Add(Property->GetName());
			const FStructProperty* Struct = CastField<FStructProperty>(Property);
			Owner = Struct ? Struct->Struct : nullptr;
		}
		if (!Property || !(Property->IsA<FFloatProperty>() || Property->IsA<FDoubleProperty>() || Property->IsA<FBoolProperty>()))
		{
			OutError = FString::Printf(TEXT("%s is not a float, double or bool property; keys.property keys those."), *Text);
			return false;
		}
		Out.Object = Object;
		Out.Property = Property;
		Out.Leaf = Property->GetFName();
		Out.Path = FString::Join(Path, TEXT("."));
		return true;
	}

	// --- Cameras --------------------------------------------------------------------------------------------------

	ACineCameraActor* PlaceCamera(UWorld& World, const FString& Label, const FVector& Location, const FRotator& Rotation, const FString& Folder)
	{
		FActorSpawnParameters Parameters;
		Parameters.ObjectFlags |= RF_Transactional;
		ACineCameraActor* Camera = World.SpawnActor<ACineCameraActor>(Location, Rotation, Parameters);
		if (!Camera) return nullptr;
		Camera->SetActorLabel(Label);
		Camera->SetFolderPath(FName(*Folder));
		return Camera;
	}

	bool IsFilmback(const FString& Name)
	{
		return UCineCameraSettings::GetFilmbackPresets().ContainsByPredicate([&Name](const FNamedFilmbackPreset& Preset) { return Preset.Name.Equals(Name, ESearchCase::IgnoreCase); });
	}

	FString FilmbackNames()
	{
		TArray<FString> Names;
		for (const FNamedFilmbackPreset& Preset : UCineCameraSettings::GetFilmbackPresets()) Names.Add(Preset.Name);
		return FString::Join(Names, TEXT(", "));
	}

	/** The camera's filmback, lens and aperture as the op asks; the aspect ratio letterboxes by narrowing the sensor. */
	void SetLens(ACineCameraActor& Camera, const FHyperAISequenceOp& Op, const float FocalLength, const float Aperture)
	{
		UCineCameraComponent* Lens = Camera.GetCineCameraComponent();
		Lens->Modify();
		if (!Op.Filmback.TrimStartAndEnd().IsEmpty())
		{
			for (const FNamedFilmbackPreset& Preset : UCineCameraSettings::GetFilmbackPresets())
			{
				if (Preset.Name.Equals(Op.Filmback.TrimStartAndEnd(), ESearchCase::IgnoreCase)) Lens->SetFilmbackPresetByName(Preset.Name);
			}
		}
		if (Op.AspectRatio > 0.f)
		{
			FCameraFilmbackSettings Filmback = Lens->Filmback;
			Filmback.SensorHeight = Filmback.SensorWidth / Op.AspectRatio;
			Lens->SetFilmback(Filmback);
		}
		if (FocalLength > 0.f) Lens->SetCurrentFocalLength(FocalLength);
		if (Aperture > 0.f) Lens->SetCurrentAperture(Aperture);
	}

	void FocusOn(ACineCameraActor& Camera, AActor* Subject, const double HeightAboveOrigin)
	{
		UCineCameraComponent* Lens = Camera.GetCineCameraComponent();
		FCameraFocusSettings Focus = Lens->FocusSettings;
		if (Subject)
		{
			Focus.FocusMethod = ECameraFocusMethod::Tracking;
			Focus.TrackingFocusSettings.ActorToTrack = Subject;
			Focus.TrackingFocusSettings.RelativeOffset = FVector(0.0, 0.0, HeightAboveOrigin);
			Focus.bSmoothFocusChanges = false;
		}
		Lens->SetFocusSettings(Focus);
	}

	// --- Shot language --------------------------------------------------------------------------------------------

	/** A shot size: how much of the subject shows (from the top down), how full the frame is, a lens for it, a stop. */
	struct FFraming
	{
		const TCHAR* Name;
		double Portion;
		double Fill;
		float Lens;
		float Aperture;
		int32 Size;
	};

	const TArray<FFraming>& Framings()
	{
		static const TArray<FFraming> All = {
			{TEXT("extreme_wide"), 1.0, 0.10, 18.f, 8.f, 0},
			{TEXT("wide"), 1.0, 0.35, 24.f, 5.6f, 1},
			{TEXT("full"), 1.0, 0.87, 35.f, 4.f, 2},
			{TEXT("medium_wide"), 0.66, 0.9, 35.f, 4.f, 3},
			{TEXT("medium"), 0.5, 0.88, 50.f, 2.8f, 4},
			{TEXT("medium_close"), 0.33, 0.85, 65.f, 2.8f, 5},
			{TEXT("close"), 0.21, 0.8, 85.f, 2.f, 6},
			{TEXT("extreme_close"), 0.1, 0.95, 100.f, 2.f, 7}};
		return All;
	}

	const FFraming* FindFraming(const FString& Name)
	{
		FString Wanted = Lower(Name);
		if (Wanted == TEXT("ews") || Wanted == TEXT("establishing")) Wanted = TEXT("extreme_wide");
		else if (Wanted == TEXT("ws") || Wanted == TEXT("long")) Wanted = TEXT("wide");
		else if (Wanted == TEXT("fs") || Wanted == TEXT("full_body")) Wanted = TEXT("full");
		else if (Wanted == TEXT("cowboy") || Wanted == TEXT("mws")) Wanted = TEXT("medium_wide");
		else if (Wanted == TEXT("ms")) Wanted = TEXT("medium");
		else if (Wanted == TEXT("mcu")) Wanted = TEXT("medium_close");
		else if (Wanted == TEXT("cu") || Wanted == TEXT("close_up")) Wanted = TEXT("close");
		else if (Wanted == TEXT("ecu") || Wanted == TEXT("extreme_close_up")) Wanted = TEXT("extreme_close");
		return Framings().FindByPredicate([&Wanted](const FFraming& Framing) { return Wanted == Framing.Name; });
	}

	/** A subject's standing volume: its feet, height, eyes and facing, from its collision if it has any. */
	struct FSubject
	{
		AActor* Actor = nullptr;
		FVector Base = FVector::ZeroVector;
		double Height = 180.0;
		double Radius = 40.0;
		double Eye = 0.0;
		FVector Forward = FVector::ForwardVector;
	};

	FSubject DescribeSubject(AActor& Actor)
	{
		FSubject Out;
		Out.Actor = &Actor;
		FVector Origin;
		FVector Extent;
		Actor.GetActorBounds(/*bOnlyCollidingComponents=*/true, Origin, Extent);
		if (Extent.IsNearlyZero()) Actor.GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent);
		if (Extent.IsNearlyZero())
		{
			Origin = Actor.GetActorLocation();
			Extent = FVector(40.0, 40.0, 90.0);
		}
		Out.Height = FMath::Max(2.0 * Extent.Z, 10.0);
		Out.Radius = FMath::Max(FMath::Max(Extent.X, Extent.Y), 5.0);
		Out.Base = FVector(Origin.X, Origin.Y, Origin.Z - Extent.Z);
		// A standing person's eyes are about 93% of the way up.
		Out.Eye = Out.Base.Z + Out.Height * 0.93;
		const FVector Facing = Actor.GetActorForwardVector().GetSafeNormal2D();
		Out.Forward = Facing.IsNearlyZero() ? FVector::ForwardVector : Facing;
		return Out;
	}

	bool SideAngle(const FString& Side, double& OutDegrees)
	{
		const FString Value = Lower(Side);
		if (Value.IsEmpty() || Value == TEXT("front")) OutDegrees = 0.0;
		else if (Value == TEXT("three_quarter_left") || Value == TEXT("3/4_left")) OutDegrees = -40.0;
		else if (Value == TEXT("three_quarter_right") || Value == TEXT("3/4_right")) OutDegrees = 40.0;
		else if (Value == TEXT("profile_left") || Value == TEXT("left")) OutDegrees = -90.0;
		else if (Value == TEXT("profile_right") || Value == TEXT("right")) OutDegrees = 90.0;
		else if (Value == TEXT("back") || Value == TEXT("behind")) OutDegrees = 180.0;
		else return false;
		return true;
	}

	bool IsAngle(const FString& Angle)
	{
		static const TSet<FString> Angles = {TEXT(""), TEXT("eye"), TEXT("eye_level"), TEXT("high"), TEXT("low"), TEXT("overhead"), TEXT("birds_eye"), TEXT("dutch")};
		return Angles.Contains(Lower(Angle));
	}

	bool IsMove(const FString& Move)
	{
		static const TSet<FString> Moves = {TEXT(""), TEXT("static"), TEXT("push_in"), TEXT("pull_out"), TEXT("truck_left"), TEXT("truck_right"), TEXT("pedestal_up"),
			TEXT("pedestal_down"), TEXT("orbit_left"), TEXT("orbit_right"), TEXT("crane_up"), TEXT("crane_down"), TEXT("dolly_zoom")};
		return Moves.Contains(Lower(Move));
	}

	/** A shot solved: where the camera looks, from how far, from which direction, and through what lens. */
	struct FShot
	{
		FVector Aim = FVector::ZeroVector;
		double Distance = 0.0;
		double Yaw = 0.0;
		double Elevation = 0.0;
		double Roll = 0.0;
		double Visible = 0.0;
		double Floor = 0.0;
		float Lens = 50.f;
		FString Move;
	};

	struct FPose
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		float Lens = 50.f;
	};

	/** The camera at a point through the shot, T from 0 to 1 already eased. */
	FPose PoseAt(const FShot& Shot, const double T)
	{
		double Distance = Shot.Distance;
		double Yaw = Shot.Yaw;
		double Elevation = Shot.Elevation;
		float Lens = Shot.Lens;
		FVector Offset = FVector::ZeroVector;
		const FString& Move = Shot.Move;
		if (Move == TEXT("push_in")) Distance *= FMath::Lerp(1.35, 1.0, T);
		else if (Move == TEXT("pull_out")) Distance *= FMath::Lerp(1.0, 1.45, T);
		else if (Move == TEXT("orbit_left")) Yaw -= 35.0 * T;
		else if (Move == TEXT("orbit_right")) Yaw += 35.0 * T;
		else if (Move == TEXT("crane_up")) Elevation = FMath::Min(Elevation + 30.0 * T, 80.0);
		else if (Move == TEXT("crane_down")) Elevation = FMath::Min(Elevation + 30.0 * (1.0 - T), 80.0);
		else if (Move == TEXT("dolly_zoom"))
		{
			// The subject keeps its size while the background swells: the lens widens as the camera closes in.
			Distance *= FMath::Lerp(1.0, 0.55, T);
			Lens *= static_cast<float>(FMath::Lerp(1.0, 0.55, T));
		}
		const FVector Toward = FRotator(Elevation, Yaw, 0.0).Vector();
		FPose Pose;
		Pose.Location = Shot.Aim + Toward * Distance;
		const FVector Across = FRotator(0.0, Yaw + 90.0, 0.0).Vector();
		if (Move == TEXT("truck_left")) Offset = -Across * (0.4 * Shot.Distance * T);
		else if (Move == TEXT("truck_right")) Offset = Across * (0.4 * Shot.Distance * T);
		else if (Move == TEXT("pedestal_up")) Offset.Z = 0.35 * Shot.Visible * T;
		else if (Move == TEXT("pedestal_down")) Offset.Z = -0.35 * Shot.Visible * T;
		Pose.Location += Offset;
		Pose.Location.Z = FMath::Max(Pose.Location.Z, Shot.Floor);
		Pose.Rotation = (Shot.Aim - Pose.Location).Rotation();
		Pose.Rotation.Roll = Shot.Roll;
		Pose.Lens = Lens;
		return Pose;
	}

	/** The shot a shot.frame op asks for, solved against the camera's sensor. */
	bool SolveShot(UWorld& World, const FHyperAISequenceOp& Op, const double SensorWidth, const double SensorHeight, FShot& Out, TArray<FSubject>& OutSubjects,
		FSubject& OutOver, FString& OutError)
	{
		for (const FString& Label : Op.Subjects)
		{
			AActor* Actor = FindActor(World, Label);
			if (!Actor)
			{
				OutError = FString::Printf(TEXT("No actor is labelled %s."), *Label);
				return false;
			}
			OutSubjects.Add(DescribeSubject(*Actor));
		}
		const bool bOver = !Op.Over.TrimStartAndEnd().IsEmpty();
		const FFraming* Framing = FindFraming(Op.Framing.IsEmpty() ? FString(bOver ? TEXT("medium_close") : TEXT("medium")) : Op.Framing);
		const FSubject& Main = OutSubjects[0];
		const bool bPortrait = Framing->Portion < 1.0;
		// How much of the scene the frame must hold at the subject, top to bottom.
		double Visible = Framing->Portion * Main.Height / Framing->Fill;
		FVector Aim(Main.Base.X, Main.Base.Y, 0.0);
		// Portraits put the eyes on the upper third; wider shots keep the whole figure in, a little low in the frame.
		Aim.Z = bPortrait ? Main.Eye - Visible / 6.0 : Main.Base.Z + Main.Height * 0.5 + (Visible - Main.Height) * 0.15;
		double Yaw = 0.0;
		double Width = 0.0;
		if (OutSubjects.Num() > 1)
		{
			// A two-shot: aimed between them, from the side they face, wide enough to hold both.
			const FSubject& Other = OutSubjects[1];
			const FVector Line = FVector(Other.Base.X - Main.Base.X, Other.Base.Y - Main.Base.Y, 0.0);
			Aim.X = (Main.Base.X + Other.Base.X) * 0.5;
			Aim.Y = (Main.Base.Y + Other.Base.Y) * 0.5;
			FVector Facing = FRotator(0.0, Line.Rotation().Yaw + 90.0, 0.0).Vector();
			if (FVector::DotProduct(Facing, Main.Forward + Other.Forward) < 0.0) Facing = -Facing;
			Yaw = Facing.Rotation().Yaw;
			Width = (Line.Size() + Main.Radius + Other.Radius) / 0.8;
		}
		else
		{
			Yaw = Main.Forward.Rotation().Yaw;
		}
		double Side = 0.0;
		SideAngle(Op.Side, Side);
		Yaw += Side;
		float Lens = Op.FocalLength > 0.f ? Op.FocalLength : Framing->Lens;
		double Distance = Visible * Lens / SensorHeight;
		if (Width > 0.0) Distance = FMath::Max(Distance, Width * Lens / SensorWidth);
		double Elevation = 0.0;
		double Roll = 0.0;
		const FString Angle = Lower(Op.Angle);
		if (bOver)
		{
			// Over the shoulder: the camera just behind the foreground actor's shoulder, the lens chosen to frame the subject.
			AActor* OverActor = FindActor(World, Op.Over);
			if (!OverActor)
			{
				OutError = FString::Printf(TEXT("No actor is labelled %s."), *Op.Over);
				return false;
			}
			OutOver = DescribeSubject(*OverActor);
			const FVector Axis = FVector(OutOver.Base.X - Main.Base.X, OutOver.Base.Y - Main.Base.Y, 0.0).GetSafeNormal();
			const FVector Shoulder = FRotator(0.0, (-Axis).Rotation().Yaw + 90.0, 0.0).Vector() * (Lower(Op.Side).Contains(TEXT("left")) ? -1.0 : 1.0);
			const FVector Camera = FVector(OutOver.Base.X, OutOver.Base.Y, OutOver.Eye + 8.0) + Axis * (OutOver.Radius + 45.0) + Shoulder * (OutOver.Radius + 10.0);
			const FVector Toward = Camera - Aim;
			Distance = Toward.Size();
			Yaw = Toward.Rotation().Yaw;
			Elevation = Toward.Rotation().Pitch;
			Lens = Op.FocalLength > 0.f ? Op.FocalLength : FMath::Clamp(static_cast<float>(Distance * SensorHeight / Visible), 18.f, 200.f);
		}
		else if (Angle == TEXT("high")) Elevation = 25.0;
		else if (Angle == TEXT("low")) Elevation = -18.0;
		else if (Angle == TEXT("overhead") || Angle == TEXT("birds_eye")) Elevation = 80.0;
		else
		{
			// Eye level: the camera at the subject's eyes, looking at the frame's center.
			Elevation = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp((Main.Eye - Aim.Z) / FMath::Max(Distance, 1.0), -0.5, 0.5)));
			if (Angle == TEXT("dutch")) Roll = Side < 0.0 ? -15.0 : 15.0;
		}
		Out.Aim = Aim;
		Out.Distance = Distance;
		Out.Yaw = Yaw;
		Out.Elevation = Elevation;
		Out.Roll = Roll;
		Out.Visible = Visible;
		Out.Lens = Lens;
		Out.Floor = Main.Base.Z + 20.0;
		Out.Move = Lower(Op.Move).IsEmpty() ? FString(TEXT("static")) : Lower(Op.Move);
		return true;
	}

	/** A slow, small wander, the same for the same shot. */
	FVector Drift(const double Seconds, const double Seed)
	{
		const float At = static_cast<float>(Seconds * 0.7 + Seed);
		return FVector(FMath::PerlinNoise1D(At), FMath::PerlinNoise1D(At + 17.3f), FMath::PerlinNoise1D(At + 41.1f));
	}

	// --- Review ---------------------------------------------------------------------------------------------------

	struct FCut
	{
		double Start = 0.0;
		double End = 0.0;
		FString Camera;
		ACineCameraActor* Actor = nullptr;
		FVector Location = FVector::ZeroVector;
		TArray<AActor*> Subjects;
		AActor* Over = nullptr;
		int32 Size = INDEX_NONE;
	};

	FString Tagged(const AActor& Actor, const TCHAR* Prefix)
	{
		for (const FName& Tag : Actor.Tags)
		{
			const FString Text = Tag.ToString();
			if (Text.StartsWith(Prefix)) return Text.RightChop(FCString::Strlen(Prefix));
		}
		return FString();
	}

	TArray<FCut> ReadCuts(UWorld& World, ULevelSequence& Sequence)
	{
		TArray<FCut> Cuts;
		UMovieScene* Scene = Sequence.GetMovieScene();
		const UMovieSceneCameraCutTrack* Track = Scene ? Cast<UMovieSceneCameraCutTrack>(Scene->GetCameraCutTrack()) : nullptr;
		if (!Track) return Cuts;
		for (UMovieSceneSection* Section : Track->GetAllSections())
		{
			const UMovieSceneCameraCutSection* Cut = Cast<UMovieSceneCameraCutSection>(Section);
			if (!Cut || !Cut->HasStartFrame()) continue;
			FCut& Out = Cuts.AddDefaulted_GetRef();
			Out.Start = ToSeconds(*Scene, Cut->GetInclusiveStartFrame());
			Out.End = Cut->HasEndFrame() ? ToSeconds(*Scene, Cut->GetExclusiveEndFrame()) : Out.Start;
			const FGuid Guid = Cut->GetCameraBindingID().GetGuid();
			Out.Camera = BindingName(*Scene, Guid);
			Out.Actor = FindCamera(World, Out.Camera);
			if (!Out.Actor) continue;
			Out.Location = Out.Actor->GetActorLocation();
			if (const UMovieScene3DTransformTrack* Transform = Scene->FindTrack<UMovieScene3DTransformTrack>(Guid))
			{
				for (UMovieSceneSection* Keys : Transform->GetAllSections())
				{
					FRotator Unused;
					if (const UMovieScene3DTransformSection* Typed = Cast<UMovieScene3DTransformSection>(Keys)) EvaluateTransform(*Typed, Cut->GetInclusiveStartFrame(), Out.Location, Unused);
				}
			}
			TArray<FString> Labels;
			Tagged(*Out.Actor, SubjectTag).ParseIntoArray(Labels, TEXT("|"));
			for (const FString& Label : Labels)
			{
				if (AActor* Subject = FindActor(World, Label)) Out.Subjects.Add(Subject);
			}
			Out.Over = FindActor(World, Tagged(*Out.Actor, OverTag));
			const FString Size = Tagged(*Out.Actor, SizeTag);
			Out.Size = Size.IsEmpty() ? INDEX_NONE : FCString::Atoi(*Size);
		}
		Cuts.Sort([](const FCut& A, const FCut& B) { return A.Start < B.Start; });
		return Cuts;
	}

	/** The cut as an editor reviews it: gaps, flash cuts, jump cuts, crossing the line, hidden subjects, buried cameras. */
	TArray<FString> Review(UWorld& World, ULevelSequence& Sequence)
	{
		TArray<FString> Notes;
		UMovieScene* Scene = Sequence.GetMovieScene();
		if (!Scene) return Notes;
		const TArray<FCut> Cuts = ReadCuts(World, Sequence);
		if (Cuts.IsEmpty()) return Notes;
		const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
		const double Begin = ToSeconds(*Scene, UE::MovieScene::DiscreteInclusiveLower(Range));
		const double Finish = ToSeconds(*Scene, UE::MovieScene::DiscreteExclusiveUpper(Range));
		if (Cuts[0].Start > Begin + 0.05) Notes.Add(FString::Printf(TEXT("No camera from %.2f to %.2f s: the viewer sees the level's own camera."), Begin, Cuts[0].Start));
		if (Cuts.Last().End + 0.05 < Finish) Notes.Add(FString::Printf(TEXT("No camera from %.2f to %.2f s."), Cuts.Last().End, Finish));
		// The line of action: set by the latest shot that holds two people, and kept by the shots of either of them.
		AActor* LineA = nullptr;
		AActor* LineB = nullptr;
		double LineSide = 0.0;
		for (int32 Index = 0; Index < Cuts.Num(); ++Index)
		{
			const FCut& Cut = Cuts[Index];
			const FString When = FString::Printf(TEXT("%.2f s (%s)"), Cut.Start, *Cut.Camera);
			if (Cut.End - Cut.Start < 1.0) Notes.Add(When + FString::Printf(TEXT(": a %.2f s shot flashes by; hold it at least a second unless the flash is the point."), Cut.End - Cut.Start));
			if (!Cut.Actor) continue;
			FCollisionQueryParams Query(SCENE_QUERY_STAT(HyperAISequenceReview), /*bTraceComplex=*/true, Cut.Actor);
			if (World.OverlapAnyTestByChannel(Cut.Location, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(12.f), Query))
			{
				Notes.Add(When + TEXT(": the camera sits inside geometry."));
			}
			for (AActor* Subject : Cut.Subjects)
			{
				const FSubject Info = DescribeSubject(*Subject);
				FCollisionQueryParams Sight(SCENE_QUERY_STAT(HyperAISequenceSight), /*bTraceComplex=*/true, Cut.Actor);
				Sight.AddIgnoredActor(Subject);
				if (Cut.Over) Sight.AddIgnoredActor(Cut.Over);
				FHitResult Hit;
				if (World.LineTraceSingleByChannel(Hit, Cut.Location, FVector(Info.Base.X, Info.Base.Y, Info.Eye), ECC_Visibility, Sight) && Hit.GetActor())
				{
					Notes.Add(When + FString::Printf(TEXT(": %s is hidden behind %s."), *Subject->GetActorLabel(), *Hit.GetActor()->GetActorLabel()));
				}
			}
			if (Index > 0 && Cuts[Index - 1].Actor)
			{
				const FCut& Before = Cuts[Index - 1];
				for (AActor* Subject : Cut.Subjects)
				{
					if (!Before.Subjects.Contains(Subject)) continue;
					const FVector At = Subject->GetActorLocation();
					const FVector A = (Before.Location - At).GetSafeNormal2D();
					const FVector B = (Cut.Location - At).GetSafeNormal2D();
					const double Turn = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(A, B), -1.0, 1.0)));
					const bool bSizeKnown = Before.Size != INDEX_NONE && Cut.Size != INDEX_NONE;
					if (Turn < 30.0 && (!bSizeKnown || FMath::Abs(Before.Size - Cut.Size) < 2))
					{
						Notes.Add(When + FString::Printf(TEXT(": a jump cut on %s from the shot before (%.0f degrees around it%s); move the camera 30 degrees or more, or change the size two steps."),
							*Subject->GetActorLabel(), Turn, bSizeKnown ? TEXT(", nearly the same size") : TEXT("")));
					}
				}
			}
			TArray<AActor*> Pair = Cut.Subjects;
			if (Cut.Over) Pair.AddUnique(Cut.Over);
			// A new pair of people draws a new line; the same pair, from either side of the table, keeps it.
			if (Pair.Num() == 2 && !(Pair.Contains(LineA) && Pair.Contains(LineB)))
			{
				LineA = Pair[0];
				LineB = Pair[1];
				LineSide = 0.0;
			}
			if (LineA && LineB && (Pair.Contains(LineA) || Pair.Contains(LineB)))
			{
				const FVector Line = (LineB->GetActorLocation() - LineA->GetActorLocation()).GetSafeNormal2D();
				// How far the camera stands off the line, in cm, signed by the side it is on.
				const double Side = FVector::CrossProduct(Line, Cut.Location - LineA->GetActorLocation()).Z;
				if (LineSide != 0.0 && FMath::Sign(Side) != FMath::Sign(LineSide) && FMath::Abs(Side) > 10.0)
				{
					Notes.Add(When + FString::Printf(TEXT(": the camera crosses the line between %s and %s, so they swap sides of the screen."), *LineA->GetActorLabel(), *LineB->GetActorLabel()));
				}
				if (FMath::Abs(Side) > 10.0) LineSide = Side;
			}
		}
		return Notes;
	}

	// --- Plan -----------------------------------------------------------------------------------------------------

	/** What the check knows the plan will have made by each op: the sequence, and the cameras. */
	struct FPlanned
	{
		bool bSequence = false;
		TSet<FString> Cameras;
	};

	bool CheckKeys(const TArray<FHyperAISequenceKey>& Keys, FString& OutError)
	{
		if (Keys.IsEmpty() || Keys.Num() > C::MaxKeysPerOp)
		{
			OutError = FString::Printf(TEXT("Give 1 to %d Keys."), C::MaxKeysPerOp);
			return false;
		}
		for (const FHyperAISequenceKey& Key : Keys)
		{
			EInterp Interp;
			if (!(Key.Time >= 0.f && Key.Time <= C::MaxSeconds))
			{
				OutError = FString::Printf(TEXT("A key's Time is 0 to %.0f seconds."), C::MaxSeconds);
				return false;
			}
			if (!ParseInterp(Key.Interp, Interp))
			{
				OutError = FString::Printf(TEXT("'%s' is not an Interp: auto, linear or constant."), *Key.Interp);
				return false;
			}
		}
		return true;
	}

	bool Check(UWorld& World, const FHyperAISequencePlanRequest& Request, FString& OutError)
	{
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		const FString Path = PackageOf(Request.Sequence);
		FPlanned Plan;
		Plan.bSequence = Load<ULevelSequence>(Path) != nullptr;
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAISequenceOp& Op = Request.Operations[Index];
			const FString Kind = KindOf(Op);
			auto Fail = [&OutError, &Op, Index](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Kind, *Message);
				return false;
			};
			FString Why;
			FFrameRate Rate;
			if (Op.FrameRate != 0.f && !ParseFrameRate(Op.FrameRate, Rate)) return Fail(TEXT("FrameRate is a whole number of frames per second, or 23.976, 29.97 or 59.94."));
			if (!(Op.Time >= 0.f && Op.Time <= C::MaxSeconds) || !(Op.Duration >= 0.f && Op.Duration <= C::MaxSeconds))
			{
				return Fail(FString::Printf(TEXT("Time and Duration are 0 to %.0f seconds."), C::MaxSeconds));
			}
			if (!Op.Filmback.IsEmpty() && !IsFilmback(Op.Filmback.TrimStartAndEnd())) return Fail(FString::Printf(TEXT("'%s' is not a filmback; they are %s."), *Op.Filmback, *FilmbackNames()));
			if (Op.AspectRatio != 0.f && !(Op.AspectRatio >= 0.5f && Op.AspectRatio <= 4.f)) return Fail(TEXT("AspectRatio is 0.5 to 4 (2.39 for widescreen)."));
			if (Op.FocalLength != 0.f && !(Op.FocalLength >= 4.f && Op.FocalLength <= 1000.f)) return Fail(TEXT("FocalLength is 4 to 1000 mm."));
			if (Op.Aperture != 0.f && !(Op.Aperture >= 0.7f && Op.Aperture <= 32.f)) return Fail(TEXT("Aperture is f/0.7 to f/32."));
			if (!Op.FocusOn.IsEmpty() && !FindActor(World, Op.FocusOn)) return Fail(FString::Printf(TEXT("No actor is labelled %s."), *Op.FocusOn));
			if (Kind == TEXT("sequence.create"))
			{
				if (Plan.bSequence) return Fail(FString::Printf(TEXT("%s exists already; sequence.set changes it."), *Path));
				if (!Path.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Path) || FPackageName::DoesPackageExist(Path))
				{
					return Fail(TEXT("Name the plan's Sequence as a free path under /Game, like /Game/Cinematics/LS_Intro."));
				}
				Plan.bSequence = true;
				continue;
			}
			if (!Plan.bSequence) return Fail(FString::Printf(TEXT("%s is not a level sequence; start the plan with sequence.create."), *Request.Sequence));
			if (Kind == TEXT("sequence.set"))
			{
				if (Op.FrameRate == 0.f && Op.Duration == 0.f) return Fail(TEXT("Give the FrameRate or Duration to change."));
			}
			else if (Kind == TEXT("actors.bind"))
			{
				if (Op.Actors.IsEmpty()) return Fail(TEXT("Name the Actors to bind."));
				for (const FString& Label : Op.Actors)
				{
					if (!FindActor(World, Label)) return Fail(FString::Printf(TEXT("No actor is labelled %s."), *Label));
				}
			}
			else if (Kind == TEXT("camera.add"))
			{
				if (Op.Camera.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("Name the Camera."));
				if (FindActor(World, Op.Camera) || Plan.Cameras.Contains(Op.Camera.ToLower())) return Fail(FString::Printf(TEXT("An actor is labelled %s already."), *Op.Camera));
				if (!Op.LookAt.IsEmpty() && !FindActor(World, Op.LookAt)) return Fail(FString::Printf(TEXT("No actor is labelled %s."), *Op.LookAt));
				Plan.Cameras.Add(Op.Camera.ToLower());
			}
			else if (Kind == TEXT("keys.transform") || Kind == TEXT("keys.property"))
			{
				AActor* Actor = FindActor(World, Op.Actor);
				const bool bPlannedCamera = Plan.Cameras.Contains(Op.Actor.ToLower());
				if (!Actor && !bPlannedCamera) return Fail(FString::Printf(TEXT("No actor is labelled %s."), *Op.Actor));
				if (!CheckKeys(Op.Keys, Why)) return Fail(Why);
				if (Kind == TEXT("keys.property"))
				{
					// A camera the plan makes is checked against a camera's class defaults.
					AActor* Target = Actor ? Actor : GetMutableDefault<ACineCameraActor>();
					FPropertyTarget Property;
					if (!ResolveProperty(*Target, Op.Property, Property, Why)) return Fail(Why);
				}
			}
			else if (Kind == TEXT("cut.add"))
			{
				if (!FindCamera(World, Op.Camera) && !Plan.Cameras.Contains(Op.Camera.ToLower())) return Fail(FString::Printf(TEXT("No cine camera is labelled %s."), *Op.Camera));
			}
			else if (Kind == TEXT("fade.add"))
			{
				if (!CheckKeys(Op.Keys, Why)) return Fail(Why);
			}
			else if (Kind == TEXT("audio.add"))
			{
				if (!Load<USoundBase>(Op.Asset)) return Fail(FString::Printf(TEXT("%s is not a sound."), *Op.Asset));
			}
			else if (Kind == TEXT("shot.add"))
			{
				const ULevelSequence* Shot = Load<ULevelSequence>(Op.Asset);
				if (!Shot) return Fail(FString::Printf(TEXT("%s is not a level sequence."), *Op.Asset));
				if (PackageOf(Op.Asset).Equals(Path, ESearchCase::IgnoreCase)) return Fail(TEXT("A sequence cannot be a shot of itself."));
			}
			else if (Kind == TEXT("shot.frame"))
			{
				if (Op.Subjects.IsEmpty() || Op.Subjects.Num() > 2) return Fail(TEXT("Name one or two Subjects."));
				for (const FString& Label : Op.Subjects)
				{
					if (!FindActor(World, Label)) return Fail(FString::Printf(TEXT("No actor is labelled %s."), *Label));
				}
				if (!Op.Over.IsEmpty() && !FindActor(World, Op.Over)) return Fail(FString::Printf(TEXT("No actor is labelled %s."), *Op.Over));
				if (!Op.Over.IsEmpty() && Op.Subjects.Num() != 1) return Fail(TEXT("An over-the-shoulder shot has one Subject."));
				if (!Op.Framing.IsEmpty() && !FindFraming(Op.Framing))
				{
					return Fail(TEXT("Framing is extreme_wide, wide, full, medium_wide, medium, medium_close, close or extreme_close."));
				}
				double Side = 0.0;
				if (!SideAngle(Op.Side, Side)) return Fail(TEXT("Side is front, three_quarter_left, three_quarter_right, profile_left, profile_right or back."));
				if (!IsAngle(Op.Angle)) return Fail(TEXT("Angle is eye, high, low, overhead or dutch."));
				if (!IsMove(Op.Move))
				{
					return Fail(TEXT("Move is static, push_in, pull_out, truck_left, truck_right, pedestal_up, pedestal_down, orbit_left, orbit_right, crane_up, crane_down or dolly_zoom."));
				}
				const AActor* Existing = FindActor(World, Op.Camera);
				if (Existing && !Existing->IsA<ACineCameraActor>()) return Fail(FString::Printf(TEXT("%s is not a cine camera."), *Op.Camera));
				if (!Op.Camera.IsEmpty()) Plan.Cameras.Add(Op.Camera.ToLower());
			}
			else
			{
				return Fail(TEXT("Unknown op; see hyper_sequence_apply_plan's description."));
			}
		}
		return true;
	}

	/** A camera label for a framed shot that none has yet. */
	FString NewCameraLabel(UWorld& World, const FString& SequenceName)
	{
		for (int32 Index = 1;; ++Index)
		{
			const FString Label = FString::Printf(TEXT("Cam_%s_%02d"), *SequenceName, Index);
			if (!FindActor(World, Label)) return Label;
		}
	}

	/** One op, inside the plan's undo step. */
	bool Apply(UWorld& World, ULevelSequence*& Sequence, const FString& Path, const FHyperAISequenceOp& Op, FHyperAISequencePlanReport& Report, FString& OutError)
	{
		const FString Kind = KindOf(Op);
		if (Kind == TEXT("sequence.create"))
		{
			UPackage* Package = CreatePackage(*Path);
			Sequence = NewObject<ULevelSequence>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
			Sequence->Initialize();
			UMovieScene* Scene = Sequence->GetMovieScene();
			FFrameRate Rate(24, 1);
			if (Op.FrameRate > 0.f) ParseFrameRate(Op.FrameRate, Rate);
			Scene->SetDisplayRate(Rate);
			const double Seconds = Op.Duration > 0.f ? Op.Duration : 10.0;
			Scene->SetPlaybackRange(FFrameNumber(0), ToFrame(*Scene, Seconds).Value);
			FAssetRegistryModule::AssetCreated(Sequence);
			Package->MarkPackageDirty();
			Report.Notes.Add(FString::Printf(TEXT("%s made: %s, %.2f s."), *Sequence->GetPathName(), *Rate.ToPrettyText().ToString(), Seconds));
			return true;
		}
		if (!Sequence)
		{
			OutError = TEXT("The plan's sequence is not there.");
			return false;
		}
		UMovieScene* Scene = Sequence->GetMovieScene();
		Sequence->Modify();
		Scene->Modify();
		const FString Folder = TEXT("Cinematics/") + Sequence->GetName();
		if (Kind == TEXT("sequence.set"))
		{
			FFrameRate Rate;
			if (Op.FrameRate > 0.f && ParseFrameRate(Op.FrameRate, Rate)) Scene->SetDisplayRate(Rate);
			if (Op.Duration > 0.f)
			{
				const FFrameNumber Start = UE::MovieScene::DiscreteInclusiveLower(Scene->GetPlaybackRange());
				Scene->SetPlaybackRange(Start, ToFrame(*Scene, Op.Duration).Value);
			}
			Report.Notes.Add(FString::Printf(TEXT("Sequence set: %s, %.2f s."), *Scene->GetDisplayRate().ToPrettyText().ToString(),
				ToSeconds(*Scene, FFrameNumber(UE::MovieScene::DiscreteSize(Scene->GetPlaybackRange())))));
			return true;
		}
		if (Kind == TEXT("actors.bind"))
		{
			for (const FString& Label : Op.Actors)
			{
				AActor* Actor = FindActor(World, Label);
				if (!Actor)
				{
					OutError = FString::Printf(TEXT("No actor is labelled %s."), *Label);
					return false;
				}
				BindActor(*Sequence, *Actor, World);
			}
			Report.Notes.Add(FString::Printf(TEXT("%d actor(s) bound."), Op.Actors.Num()));
			return true;
		}
		if (Kind == TEXT("camera.add"))
		{
			FRotator Rotation = Op.Rotation;
			if (AActor* Target = FindActor(World, Op.LookAt)) Rotation = (Target->GetActorLocation() - Op.Location).Rotation();
			ACineCameraActor* Camera = PlaceCamera(World, Op.Camera.TrimStartAndEnd(), Op.Location, Rotation, Folder);
			if (!Camera)
			{
				OutError = TEXT("The level refused a new camera.");
				return false;
			}
			SetLens(*Camera, Op, Op.FocalLength, Op.Aperture);
			if (AActor* Focus = FindActor(World, Op.FocusOn)) FocusOn(*Camera, Focus, DescribeSubject(*Focus).Eye - Focus->GetActorLocation().Z);
			BindActor(*Sequence, *Camera, World);
			Report.Notes.Add(FString::Printf(TEXT("Camera %s placed at (%.0f, %.0f, %.0f), %.0f mm."), *Camera->GetActorLabel(), Op.Location.X, Op.Location.Y, Op.Location.Z,
				Camera->GetCineCameraComponent()->CurrentFocalLength));
			return true;
		}
		if (Kind == TEXT("keys.transform") || Kind == TEXT("keys.property"))
		{
			AActor* Actor = FindActor(World, Op.Actor);
			if (!Actor)
			{
				OutError = FString::Printf(TEXT("No actor is labelled %s."), *Op.Actor);
				return false;
			}
			Actor->Modify();
			TArray<FHyperAISequenceKey> Keys = Op.Keys;
			Keys.Sort([](const FHyperAISequenceKey& A, const FHyperAISequenceKey& B) { return A.Time < B.Time; });
			const FFrameNumber From = ToFrame(*Scene, Keys[0].Time);
			const FFrameNumber To = ToFrame(*Scene, Keys.Last().Time);
			if (Kind == TEXT("keys.transform"))
			{
				UMovieScene3DTransformSection* Section = TransformSection(*Scene, BindActor(*Sequence, *Actor, World));
				if (!Section)
				{
					OutError = TEXT("The transform track would not take a section.");
					return false;
				}
				Section->Modify();
				ClearTransformKeys(*Section, From, To);
				for (const FHyperAISequenceKey& Key : Keys)
				{
					EInterp Interp;
					ParseInterp(Key.Interp, Interp);
					KeyTransform(*Section, ToFrame(*Scene, Key.Time), Key.Location, Key.Rotation, Interp);
				}
				Report.Notes.Add(FString::Printf(TEXT("%s: %d transform key(s) from %.2f to %.2f s."), *Actor->GetActorLabel(), Keys.Num(), Keys[0].Time, Keys.Last().Time));
			}
			else
			{
				FPropertyTarget Target;
				if (!ResolveProperty(*Actor, Op.Property, Target, OutError)) return false;
				const FGuid Guid = Target.Component ? BindComponent(*Sequence, *Actor, *Target.Component, World) : BindActor(*Sequence, *Actor, World);
				UMovieScenePropertyTrack* Track = nullptr;
				for (UMovieSceneTrack* Existing : Scene->FindBinding(Guid)->GetTracks())
				{
					UMovieScenePropertyTrack* Typed = Cast<UMovieScenePropertyTrack>(Existing);
					if (Typed && Typed->GetPropertyPath().ToString() == Target.Path) Track = Typed;
				}
				if (!Track)
				{
					const UClass* TrackClass = Target.Property->IsA<FFloatProperty>() ? UMovieSceneFloatTrack::StaticClass()
						: Target.Property->IsA<FDoubleProperty>() ? UMovieSceneDoubleTrack::StaticClass() : UMovieSceneBoolTrack::StaticClass();
					Track = Cast<UMovieScenePropertyTrack>(Scene->AddTrack(const_cast<UClass*>(TrackClass), Guid));
					if (Track) Track->SetPropertyNameAndPath(Target.Leaf, Target.Path);
				}
				UMovieSceneSection* Section = Track ? InfiniteSection<UMovieSceneSection>(*Track) : nullptr;
				if (!Section)
				{
					OutError = TEXT("The property track would not take a section.");
					return false;
				}
				Section->Modify();
				FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
				FMovieSceneFloatChannel* Float = Proxy.GetChannel<FMovieSceneFloatChannel>(0);
				FMovieSceneDoubleChannel* Double = Proxy.GetChannel<FMovieSceneDoubleChannel>(0);
				FMovieSceneBoolChannel* Bool = Proxy.GetChannel<FMovieSceneBoolChannel>(0);
				if (Float) ClearKeys(*Float, From, To);
				if (Double) ClearKeys(*Double, From, To);
				if (Bool) ClearKeys(*Bool, From, To);
				for (const FHyperAISequenceKey& Key : Keys)
				{
					EInterp Interp;
					ParseInterp(Key.Interp, Interp);
					const FFrameNumber Frame = ToFrame(*Scene, Key.Time);
					if (Float) AddKey(*Float, Frame, Key.Value, Interp);
					else if (Double) AddKey(*Double, Frame, static_cast<double>(Key.Value), Interp);
					else if (Bool) Bool->GetData().UpdateOrAddKey(Frame, Key.Value > 0.5f);
				}
				Report.Notes.Add(FString::Printf(TEXT("%s.%s: %d key(s) from %.2f to %.2f s."), *Actor->GetActorLabel(), *Target.Path, Keys.Num(), Keys[0].Time, Keys.Last().Time));
			}
			ReachTo(*Scene, Keys.Last().Time);
			return true;
		}
		if (Kind == TEXT("cut.add"))
		{
			ACineCameraActor* Camera = FindCamera(World, Op.Camera);
			if (!Camera)
			{
				OutError = FString::Printf(TEXT("No cine camera is labelled %s."), *Op.Camera);
				return false;
			}
			CutTo(*Scene, BindActor(*Sequence, *Camera, World), ToFrame(*Scene, Op.Time));
			Report.Notes.Add(FString::Printf(TEXT("Cut to %s at %.2f s."), *Camera->GetActorLabel(), Op.Time));
			return true;
		}
		if (Kind == TEXT("fade.add"))
		{
			UMovieSceneFadeTrack* Fade = Scene->FindTrack<UMovieSceneFadeTrack>();
			if (!Fade) Fade = Scene->AddTrack<UMovieSceneFadeTrack>();
			UMovieSceneSection* Section = Fade ? InfiniteSection<UMovieSceneSection>(*Fade) : nullptr;
			FMovieSceneFloatChannel* Channel = Section ? Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr;
			if (!Channel)
			{
				OutError = TEXT("The fade track would not take keys.");
				return false;
			}
			Section->Modify();
			TArray<FHyperAISequenceKey> Keys = Op.Keys;
			Keys.Sort([](const FHyperAISequenceKey& A, const FHyperAISequenceKey& B) { return A.Time < B.Time; });
			ClearKeys(*Channel, ToFrame(*Scene, Keys[0].Time), ToFrame(*Scene, Keys.Last().Time));
			for (const FHyperAISequenceKey& Key : Keys)
			{
				EInterp Interp;
				ParseInterp(Key.Interp, Interp);
				AddKey(*Channel, ToFrame(*Scene, Key.Time), FMath::Clamp(Key.Value, 0.f, 1.f), Interp);
			}
			ReachTo(*Scene, Keys.Last().Time);
			Report.Notes.Add(FString::Printf(TEXT("Fade: %d key(s) from %.2f to %.2f s."), Keys.Num(), Keys[0].Time, Keys.Last().Time));
			return true;
		}
		if (Kind == TEXT("audio.add"))
		{
			USoundBase* Sound = Load<USoundBase>(Op.Asset);
			UMovieSceneAudioTrack* Audio = Scene->AddTrack<UMovieSceneAudioTrack>();
			if (!Sound || !Audio || !Audio->AddNewSound(Sound, ToFrame(*Scene, Op.Time)))
			{
				OutError = FString::Printf(TEXT("%s could not go on an audio track."), *Op.Asset);
				return false;
			}
			// A looping sound has no end to reach for.
			const float Length = Sound->GetDuration();
			if (Length < 3600.f) ReachTo(*Scene, Op.Time + Length);
			Report.Notes.Add(Length < 3600.f ? FString::Printf(TEXT("Audio %s at %.2f s (%.2f s long)."), *Sound->GetName(), Op.Time, Length)
				: FString::Printf(TEXT("Audio %s at %.2f s, looping."), *Sound->GetName(), Op.Time));
			return true;
		}
		if (Kind == TEXT("shot.add"))
		{
			ULevelSequence* Shot = Load<ULevelSequence>(Op.Asset);
			UMovieSceneCinematicShotTrack* Shots = Scene->FindTrack<UMovieSceneCinematicShotTrack>();
			if (!Shots) Shots = Scene->AddTrack<UMovieSceneCinematicShotTrack>();
			if (!Shot || !Shots)
			{
				OutError = FString::Printf(TEXT("%s could not go on the shot track."), *Op.Asset);
				return false;
			}
			const UMovieScene* ShotScene = Shot->GetMovieScene();
			const double Own = ShotScene ? ShotScene->GetTickResolution().AsSeconds(FFrameTime(UE::MovieScene::DiscreteSize(ShotScene->GetPlaybackRange()))) : 0.0;
			const double Seconds = Op.Duration > 0.f ? Op.Duration : Own;
			if (!(Seconds > 0.0))
			{
				OutError = FString::Printf(TEXT("%s has no length; give the shot a Duration."), *Op.Asset);
				return false;
			}
			Shots->Modify();
			Shots->AddSequenceOnRow(Shot, ToFrame(*Scene, Op.Time), ToFrame(*Scene, Seconds).Value, INDEX_NONE);
			ReachTo(*Scene, Op.Time + Seconds);
			Report.Notes.Add(FString::Printf(TEXT("Shot %s at %.2f s for %.2f s."), *Shot->GetName(), Op.Time, Seconds));
			return true;
		}
		if (Kind == TEXT("shot.frame"))
		{
			const FString Label = Op.Camera.TrimStartAndEnd().IsEmpty() ? NewCameraLabel(World, Sequence->GetName()) : Op.Camera.TrimStartAndEnd();
			ACineCameraActor* Camera = FindCamera(World, Label);
			if (!Camera) Camera = PlaceCamera(World, Label, FVector::ZeroVector, FRotator::ZeroRotator, Folder);
			if (!Camera)
			{
				OutError = TEXT("The level refused a new camera.");
				return false;
			}
			Camera->Modify();
			// The sensor first: the framing is solved against it.
			SetLens(*Camera, Op, 0.f, 0.f);
			const FCameraFilmbackSettings& Filmback = Camera->GetCineCameraComponent()->Filmback;
			FShot Shot;
			TArray<FSubject> Subjects;
			FSubject Over;
			if (!SolveShot(World, Op, Filmback.SensorWidth, Filmback.SensorHeight, Shot, Subjects, Over, OutError)) return false;
			const FFraming* Framing = FindFraming(Op.Framing.IsEmpty() ? FString(Op.Over.IsEmpty() ? TEXT("medium") : TEXT("medium_close")) : Op.Framing);
			const double Start = Op.Time;
			const double Length = Op.Duration > 0.f ? Op.Duration : 3.0;
			const FGuid Guid = BindActor(*Sequence, *Camera, World);
			UMovieScene3DTransformSection* Section = TransformSection(*Scene, Guid);
			if (!Section)
			{
				OutError = TEXT("The camera's transform track would not take a section.");
				return false;
			}
			Section->Modify();
			const FFrameNumber First = ToFrame(*Scene, Start);
			const FFrameNumber Last = ToFrame(*Scene, Start + Length);
			ClearTransformKeys(*Section, First, Last);
			const bool bMoving = Shot.Move != TEXT("static");
			const int32 Samples = bMoving || Op.bHandheld ? FMath::Max(2, FMath::RoundToInt32(Length * (Op.bHandheld ? 6.0 : 4.0)) + 1) : 1;
			const double Seed = static_cast<double>(GetTypeHash(Label) % 997);
			FPose Opening;
			double LastYaw = 0.0;
			TArray<float> Lenses;
			TArray<FFrameNumber> Frames;
			for (int32 Index = 0; Index < Samples; ++Index)
			{
				const double Along = Samples > 1 ? static_cast<double>(Index) / (Samples - 1) : 0.0;
				// Operators ease into and out of a move.
				FPose Pose = PoseAt(Shot, bMoving ? Along * Along * (3.0 - 2.0 * Along) : 0.0);
				const double Seconds = Start + Along * Length;
				if (Op.bHandheld)
				{
					const FVector Wander = Drift(Seconds, Seed);
					Pose.Location += Wander * 1.5;
					Pose.Rotation += FRotator(Wander.X * 0.35, Wander.Y * 0.35, Wander.Z * 0.2);
				}
				// Keep the yaw continuous, so an orbit never spins the long way round between keys.
				if (Index > 0) Pose.Rotation.Yaw = LastYaw + FMath::UnwindDegrees(Pose.Rotation.Yaw - LastYaw);
				LastYaw = Pose.Rotation.Yaw;
				if (Index == 0) Opening = Pose;
				const FFrameNumber Frame = ToFrame(*Scene, Seconds);
				KeyTransform(*Section, Frame, Pose.Location, Pose.Rotation, Samples > 1 ? EInterp::Auto : EInterp::Constant);
				Lenses.Add(Pose.Lens);
				Frames.Add(Frame);
			}
			Camera->SetActorLocationAndRotation(Opening.Location, Opening.Rotation);
			const float Aperture = Op.Aperture > 0.f ? Op.Aperture : Framing->Aperture;
			SetLens(*Camera, Op, Opening.Lens, Aperture);
			if (Shot.Move == TEXT("dolly_zoom"))
			{
				// The lens changes through the shot, so it is keyed too.
				UCineCameraComponent* Lens = Camera->GetCineCameraComponent();
				FPropertyTarget Target;
				FString Unused;
				if (ResolveProperty(*Camera, TEXT("CineCameraComponent.CurrentFocalLength"), Target, Unused))
				{
					const FGuid LensGuid = BindComponent(*Sequence, *Camera, *Lens, World);
					UMovieSceneFloatTrack* Track = nullptr;
					for (UMovieSceneTrack* Existing : Scene->FindBinding(LensGuid)->GetTracks())
					{
						UMovieSceneFloatTrack* Typed = Cast<UMovieSceneFloatTrack>(Existing);
						if (Typed && Typed->GetPropertyPath().ToString() == Target.Path) Track = Typed;
					}
					if (!Track)
					{
						Track = Scene->AddTrack<UMovieSceneFloatTrack>(LensGuid);
						Track->SetPropertyNameAndPath(Target.Leaf, Target.Path);
					}
					UMovieSceneSection* LensSection = InfiniteSection<UMovieSceneSection>(*Track);
					if (FMovieSceneFloatChannel* Channel = LensSection ? LensSection->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0) : nullptr)
					{
						ClearKeys(*Channel, First, Last);
						for (int32 Index = 0; Index < Frames.Num(); ++Index) AddKey(*Channel, Frames[Index], Lenses[Index], EInterp::Auto);
					}
				}
			}
			AActor* FocusTarget = Op.FocusOn.IsEmpty() ? Subjects[0].Actor : FindActor(World, Op.FocusOn);
			if (FocusTarget) FocusOn(*Camera, FocusTarget, DescribeSubject(*FocusTarget).Eye - FocusTarget->GetActorLocation().Z);
			// What the shot is of, for the review.
			Camera->Tags.RemoveAll([](const FName& Tag)
			{
				const FString Text = Tag.ToString();
				return Text.StartsWith(SubjectTag) || Text.StartsWith(OverTag) || Text.StartsWith(SizeTag);
			});
			TArray<FString> Names;
			for (const FSubject& Subject : Subjects) Names.Add(Subject.Actor->GetActorLabel());
			Camera->Tags.Add(FName(*(FString(SubjectTag) + FString::Join(Names, TEXT("|")))));
			Camera->Tags.Add(FName(*FString::Printf(TEXT("%s%d"), SizeTag, Framing->Size)));
			if (Over.Actor) Camera->Tags.Add(FName(*(FString(OverTag) + Over.Actor->GetActorLabel())));
			CutTo(*Scene, Guid, First);
			ReachTo(*Scene, Start + Length);
			Report.Notes.Add(FString::Printf(TEXT("%s: %s shot of %s%s%s, %s, %.0f mm at f/%.1f, %.1f m away, %s%s, %.2f to %.2f s."), *Label, *FString(Framing->Name),
				*FString::Join(Names, TEXT(" and ")), Op.Over.IsEmpty() ? TEXT("") : *(TEXT(" over ") + Op.Over + TEXT("'s shoulder")),
				Op.Side.IsEmpty() ? TEXT("") : *(TEXT(", ") + Lower(Op.Side)), Op.Angle.IsEmpty() ? TEXT("eye level") : *Lower(Op.Angle), Opening.Lens, Aperture,
				Shot.Distance / 100.0, *Shot.Move, Op.bHandheld ? TEXT(", hand-held") : TEXT(""), Start, Start + Length));
			return true;
		}
		OutError = TEXT("Unknown op.");
		return false;
	}

	bool SaveAsset(UObject& Asset, FString& OutError)
	{
		UPackage* Package = Asset.GetOutermost();
		if (!FHyperAIStudioSourceControl::MakeEditable({Package->GetName()}, OutError)) return false;
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Package, &Asset, *File, Args))
		{
			OutError = FString::Printf(TEXT("%s could not be saved."), *Package->GetName());
			return false;
		}
		return true;
	}

	/** The plan for real: every op as one undo step, the cut reviewed, the sequence saved, and the level if asked. */
	FHyperAISequencePlanReport Execute(const FHyperAISequencePlanRequest& Request, const FString& OperationId)
	{
		FHyperAISequencePlanReport Report;
		Report.OperationId = OperationId;
		UWorld* World = EditorWorld();
		if (!World)
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("No level is open in the editor.");
			return Report;
		}
		const FString Path = PackageOf(Request.Sequence);
		ULevelSequence* Sequence = Load<ULevelSequence>(Path);
		{
			const FScopedTransaction Transaction(LOCTEXT("SequencePlan", "HyperAI: Edit Sequence"));
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				FString Error;
				if (!Apply(*World, Sequence, Path, Request.Operations[Index], Report, Error))
				{
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Request.Operations[Index].Kind, *Error,
						Report.Applied > 0 ? TEXT(" The operations before it stay applied, unsaved; Edit > Undo takes the plan back.") : TEXT(""));
					return Report;
				}
				++Report.Applied;
			}
			if (UMovieSceneCameraCutTrack* Cuts = Sequence ? Cast<UMovieSceneCameraCutTrack>(Sequence->GetMovieScene()->GetCameraCutTrack()) : nullptr)
			{
				Cuts->RearrangeAllSections();
			}
		}
		if (!Sequence)
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("The plan left no sequence.");
			return Report;
		}
		Report.Review = Review(*World, *Sequence);
		FString Error;
		if (!SaveAsset(*Sequence, Error))
		{
			Report.Status = TEXT("save_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.Saved.Add(Sequence->GetPathName());
		if (Request.bSave)
		{
			if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/false))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = TEXT("The level could not be saved; its cameras are placed in memory.");
				return Report;
			}
			Report.Saved.Add(World->GetOutermost()->GetName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied; %s%s."), Report.Applied,
			Report.Review.IsEmpty() ? TEXT("the cut reviews clean") : *FString::Printf(TEXT("%d review note(s)"), Report.Review.Num()),
			Request.bSave ? TEXT(", level saved") : TEXT("; the level's cameras are unsaved"));
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAISequencePlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Saved.IsEmpty() ? FString() : Done.Saved[0].Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	FString DescribeTrack(const UMovieScene& Scene, UMovieSceneTrack& Track)
	{
		FString Name = Track.GetClass()->GetName();
		Name.RemoveFromStart(TEXT("MovieScene"));
		Name.RemoveFromEnd(TEXT("Track"));
		if (const UMovieScenePropertyTrack* Property = Cast<UMovieScenePropertyTrack>(&Track)) Name += TEXT(" ") + Property->GetPropertyPath().ToString();
		int32 Keys = 0;
		TOptional<FFrameNumber> Earliest;
		TOptional<FFrameNumber> Latest;
		for (UMovieSceneSection* Section : Track.GetAllSections())
		{
			for (const FMovieSceneChannelEntry& Entry : Section->GetChannelProxy().GetAllEntries())
			{
				for (FMovieSceneChannel* Channel : Entry.GetChannels())
				{
					if (!Channel) continue;
					TArray<FFrameNumber> Times;
					Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);
					Keys = FMath::Max(Keys, Times.Num());
					for (const FFrameNumber& Time : Times)
					{
						Earliest = Earliest.IsSet() ? FMath::Min(Earliest.GetValue(), Time) : Time;
						Latest = Latest.IsSet() ? FMath::Max(Latest.GetValue(), Time) : Time;
					}
				}
			}
		}
		if (Keys == 0) return FString::Printf(TEXT("%s: %d section(s), no keys"), *Name, Track.GetAllSections().Num());
		return FString::Printf(TEXT("%s: %d key(s), %.2f to %.2f s"), *Name, Keys, ToSeconds(Scene, Earliest.GetValue()), ToSeconds(Scene, Latest.GetValue()));
	}
}

FHyperAISequenceInspectReport FHyperAIStudioSequencerContracts::Inspect(const FHyperAISequenceInspectRequest& Request)
{
	using namespace HyperAIStudio::SequencerTools::Private;
	FHyperAISequenceInspectReport Report;
	UWorld* World = EditorWorld();
	if (Request.Sequence.TrimStartAndEnd().IsEmpty())
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(ULevelSequence::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.PackageName.ToString().StartsWith(TEXT("/Game/")) && Report.Sequences.Num() < 100) Report.Sequences.Add(Asset.GetObjectPathString());
		}
		Report.Sequences.Sort();
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%d level sequence(s) under /Game%s."), Assets.Num(), Assets.Num() > 100 ? TEXT(", the first 100 listed") : TEXT(""));
		return Report;
	}
	ULevelSequence* Sequence = Load<ULevelSequence>(PackageOf(Request.Sequence));
	UMovieScene* Scene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!Scene)
	{
		Report.Diagnostic = FString::Printf(TEXT("%s is not a level sequence."), *Request.Sequence);
		return Report;
	}
	Report.FrameRate = static_cast<float>(Scene->GetDisplayRate().AsDecimal());
	const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
	Report.Start = static_cast<float>(ToSeconds(*Scene, UE::MovieScene::DiscreteInclusiveLower(Range)));
	Report.Duration = static_cast<float>(ToSeconds(*Scene, FFrameNumber(UE::MovieScene::DiscreteSize(Range))));
	for (int32 Index = 0; Index < Scene->GetPossessableCount(); ++Index)
	{
		const FMovieScenePossessable& Possessable = Scene->GetPossessable(Index);
		FHyperAISequenceBindingInfo& Info = Report.Bindings.AddDefaulted_GetRef();
		Info.Name = Possessable.GetParent().IsValid() ? BindingName(*Scene, Possessable.GetParent()) + TEXT(".") + Possessable.GetName() : Possessable.GetName();
		Info.Class = Possessable.GetPossessedObjectClass() ? Possessable.GetPossessedObjectClass()->GetName() : FString();
		if (const FMovieSceneBinding* Binding = Scene->FindBinding(Possessable.GetGuid()))
		{
			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				if (Track) Info.Tracks.Add(DescribeTrack(*Scene, *Track));
			}
		}
	}
	for (UMovieSceneTrack* Track : Scene->GetTracks())
	{
		if (const UMovieSceneCinematicShotTrack* Shots = Cast<UMovieSceneCinematicShotTrack>(Track))
		{
			for (UMovieSceneSection* Section : Shots->GetAllSections())
			{
				const UMovieSceneSubSection* Shot = Cast<UMovieSceneSubSection>(Section);
				if (!Shot || !Shot->HasStartFrame() || !Shot->HasEndFrame()) continue;
				Report.Shots.Add(FString::Printf(TEXT("%.2f to %.2f s: %s"), ToSeconds(*Scene, Shot->GetInclusiveStartFrame()), ToSeconds(*Scene, Shot->GetExclusiveEndFrame()),
					Shot->GetSequence() ? *Shot->GetSequence()->GetPathName() : TEXT("(no sequence)")));
			}
		}
		else if (const UMovieSceneAudioTrack* Audio = Cast<UMovieSceneAudioTrack>(Track))
		{
			for (UMovieSceneSection* Section : Audio->GetAllSections())
			{
				const UMovieSceneAudioSection* Sound = Cast<UMovieSceneAudioSection>(Section);
				if (Sound && Sound->HasStartFrame())
				{
					Report.Tracks.Add(FString::Printf(TEXT("Audio: %s at %.2f s"), Sound->GetSound() ? *Sound->GetSound()->GetName() : TEXT("(none)"), ToSeconds(*Scene, Sound->GetInclusiveStartFrame())));
				}
			}
		}
		else if (Track)
		{
			Report.Tracks.Add(DescribeTrack(*Scene, *Track));
		}
	}
	if (World)
	{
		for (const FCut& Cut : ReadCuts(*World, *Sequence))
		{
			FString Line = FString::Printf(TEXT("%.2f to %.2f s: %s"), Cut.Start, Cut.End, *Cut.Camera);
			if (Cut.Actor) Line += FString::Printf(TEXT(" (%.0f mm)"), Cut.Actor->GetCineCameraComponent()->CurrentFocalLength);
			Report.Cuts.Add(Line);
		}
		Report.Review = Review(*World, *Sequence);
	}
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%s: %.2f fps, %.2f s, %d binding(s), %d cut(s), %d shot(s); %s."), *Sequence->GetName(), Report.FrameRate, Report.Duration,
		Report.Bindings.Num(), Report.Cuts.Num(), Report.Shots.Num(), Report.Review.IsEmpty() ? TEXT("the cut reviews clean") : *FString::Printf(TEXT("%d review note(s)"), Report.Review.Num()));
	return Report;
}

FHyperAISequencePlanReport FHyperAIStudioSequencerContracts::ApplyPlan(const FHyperAISequencePlanRequest& Request)
{
	using namespace HyperAIStudio::SequencerTools::Private;
	FHyperAISequencePlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("sequence-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	UWorld* World = EditorWorld();
	FString Error;
	if (!World)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("No level is open in the editor.");
		return Report;
	}
	if (!Check(*World, Request, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Request.bSave && World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("This level has never been saved, so there is nowhere to save its cameras; save the level first or leave bSave off.");
		return Report;
	}
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked; nothing was changed."), Request.Operations.Num());
		return Report;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("sequence.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.Sequence;
		Summary.Effects.Add(FString::Printf(TEXT("Edit %s: %d operation(s), one undo step%s"), *Request.Sequence, Request.Operations.Num(), Request.bSave ? TEXT(", then the level saved") : TEXT("")));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAISequenceOp& Op = Request.Operations[Index];
			const FString Target = !Op.Camera.IsEmpty() ? Op.Camera : !Op.Actor.IsEmpty() ? Op.Actor : !Op.Subjects.IsEmpty() ? FString::Join(Op.Subjects, TEXT(", ")) : Op.Asset;
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s"), *Op.Kind, *Target.Left(160)));
		}
		Summary.Touches.Add(Request.Sequence);
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				Record(OperationId, Execute(Request, OperationId));
			}, Error))
		{
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		return Report;
	}
	return Execute(Request, Report.OperationId);
}

FHyperAISequenceInspectReport UHyperAIStudioSequencerToolset::hyper_sequence_inspect(const FHyperAISequenceInspectRequest& Request)
{
	return FHyperAIStudioSequencerContracts::Inspect(Request);
}

FHyperAISequencePlanReport UHyperAIStudioSequencerToolset::hyper_sequence_apply_plan(const FHyperAISequencePlanRequest& Request)
{
	return FHyperAIStudioSequencerContracts::ApplyPlan(Request);
}

void FHyperAIStudioSequencerRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioSequencerRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioSequencerRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioSequencerToolset::StaticClass(),
			FHyperAIStudioSequencerContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioSequencer, Warning, TEXT("Could not unregister the Sequencer toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioSequencerRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioSequencerContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioSequencer, Verbose, TEXT("The Sequencer source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioSequencerToolset::StaticClass(),
		FHyperAIStudioSequencerContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioSequencer, Error, TEXT("Sequencer cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
