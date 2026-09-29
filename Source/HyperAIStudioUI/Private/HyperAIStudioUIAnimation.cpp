// Games by Hyper 2026.

#include "HyperAIStudioUIAnimation.h"

#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/WidgetTree.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExecutionQueue.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioUIWidgetOps.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "UMGEditorProjectSettings.h"
#include "UObject/SoftObjectPath.h"
#include "WidgetBlueprint.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioUIAnimation"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioUIAnimation, Log, All);

namespace HyperAIStudio::UI::Animation
{
	namespace
	{
		constexpr const TCHAR* PayloadTypeId = TEXT("hyperai.payload.ui.widget-animation.v1");
		constexpr const TCHAR* ResultTypeId = TEXT("hyperai.result.ui.widget-animation.v1");
		constexpr const TCHAR* InspectVariantId = TEXT("ui.inspect.v1");
		constexpr const TCHAR* ValidateVariantId = TEXT("ui.validate.v1");
		constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.ui.inspect.v1");
		constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.ui.validate.v1");
		constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.ui.inspect.v1");
		constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.ui.validate.v1");
		constexpr int32 MaxKeys = 64;
		constexpr double MaxLengthSeconds = 600.0;
		const FName OpacityProperty(TEXT("RenderOpacity"));
		const FName TransformProperty(TEXT("RenderTransform"));

		FString Seal(const TCHAR* Canonical)
		{
			return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
		}

		const FString& PayloadSchema()
		{
			static const FString Value = Seal(TEXT("ui.widget_edit.payload.v2|target|base_revision|operations[animation.create(name,seconds|frames),animation.add_float_track(widget,animation,float_property,keys,cubic),animation.add_transform_track(widget,animation,channel,keys,cubic),widget.add(name,parent,class),widget.set_property(widget,id,value),widget.set_style(widget,id,color),widget.set_slot(widget,id,value),widget.set_navigation(widget,direction,rule,target)]"));
			return Value;
		}

		const FString& ResultSchema()
		{
			static const FString Value = Seal(TEXT("ui.widget_animation.result.v1|phase|content_key"));
			return Value;
		}

		const FString& InspectPayloadSchema()
		{
			static const FString Value = Seal(TEXT("ui.inspect.payload.v1|targets|tree|layout|animations|legacy|mvvm|volatile|page|cursor"));
			return Value;
		}

		const FString& ValidatePayloadSchema()
		{
			static const FString Value = Seal(TEXT("ui.validate.payload.v1|targets|layout|accessibility|animations|bindings|mvvm|issues|deadline|output"));
			return Value;
		}

		const FString& InspectResultSchema()
		{
			static const FString Value = Seal(TEXT("ui.inspect.result.v1|fingerprints|records|issues|cursor|bounded"));
			return Value;
		}

		const FString& ValidateResultSchema()
		{
			static const FString Value = Seal(TEXT("ui.validate.result.v1|fingerprints|counts|issues|bounded"));
			return Value;
		}

		void AppendToken(FString& Out, const FString& Value)
		{
			Out += FString::Printf(TEXT("%d:%s|"), Value.Len(), *Value);
		}

		int32 ClampBytes(const int64 Bytes)
		{
			return static_cast<int32>(FMath::Clamp<int64>(Bytes, 1, MAX_int32));
		}

		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty() || Value.Len() > 64 || FChar::IsDigit(Value[0])) return false;
			for (const TCHAR Character : Value)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_')) return false;
			}
			return true;
		}

		bool IsTrackKind(const FString& Kind)
		{
			return Kind == TEXT("animation.add_float_track") || Kind == TEXT("animation.add_transform_track");
		}

		struct FKey
		{
			double Time = 0.0;
			float Value = 0.f;
		};

		/** "0=0.3, 1=1": seconds=value pairs, commas or semicolons between them. Opacity and progress run 0 to 1. */
		bool ParseKeys(const FString& Text, const double Length, const bool bUnitRange, TArray<FKey>& Out, FString& OutError)
		{
			TArray<FString> Parts;
			Text.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(Parts, TEXT(","), /*CullEmpty=*/true);
			if (Parts.IsEmpty() || Parts.Num() > MaxKeys)
			{
				OutError = FString::Printf(TEXT("string_value must hold 1 to %d keys as seconds=value pairs, e.g. \"0=0.3, 1=1\"."), MaxKeys);
				return false;
			}
			Out.Reset();
			for (const FString& Part : Parts)
			{
				FString TimeText;
				FString ValueText;
				FKey Key;
				if (!Part.Split(TEXT("="), &TimeText, &ValueText) || !LexTryParseString(Key.Time, *TimeText.TrimStartAndEnd())
					|| !LexTryParseString(Key.Value, *ValueText.TrimStartAndEnd()) || !FMath::IsFinite(Key.Time) || !FMath::IsFinite(Key.Value))
				{
					OutError = FString::Printf(TEXT("'%s' is not a seconds=value key."), *Part.TrimStartAndEnd());
					return false;
				}
				if (Key.Time < 0.0 || Key.Time > Length + KINDA_SMALL_NUMBER)
				{
					OutError = FString::Printf(TEXT("A key at %g s lies outside the animation's 0 to %g s."), Key.Time, Length);
					return false;
				}
				if (bUnitRange && (Key.Value < 0.f || Key.Value > 1.f))
				{
					OutError = FString::Printf(TEXT("Keys for this property run from 0 to 1; %g is outside."), Key.Value);
					return false;
				}
				if (Out.ContainsByPredicate([&Key](const FKey& Other) { return FMath::IsNearlyEqual(Other.Time, Key.Time); }))
				{
					OutError = FString::Printf(TEXT("Two keys sit at %g s."), Key.Time);
					return false;
				}
				Out.Add(Key);
			}
			Out.Sort([](const FKey& A, const FKey& B) { return A.Time < B.Time; });
			return true;
		}

		bool IsUnitRange(const FName Property)
		{
			return Property == OpacityProperty || Property == FName(TEXT("Percent"));
		}

		/** Seconds from number_value, or else start/end frames at the project's widget animation frame rate. */
		double RequestedLength(const FHyperAIUIPlanOperation& Create)
		{
			if (Create.bHasNumberValue) return Create.NumberValue;
			const int32 Rate = FMath::Max(1, GetDefault<UUMGEditorProjectSettings>()->DefaultWidgetAnimationFrameRate);
			return static_cast<double>(Create.EndFrame - Create.StartFrame) / Rate;
		}

		double LengthOf(const UWidgetAnimation& Animation)
		{
			const UMovieScene* Scene = Animation.GetMovieScene();
			if (!Scene) return 0.0;
			const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
			return Scene->GetTickResolution().AsSeconds(FFrameTime(Range.GetUpperBoundValue() - Range.GetLowerBoundValue()));
		}

		UWidgetAnimation* FindAnimation(const UWidgetBlueprint& Blueprint, const FString& Name)
		{
			for (UWidgetAnimation* Animation : Blueprint.Animations)
			{
				if (Animation && Animation->GetName() == Name) return Animation;
			}
			return nullptr;
		}

		UWidget* FindWidget(const UWidgetBlueprint& Blueprint, const FString& Subject)
		{
			return Blueprint.WidgetTree ? Blueprint.WidgetTree->FindWidget(FName(*WidgetOps::WidgetNameOf(Subject))) : nullptr;
		}

		struct FTransformChannel
		{
			const TCHAR* Name;
			EMovieScene2DTransformChannel Bit;
		};

		const FTransformChannel TransformChannels[] = {
			{TEXT("translation_x"), EMovieScene2DTransformChannel::TranslationX}, {TEXT("translation_y"), EMovieScene2DTransformChannel::TranslationY},
			{TEXT("angle"), EMovieScene2DTransformChannel::Rotation}, {TEXT("scale_x"), EMovieScene2DTransformChannel::ScaleX},
			{TEXT("scale_y"), EMovieScene2DTransformChannel::ScaleY}, {TEXT("shear_x"), EMovieScene2DTransformChannel::ShearX},
			{TEXT("shear_y"), EMovieScene2DTransformChannel::ShearY}};

		const FTransformChannel* FindTransformChannel(const FString& Name)
		{
			for (const FTransformChannel& Channel : TransformChannels)
			{
				if (Name.Equals(Channel.Name, ESearchCase::IgnoreCase)) return &Channel;
			}
			return nullptr;
		}

		FMovieSceneFloatChannel& ChannelOf(UMovieScene2DTransformSection& Section, const EMovieScene2DTransformChannel Bit)
		{
			switch (Bit)
			{
			case EMovieScene2DTransformChannel::TranslationX: return Section.Translation[0];
			case EMovieScene2DTransformChannel::TranslationY: return Section.Translation[1];
			case EMovieScene2DTransformChannel::Rotation: return Section.Rotation;
			case EMovieScene2DTransformChannel::ScaleX: return Section.Scale[0];
			case EMovieScene2DTransformChannel::ScaleY: return Section.Scale[1];
			case EMovieScene2DTransformChannel::ShearX: return Section.Shear[0];
			default: return Section.Shear[1];
			}
		}

		/** The binding a widget has in an animation: one possessable per widget, by name, not through its slot. */
		FGuid FindBinding(const UWidgetAnimation& Animation, const FName Widget)
		{
			for (const FWidgetAnimationBinding& Binding : Animation.AnimationBindings)
			{
				if (Binding.WidgetName == Widget && Binding.SlotWidgetName.IsNone()) return Binding.AnimationGuid;
			}
			return FGuid();
		}

		UMovieSceneFloatSection* FindFloatSection(const UWidgetAnimation& Animation, const FName Widget, const FName Property)
		{
			const UMovieScene* Scene = Animation.GetMovieScene();
			const FGuid Guid = FindBinding(Animation, Widget);
			if (!Scene || !Guid.IsValid()) return nullptr;
			for (UMovieSceneTrack* Track : Scene->FindTracks(UMovieSceneFloatTrack::StaticClass(), Guid))
			{
				const UMovieSceneFloatTrack* Float = Cast<UMovieSceneFloatTrack>(Track);
				if (Float && Float->GetPropertyName() == Property && !Float->GetAllSections().IsEmpty())
				{
					return Cast<UMovieSceneFloatSection>(Float->GetAllSections()[0]);
				}
			}
			return nullptr;
		}

		UMovieScene2DTransformSection* FindTransformSection(const UWidgetAnimation& Animation, const FName Widget)
		{
			const UMovieScene* Scene = Animation.GetMovieScene();
			const FGuid Guid = FindBinding(Animation, Widget);
			if (!Scene || !Guid.IsValid()) return nullptr;
			for (UMovieSceneTrack* Track : Scene->FindTracks(UMovieScene2DTransformTrack::StaticClass(), Guid))
			{
				if (Track && !Track->GetAllSections().IsEmpty())
				{
					return Cast<UMovieScene2DTransformSection>(Track->GetAllSections()[0]);
				}
			}
			return nullptr;
		}

		bool IsOpenInEditor(UWidgetBlueprint& Blueprint)
		{
			UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			return Editors && Editors->FindEditorForAsset(&Blueprint, /*bFocusIfOpen=*/false) != nullptr;
		}

		/**
		 * Checks every op in order against the Widget Blueprint as the earlier ops leave it: widgets the plan adds
		 * can be laid out, given navigation and animated in the same plan.
		 */
		bool ValidateOps(const UWidgetBlueprint& Blueprint, const TArray<FHyperAIUIPlanOperation>& Operations, TArray<FString>& OutNotes, FString& OutError)
		{
			WidgetOps::FPlannedTree Tree(Blueprint);
			TMap<FString, double> Lengths;
			for (const UWidgetAnimation* Animation : Blueprint.Animations)
			{
				if (Animation) Lengths.Add(Animation->GetName(), LengthOf(*Animation));
			}
			TSet<FString> Tracks;
			for (int32 Index = 0; Index < Operations.Num(); ++Index)
			{
				const FHyperAIUIPlanOperation& Operation = Operations[Index];
				auto Fail = [&](const FString& Message)
				{
					OutError = FString::Printf(TEXT("Op %d (%s): %s"), Index, *Operation.Kind, *Message);
					return false;
				};
				if (WidgetOps::IsWidgetOp(Operation.Kind))
				{
					FString Note;
					FString Error;
					if (!WidgetOps::Validate(Operation, Blueprint, Tree, Note, Error)) return Fail(Error);
					OutNotes.Add(Note);
					continue;
				}
				if (Operation.Kind == TEXT("animation.create"))
				{
					if (!IsIdentifier(Operation.Name)) return Fail(TEXT("name must be an identifier (letters, digits and _, up to 64)."));
					if (Lengths.Contains(Operation.Name)) return Fail(FString::Printf(TEXT("the blueprint already has an animation named %s."), *Operation.Name));
					if (Tree.Widgets.Contains(FName(*Operation.Name)))
					{
						return Fail(FString::Printf(TEXT("%s is already a widget's name; animations are variables and need their own."), *Operation.Name));
					}
					const double Length = RequestedLength(Operation);
					if (!(Length > 0.0 && Length <= MaxLengthSeconds))
					{
						return Fail(FString::Printf(TEXT("the length must be more than 0 and at most %g s: set number_value to the seconds."), MaxLengthSeconds));
					}
					Lengths.Add(Operation.Name, Length);
					OutNotes.Add(FString::Printf(TEXT("%s: %g s"), *Operation.Name, Length));
					continue;
				}
				if (!IsTrackKind(Operation.Kind))
				{
					return Fail(TEXT("this kind does not run in an edit plan."));
				}
				const FString WidgetName = WidgetOps::WidgetNameOf(Operation.SubjectId);
				const WidgetOps::FPlannedWidget* Widget = Tree.Find(WidgetName);
				if (!Widget || !Widget->Class)
				{
					return Fail(FString::Printf(TEXT("no widget named '%s'; it has %s."), *WidgetName, *Tree.Names()));
				}
				const double* Length = Lengths.Find(Operation.SecondaryName);
				if (!Length)
				{
					return Fail(FString::Printf(TEXT("no animation named '%s'; create it first in this plan with animation.create."), *Operation.SecondaryName));
				}
				const bool bTransform = Operation.Kind == TEXT("animation.add_transform_track");
				FName Property = OpacityProperty;
				if (bTransform)
				{
					if (!FindTransformChannel(Operation.Name))
					{
						return Fail(TEXT("name is the transform channel: translation_x, translation_y, angle, scale_x, scale_y, shear_x or shear_y."));
					}
				}
				else
				{
					const FProperty* Float = WidgetOps::FindAnimatableFloat(*Widget->Class, Operation.Name);
					if (!Float)
					{
						return Fail(FString::Printf(TEXT("%s is not a float property of %s that an animation can key. It has: %s. Move, turn and scale widgets with animation.add_transform_track."),
							*Operation.Name, *Widget->Class->GetName(), *WidgetOps::AnimatableFloatNames(*Widget->Class)));
					}
					Property = Float->GetFName();
				}
				TArray<FKey> Keys;
				FString KeyError;
				if (!ParseKeys(Operation.StringValue, *Length, !bTransform && IsUnitRange(Property), Keys, KeyError)) return Fail(KeyError);
				const FString Track = Operation.SecondaryName + TEXT("|") + WidgetName + TEXT("|") + (bTransform ? Operation.Name.ToLower() : Property.ToString());
				if (Tracks.Contains(Track)) return Fail(TEXT("the plan already keys this widget's channel in that animation."));
				Tracks.Add(Track);
				OutNotes.Add(FString::Printf(TEXT("%s: %s.%s %g to %g over %d keys"), *Operation.SecondaryName, *WidgetName,
					bTransform ? *Operation.Name.ToLower() : *Property.ToString(), Keys[0].Value, Keys.Last().Value, Keys.Num()));
			}
			return true;
		}

		void AddKeys(FMovieSceneFloatChannel& Channel, const UMovieScene& Scene, const TArray<FKey>& Keys, const bool bCubic)
		{
			Channel.Reset();
			for (const FKey& Key : Keys)
			{
				const FFrameNumber Frame = (Key.Time * Scene.GetTickResolution()).RoundToFrame();
				if (bCubic) Channel.AddCubicKey(Frame, Key.Value);
				else Channel.AddLinearKey(Frame, Key.Value);
			}
		}

		bool ApplyOps(UWidgetBlueprint& Blueprint, const TArray<FHyperAIUIPlanOperation>& Operations, FString& OutError)
		{
			Blueprint.Modify();
			const int32 DisplayRate = FMath::Max(1, GetDefault<UUMGEditorProjectSettings>()->DefaultWidgetAnimationFrameRate);
			for (const FHyperAIUIPlanOperation& Operation : Operations)
			{
				if (WidgetOps::IsWidgetOp(Operation.Kind))
				{
					if (!WidgetOps::Apply(Blueprint, Operation, OutError)) return false;
					continue;
				}
				if (Operation.Kind == TEXT("animation.create"))
				{
					// The Animations tab's recipe: its own movie scene, a playback range, then a blueprint variable.
					UWidgetAnimation* Animation = NewObject<UWidgetAnimation>(&Blueprint, FName(*Operation.Name), RF_Transactional);
					Animation->SetDisplayLabel(Operation.Name);
					UMovieScene* Scene = NewObject<UMovieScene>(Animation, FName(*Operation.Name), RF_Transactional);
					Animation->MovieScene = Scene;
					Scene->SetDisplayRate(FFrameRate(DisplayRate, 1));
					const double Length = RequestedLength(Operation);
					const FFrameNumber End = (Length * Scene->GetTickResolution()).FloorToFrame();
					Scene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), End + 1));
					Scene->GetEditorData().WorkStart = 0.0;
					Scene->GetEditorData().WorkEnd = Length;
					Scene->GetEditorData().ViewStart = 0.0;
					Scene->GetEditorData().ViewEnd = Length;
					Blueprint.Animations.Add(Animation);
					Blueprint.OnVariableAdded(Animation->GetFName());
					continue;
				}
				UWidgetAnimation* Animation = FindAnimation(Blueprint, Operation.SecondaryName);
				UWidget* Widget = FindWidget(Blueprint, Operation.SubjectId);
				UMovieScene* Scene = Animation ? Animation->GetMovieScene() : nullptr;
				if (!Scene || !Widget)
				{
					OutError = FString::Printf(TEXT("%s or its animation %s is gone."), *Operation.SubjectId, *Operation.SecondaryName);
					return false;
				}
				Animation->Modify();
				Scene->Modify();
				// A direct binding by widget name, the shape BindPossessableObject writes for a non-slot widget.
				FGuid Guid = FindBinding(*Animation, Widget->GetFName());
				if (!Guid.IsValid())
				{
					Guid = Scene->AddPossessable(Widget->GetName(), Widget->GetClass());
					FWidgetAnimationBinding Binding;
					Binding.AnimationGuid = Guid;
					Binding.WidgetName = Widget->GetFName();
					Binding.bIsRootWidget = false;
					Animation->AnimationBindings.Add(Binding);
				}
				TArray<FKey> Keys;
				FString Unused;
				const bool bCubic = Operation.bHasBoolValue && Operation.bBoolValue;
				if (Operation.Kind == TEXT("animation.add_transform_track"))
				{
					const FTransformChannel* Channel = FindTransformChannel(Operation.Name);
					ParseKeys(Operation.StringValue, LengthOf(*Animation), /*bUnitRange=*/false, Keys, Unused);
					UMovieScene2DTransformSection* Section = FindTransformSection(*Animation, Widget->GetFName());
					if (!Section)
					{
						UMovieScene2DTransformTrack* Track = Scene->AddTrack<UMovieScene2DTransformTrack>(Guid);
						Track->SetPropertyNameAndPath(TransformProperty, TransformProperty.ToString());
						Section = CastChecked<UMovieScene2DTransformSection>(Track->CreateNewSection());
						Section->SetRange(Scene->GetPlaybackRange());
						// Only keyed channels animate; the rest keep the widget's own transform.
						Section->SetMask(FMovieScene2DTransformMask(EMovieScene2DTransformChannel::None));
						Track->AddSection(*Section);
					}
					Section->Modify();
					Section->SetMask(FMovieScene2DTransformMask(Section->GetMask().GetChannels() | Channel->Bit));
					AddKeys(ChannelOf(*Section, Channel->Bit), *Scene, Keys, bCubic);
					continue;
				}
				const FProperty* Float = WidgetOps::FindAnimatableFloat(*Widget->GetClass(), Operation.Name);
				const FName Property = Float ? Float->GetFName() : OpacityProperty;
				ParseKeys(Operation.StringValue, LengthOf(*Animation), IsUnitRange(Property), Keys, Unused);
				for (UMovieSceneTrack* Existing : Scene->FindTracks(UMovieSceneFloatTrack::StaticClass(), Guid))
				{
					const UMovieSceneFloatTrack* Previous = Cast<UMovieSceneFloatTrack>(Existing);
					if (Previous && Previous->GetPropertyName() == Property) Scene->RemoveTrack(*Existing);
				}
				UMovieSceneFloatTrack* Track = Scene->AddTrack<UMovieSceneFloatTrack>(Guid);
				Track->SetPropertyNameAndPath(Property, Property.ToString());
				UMovieSceneFloatSection* Section = CastChecked<UMovieSceneFloatSection>(Track->CreateNewSection());
				Section->SetRange(Scene->GetPlaybackRange());
				Track->AddSection(*Section);
				AddKeys(Section->GetChannel(), *Scene, Keys, bCubic);
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
			return true;
		}

		/** After compile: every op's effect is in the Widget Blueprint, or the name of the first that is not. */
		bool AllLanded(const UWidgetBlueprint& Blueprint, const TArray<FHyperAIUIPlanOperation>& Operations, FString& OutMissing)
		{
			for (const FHyperAIUIPlanOperation& Operation : Operations)
			{
				if (WidgetOps::IsWidgetOp(Operation.Kind))
				{
					if (!WidgetOps::Landed(Blueprint, Operation, OutMissing)) return false;
					continue;
				}
				const bool bCreate = Operation.Kind == TEXT("animation.create");
				const UWidgetAnimation* Animation = FindAnimation(Blueprint, bCreate ? Operation.Name : Operation.SecondaryName);
				const UWidget* Widget = bCreate ? nullptr : FindWidget(Blueprint, Operation.SubjectId);
				OutMissing = bCreate ? Operation.Name : Operation.SecondaryName + TEXT(" ") + Operation.SubjectId + TEXT(".") + Operation.Name;
				if (!Animation) return false;
				if (bCreate) continue;
				if (!Widget) return false;
				if (Operation.Kind == TEXT("animation.add_transform_track"))
				{
					UMovieScene2DTransformSection* Section = FindTransformSection(*Animation, Widget->GetFName());
					const FTransformChannel* Channel = FindTransformChannel(Operation.Name);
					if (!Section || !Channel || !EnumHasAnyFlags(Section->GetMask().GetChannels(), Channel->Bit)
						|| ChannelOf(*Section, Channel->Bit).GetNumKeys() == 0) return false;
					continue;
				}
				const FProperty* Float = WidgetOps::FindAnimatableFloat(*Widget->GetClass(), Operation.Name);
				const UMovieSceneFloatSection* Section = Float ? FindFloatSection(*Animation, Widget->GetFName(), Float->GetFName()) : nullptr;
				if (!Section || Section->GetChannel().GetNumKeys() == 0) return false;
			}
			OutMissing.Reset();
			return true;
		}

		FString ComputeSemanticFingerprint(const FHyperAIStudioUIAnimationPayload& Payload)
		{
			FString Canonical;
			AppendToken(Canonical, TEXT("hyperai.ui.widget-edit-intent.v2"));
			AppendToken(Canonical, Payload.TargetPath);
			AppendToken(Canonical, Payload.BaseRevision);
			for (const FHyperAIUIPlanOperation& Operation : Payload.Operations)
			{
				AppendToken(Canonical, Operation.Kind);
				AppendToken(Canonical, Operation.SubjectId);
				AppendToken(Canonical, Operation.SecondaryName);
				AppendToken(Canonical, Operation.Name);
				AppendToken(Canonical, Operation.StringValue);
				AppendToken(Canonical, Operation.ExpectedElementFingerprint);
				AppendToken(Canonical, FString::Printf(TEXT("%d|%.17g|%d|%d|%d|%d|%d|%.9g|%.9g|%.9g|%.9g"), Operation.bHasNumberValue ? 1 : 0, Operation.NumberValue,
					Operation.StartFrame, Operation.EndFrame, Operation.bHasBoolValue ? 1 : 0, Operation.bBoolValue ? 1 : 0, Operation.bHasColorValue ? 1 : 0,
					Operation.ColorValue.R, Operation.ColorValue.G, Operation.ColorValue.B, Operation.ColorValue.A));
			}
			return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
		}

		FHyperAIStudioDomainAdapterResult ExecutePhase(const EHyperAIStudioDomainExecutionActionKind Phase, const FHyperAIStudioUIAnimationPayload& Payload)
		{
			FHyperAIStudioDomainAdapterResult Result;
			auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status, const FString& Diagnostic,
				const TCHAR* ResultPhase = nullptr, const FString& Key = FString())
			{
				UE_LOG(LogHyperAIStudioUIAnimation, Log, TEXT("Widget edit phase %d finished %s. %s"), static_cast<int32>(Phase), *Status, *Diagnostic);
				Result.Outcome = Outcome;
				Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
				Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
				if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
				{
					const TSharedRef<FHyperAIStudioUIAnimationResultPayload, ESPMode::ThreadSafe> Output =
						MakeShared<FHyperAIStudioUIAnimationResultPayload, ESPMode::ThreadSafe>();
					Output->Phase = ResultPhase;
					Output->ContentKey = Key;
					Result.Payload = Output;
				}
				return Result;
			};
			const bool bApply = Phase == EHyperAIStudioDomainExecutionActionKind::Apply;
			const EHyperAIStudioDomainDispatchOutcome FailedOutcome = bApply
				? EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect
				: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect;
			if (!IsInGameThread())
			{
				return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Widget Blueprint edits run on the game thread."));
			}
			UWidgetBlueprint* Blueprint = FindObject<UWidgetBlueprint>(nullptr, *Payload.TargetPath);
			if (!Blueprint)
			{
				return Finish(FailedOutcome, TEXT("target_not_loaded"), FString::Printf(TEXT("%s is no longer loaded."), *Payload.TargetPath));
			}
			switch (Phase)
			{
			case EHyperAIStudioDomainExecutionActionKind::Apply:
			{
				if (IsOpenInEditor(*Blueprint))
				{
					return Finish(FailedOutcome, TEXT("target_open_in_editor"), TEXT("The Widget Blueprint was opened in its editor after planning; nothing was applied."));
				}
				TArray<FString> Notes;
				FString Error;
				if (ComputeRevision(*Blueprint) != Payload.BaseRevision || !ValidateOps(*Blueprint, Payload.Operations, Notes, Error))
				{
					return Finish(FailedOutcome, TEXT("stale_revision"), TEXT("The Widget Blueprint changed after planning; nothing was applied. ") + Error);
				}
				bool bApplied = false;
				{
					const FScopedTransaction Transaction(LOCTEXT("ApplyWidgetAnimationPlan", "HyperAI: Apply Widget Blueprint Plan"));
					bApplied = ApplyOps(*Blueprint, Payload.Operations, Error);
				}
				if (!bApplied)
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("edit_op_failed"),
						Error + TEXT(" Earlier ops of the plan are in memory; undo reverts them."));
				}
				return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"), FString::Join(Notes, TEXT("; ")), TEXT("applied"));
			}
			case EHyperAIStudioDomainExecutionActionKind::Compile:
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
				if (Blueprint->Status == BS_Error)
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("compile_error"),
						TEXT("The Widget Blueprint does not compile after the edit; undo reverts it."));
				}
				return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("compiled"), TEXT("Compiled."), TEXT("compiled"));
			case EHyperAIStudioDomainExecutionActionKind::Validate:
			{
				FString Missing;
				if (!AllLanded(*Blueprint, Payload.Operations, Missing))
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("edit_missing"),
						FString::Printf(TEXT("%s did not land in the Widget Blueprint; undo reverts the edit."), *Missing));
				}
				return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"), TEXT("Every widget, setting, animation and track is in place."), TEXT("validated"));
			}
			case EHyperAIStudioDomainExecutionActionKind::Save:
				if (!UEditorLoadingAndSavingUtils::SavePackages({Blueprint->GetOutermost()}, /*bOnlyDirty=*/false))
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
						TEXT("The Widget Blueprint changed but did not save; it remains in memory."));
				}
				return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Package saved."), TEXT("saved"));
			case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
				return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"), TEXT("Widget Blueprint captured for verification."),
					TEXT("completed"), ComputeRevision(*Blueprint));
			default:
				return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Widget Blueprint plans have no such phase."));
			}
		}
	}

	bool IsExecutablePlan(const TArray<FHyperAIUIPlanOperation>& Operations)
	{
		return !Operations.IsEmpty() && !Operations.ContainsByPredicate([](const FHyperAIUIPlanOperation& Operation)
		{
			const bool bWidget = WidgetOps::IsWidgetOp(Operation.Kind);
			// A widget op with a per-element fingerprint is the older staged-only contract; it keeps that route.
			return !(Operation.Kind == TEXT("animation.create") || IsTrackKind(Operation.Kind)
				|| (bWidget && Operation.ExpectedElementFingerprint.IsEmpty()));
		});
	}

	FString ComputeRevision(const UWidgetBlueprint& Blueprint)
	{
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.ui.widget-edit-revision.v2"));
		AppendToken(Canonical, Blueprint.GetPathName());
		// The saved package and whether it is dirty catch any property edit made outside a plan since.
		const UPackage* Package = Blueprint.GetOutermost();
		AppendToken(Canonical, Package ? LexToString(Package->GetSavedHash()) : FString());
		AppendToken(Canonical, Package && Package->IsDirty() ? TEXT("dirty") : TEXT("clean"));
		TArray<FString> Widgets;
		WidgetOps::AppendTreeRevision(Blueprint, Widgets);
		for (const FString& Widget : Widgets) AppendToken(Canonical, Widget);
		for (const UWidgetAnimation* Animation : Blueprint.Animations)
		{
			const UMovieScene* Scene = Animation ? Animation->GetMovieScene() : nullptr;
			if (!Scene)
			{
				AppendToken(Canonical, TEXT("(empty)"));
				continue;
			}
			const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
			AppendToken(Canonical, FString::Printf(TEXT("%s|%d|%d|%d/%d"), *Animation->GetName(), Range.GetLowerBoundValue().Value,
				Range.GetUpperBoundValue().Value, Scene->GetTickResolution().Numerator, Scene->GetTickResolution().Denominator));
			for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
			{
				AppendToken(Canonical, Binding.WidgetName.ToString() + TEXT("|") + Binding.SlotWidgetName.ToString() + TEXT("|") + Binding.AnimationGuid.ToString());
				auto AppendChannel = [&Canonical](const TCHAR* Label, const FMovieSceneFloatChannel& Channel)
				{
					AppendToken(Canonical, Label);
					const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
					const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
					for (int32 Index = 0; Index < Times.Num() && Index < Values.Num(); ++Index)
					{
						AppendToken(Canonical, FString::Printf(TEXT("%d=%.9g/%d"), Times[Index].Value, Values[Index].Value, static_cast<int32>(Values[Index].InterpMode)));
					}
				};
				for (UMovieSceneTrack* Track : Scene->FindTracks(UMovieSceneFloatTrack::StaticClass(), Binding.AnimationGuid))
				{
					const UMovieSceneFloatTrack* Float = Cast<UMovieSceneFloatTrack>(Track);
					if (!Float) continue;
					for (const UMovieSceneSection* Section : Float->GetAllSections())
					{
						if (const UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
						{
							AppendChannel(*Float->GetPropertyName().ToString(), FloatSection->GetChannel());
						}
					}
				}
				for (UMovieSceneTrack* Track : Scene->FindTracks(UMovieScene2DTransformTrack::StaticClass(), Binding.AnimationGuid))
				{
					if (!Track) continue;
					for (UMovieSceneSection* Section : Track->GetAllSections())
					{
						UMovieScene2DTransformSection* Transform = Cast<UMovieScene2DTransformSection>(Section);
						if (!Transform) continue;
						AppendToken(Canonical, FString::Printf(TEXT("transform|%u"), static_cast<uint32>(Transform->GetMask().GetChannels())));
						for (const FTransformChannel& Channel : TransformChannels)
						{
							AppendChannel(Channel.Name, ChannelOf(*Transform, Channel.Bit));
						}
					}
				}
			}
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FHyperAIUIApplyPlanReport BuildPlan(const FHyperAIUIApplyPlanRequest& Request)
	{
		FHyperAIUIApplyPlanReport Report;
		Report.bDryRun = Request.bDryRun;
		Report.OperationId = Request.OperationId.Left(FHyperAIStudioDomainLimits::MaxOperationIdChars);
		Report.SafetyClass = TEXT("edit");
		Report.VariantId = VariantId;
		auto Reject = [&Report](const TCHAR* Status, const FString& Diagnostic)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic.Left(1024);
			return Report;
		};
		if (!IsInGameThread())
		{
			return Reject(TEXT("game_thread_required"), TEXT("Widget Blueprint planning runs on the game thread."));
		}
		if (!FHyperAIStudioUIContracts::IsCanonicalProjectObjectPath(Request.TargetPath)
			|| Request.Operations.Num() > FHyperAIStudioUIContracts::MaxOperations
			|| Request.MaxOutputBytes < 4096 || Request.MaxOutputBytes > FHyperAIStudioUIContracts::MaxOutputBytes)
		{
			return Reject(TEXT("invalid_plan_bounds"), TEXT("Pass target_path as a canonical /Game object path, up to 64 ops, and output bounds of 4-256 KB."));
		}
		if (Request.bDryRun && (!Request.OperationId.IsEmpty() || !Request.ExpectedPlanHash.IsEmpty()))
		{
			return Reject(TEXT("unexpected_submission_fields"), TEXT("A dry run takes no operation_id or expected_plan_hash."));
		}
		if (!Request.bDryRun && (!FHyperAIStudioExtensionRuntime::IsValidOperationId(Request.OperationId)
			|| !FHyperAIStudioExtensionRuntime::IsValidSubmissionPlanHash(Request.ExpectedPlanHash)))
		{
			return Reject(TEXT("invalid_submission_fields"), FHyperAIStudioExtensionRuntime::InvalidSubmissionMessage());
		}
		UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FSoftObjectPath(Request.TargetPath).TryLoad());
		if (!Blueprint || Blueprint->GetPathName() != Request.TargetPath)
		{
			return Reject(TEXT("target_not_found"), FString::Printf(TEXT("No Widget Blueprint at %s; create it first (Epic's UMGToolSet or the Content Browser)."), *Request.TargetPath));
		}
		if (IsOpenInEditor(*Blueprint))
		{
			return Reject(TEXT("target_open_in_editor"), TEXT("Close the Widget Blueprint's editor tab first, so it changes in one place."));
		}
		TArray<FString> Notes;
		FString Error;
		if (!ValidateOps(*Blueprint, Request.Operations, Notes, Error))
		{
			return Reject(TEXT("invalid_op"), Error);
		}
		const TSharedRef<FHyperAIStudioUIAnimationPayload, ESPMode::ThreadSafe> Payload =
			MakeShared<FHyperAIStudioUIAnimationPayload, ESPMode::ThreadSafe>();
		Payload->TargetPath = Request.TargetPath;
		Payload->BaseRevision = ComputeRevision(*Blueprint);
		Payload->Operations = Request.Operations;
		Payload->SemanticFingerprint = ComputeSemanticFingerprint(*Payload);
		if (Payload->GetBoundedByteSize() > FHyperAIStudioDomainLimits::MaxRequestBytes)
		{
			return Reject(TEXT("payload_bound_exceeded"), TEXT("The ops exceed the bounded request size; split them into smaller plans."));
		}
		Report.BasePersistedFingerprint = Payload->BaseRevision;
		Report.SemanticFingerprint = Payload->SemanticFingerprint;
		Report.PlanHash = Payload->SemanticFingerprint;
		for (const FHyperAIUIPlanOperation& Operation : Request.Operations)
		{
			if (Operation.Kind == TEXT("animation.create")) ++Report.Effects.AnimationCreates;
			else if (IsTrackKind(Operation.Kind)) ++Report.Effects.AnimationEdits;
			else if (Operation.Kind == TEXT("widget.add")) ++Report.Effects.WidgetAdds;
			else if (Operation.Kind == TEXT("widget.set_style")) ++Report.Effects.StyleEdits;
			else if (Operation.Kind == TEXT("widget.set_slot")) ++Report.Effects.SlotEdits;
			else if (Operation.Kind == TEXT("widget.set_navigation")) ++Report.Effects.NavigationEdits;
			else ++Report.Effects.PropertyEdits;
		}
		Report.Effects.OperationCount = Request.Operations.Num();
		Report.Effects.bWouldCompileOnce = Report.Effects.bWouldSaveOnce = Report.Effects.bWouldValidateOnce = Report.Effects.bWouldVerifyFreshOnce = true;

		FHyperAIStudioTrustedArtifactRequest Trusted;
		Trusted.PackId = FHyperAIStudioUIContracts::PackId;
		Trusted.ToolName = TEXT("hyper_ui_apply_plan");
		Trusted.VariantId = VariantId;
		Trusted.Safety = EHyperAIStudioDomainSafety::Edit;
		Trusted.ArtifactSemanticFingerprint = Payload->SemanticFingerprint;
		Trusted.EffectTarget = Request.TargetPath;
		Trusted.DeadlineMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactDeadlineMs;
		Trusted.MaxNativeOperations = FMath::Clamp(Request.Operations.Num() + 8, 9, 128);
		Trusted.MaxGameThreadMs = FHyperAIStudioTypedArtifactLimits::MaxArtifactGameThreadMs;
		Trusted.MaxOutputBytes = Request.MaxOutputBytes;
		Trusted.MaxResultBytes = 256;
		Trusted.StageLifetimeMs = 15000;
		Trusted.bCompileOnce = true;
		Trusted.bSaveOnce = true;
		Trusted.bValidateOnce = true;
		Trusted.bVerifyFreshOnce = true;
		FHyperAIStudioTrustedPreparedArtifact Prepared;
		FHyperAIStudioTrustedPrepareReport Prepare;
		if (!FHyperAIStudioTrustedExecutionFacade::PrepareDryRun(Trusted, Payload,
				MakeShared<FHyperAIStudioUIAnimationFreshVerifier, ESPMode::ThreadSafe>(), Prepared, Prepare, Error)
			|| !Prepare.bPrepared)
		{
			FString Diagnostic = Error.IsEmpty() ? Prepare.Status.Diagnostic : Error;
			if (!Prepare.Status.BlockingPrerequisiteIds.IsEmpty())
			{
				Diagnostic += TEXT(" Blocking: ") + FString::Join(Prepare.Status.BlockingPrerequisiteIds, TEXT(", "));
			}
			return Reject(Prepare.Status.StatusCode.IsEmpty() ? TEXT("prepare_failed") : *Prepare.Status.StatusCode, Diagnostic);
		}
		Report.bTypedPrepared = true;
		Report.AuthorizationPlanHash = Prepare.PlanHash;
		if (Request.bDryRun)
		{
			Report.bOk = true;
			Report.Status = TEXT("planned");
			Report.Diagnostic = FString::Join(Notes, TEXT("; "))
				+ TEXT(". Nothing changed. To apply, resubmit with bDryRun false and an operation_id (expected_plan_hash optional).");
			return Report;
		}
		if (!FHyperAIStudioExtensionRuntime::IsAcceptedPlanHash(Request.ExpectedPlanHash, Payload->SemanticFingerprint))
		{
			return Reject(TEXT("plan_hash_mismatch"), TEXT("The plan changed since its dry run (ops or the Widget Blueprint). Dry-run again and review it."));
		}
		if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
		{
			FHyperAIStudioApprovalSummary Summary;
			Summary.PackId = FHyperAIStudioUIContracts::PackId;
			Summary.ToolName = TEXT("hyper_ui_apply_plan");
			Summary.VariantId = VariantId;
			Summary.Safety = EHyperAIStudioDomainSafety::Edit;
			Summary.EffectTarget = Request.TargetPath;
			Summary.PlanHash = Payload->SemanticFingerprint;
			Summary.Effects = Notes;
			Summary.Effects.Add(TEXT("compile and save"));
			Summary.Touches.Add(Request.TargetPath);
			if (!FHyperAIStudioApprovalGate::Request(Prepared, Request.OperationId, Summary, Error))
			{
				return Reject(TEXT("approval_not_queued"), Error);
			}
			Report.bOk = true;
			Report.Status = TEXT("awaiting_user_approval");
			Report.Diagnostic = TEXT("Waiting for approval in HyperAI Chat, Activity panel. Nothing changed yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true: it starts once approved.");
			return Report;
		}
		// Another agent's edit may own the project lane; then this one waits its turn, and hyper_operation_wait
		// waits through the queue as well as the edit.
		FString QueueStatus;
		FString QueueError;
		if (!FHyperAIStudioExecutionQueue::Submit(Prepared, Request.OperationId, QueueStatus, QueueError))
		{
			return Reject(*QueueStatus, QueueError);
		}
		Report.bStaged = QueueStatus == TEXT("submitted");
		Report.bExecutionSubmitted = Report.bStaged;
		Report.bOk = true;
		Report.Status = QueueStatus;
		Report.Diagnostic = FString::Join(Notes, TEXT("; "))
			+ TEXT(". The edit, compile and save run over the next editor ticks.");
		return Report;
	}
}

FString FHyperAIStudioUIAnimationPayload::GetTypeId() const
{
	return HyperAIStudio::UI::Animation::PayloadTypeId;
}

FString FHyperAIStudioUIAnimationPayload::GetSchemaFingerprint() const
{
	return HyperAIStudio::UI::Animation::PayloadSchema();
}

int32 FHyperAIStudioUIAnimationPayload::GetBoundedByteSize() const
{
	int64 Size = 256 + 2ll * (TargetPath.Len() + BaseRevision.Len() + SemanticFingerprint.Len());
	for (const FHyperAIUIPlanOperation& Operation : Operations)
	{
		Size += 128 + 2ll * (Operation.Kind.Len() + Operation.SubjectId.Len() + Operation.SecondaryName.Len() + Operation.Name.Len()
			+ Operation.StringValue.Len() + Operation.ExpectedElementFingerprint.Len());
	}
	return HyperAIStudio::UI::Animation::ClampBytes(Size);
}

FString FHyperAIStudioUIAnimationPayload::GetSemanticFingerprint() const
{
	return SemanticFingerprint;
}

TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> FHyperAIStudioUIAnimationPayload::CloneImmutable() const
{
	const TSharedRef<FHyperAIStudioUIAnimationPayload, ESPMode::ThreadSafe> Clone = MakeShared<FHyperAIStudioUIAnimationPayload, ESPMode::ThreadSafe>();
	Clone->TargetPath = TargetPath;
	Clone->BaseRevision = BaseRevision;
	Clone->Operations = Operations;
	Clone->SemanticFingerprint = SemanticFingerprint;
	return Clone;
}

FString FHyperAIStudioUIAnimationResultPayload::GetTypeId() const { return HyperAIStudio::UI::Animation::ResultTypeId; }
FString FHyperAIStudioUIAnimationResultPayload::GetSchemaFingerprint() const { return HyperAIStudio::UI::Animation::ResultSchema(); }
int32 FHyperAIStudioUIAnimationResultPayload::GetBoundedByteSize() const { return 64 + 2 * (Phase.Len() + ContentKey.Len()); }

FString FHyperAIStudioUIInspectPayload::GetTypeId() const { return HyperAIStudio::UI::Animation::InspectPayloadTypeId; }
FString FHyperAIStudioUIInspectPayload::GetSchemaFingerprint() const { return HyperAIStudio::UI::Animation::InspectPayloadSchema(); }
int32 FHyperAIStudioUIInspectPayload::GetBoundedByteSize() const
{
	int64 Size = 256 + 2ll * Request.Cursor.Len();
	for (const FString& Path : Request.TargetPaths) Size += 16 + 2ll * Path.Len();
	return HyperAIStudio::UI::Animation::ClampBytes(Size);
}

FString FHyperAIStudioUIValidatePayload::GetTypeId() const { return HyperAIStudio::UI::Animation::ValidatePayloadTypeId; }
FString FHyperAIStudioUIValidatePayload::GetSchemaFingerprint() const { return HyperAIStudio::UI::Animation::ValidatePayloadSchema(); }
int32 FHyperAIStudioUIValidatePayload::GetBoundedByteSize() const
{
	int64 Size = 256;
	for (const FString& Path : Request.TargetPaths) Size += 16 + 2ll * Path.Len();
	return HyperAIStudio::UI::Animation::ClampBytes(Size);
}

FString FHyperAIStudioUIInspectResultPayload::GetTypeId() const { return HyperAIStudio::UI::Animation::InspectResultTypeId; }
FString FHyperAIStudioUIInspectResultPayload::GetSchemaFingerprint() const { return HyperAIStudio::UI::Animation::InspectResultSchema(); }
int32 FHyperAIStudioUIInspectResultPayload::GetBoundedByteSize() const
{
	// ponytail: a coarse bound; the inspect report bounds its own output by max_output_bytes.
	return HyperAIStudio::UI::Animation::ClampBytes(1024 + 6ll * (Report.Status.Len() + Report.Diagnostic.Len())
		+ 4096ll * Report.Records.Num() + 512ll * Report.Issues.Num());
}

FString FHyperAIStudioUIValidateResultPayload::GetTypeId() const { return HyperAIStudio::UI::Animation::ValidateResultTypeId; }
FString FHyperAIStudioUIValidateResultPayload::GetSchemaFingerprint() const { return HyperAIStudio::UI::Animation::ValidateResultSchema(); }
int32 FHyperAIStudioUIValidateResultPayload::GetBoundedByteSize() const
{
	return HyperAIStudio::UI::Animation::ClampBytes(1024 + 6ll * (Report.Status.Len() + Report.Diagnostic.Len()) + 512ll * Report.Issues.Num());
}

const FHyperAIStudioDomainAdapterDescriptor& FHyperAIStudioUIAnimationAdapter::Describe()
{
	using namespace HyperAIStudio::UI::Animation;
	static const FHyperAIStudioDomainAdapterDescriptor Descriptor = []
	{
		FHyperAIStudioDomainAdapterDescriptor Value;
		Value.PackId = FHyperAIStudioUIContracts::PackId;
		Value.AdapterId = TEXT("adapter.ui.widget_animation.ue58");
		Value.SemanticVersion = TEXT("2.0.0");
		Value.AdapterVersion = 2;
		Value.Variants.Add({TEXT("hyper_ui_inspect"), InspectVariantId, InspectPayloadTypeId, InspectPayloadSchema(),
			InspectResultTypeId, InspectResultSchema(), EHyperAIStudioDomainSafety::Read});
		Value.Variants.Add({TEXT("hyper_ui_apply_plan"), VariantId, PayloadTypeId, PayloadSchema(),
			ResultTypeId, ResultSchema(), EHyperAIStudioDomainSafety::Edit});
		Value.Variants.Add({TEXT("hyper_ui_validate"), ValidateVariantId, ValidatePayloadTypeId, ValidatePayloadSchema(),
			ValidateResultTypeId, ValidateResultSchema(), EHyperAIStudioDomainSafety::Read});
		Value.ContractFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeContractFingerprint(Value);
		Value.AdapterFingerprint = FHyperAIStudioDomainAdapterRegistry::ComputeAdapterFingerprint(Value);
		return Value;
	}();
	return Descriptor;
}

FHyperAIStudioDomainAdapterResult FHyperAIStudioUIAnimationAdapter::Execute(
	const FHyperAIStudioDomainDispatchContext& Context,
	const IHyperAIStudioDomainRequestPayload& Payload)
{
	using namespace HyperAIStudio::UI::Animation;
	FHyperAIStudioDomainAdapterResult Result;
	Result.Outcome = EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect;
	auto Matches = [&](const TCHAR* Tool, const TCHAR* Variant, const EHyperAIStudioDomainSafety Safety, const TCHAR* TypeId, const FString& Schema)
	{
		return Context.Binding.PackId == FHyperAIStudioUIContracts::PackId && Context.Binding.ToolName == Tool
			&& Context.Binding.VariantId == Variant && Context.Safety == Safety && Payload.GetTypeId() == TypeId
			&& Payload.GetSchemaFingerprint() == Schema;
	};
	auto Succeed = [&Result](const TSharedRef<IHyperAIStudioDomainResultPayload, ESPMode::ThreadSafe>& Output, const FString& Status, const FString& Diagnostic)
	{
		Result.Outcome = EHyperAIStudioDomainDispatchOutcome::Succeeded;
		Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
		Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
		Result.Payload = Output;
		return Result;
	};
	if (Matches(TEXT("hyper_ui_inspect"), InspectVariantId, EHyperAIStudioDomainSafety::Read, InspectPayloadTypeId, InspectPayloadSchema()))
	{
		const TSharedRef<FHyperAIStudioUIInspectResultPayload, ESPMode::ThreadSafe> Output = MakeShared<FHyperAIStudioUIInspectResultPayload, ESPMode::ThreadSafe>();
		Output->Report = UHyperAIStudioUIToolset::hyper_ui_inspect(static_cast<const FHyperAIStudioUIInspectPayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(TEXT("hyper_ui_validate"), ValidateVariantId, EHyperAIStudioDomainSafety::Read, ValidatePayloadTypeId, ValidatePayloadSchema()))
	{
		const TSharedRef<FHyperAIStudioUIValidateResultPayload, ESPMode::ThreadSafe> Output = MakeShared<FHyperAIStudioUIValidateResultPayload, ESPMode::ThreadSafe>();
		Output->Report = UHyperAIStudioUIToolset::hyper_ui_validate(static_cast<const FHyperAIStudioUIValidatePayload&>(Payload).Request);
		return Succeed(Output, Output->Report.Status, Output->Report.Diagnostic);
	}
	if (Matches(TEXT("hyper_ui_apply_plan"), VariantId, EHyperAIStudioDomainSafety::Edit, PayloadTypeId, PayloadSchema()))
	{
		const FHyperAIStudioUIAnimationPayload& Typed = static_cast<const FHyperAIStudioUIAnimationPayload&>(Payload);
		if (!IsExecutablePlan(Typed.Operations) || ComputeSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
		{
			Result.StatusCode = TEXT("typed_payload_drift");
			Result.Diagnostic = TEXT("The sealed Widget Blueprint plan drifted.");
			return Result;
		}
		return ExecutePhase(Context.ActionKind, Typed);
	}
	Result.StatusCode = TEXT("typed_binding_mismatch");
	Result.Diagnostic = TEXT("Widget Blueprint adapter rejected a non-exact tool, variant, safety, schema, or DTO binding.");
	return Result;
}

FString FHyperAIStudioUIAnimationFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioUIAnimationAdapter::Describe().AdapterFingerprint;
}

bool FHyperAIStudioUIAnimationFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request, FString& OutCanonicalEffectTarget, FString& OutError)
{
	using namespace HyperAIStudio::UI::Animation;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != PayloadTypeId || Request.GetSchemaFingerprint() != PayloadSchema())
	{
		OutError = TEXT("Fresh verifier received the wrong typed Widget Blueprint request schema.");
		return false;
	}
	const FHyperAIStudioUIAnimationPayload& Typed = static_cast<const FHyperAIStudioUIAnimationPayload&>(Request);
	if (!FHyperAIStudioUIContracts::IsCanonicalProjectObjectPath(Typed.TargetPath) || !IsExecutablePlan(Typed.Operations)
		|| ComputeSemanticFingerprint(Typed) != Typed.SemanticFingerprint)
	{
		OutError = TEXT("Fresh verifier rejected the canonical target or semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = Typed.TargetPath;
	return true;
}

bool FHyperAIStudioUIAnimationFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request, const IHyperAIStudioDomainResultPayload& Result, FString& OutPostconditionHash, FString& OutError)
{
	using namespace HyperAIStudio::UI::Animation;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh Widget Blueprint verification requires the game thread.");
		return false;
	}
	FString Target;
	if (!ResolveCanonicalEffectTarget(Request, Target, OutError) || Result.GetTypeId() != ResultTypeId || Result.GetSchemaFingerprint() != ResultSchema())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioUIAnimationPayload& Typed = static_cast<const FHyperAIStudioUIAnimationPayload&>(Request);
	const FHyperAIStudioUIAnimationResultPayload& TypedResult = static_cast<const FHyperAIStudioUIAnimationResultPayload&>(Result);
	const UWidgetBlueprint* Blueprint = FindObject<UWidgetBlueprint>(nullptr, *Target);
	if (TypedResult.Phase != TEXT("completed") || !Blueprint)
	{
		OutError = TEXT("Only a completed fresh capture of a loaded Widget Blueprint may be verified.");
		return false;
	}
	const FString Key = ComputeRevision(*Blueprint);
	if (Key != TypedResult.ContentKey)
	{
		OutError = TEXT("The Widget Blueprint changed between fresh capture and verification.");
		return false;
	}
	if (Blueprint->GetOutermost()->IsDirty())
	{
		OutError = TEXT("The Widget Blueprint still has unsaved changes after the save phase.");
		return false;
	}
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(
		TEXT("hyperai.ui.widget-edit-postcondition.v2|") + Typed.SemanticFingerprint + TEXT("|") + Key);
	if (!FHyperAIStudioUIContracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

void FHyperAIStudioUIAnimationBackend::Register()
{
	if (AdapterHandle.IsValid() || !FHyperAIStudioTrustedExecutionFacade::IsAvailable()) return;
	FString Error;
	Adapter = MakeShared<FHyperAIStudioUIAnimationAdapter, ESPMode::ThreadSafe>();
	if (!FHyperAIStudioTrustedExecutionFacade::RegisterAdapter(Adapter.ToSharedRef(), AdapterHandle, Error)
		|| !FHyperAIStudioTrustedExecutionFacade::RegisterLiveProbe(AdapterHandle, FHyperAIStudioUIContracts::LiveProbeId, ProbeHandle, Error))
	{
		UE_LOG(LogHyperAIStudioUIAnimation, Warning, TEXT("Widget Blueprint plans stay staged-only: %s"), *Error);
		Unregister();
		return;
	}
	const auto Observe = []()
	{
		FHyperAIStudioTrustedProbeResult Observation;
		Observation.bReady = GEditor != nullptr && FModuleManager::Get().IsModuleLoaded(TEXT("UMGEditor"));
		Observation.StatusCode = Observation.bReady ? TEXT("ready") : TEXT("umg_editor_unavailable");
		Observation.Diagnostic = Observation.bReady ? TEXT("Widget Blueprints can be edited.") : TEXT("The UMG editor module is not loaded.");
		return Observation;
	};
	if (!ProbePublisher.Start(ProbeHandle, Observe, Error))
	{
		UE_LOG(LogHyperAIStudioUIAnimation, Warning, TEXT("Widget Blueprint plans stay staged-only: the live probe did not start: %s"), *Error);
		Unregister();
	}
}

void FHyperAIStudioUIAnimationBackend::Unregister()
{
	if (!IsInGameThread()) return;
	ProbePublisher.Stop();
	FString Error;
	if (ProbeHandle.IsValid() && FHyperAIStudioTrustedExecutionFacade::UnregisterLiveProbe(ProbeHandle, Error))
	{
		ProbeHandle = {};
	}
	if (AdapterHandle.IsValid())
	{
		const EHyperAIStudioDomainUnregisterResult Outcome = FHyperAIStudioTrustedExecutionFacade::UnregisterAdapter(AdapterHandle, Error);
		if (Outcome == EHyperAIStudioDomainUnregisterResult::Removed || Outcome == EHyperAIStudioDomainUnregisterResult::NotFound)
		{
			AdapterHandle = {};
			Adapter.Reset();
		}
	}
}

#undef LOCTEXT_NAMESPACE
