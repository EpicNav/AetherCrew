// Games by Hyper 2026.

#include "HyperAIStudioSoundMix.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSoundCueGraph.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"
#include "SubmixEffects/AudioMixerSubmixEffectDynamicsProcessor.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "SubmixEffects/AudioMixerSubmixEffectReverb.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::AudioAuthoring::Mix
{
	namespace
	{
		using C = FHyperAIStudioAudioAuthoringContracts;

		/** Where a setting lives: the asset's own settings struct first (a class's Properties, attenuation's Attenuation), then the asset. */
		struct FTarget
		{
			const UStruct* Struct = nullptr;
			void* Container = nullptr;
			UObject* Owner = nullptr;
		};

		/** Links between assets, which the plan's own fields set so both ends stay in step. */
		const TCHAR* const Blocked[] = {TEXT("ParentClass"), TEXT("ChildClasses"), TEXT("ParentSubmix"), TEXT("ChildSubmixes"),
			TEXT("SubmixEffectChain"), TEXT("SoundClassEffects"), TEXT("EQSettings"), TEXT("Properties"), TEXT("Attenuation"),
			TEXT("Concurrency"), TEXT("Settings"), TEXT("bChanged")};

		const TCHAR* SettingsStructOf(const UClass& Class)
		{
			if (Class.IsChildOf<USoundClass>()) return TEXT("Properties");
			if (Class.IsChildOf<USoundAttenuation>()) return TEXT("Attenuation");
			if (Class.IsChildOf<USoundConcurrency>()) return TEXT("Concurrency");
			if (Class.IsChildOf<USoundEffectSubmixPreset>()) return TEXT("Settings");
			return nullptr;
		}

		TArray<FTarget> TargetsOf(UObject& Object)
		{
			TArray<FTarget> Out;
			if (const TCHAR* Name = SettingsStructOf(*Object.GetClass()))
			{
				// Found through the property, not the struct's own StaticStruct, which the engine does not export for all of them.
				if (const FStructProperty* Property = FindFProperty<FStructProperty>(Object.GetClass(), Name))
				{
					Out.Add({Property->Struct, Property->ContainerPtrToValuePtr<void>(&Object), &Object});
				}
			}
			Out.Add({Object.GetClass(), &Object, &Object});
			return Out;
		}

		bool IsSettable(const FProperty& Property)
		{
			if (!Property.HasAnyPropertyFlags(CPF_Edit) || Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_Transient))
			{
				return false;
			}
			for (const TCHAR* Name : Blocked)
			{
				if (Property.GetFName() == FName(Name))
				{
					return false;
				}
			}
			return true;
		}

		FProperty* FindSetting(UObject& Object, const FString& Name, FTarget& OutTarget)
		{
			for (const FTarget& Target : TargetsOf(Object))
			{
				if (FProperty* Property = FindFProperty<FProperty>(Target.Struct, FName(*Name)))
				{
					OutTarget = Target;
					return Property;
				}
			}
			return nullptr;
		}

		FString Export(const FProperty& Property, const void* Value, UObject* Owner)
		{
			FString Text;
			// Its own delta: with none, values equal to the type's zero are left out.
			Property.ExportText_Direct(Text, Value, Value, Owner, PPF_None);
			return Text;
		}

		/**
		 * Text import reads a word as 0 for a number and anything but true as false, so numbers and switches are checked
		 * here first: "loud" is refused rather than set to 0.
		 */
		bool FitsKind(const FProperty& Property, const FString& Value, FString& OutError)
		{
			const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
			if (Numeric && !Numeric->IsEnum() && !Value.IsNumeric())
			{
				OutError = FString::Printf(TEXT("%s is a number, not '%s'."), *Property.GetName(), *Value);
				return false;
			}
			if (Property.IsA<FBoolProperty>() && !(Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase)
				|| Value == TEXT("1") || Value == TEXT("0")))
			{
				OutError = FString::Printf(TEXT("%s is true or false, not '%s'."), *Property.GetName(), *Value);
				return false;
			}
			return true;
		}

		/** Imports Value into a copy of the setting; the new value's text, or false with the reason. Nothing is changed. */
		bool TryImport(const FProperty& Property, const FTarget& Target, const FString& Value, FString& OutNew, FString& OutError)
		{
			FString Reason;
			if (!FHyperAIStudioNames::CanImportText(Property, Value, *Property.GetName(), &Reason))
			{
				OutError = Reason;
				return false;
			}
			if (!FitsKind(Property, Value, OutError))
			{
				return false;
			}
			void* Copy = Property.AllocateAndInitializeValue();
			Property.CopyCompleteValue(Copy, Property.ContainerPtrToValuePtr<void>(Target.Container));
			FStringOutputDevice Errors;
			const TCHAR* End = Property.ImportText_Direct(*Value, Copy, Target.Owner, PPF_None, &Errors);
			const bool bOk = End != nullptr && Errors.IsEmpty();
			if (bOk)
			{
				OutNew = Export(Property, Copy, Target.Owner);
			}
			Property.DestroyAndFreeValue(Copy);
			if (!bOk)
			{
				OutError = FString::Printf(TEXT("%s does not take '%s'%s"), *Property.GetName(), *Value,
					Errors.IsEmpty() ? TEXT(".") : *FString::Printf(TEXT(": %s"), *Errors.TrimStartAndEnd()));
			}
			return bOk;
		}

		/** The settings an inspect shows unless asked for all. */
		const TArray<FName>& CommonSettings(const UClass& Class)
		{
			static const TArray<FName> SoundClass = {TEXT("Volume"), TEXT("Pitch"), TEXT("LowPassFilterFrequency"), TEXT("AttenuationDistanceScale"),
				TEXT("bApplyEffects"), TEXT("bAlwaysPlay"), TEXT("bIsUISound"), TEXT("bIsMusic"), TEXT("bReverb"), TEXT("Default2DReverbSendAmount"),
				TEXT("bCenterChannelOnly"), TEXT("bApplyAmbientVolumes"), TEXT("OutputTarget"), TEXT("DefaultSubmix")};
			static const TArray<FName> MixSettings = {TEXT("bApplyEQ"), TEXT("EQPriority"), TEXT("InitialDelay"), TEXT("FadeInTime"), TEXT("Duration"), TEXT("FadeOutTime")};
			static const TArray<FName> Attenuation = {TEXT("bAttenuate"), TEXT("bSpatialize"), TEXT("DistanceAlgorithm"), TEXT("AttenuationShape"),
				TEXT("AttenuationShapeExtents"), TEXT("FalloffDistance"), TEXT("dBAttenuationAtMax"), TEXT("FalloffMode"), TEXT("bAttenuateWithLPF"),
				TEXT("LPFRadiusMin"), TEXT("LPFRadiusMax"), TEXT("LPFFrequencyAtMin"), TEXT("LPFFrequencyAtMax"), TEXT("bEnableOcclusion"),
				TEXT("OcclusionLowPassFilterFrequency"), TEXT("OcclusionVolumeAttenuation"), TEXT("bEnableReverbSend"), TEXT("ReverbWetLevelMin"),
				TEXT("ReverbWetLevelMax"), TEXT("bEnableListenerFocus"), TEXT("NonSpatializedRadiusStart"), TEXT("NonSpatializedRadiusEnd"),
				TEXT("SpatializationAlgorithm")};
			static const TArray<FName> Concurrency = {TEXT("MaxCount"), TEXT("bLimitToOwner"), TEXT("ResolutionRule"), TEXT("RetriggerTime"),
				TEXT("VolumeScale"), TEXT("VolumeScaleMode"), TEXT("VoiceStealReleaseTime")};
			static const TArray<FName> Submix = {TEXT("OutputVolumeModulation"), TEXT("WetLevelModulation"), TEXT("DryLevelModulation")};
			static const TArray<FName> Reverb = {TEXT("DecayTime"), TEXT("Density"), TEXT("Diffusion"), TEXT("Gain"), TEXT("GainHF"),
				TEXT("ReflectionsDelay"), TEXT("LateDelay"), TEXT("WetLevel"), TEXT("DryLevel")};
			static const TArray<FName> Dynamics = {TEXT("DynamicsProcessorType"), TEXT("ThresholdDb"), TEXT("Ratio"), TEXT("AttackTimeMsec"),
				TEXT("ReleaseTimeMsec"), TEXT("KneeBandwidthDb"), TEXT("LookAheadMsec"), TEXT("InputGainDb"), TEXT("OutputGainDb")};
			static const TArray<FName> Cue = {TEXT("VolumeMultiplier"), TEXT("PitchMultiplier"), TEXT("SoundClassObject"), TEXT("AttenuationSettings"),
				TEXT("ConcurrencySet"), TEXT("Priority"), TEXT("bOverrideAttenuation")};
			static const TArray<FName> Sound = {TEXT("bLooping"), TEXT("Volume"), TEXT("Pitch"), TEXT("SoundClassObject"), TEXT("AttenuationSettings"),
				TEXT("ConcurrencySet"), TEXT("Priority")};
			static const TArray<FName> None;
			if (Class.IsChildOf<USoundCue>()) return Cue;
			if (Class.IsChildOf<USoundBase>()) return Sound;
			if (Class.IsChildOf<USoundClass>()) return SoundClass;
			if (Class.IsChildOf<USoundMix>()) return MixSettings;
			if (Class.IsChildOf<USoundAttenuation>()) return Attenuation;
			if (Class.IsChildOf<USoundConcurrency>()) return Concurrency;
			if (Class.IsChildOf<USoundSubmix>()) return Submix;
			if (Class.IsChildOf<USubmixEffectReverbPreset>()) return Reverb;
			if (Class.IsChildOf<USubmixEffectDynamicsProcessorPreset>()) return Dynamics;
			return None;
		}

		UClass* EffectClassOf(const FString& Kind)
		{
			if (Kind == TEXT("reverb")) return USubmixEffectReverbPreset::StaticClass();
			if (Kind == TEXT("eq")) return USubmixEffectSubmixEQPreset::StaticClass();
			if (Kind == TEXT("compressor")) return USubmixEffectDynamicsProcessorPreset::StaticClass();
			return nullptr;
		}

		FString EffectKindOf(const UObject& Preset)
		{
			if (Preset.IsA<USubmixEffectReverbPreset>()) return TEXT("reverb");
			if (Preset.IsA<USubmixEffectSubmixEQPreset>()) return TEXT("eq");
			if (Preset.IsA<USubmixEffectDynamicsProcessorPreset>()) return TEXT("compressor");
			return Preset.GetClass()->GetName();
		}

		/** An asset already in the project at Package, of Class; null when the package is free. Refuses one of another class. */
		bool Resolve(const FString& Package, UClass& Class, const TCHAR* Field, TWeakObjectPtr<UObject>& OutExisting, FString& OutError)
		{
			FString Reason;
			if (!FHyperAIStudioNames::IsPackagePath(Package, Field, &Reason))
			{
				OutError = Reason;
				return false;
			}
			if (FHyperAIStudioNames::IsFreePackage(Package))
			{
				OutExisting = nullptr;
				return true;
			}
			UObject* Found = CueGraph::LoadAsset(Package, *UObject::StaticClass());
			if (!Found || !Found->IsA(&Class))
			{
				OutError = FString::Printf(TEXT("%s is %s, not a %s."), *Package, Found ? *Found->GetClass()->GetName() : TEXT("something that does not load"), *Class.GetName());
				return false;
			}
			OutExisting = Found;
			return true;
		}

		UObject* Make(const FString& Package, UClass& Class)
		{
			UPackage* Outer = CreatePackage(*Package);
			UObject* Asset = NewObject<UObject>(Outer, &Class, FName(*FPackageName::GetShortName(Package)), RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Asset);
			Outer->MarkPackageDirty();
			return Asset;
		}

		bool ValidBands(const TArray<FHyperAISoundEQBand>& Bands, const bool bDb, FString& OutError)
		{
			for (const FHyperAISoundEQBand& Band : Bands)
			{
				const bool bGain = bDb ? (Band.Gain >= -90.f && Band.Gain <= 20.f) : (Band.Gain >= 0.f && Band.Gain <= 10.f);
				if (!(Band.Frequency > 0.f && Band.Frequency <= 20000.f) || !bGain || !(Band.Bandwidth > 0.f && Band.Bandwidth <= 2.f))
				{
					OutError = bDb ? TEXT("Each band: Frequency 1-20000 Hz, Gain -90 to 20 dB, Bandwidth above 0 and up to 2 octaves.")
						: TEXT("Each band: Frequency 1-20000 Hz, Gain 0-10 (linear, 1 is flat), Bandwidth above 0 and up to 2 octaves.");
					return false;
				}
			}
			return true;
		}

		/** The parent chain from Start, through the plan's own parents where it sets them; true when it reaches Self. */
		template <typename TAsset>
		bool Loops(const FString& Self, const FString& Start, const TMap<FString, FString>& Planned, TAsset* (*ParentOf)(const FString&))
		{
			FString At = Start;
			for (int32 Step = 0; Step < 256 && !At.IsEmpty(); ++Step)
			{
				if (At.Equals(Self, ESearchCase::IgnoreCase))
				{
					return true;
				}
				if (const FString* Next = Planned.Find(At.ToLower()))
				{
					At = *Next == TEXT("none") ? FString() : *Next;
					continue;
				}
				TAsset* Parent = ParentOf(At);
				At = Parent ? Parent->GetOutermost()->GetName() : FString();
			}
			return false;
		}

		USoundClass* ClassParentOf(const FString& Package)
		{
			const USoundClass* Class = Cast<USoundClass>(CueGraph::LoadAsset(Package, *USoundClass::StaticClass()));
			return Class ? Class->ParentClass.Get() : nullptr;
		}

		USoundSubmixBase* SubmixParentOf(const FString& Package)
		{
			const USoundSubmix* Submix = Cast<USoundSubmix>(CueGraph::LoadAsset(Package, *USoundSubmix::StaticClass()));
			return Submix ? Submix->ParentSubmix.Get() : nullptr;
		}

		FString Line(const FString& Package, const FString& What)
		{
			return FString::Printf(TEXT("%s: %s"), *FPackageName::GetShortName(Package), *What);
		}
	}

	FString KindOf(const UObject& Asset)
	{
		if (Asset.IsA<USoundClass>()) return TEXT("sound_class");
		if (Asset.IsA<USoundMix>()) return TEXT("sound_mix");
		if (Asset.IsA<USoundAttenuation>()) return TEXT("attenuation");
		if (Asset.IsA<USoundConcurrency>()) return TEXT("concurrency");
		if (Asset.IsA<USoundSubmix>()) return TEXT("submix");
		return FString();
	}

	UClass* ClassOfKind(const FString& Kind)
	{
		if (Kind == TEXT("sound_class")) return USoundClass::StaticClass();
		if (Kind == TEXT("sound_mix")) return USoundMix::StaticClass();
		if (Kind == TEXT("attenuation")) return USoundAttenuation::StaticClass();
		if (Kind == TEXT("concurrency")) return USoundConcurrency::StaticClass();
		if (Kind == TEXT("submix")) return USoundSubmix::StaticClass();
		return nullptr;
	}

	bool Check(const FHyperAISoundMixPlanRequest& Request, TArray<FCheckedAsset>& Out, FHyperAISoundMixPlanReport& Report)
	{
		Out.Reset();
		Report.Changes.Reset();
		auto Refuse = [&Report](const FString& Why)
		{
			Report.bOk = false;
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Why;
			return false;
		};
		if (Request.Assets.IsEmpty() || Request.Assets.Num() > C::MaxMixAssets)
		{
			return Refuse(FString::Printf(TEXT("A plan has 1 to %d assets."), C::MaxMixAssets));
		}
		// Packages the plan makes or changes, with their class: later entries may name them as parents or adjusted classes.
		TMap<FString, UClass*> Planned;
		TMap<FString, FString> PlannedClassParents;
		TMap<FString, FString> PlannedSubmixParents;
		for (int32 Index = 0; Index < Request.Assets.Num(); ++Index)
		{
			FCheckedAsset& Asset = Out.AddDefaulted_GetRef();
			Asset.Spec = Request.Assets[Index];
			FHyperAISoundMixAsset& Spec = Asset.Spec;
			Spec.Kind = Spec.Kind.TrimStartAndEnd().ToLower();
			const FString Where = FString::Printf(TEXT("Assets[%d] (%s %s): "), Index, *Spec.Kind, *Spec.Path);
			Asset.Class = ClassOfKind(Spec.Kind);
			if (!Asset.Class)
			{
				return Refuse(Where + TEXT("Kind is sound_class, sound_mix, attenuation, concurrency or submix."));
			}
			Asset.Package = CueGraph::PackageOf(Spec.Path);
			FString Error;
			if (!Resolve(Asset.Package, *Asset.Class, TEXT("Path"), Asset.Existing, Error))
			{
				return Refuse(Where + Error);
			}
			if (Planned.Contains(Asset.Package.ToLower()))
			{
				return Refuse(Where + TEXT("the plan names this asset twice; put all of its changes in one entry."));
			}
			Planned.Add(Asset.Package.ToLower(), Asset.Class);
			UObject* Subject = Asset.Existing.IsValid() ? Asset.Existing.Get() : Asset.Class->GetDefaultObject();
			if (!Asset.Existing.IsValid())
			{
				Report.Changes.Add(Line(Asset.Package, FString::Printf(TEXT("new %s"), *Spec.Kind)));
			}
			if (Spec.Settings.Num() > C::MaxSettings)
			{
				return Refuse(Where + FString::Printf(TEXT("at most %d settings per asset."), C::MaxSettings));
			}
			for (const FHyperAIAudioSetting& Setting : Spec.Settings)
			{
				FTarget Target;
				const FProperty* Property = FindSetting(*Subject, Setting.Name.TrimStartAndEnd(), Target);
				if (!Property || !IsSettable(*Property))
				{
					return Refuse(Where + FString::Printf(TEXT("%s is not a setting a plan can change here; hyper_sound_asset_inspect with bAllSettings lists them."), *Setting.Name));
				}
				FString New;
				if (!TryImport(*Property, Target, Setting.Value.TrimStartAndEnd(), New, Error))
				{
					return Refuse(Where + Error);
				}
				const FString Old = Export(*Property, Property->ContainerPtrToValuePtr<void>(Target.Container), Target.Owner);
				Report.Changes.Add(Line(Asset.Package, FString::Printf(TEXT("%s %s -> %s"), *Property->GetName(), *Old, *New)));
			}

			const FString Parent = Spec.Parent.TrimStartAndEnd();
			if (!Parent.IsEmpty())
			{
				if (Spec.Kind != TEXT("sound_class") && Spec.Kind != TEXT("submix"))
				{
					return Refuse(Where + TEXT("only a sound_class or a submix has a Parent."));
				}
				if (Parent.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					Asset.ParentPackage = TEXT("none");
				}
				else
				{
					Asset.ParentPackage = CueGraph::PackageOf(Parent);
					UClass* const* PlannedClass = Planned.Find(Asset.ParentPackage.ToLower());
					TWeakObjectPtr<UObject> ParentAsset;
					if (PlannedClass ? *PlannedClass != Asset.Class : !Resolve(Asset.ParentPackage, *Asset.Class, TEXT("Parent"), ParentAsset, Error) || !ParentAsset.IsValid())
					{
						return Refuse(Where + (Error.IsEmpty() ? FString::Printf(TEXT("Parent %s is not a %s in the project or made earlier in this plan."), *Parent, *Asset.Class->GetName()) : Error));
					}
				}
				TMap<FString, FString>& Parents = Spec.Kind == TEXT("sound_class") ? PlannedClassParents : PlannedSubmixParents;
				Parents.Add(Asset.Package.ToLower(), Asset.ParentPackage);
				const bool bLoops = Asset.ParentPackage != TEXT("none") && (Spec.Kind == TEXT("sound_class")
					? Loops<USoundClass>(Asset.Package, Asset.ParentPackage, Parents, &ClassParentOf)
					: Loops<USoundSubmixBase>(Asset.Package, Asset.ParentPackage, Parents, &SubmixParentOf));
				if (bLoops)
				{
					return Refuse(Where + FString::Printf(TEXT("%s is %s's own child, so it cannot be its parent."), *Parent, *FPackageName::GetShortName(Asset.Package)));
				}
				Report.Changes.Add(Line(Asset.Package, TEXT("parent -> ") + (Asset.ParentPackage == TEXT("none") ? FString(TEXT("none")) : FPackageName::GetShortName(Asset.ParentPackage))));
			}

			if (!Spec.Adjusters.IsEmpty() || !Spec.EQBands.IsEmpty())
			{
				if (Spec.Kind != TEXT("sound_mix"))
				{
					return Refuse(Where + TEXT("Adjusters and EQBands belong to a sound_mix."));
				}
				for (const FHyperAISoundMixAdjuster& Adjuster : Spec.Adjusters)
				{
					const FString ClassPackage = CueGraph::PackageOf(Adjuster.SoundClass);
					UClass* const* PlannedClass = Planned.Find(ClassPackage.ToLower());
					TWeakObjectPtr<UObject> Adjusted;
					Error.Reset();
					if (PlannedClass ? *PlannedClass != USoundClass::StaticClass() : !Resolve(ClassPackage, *USoundClass::StaticClass(), TEXT("SoundClass"), Adjusted, Error) || !Adjusted.IsValid())
					{
						return Refuse(Where + (Error.IsEmpty() ? FString::Printf(TEXT("adjuster SoundClass %s is not a Sound Class in the project or made earlier in this plan."), *Adjuster.SoundClass) : Error));
					}
					if (!(Adjuster.Volume >= 0.f && Adjuster.Volume <= 4.f) || !(Adjuster.Pitch > 0.f && Adjuster.Pitch <= 4.f)
						|| !(Adjuster.LowPassFilterFrequency > 0.f && Adjuster.LowPassFilterFrequency <= 20000.f))
					{
						return Refuse(Where + TEXT("an adjuster's Volume is 0-4, Pitch above 0 and up to 4, LowPassFilterFrequency 1-20000 Hz."));
					}
					Asset.AdjusterPackages.Add(ClassPackage);
					Report.Changes.Add(Line(Asset.Package, FString::Printf(TEXT("adjust %s: volume %g, pitch %g, low pass %g Hz%s"), *FPackageName::GetShortName(ClassPackage),
						Adjuster.Volume, Adjuster.Pitch, Adjuster.LowPassFilterFrequency, Adjuster.bApplyToChildren ? TEXT(", and its children") : TEXT(""))));
				}
				if (Spec.EQBands.Num() > 4)
				{
					return Refuse(Where + TEXT("a Sound Mix has 4 EQ bands."));
				}
				if (!ValidBands(Spec.EQBands, /*bDb=*/false, Error))
				{
					return Refuse(Where + Error);
				}
				if (!Spec.EQBands.IsEmpty())
				{
					Report.Changes.Add(Line(Asset.Package, FString::Printf(TEXT("EQ on, %d band(s)"), Spec.EQBands.Num())));
				}
			}

			if (!Spec.Effects.IsEmpty())
			{
				if (Spec.Kind != TEXT("submix"))
				{
					return Refuse(Where + TEXT("Effects belong to a submix."));
				}
				if (Spec.Effects.Num() > 8)
				{
					return Refuse(Where + TEXT("at most 8 effects on a submix."));
				}
				TSet<FString> Presets;
				for (int32 EffectIndex = 0; EffectIndex < Spec.Effects.Num(); ++EffectIndex)
				{
					FCheckedEffect& Effect = Asset.Effects.AddDefaulted_GetRef();
					Effect.Spec = Spec.Effects[EffectIndex];
					Effect.Spec.Kind = Effect.Spec.Kind.TrimStartAndEnd().ToLower();
					const FString EffectWhere = Where + FString::Printf(TEXT("Effects[%d] (%s): "), EffectIndex, *Effect.Spec.Kind);
					Effect.Class = EffectClassOf(Effect.Spec.Kind);
					if (!Effect.Class)
					{
						return Refuse(EffectWhere + TEXT("Kind is reverb, eq or compressor."));
					}
					Effect.Package = Effect.Spec.Preset.TrimStartAndEnd().IsEmpty()
						? FString::Printf(TEXT("%s_%s%s"), *Asset.Package, *Effect.Spec.Kind, EffectIndex > 0 ? *FString::Printf(TEXT("%d"), EffectIndex + 1) : TEXT(""))
						: CueGraph::PackageOf(Effect.Spec.Preset);
					if (Presets.Contains(Effect.Package.ToLower()) || Planned.Contains(Effect.Package.ToLower()))
					{
						return Refuse(EffectWhere + FString::Printf(TEXT("%s is used twice in the plan."), *Effect.Package));
					}
					Presets.Add(Effect.Package.ToLower());
					if (!Resolve(Effect.Package, *Effect.Class, TEXT("Preset"), Effect.Existing, Error))
					{
						return Refuse(EffectWhere + Error);
					}
					UObject* Preset = Effect.Existing.IsValid() ? Effect.Existing.Get() : Effect.Class->GetDefaultObject();
					for (const FHyperAIAudioSetting& Setting : Effect.Spec.Settings)
					{
						FTarget Target;
						const FProperty* Property = FindSetting(*Preset, Setting.Name.TrimStartAndEnd(), Target);
						FString New;
						if (!Property || Target.Owner != Preset || Target.Struct == Preset->GetClass() || !IsSettable(*Property)
							|| !TryImport(*Property, Target, Setting.Value.TrimStartAndEnd(), New, Error))
						{
							return Refuse(EffectWhere + (Error.IsEmpty() ? FString::Printf(TEXT("%s is not one of this effect's settings."), *Setting.Name) : Error));
						}
					}
					if (!Effect.Spec.Bands.IsEmpty() && Effect.Spec.Kind != TEXT("eq"))
					{
						return Refuse(EffectWhere + TEXT("Bands belong to an eq effect."));
					}
					if (Effect.Spec.Bands.Num() > 16 || !ValidBands(Effect.Spec.Bands, /*bDb=*/true, Error))
					{
						return Refuse(EffectWhere + (Error.IsEmpty() ? FString(TEXT("at most 16 bands.")) : Error));
					}
					Report.Changes.Add(Line(Asset.Package, FString::Printf(TEXT("effect %d: %s (%s%s)"), EffectIndex + 1, *Effect.Spec.Kind,
						Effect.Existing.IsValid() ? TEXT("reusing ") : TEXT("new "), *FPackageName::GetShortName(Effect.Package))));
				}
			}
		}
		return true;
	}

	bool ApplySetting(UObject& Asset, const FHyperAIAudioSetting& Setting, FString& OutError)
	{
		FTarget Target;
		const FProperty* Property = FindSetting(Asset, Setting.Name.TrimStartAndEnd(), Target);
		if (!Property || !IsSettable(*Property))
		{
			OutError = FString::Printf(TEXT("%s is not a setting of %s."), *Setting.Name, *Asset.GetName());
			return false;
		}
		FString Reason;
		if (!FHyperAIStudioNames::CanImportText(*Property, Setting.Value, *Property->GetName(), &Reason))
		{
			OutError = Reason;
			return false;
		}
		if (!FitsKind(*Property, Setting.Value.TrimStartAndEnd(), OutError))
		{
			return false;
		}
		FStringOutputDevice Errors;
		const TCHAR* End = Property->ImportText_InContainer(*Setting.Value.TrimStartAndEnd(), Target.Container, Target.Owner, PPF_None, &Errors);
		if (!End || !Errors.IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s did not take '%s'."), *Property->GetName(), *Setting.Value);
			return false;
		}
		return true;
	}

	bool Apply(const TArray<FCheckedAsset>& Checked, TArray<UObject*>& OutTouched, FHyperAISoundMixPlanReport& Report)
	{
		TMap<FString, UObject*> Made;
		auto Find = [&Made](const FString& Package, UClass& Class) -> UObject*
		{
			if (UObject* const* Planned = Made.Find(Package.ToLower()))
			{
				return *Planned;
			}
			return CueGraph::LoadAsset(Package, Class);
		};
		auto Fail = [&Report](const FString& Why)
		{
			Report.bOk = false;
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Why + TEXT(" Edit > Undo takes back what the plan changed before this.");
			return false;
		};
		for (const FCheckedAsset& Asset : Checked)
		{
			UObject* Object = Asset.Existing.Get();
			if (!Object)
			{
				if (!FHyperAIStudioNames::IsFreePackage(Asset.Package))
				{
					return Fail(FString::Printf(TEXT("%s appeared after the plan was checked; nothing was made over it."), *Asset.Package));
				}
				// A new asset takes its settings without PreEditChange, which would let undo blank them.
				Object = Make(Asset.Package, *Asset.Class);
				Report.Created.Add(Object->GetPathName());
			}
			else
			{
				Object->Modify();
			}
			Made.Add(Asset.Package.ToLower(), Object);
			OutTouched.AddUnique(Object);
			for (const FHyperAIAudioSetting& Setting : Asset.Spec.Settings)
			{
				FString Error;
				if (!ApplySetting(*Object, Setting, Error))
				{
					return Fail(Error);
				}
			}

			if (USoundClass* SoundClass = Cast<USoundClass>(Object); SoundClass && !Asset.ParentPackage.IsEmpty())
			{
				USoundClass* NewParent = Asset.ParentPackage == TEXT("none") ? nullptr : Cast<USoundClass>(Find(Asset.ParentPackage, *USoundClass::StaticClass()));
				if (Asset.ParentPackage != TEXT("none") && !NewParent)
				{
					return Fail(FString::Printf(TEXT("The parent %s is gone."), *Asset.ParentPackage));
				}
				// SetParentClass takes the class from its old parent's children but does not add it to the new one's.
				if (USoundClass* OldParent = SoundClass->ParentClass)
				{
					OldParent->Modify();
					OutTouched.AddUnique(OldParent);
				}
				if (NewParent)
				{
					NewParent->Modify();
					OutTouched.AddUnique(NewParent);
				}
				SoundClass->SetParentClass(NewParent);
				if (NewParent)
				{
					NewParent->ChildClasses.AddUnique(SoundClass);
				}
				SoundClass->RefreshAllGraphs(false);
			}

			if (USoundSubmix* Submix = Cast<USoundSubmix>(Object))
			{
				if (!Asset.ParentPackage.IsEmpty())
				{
					USoundSubmix* NewParent = Asset.ParentPackage == TEXT("none") ? nullptr : Cast<USoundSubmix>(Find(Asset.ParentPackage, *USoundSubmix::StaticClass()));
					if (Asset.ParentPackage != TEXT("none") && !NewParent)
					{
						return Fail(FString::Printf(TEXT("The parent %s is gone."), *Asset.ParentPackage));
					}
					if (USoundSubmixBase* OldParent = Submix->ParentSubmix)
					{
						OldParent->Modify();
						OutTouched.AddUnique(OldParent);
					}
					// It adds the submix to its new parent's children without recording the parent for undo.
					if (NewParent)
					{
						NewParent->Modify();
						OutTouched.AddUnique(NewParent);
					}
					Submix->SetParentSubmix(NewParent, /*bModifyAssets=*/true);
				}
				if (!Asset.Effects.IsEmpty())
				{
					TArray<TObjectPtr<USoundEffectSubmixPreset>> Chain;
					for (const FCheckedEffect& Effect : Asset.Effects)
					{
						UObject* Preset = Effect.Existing.Get();
						if (!Preset)
						{
							if (!FHyperAIStudioNames::IsFreePackage(Effect.Package))
							{
								return Fail(FString::Printf(TEXT("%s appeared after the plan was checked; nothing was made over it."), *Effect.Package));
							}
							Preset = Make(Effect.Package, *Effect.Class);
							Report.Created.Add(Preset->GetPathName());
						}
						else
						{
							Preset->Modify();
						}
						for (const FHyperAIAudioSetting& Setting : Effect.Spec.Settings)
						{
							FString Error;
							if (!ApplySetting(*Preset, Setting, Error))
							{
								return Fail(Error);
							}
						}
						if (USubmixEffectSubmixEQPreset* EQ = Cast<USubmixEffectSubmixEQPreset>(Preset); EQ && !Effect.Spec.Bands.IsEmpty())
						{
							EQ->Settings.EQBands.Reset();
							for (const FHyperAISoundEQBand& Band : Effect.Spec.Bands)
							{
								FSubmixEffectEQBand& Out = EQ->Settings.EQBands.AddDefaulted_GetRef();
								Out.Frequency = Band.Frequency;
								Out.Bandwidth = Band.Bandwidth;
								Out.GainDb = Band.Gain;
								Out.bEnabled = true;
							}
						}
						// Settings, not SetSettings: that only feeds the effect while it plays, and the asset would save unchanged.
						Preset->PostEditChange();
						Chain.Add(Cast<USoundEffectSubmixPreset>(Preset));
						OutTouched.AddUnique(Preset);
					}
					Submix->SubmixEffectChain = Chain;
				}
			}

			if (USoundMix* SoundMix = Cast<USoundMix>(Object))
			{
				if (!Asset.Spec.Adjusters.IsEmpty())
				{
					SoundMix->SoundClassEffects.Reset();
					for (int32 Index = 0; Index < Asset.Spec.Adjusters.Num(); ++Index)
					{
						const FHyperAISoundMixAdjuster& Spec = Asset.Spec.Adjusters[Index];
						USoundClass* Adjusted = Cast<USoundClass>(Find(Asset.AdjusterPackages[Index], *USoundClass::StaticClass()));
						if (!Adjusted)
						{
							return Fail(FString::Printf(TEXT("The adjusted class %s is gone."), *Asset.AdjusterPackages[Index]));
						}
						FSoundClassAdjuster& Adjuster = SoundMix->SoundClassEffects.AddDefaulted_GetRef();
						Adjuster.SoundClassObject = Adjusted;
						Adjuster.VolumeAdjuster = Spec.Volume;
						Adjuster.PitchAdjuster = Spec.Pitch;
						Adjuster.LowPassFilterFrequency = Spec.LowPassFilterFrequency;
						Adjuster.bApplyToChildren = Spec.bApplyToChildren;
					}
				}
				if (!Asset.Spec.EQBands.IsEmpty())
				{
					SoundMix->bApplyEQ = true;
					FAudioEQEffect& EQ = SoundMix->EQSettings;
					float* Frequencies[] = {&EQ.FrequencyCenter0, &EQ.FrequencyCenter1, &EQ.FrequencyCenter2, &EQ.FrequencyCenter3};
					float* Gains[] = {&EQ.Gain0, &EQ.Gain1, &EQ.Gain2, &EQ.Gain3};
					float* Bandwidths[] = {&EQ.Bandwidth0, &EQ.Bandwidth1, &EQ.Bandwidth2, &EQ.Bandwidth3};
					for (int32 Band = 0; Band < 4; ++Band)
					{
						// Bands not given are left flat.
						const bool bGiven = Asset.Spec.EQBands.IsValidIndex(Band);
						*Frequencies[Band] = bGiven ? Asset.Spec.EQBands[Band].Frequency : *Frequencies[Band];
						*Gains[Band] = bGiven ? Asset.Spec.EQBands[Band].Gain : 1.f;
						*Bandwidths[Band] = bGiven ? Asset.Spec.EQBands[Band].Bandwidth : *Bandwidths[Band];
					}
				}
			}
			Object->PostEditChange();
			Object->MarkPackageDirty();
		}
		return true;
	}

	TArray<FHyperAIAudioSetting> ReadSettings(UObject& Asset, const bool bAll)
	{
		TArray<FHyperAIAudioSetting> Out;
		const TArray<FName>& Common = CommonSettings(*Asset.GetClass());
		for (const FTarget& Target : TargetsOf(Asset))
		{
			for (TFieldIterator<FProperty> It(Target.Struct); It; ++It)
			{
				const FProperty* Property = *It;
				const bool bWanted = bAll ? IsSettable(*Property) : Common.Contains(Property->GetFName());
				if (!bWanted || Out.ContainsByPredicate([Property](const FHyperAIAudioSetting& Setting) { return Setting.Name == Property->GetName(); }))
				{
					continue;
				}
				FHyperAIAudioSetting& Setting = Out.AddDefaulted_GetRef();
				Setting.Name = Property->GetName();
				Setting.Value = Export(*Property, Property->ContainerPtrToValuePtr<void>(Target.Container), Target.Owner).Left(2000);
			}
		}
		return Out;
	}

	void Describe(UObject& Asset, const bool bAllSettings, FHyperAISoundAssetInfo& Info)
	{
		Info.Kind = KindOf(Asset);
		Info.Settings = ReadSettings(Asset, bAllSettings);
		if (const USoundClass* SoundClass = Cast<USoundClass>(&Asset))
		{
			Info.Parent = SoundClass->ParentClass ? SoundClass->ParentClass->GetPathName() : FString();
			for (const USoundClass* Child : SoundClass->ChildClasses)
			{
				if (Child)
				{
					Info.Children.Add(Child->GetPathName());
				}
			}
		}
		else if (const USoundSubmix* Submix = Cast<USoundSubmix>(&Asset))
		{
			Info.Parent = Submix->ParentSubmix ? Submix->ParentSubmix->GetPathName() : FString();
			for (const USoundSubmixBase* Child : Submix->ChildSubmixes)
			{
				if (Child)
				{
					Info.Children.Add(Child->GetPathName());
				}
			}
			for (USoundEffectSubmixPreset* Preset : Submix->SubmixEffectChain)
			{
				if (!Preset)
				{
					Info.Details.Add(TEXT("effect: (empty slot)"));
					continue;
				}
				TArray<FString> Values;
				for (const FHyperAIAudioSetting& Setting : ReadSettings(*Preset, bAllSettings))
				{
					Values.Add(Setting.Name + TEXT("=") + Setting.Value);
				}
				if (const USubmixEffectSubmixEQPreset* EQ = Cast<USubmixEffectSubmixEQPreset>(Preset))
				{
					for (const FSubmixEffectEQBand& Band : EQ->Settings.EQBands)
					{
						Values.Add(FString::Printf(TEXT("band %g Hz %+g dB bw %g%s"), Band.Frequency, Band.GainDb, Band.Bandwidth, Band.bEnabled ? TEXT("") : TEXT(" (off)")));
					}
				}
				Info.Details.Add(FString::Printf(TEXT("effect %s %s: %s"), *EffectKindOf(*Preset), *Preset->GetPathName(), *FString::Join(Values, TEXT(", "))));
			}
		}
		else if (const USoundMix* SoundMix = Cast<USoundMix>(&Asset))
		{
			for (const FSoundClassAdjuster& Adjuster : SoundMix->SoundClassEffects)
			{
				Info.Details.Add(FString::Printf(TEXT("adjust %s: volume %g, pitch %g, low pass %g Hz%s"),
					Adjuster.SoundClassObject ? *Adjuster.SoundClassObject->GetPathName() : TEXT("(no class)"), Adjuster.VolumeAdjuster, Adjuster.PitchAdjuster,
					Adjuster.LowPassFilterFrequency, Adjuster.bApplyToChildren ? TEXT(", and its children") : TEXT("")));
				if (!Adjuster.SoundClassObject)
				{
					FHyperAISoundIssue& Issue = Info.Issues.AddDefaulted_GetRef();
					Issue.Severity = TEXT("warning");
					Issue.Code = TEXT("missing_asset");
					Issue.Message = TEXT("An adjuster names no Sound Class, so it does nothing.");
				}
			}
			if (SoundMix->bApplyEQ)
			{
				const FAudioEQEffect& EQ = SoundMix->EQSettings;
				Info.Details.Add(FString::Printf(TEXT("eq: %g Hz x%g bw %g; %g Hz x%g bw %g; %g Hz x%g bw %g; %g Hz x%g bw %g"),
					EQ.FrequencyCenter0, EQ.Gain0, EQ.Bandwidth0, EQ.FrequencyCenter1, EQ.Gain1, EQ.Bandwidth1,
					EQ.FrequencyCenter2, EQ.Gain2, EQ.Bandwidth2, EQ.FrequencyCenter3, EQ.Gain3, EQ.Bandwidth3));
			}
		}
	}
}
