// Games by Hyper 2026.

#include "HyperAIStudioLightingGate.h"

#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/MeshComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Light.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/RectLight.h"
#include "Engine/Scene.h"
#include "Engine/SkyLight.h"
#include "Engine/SpotLight.h"
#include "Engine/TextureLightProfile.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/StringOutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioLighting"

namespace HyperAIStudio::Lighting::Gate
{
	namespace
	{
		constexpr int32 HistogramBins = 16;
		constexpr int64 MaxMeasuredSamples = 1 << 20;
		constexpr int32 MaxImageDimension = 16384;
		constexpr int64 MaxImageFileBytes = 64ll * 1024 * 1024;
		constexpr int32 MaxActorsPerKind = 16;
		constexpr int32 MaxLabelChars = 128;

		const TCHAR* const Kinds[] = {TEXT("sun"), TEXT("sky_light"), TEXT("fog"), TEXT("sky_atmosphere"), TEXT("post_process")};

		UClass* ClassOf(const FString& Kind)
		{
			if (Kind == TEXT("sun")) return ADirectionalLight::StaticClass();
			if (Kind == TEXT("sky_light")) return ASkyLight::StaticClass();
			if (Kind == TEXT("fog")) return AExponentialHeightFog::StaticClass();
			if (Kind == TEXT("sky_atmosphere")) return ASkyAtmosphere::StaticClass();
			if (Kind == TEXT("post_process")) return APostProcessVolume::StaticClass();
			return nullptr;
		}

		FString SpawnLabelOf(const FString& Kind)
		{
			if (Kind == TEXT("sun")) return TEXT("HyperAI_Sun");
			if (Kind == TEXT("sky_light")) return TEXT("HyperAI_SkyLight");
			if (Kind == TEXT("fog")) return TEXT("HyperAI_HeightFog");
			if (Kind == TEXT("sky_atmosphere")) return TEXT("HyperAI_SkyAtmosphere");
			return TEXT("HyperAI_PostProcess");
		}

		/** Actors of one kind in path order; post process lists only unbound volumes unless a label names one. */
		TArray<AActor*> ActorsOfKind(UWorld& World, const FString& Kind, const FString& Label = FString())
		{
			TArray<AActor*> Actors;
			UClass* Class = ClassOf(Kind);
			if (!Class) return Actors;
			for (TActorIterator<AActor> It(&World, Class); It; ++It)
			{
				AActor* Actor = *It;
				if (!IsValid(Actor) || Actor->IsTemplate()) continue;
				if (!Label.IsEmpty() ? Actor->GetActorLabel() != Label
					: Kind == TEXT("post_process") && !CastChecked<APostProcessVolume>(Actor)->bUnbound)
				{
					continue;
				}
				Actors.Add(Actor);
			}
			Actors.Sort([](const AActor& A, const AActor& B) { return A.GetPathName() < B.GetPathName(); });
			return Actors;
		}

		AActor* FindActor(UWorld& World, const FString& Kind, const FString& Label)
		{
			const TArray<AActor*> Actors = ActorsOfKind(World, Kind, Label);
			return Actors.IsEmpty() ? nullptr : Actors[0];
		}

		/** A Blueprint sky or weather actor, and the variables that move its look, by the names its Details panel shows. */
		struct FSkySystem
		{
			/** The Blueprint class's name without _C; a child Blueprint of it counts too. */
			const TCHAR* ClassName;
			const TCHAR* Name;
			/** It has its own sun, sky light and fog, so an edit to a stock light would fight it or add a second one. */
			bool bOwnsLights;
			/** What to change instead of a stock light, for the refusal. */
			const TCHAR* Levers;
			TArray<const TCHAR*> Keys;
		};

		const TArray<FSkySystem>& SkySystems()
		{
			// Skies that own their lights first, so a variable both have resolves to the sky rather than the weather.
			static const TArray<FSkySystem> Systems = {
				{TEXT("Ultra_Dynamic_Sky"), TEXT("Ultra Dynamic Sky"), true,
					TEXT("Time of Day (the sun's angle), Sun Light Intensity, Sun Light Color, Sky Light Intensity, Cloud Coverage, Fog, Dust Amount, Saturation and Contrast"),
					{TEXT("Time of Day"), TEXT("Animate Time of Day"), TEXT("Sun Light Intensity"), TEXT("Sun Light Color"), TEXT("Moon Light Intensity"),
						TEXT("Sky Light Intensity"), TEXT("Night Brightness"), TEXT("Cloud Coverage"), TEXT("Fog"), TEXT("Dust Amount"),
						TEXT("Overall Intensity"), TEXT("Saturation"), TEXT("Contrast"), TEXT("Sun Yaw"), TEXT("Sun Pitch"),
						TEXT("Apply Exposure Settings"), TEXT("Exposure Bias Day"), TEXT("Exposure Bias Dawn/Dusk"), TEXT("Exposure Bias Night"),
						TEXT("Exposure Bias Cloudy"), TEXT("Volumetric Fog"), TEXT("Sky Mode"), TEXT("Color Mode")}},
				{TEXT("SunSky"), TEXT("Sun Sky"), true, TEXT("SolarTime (the sun's angle), Latitude, Longitude, Month and Day"),
					{TEXT("Latitude"), TEXT("Longitude"), TEXT("TimeZone"), TEXT("SolarTime"), TEXT("Month"), TEXT("Day"), TEXT("NorthOffset")}},
				{TEXT("Ultra_Dynamic_Weather"), TEXT("Ultra Dynamic Weather"), false, TEXT("Weather, Cloud Coverage, Fog, Rain and Snow"),
					{TEXT("Weather"), TEXT("Cloud Coverage"), TEXT("Fog"), TEXT("Dust Amount"), TEXT("Rain"), TEXT("Snow"), TEXT("Wind Intensity"),
						TEXT("Thunder/Lightning"), TEXT("Material Wetness")}},
				{TEXT("BP_Sky_Sphere"), TEXT("Sky Sphere"), false, TEXT("Sun Brightness, Sun Height and the sky colours"),
					{TEXT("Sun Brightness"), TEXT("Sun Height"), TEXT("Cloud Speed"), TEXT("Cloud Opacity"), TEXT("Stars Brightness"),
						TEXT("Horizon Falloff"), TEXT("Zenith Color"), TEXT("Horizon Color"), TEXT("Cloud Color"), TEXT("Overall Color")}},
			};
			return Systems;
		}

		const FSkySystem* SkySystemOf(const UClass* Class)
		{
			for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
			{
				FString Name = Current->GetName();
				Name.RemoveFromEnd(TEXT("_C"));
				for (const FSkySystem& System : SkySystems())
				{
					if (Name == System.ClassName) return &System;
				}
			}
			return nullptr;
		}

		/** Sky and weather actors: skies that own their lights first, then in path order. */
		TArray<AActor*> SkyActors(UWorld& World)
		{
			TArray<AActor*> Actors;
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (IsValid(*It) && !It->IsTemplate() && SkySystemOf(It->GetClass())) Actors.Add(*It);
			}
			Actors.Sort([](const AActor& A, const AActor& B)
			{
				const bool bOwnsA = SkySystemOf(A.GetClass())->bOwnsLights;
				const bool bOwnsB = SkySystemOf(B.GetClass())->bOwnsLights;
				return bOwnsA != bOwnsB ? bOwnsA : A.GetPathName() < B.GetPathName();
			});
			return Actors;
		}

		/** The variable a Details panel shows as Name: its own name first, then its display name. */
		FProperty* FindSkyProperty(const UClass* Class, const FString& Name)
		{
			for (TFieldIterator<FProperty> It(Class); It; ++It)
			{
				if (It->GetName().Equals(Name, ESearchCase::IgnoreCase)) return *It;
			}
#if WITH_EDITOR
			for (TFieldIterator<FProperty> It(Class); It; ++It)
			{
				if (It->GetDisplayNameText().ToString().Equals(Name, ESearchCase::IgnoreCase)) return *It;
			}
#endif
			return nullptr;
		}

		/** Only what a person could set on the placed actor in its Details panel, and of a plain type. */
		bool IsEditableSkyProperty(const FProperty& Property, FString& OutWhy)
		{
			if (!Property.HasAnyPropertyFlags(CPF_Edit) || Property.HasAnyPropertyFlags(CPF_DisableEditOnInstance | CPF_EditConst))
			{
				OutWhy = TEXT("is not editable on the placed actor");
				return false;
			}
			if (Property.ArrayDim != 1 || Property.IsA<FArrayProperty>() || Property.IsA<FMapProperty>() || Property.IsA<FSetProperty>())
			{
				OutWhy = TEXT("is a list, which set_sky_system does not edit");
				return false;
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
			{
				const UScriptStruct* Type = Struct->Struct;
				if (Type == TBaseStructure<FLinearColor>::Get() || Type == TBaseStructure<FColor>::Get() || Type == TBaseStructure<FVector>::Get()
					|| Type == TBaseStructure<FRotator>::Get() || Type == TBaseStructure<FVector2D>::Get())
				{
					return true;
				}
				OutWhy = FString::Printf(TEXT("is a %s, which set_sky_system does not edit"), Type ? *Type->GetName() : TEXT("struct"));
				return false;
			}
			if (Property.IsA<FNumericProperty>() || Property.IsA<FBoolProperty>() || Property.IsA<FEnumProperty>() || Property.IsA<FNameProperty>()
				|| Property.IsA<FStrProperty>() || Property.IsA<FTextProperty>() || Property.IsA<FObjectPropertyBase>())
			{
				return true;
			}
			OutWhy = TEXT("is of a type set_sky_system does not edit");
			return false;
		}

		const UEnum* EnumOf(const FProperty& Property)
		{
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property)) return EnumProperty->GetEnum();
			if (const FByteProperty* Byte = CastField<FByteProperty>(&Property)) return Byte->Enum;
			return nullptr;
		}

		/** Values as set_sky_system takes them back: numbers plain, enums by display name, the rest in Unreal's text form. */
		FString SkyValueText(const FProperty& Property, const AActor& Actor)
		{
			const void* Value = Property.ContainerPtrToValuePtr<void>(&Actor);
			if (const UEnum* Enum = EnumOf(Property))
			{
				const FNumericProperty* Underlying = Property.IsA<FEnumProperty>()
					? CastField<FEnumProperty>(&Property)->GetUnderlyingProperty() : CastField<FNumericProperty>(&Property);
				const int64 Raw = Underlying ? Underlying->GetSignedIntPropertyValue(Value) : 0;
				const int32 Index = Enum->GetIndexByValue(Raw);
				return Index == INDEX_NONE ? LexToString(Raw) : Enum->GetDisplayNameTextByIndex(Index).ToString();
			}
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
			{
				return Numeric->IsFloatingPoint() ? FString::Printf(TEXT("%.6g"), Numeric->GetFloatingPointPropertyValue(Value)) : LexToString(Numeric->GetSignedIntPropertyValue(Value));
			}
			if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
			{
				return Bool->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
			}
			FString Text;
			Property.ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
			return Text.Len() > 200 ? Text.Left(200) + TEXT("...") : Text;
		}

		/** The value as the Details panel would take it, checked by importing it into scratch memory; enums by display name. */
		bool ParseSkyValue(const FProperty& Property, AActor& Owner, const FString& Text, FString& OutImport, FString& OutError)
		{
			OutImport = Text.TrimStartAndEnd();
			if (const UEnum* Enum = EnumOf(Property))
			{
				TArray<FString> Names;
				for (int32 Index = 0; Index < Enum->NumEnums() - (Enum->ContainsExistingMax() ? 1 : 0); ++Index)
				{
					const FString Display = Enum->GetDisplayNameTextByIndex(Index).ToString();
					if (Display.Equals(OutImport, ESearchCase::IgnoreCase) || Enum->GetNameStringByIndex(Index).Equals(OutImport, ESearchCase::IgnoreCase)
						|| Enum->GetAuthoredNameStringByIndex(Index).Equals(OutImport, ESearchCase::IgnoreCase))
					{
						OutImport = Enum->GetNameStringByIndex(Index);
						return true;
					}
					Names.Add(Display);
				}
				OutError = FString::Printf(TEXT("takes one of: %s"), *FString::Join(Names, TEXT(", ")));
				return false;
			}
			if (Property.IsA<FBoolProperty>())
			{
				const bool bTrue = OutImport.Equals(TEXT("true"), ESearchCase::IgnoreCase) || OutImport == TEXT("1");
				const bool bFalse = OutImport.Equals(TEXT("false"), ESearchCase::IgnoreCase) || OutImport == TEXT("0");
				OutImport = bTrue ? TEXT("True") : TEXT("False");
				if (!bTrue && !bFalse) OutError = TEXT("takes true or false");
				return bTrue || bFalse;
			}
			void* Scratch = FMemory::Malloc(Property.GetSize(), Property.GetMinAlignment());
			Property.InitializeValue(Scratch);
			FStringOutputDevice Errors;
			const TCHAR* End = Property.ImportText_Direct(*OutImport, Scratch, &Owner, PPF_None, &Errors);
			bool bOk = End != nullptr && FString(End).TrimStartAndEnd().IsEmpty() && Errors.IsEmpty();
			if (bOk)
			{
				if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
				{
					const double Value = Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(Scratch)
						: static_cast<double>(Numeric->GetSignedIntPropertyValue(Scratch));
#if WITH_EDITOR
					const FString Min = Property.GetMetaData(TEXT("ClampMin"));
					const FString Max = Property.GetMetaData(TEXT("ClampMax"));
#else
					const FString Min, Max;
#endif
					if (!FMath::IsFinite(Value) || (!Min.IsEmpty() && Value < FCString::Atod(*Min)) || (!Max.IsEmpty() && Value > FCString::Atod(*Max)))
					{
						bOk = false;
						OutError = Min.IsEmpty() && Max.IsEmpty() ? FString(TEXT("needs a finite number"))
							: FString::Printf(TEXT("needs a number from %s to %s"), Min.IsEmpty() ? TEXT("any") : *Min, Max.IsEmpty() ? TEXT("any") : *Max);
					}
				}
			}
			else
			{
				OutError = FString::Printf(TEXT("does not take '%s'%s"), *OutImport.Left(64),
					Property.IsA<FStructProperty>() ? TEXT("; give it in Unreal's text form, such as (R=1,G=0.5,B=0.2,A=1)") : TEXT(""));
			}
			Property.DestroyValue(Scratch);
			FMemory::Free(Scratch);
			return bOk;
		}

		AActor* LightOwningSkyActor(UWorld& World)
		{
			for (AActor* Sky : SkyActors(World))
			{
				if (SkySystemOf(Sky->GetClass())->bOwnsLights) return Sky;
			}
			return nullptr;
		}

		/** Skies with their own exposure switch (Ultra Dynamic Sky's Apply Exposure Settings). */
		TArray<TPair<AActor*, FBoolProperty*>> ExposureSkies(UWorld& World)
		{
			TArray<TPair<AActor*, FBoolProperty*>> Skies;
			for (AActor* Sky : SkyActors(World))
			{
				FString Why;
				FBoolProperty* Applies = CastField<FBoolProperty>(FindSkyProperty(Sky->GetClass(), TEXT("Apply Exposure Settings")));
				if (Applies && IsEditableSkyProperty(*Applies, Why)) Skies.Add({Sky, Applies});
			}
			return Skies;
		}

		FString Number(const double Value)
		{
			return FString::Printf(TEXT("%.6g"), Value);
		}

		FString Colour(const FLinearColor& Value)
		{
			return FString::Printf(TEXT("%.4g,%.4g,%.4g"), Value.R, Value.G, Value.B);
		}

		/** Exposure fixed by a manual-mode volume with the physical camera off: the bias then holds -EV100. */
		bool IsManualLocked(const APostProcessVolume& Volume)
		{
			const FPostProcessSettings& Settings = Volume.Settings;
			return Settings.bOverride_AutoExposureMethod && Settings.AutoExposureMethod == AEM_Manual
				&& Settings.bOverride_AutoExposureApplyPhysicalCameraExposure && !Settings.AutoExposureApplyPhysicalCameraExposure;
		}

		bool LocksExposure(const APostProcessVolume& Volume)
		{
			const FPostProcessSettings& Settings = Volume.Settings;
			if (!Volume.bEnabled || !Volume.bUnbound || Volume.BlendWeight <= 0.f) return false;
			return (Settings.bOverride_AutoExposureMethod && Settings.AutoExposureMethod == AEM_Manual)
				|| (Settings.bOverride_AutoExposureMinBrightness && Settings.bOverride_AutoExposureMaxBrightness
					&& Settings.AutoExposureMinBrightness == Settings.AutoExposureMaxBrightness);
		}

		void ReadProperties(AActor& Actor, const FString& Kind, TArray<FHyperAILightingProperty>& Out)
		{
			auto Add = [&Out](const TCHAR* Key, const FString& Value) { Out.Add({Key, Value}); };
			if (const ADirectionalLight* Sun = Cast<ADirectionalLight>(&Actor); Sun && Sun->GetComponent())
			{
				const UDirectionalLightComponent& Light = *Sun->GetComponent();
				const FRotator Rotation = Actor.GetActorRotation();
				Add(TEXT("intensity"), Number(Light.Intensity));
				Add(TEXT("temperature"), Number(Light.Temperature));
				Add(TEXT("use_temperature"), Light.bUseTemperature ? TEXT("true") : TEXT("false"));
				Add(TEXT("color"), Colour(FLinearColor(Light.LightColor)));
				Add(TEXT("pitch"), Number(Rotation.Pitch));
				Add(TEXT("yaw"), Number(Rotation.Yaw));
				Add(TEXT("source_angle"), Number(Light.LightSourceAngle));
				Add(TEXT("atmosphere_sun_light"), Light.bAtmosphereSunLight ? TEXT("true") : TEXT("false"));
				Add(TEXT("mobility"), StaticEnum<EComponentMobility::Type>()->GetNameStringByValue(Light.Mobility));
			}
			else if (const ASkyLight* Sky = Cast<ASkyLight>(&Actor); Sky && Sky->GetLightComponent())
			{
				const USkyLightComponent& Light = *Sky->GetLightComponent();
				Add(TEXT("intensity"), Number(Light.Intensity));
				Add(TEXT("color"), Colour(FLinearColor(Light.LightColor)));
				Add(TEXT("real_time_capture"), Light.bRealTimeCapture ? TEXT("true") : TEXT("false"));
				Add(TEXT("source_type"), Light.SourceType == SLS_CapturedScene ? TEXT("captured_scene") : TEXT("specified_cubemap"));
			}
			else if (const AExponentialHeightFog* Fog = Cast<AExponentialHeightFog>(&Actor); Fog && Fog->GetComponent())
			{
				const UExponentialHeightFogComponent& Component = *Fog->GetComponent();
				Add(TEXT("density"), Number(Component.FogDensity));
				Add(TEXT("height_falloff"), Number(Component.FogHeightFalloff));
				Add(TEXT("start_distance"), Number(Component.StartDistance));
				Add(TEXT("inscattering_color"), Colour(Component.FogInscatteringLuminance));
				Add(TEXT("volumetric_fog"), Component.bEnableVolumetricFog ? TEXT("true") : TEXT("false"));
				Add(TEXT("volumetric_scattering_distribution"), Number(Component.VolumetricFogScatteringDistribution));
				Add(TEXT("volumetric_albedo"), Colour(FLinearColor(Component.VolumetricFogAlbedo)));
				Add(TEXT("volumetric_extinction_scale"), Number(Component.VolumetricFogExtinctionScale));
			}
			else if (const ASkyAtmosphere* Atmosphere = Cast<ASkyAtmosphere>(&Actor); Atmosphere && Atmosphere->GetComponent())
			{
				Add(TEXT("rayleigh_scale"), Number(Atmosphere->GetComponent()->RayleighScatteringScale));
				Add(TEXT("mie_scale"), Number(Atmosphere->GetComponent()->MieScatteringScale));
			}
			else if (const APostProcessVolume* Volume = Cast<APostProcessVolume>(&Actor))
			{
				const FPostProcessSettings& S = Volume->Settings;
				auto Overridden = [](const bool bOverride, const FString& Value) { return bOverride ? Value : FString(TEXT("default")); };
				Add(TEXT("enabled"), Volume->bEnabled ? TEXT("true") : TEXT("false"));
				Add(TEXT("unbound"), Volume->bUnbound ? TEXT("true") : TEXT("false"));
				Add(TEXT("exposure_method"), !S.bOverride_AutoExposureMethod ? FString(TEXT("default"))
					: S.AutoExposureMethod == AEM_Manual ? FString(TEXT("manual"))
					: S.AutoExposureMethod == AEM_Basic ? FString(TEXT("basic")) : FString(TEXT("histogram")));
				Add(TEXT("exposure_bias"), Overridden(S.bOverride_AutoExposureBias, Number(S.AutoExposureBias)));
				if (IsManualLocked(*Volume))
				{
					Add(TEXT("exposure_ev100"), Number(-S.AutoExposureBias));
				}
				Add(TEXT("white_temp"), Overridden(S.bOverride_WhiteTemp, Number(S.WhiteTemp)));
				Add(TEXT("saturation"), Overridden(S.bOverride_ColorSaturation, Number(S.ColorSaturation.X)));
				Add(TEXT("contrast"), Overridden(S.bOverride_ColorContrast, Number(S.ColorContrast.X)));
				Add(TEXT("gamma"), Overridden(S.bOverride_ColorGamma, Number(S.ColorGamma.X)));
				auto Channels = [](const FVector4& Value) { return Colour(FLinearColor(Value.X, Value.Y, Value.Z)); };
				Add(TEXT("shadows_gain"), Overridden(S.bOverride_ColorGainShadows, Channels(S.ColorGainShadows)));
				Add(TEXT("shadows_offset"), Overridden(S.bOverride_ColorOffsetShadows, Channels(S.ColorOffsetShadows)));
				Add(TEXT("highlights_gain"), Overridden(S.bOverride_ColorGainHighlights, Channels(S.ColorGainHighlights)));
			}
		}

		struct FPropertySpec
		{
			const TCHAR* Kind;
			const TCHAR* Key;
			bool bColour;
			float Min;
			float Max;
			/** Takes true or false. */
			bool bBool = false;
		};

		const FPropertySpec PropertySpecs[] = {
			{TEXT("sun"), TEXT("intensity"), false, 0.f, 200000.f},
			{TEXT("sun"), TEXT("temperature"), false, 1700.f, 12000.f},
			{TEXT("sun"), TEXT("color"), true, 0.f, 1.f},
			{TEXT("sun"), TEXT("pitch"), false, -90.f, 90.f},
			{TEXT("sun"), TEXT("yaw"), false, -360.f, 360.f},
			{TEXT("sun"), TEXT("source_angle"), false, 0.f, 30.f},
			{TEXT("sky_light"), TEXT("intensity"), false, 0.f, 100.f},
			{TEXT("sky_light"), TEXT("color"), true, 0.f, 1.f},
			{TEXT("fog"), TEXT("density"), false, 0.f, 1.f},
			{TEXT("fog"), TEXT("height_falloff"), false, 0.001f, 2.f},
			{TEXT("fog"), TEXT("start_distance"), false, 0.f, 1000000.f},
			{TEXT("fog"), TEXT("inscattering_color"), true, 0.f, 10.f},
			// Volumetric fog is what a spot or window light's shafts show in.
			{TEXT("fog"), TEXT("volumetric_fog"), false, 0.f, 1.f, true},
			{TEXT("fog"), TEXT("volumetric_scattering_distribution"), false, -0.9f, 0.9f},
			{TEXT("fog"), TEXT("volumetric_albedo"), true, 0.f, 1.f},
			{TEXT("fog"), TEXT("volumetric_extinction_scale"), false, 0.1f, 10.f},
			{TEXT("sky_atmosphere"), TEXT("rayleigh_scale"), false, 0.f, 2.f},
			{TEXT("sky_atmosphere"), TEXT("mie_scale"), false, 0.f, 5.f},
			{TEXT("post_process"), TEXT("exposure_bias"), false, -15.f, 15.f},
			{TEXT("post_process"), TEXT("white_temp"), false, 1500.f, 15000.f},
			{TEXT("post_process"), TEXT("saturation"), false, 0.f, 2.f},
			{TEXT("post_process"), TEXT("contrast"), false, 0.f, 2.f},
			{TEXT("post_process"), TEXT("gamma"), false, 0.2f, 3.f},
			// Grading the shadows and highlights apart: a warm sky over violet shadows is a grade, not a light.
			{TEXT("post_process"), TEXT("shadows_gain"), true, 0.f, 2.f},
			{TEXT("post_process"), TEXT("shadows_offset"), true, -0.5f, 0.5f},
			{TEXT("post_process"), TEXT("highlights_gain"), true, 0.f, 2.f},
		};

		enum ELocalLightMask : uint8
		{
			PointMask = 1,
			SpotMask = 2,
			RectMask = 4,
			AllLocal = PointMask | SpotMask | RectMask,
		};

		enum class ELightValue : uint8
		{
			Number,
			Colour,
			Bool,
			Vector,
			Rotator,
			Asset,
			Units,
		};

		/** A set_light setting, the value it takes and the light types it applies to. */
		struct FLightSpec
		{
			const TCHAR* Key;
			ELightValue Type;
			uint8 Mask;
			float Min;
			float Max;
		};

		const FLightSpec LightSpecs[] = {
			{TEXT("intensity"), ELightValue::Number, AllLocal, 0.f, 1000000.f},
			{TEXT("intensity_units"), ELightValue::Units, AllLocal, 0.f, 0.f},
			{TEXT("color"), ELightValue::Colour, AllLocal, 0.f, 1.f},
			{TEXT("temperature"), ELightValue::Number, AllLocal, 1700.f, 12000.f},
			{TEXT("use_temperature"), ELightValue::Bool, AllLocal, 0.f, 1.f},
			{TEXT("attenuation_radius"), ELightValue::Number, AllLocal, 1.f, 100000.f},
			{TEXT("source_radius"), ELightValue::Number, PointMask | SpotMask, 0.f, 10000.f},
			{TEXT("soft_source_radius"), ELightValue::Number, PointMask | SpotMask, 0.f, 10000.f},
			{TEXT("source_length"), ELightValue::Number, PointMask | SpotMask, 0.f, 10000.f},
			{TEXT("inner_cone_angle"), ELightValue::Number, SpotMask, 0.f, 80.f},
			{TEXT("outer_cone_angle"), ELightValue::Number, SpotMask, 1.f, 80.f},
			{TEXT("source_width"), ELightValue::Number, RectMask, 1.f, 10000.f},
			{TEXT("source_height"), ELightValue::Number, RectMask, 1.f, 10000.f},
			{TEXT("barn_door_angle"), ELightValue::Number, RectMask, 0.f, 90.f},
			{TEXT("barn_door_length"), ELightValue::Number, RectMask, 0.f, 10000.f},
			{TEXT("cast_shadows"), ELightValue::Bool, AllLocal, 0.f, 1.f},
			{TEXT("volumetric_scattering"), ELightValue::Number, AllLocal, 0.f, 20.f},
			{TEXT("indirect_intensity"), ELightValue::Number, AllLocal, 0.f, 20.f},
			{TEXT("specular_scale"), ELightValue::Number, AllLocal, 0.f, 4.f},
			// Off, never deleted: a light the level had stays in it.
			{TEXT("enabled"), ELightValue::Bool, AllLocal, 0.f, 1.f},
			{TEXT("ies_profile"), ELightValue::Asset, AllLocal, 0.f, 0.f},
			{TEXT("use_ies_brightness"), ELightValue::Bool, AllLocal, 0.f, 1.f},
			{TEXT("light_function"), ELightValue::Asset, AllLocal, 0.f, 0.f},
			{TEXT("location"), ELightValue::Vector, AllLocal, 0.f, 0.f},
			{TEXT("rotation"), ELightValue::Rotator, SpotMask | RectMask, 0.f, 0.f},
			{TEXT("aim_at"), ELightValue::Vector, SpotMask | RectMask, 0.f, 0.f},
		};

		/** Lights add_light places: the level can tell them from its own. */
		const FName AddedLightTag(TEXT("HyperAI.AddedLight"));

		struct FParsedOp
		{
			FString Kind;
			const FPropertySpec* Spec = nullptr;
			bool bLock = false;
			bool bUnlock = false;
			float Value = 0.f;
			FLinearColor Colour = FLinearColor::White;
			// set_actor_material: the slot on a named or the first mesh component, and the material for it.
			UMaterialInterface* Material = nullptr;
			FString ComponentName;
			int32 SlotIndex = 0;
			FName SlotName;
			// set_sky_system: the variable, by the name its Details panel shows, and its value as typed.
			FString SkyVariable;
			FString SkyValue;
			// set_light and add_light: a point, spot or rect light by label, and a setting from LightSpecs or the type to add.
			const FLightSpec* LightSpec = nullptr;
			uint8 LightMask = 0;
			FVector Vector = FVector::ZeroVector;
			FRotator Rotator = FRotator::ZeroRotator;
			bool bFlag = false;
			UObject* Asset = nullptr;
		};

		bool ParseNumber(const FString& Text, const float Min, const float Max, float& Out)
		{
			return !Text.IsEmpty() && LexTryParseString(Out, *Text.TrimStartAndEnd()) && FMath::IsFinite(Out) && Out >= Min && Out <= Max;
		}

		/** World "x,y,z" in centimetres, within 20 km of the origin. */
		bool ParseVector(const FString& Text, FVector& Out)
		{
			constexpr float Reach = 2000000.f;
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT(","));
			float X = 0.f, Y = 0.f, Z = 0.f;
			if (Parts.Num() != 3 || !ParseNumber(Parts[0], -Reach, Reach, X) || !ParseNumber(Parts[1], -Reach, Reach, Y) || !ParseNumber(Parts[2], -Reach, Reach, Z))
			{
				return false;
			}
			Out = FVector(X, Y, Z);
			return true;
		}

		bool IsLightFunction(const UObject* Object)
		{
			const UMaterialInterface* Material = Cast<UMaterialInterface>(Object);
			return Material && Material->GetMaterial() && Material->GetMaterial()->MaterialDomain == MD_LightFunction;
		}

		bool ParseLightValue(const FLightSpec& Spec, const FString& Text, FParsedOp& Out, FString& OutError)
		{
			const FString Value = Text.TrimStartAndEnd();
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","));
			switch (Spec.Type)
			{
			case ELightValue::Number:
				if (ParseNumber(Value, Spec.Min, Spec.Max, Out.Value)) return true;
				OutError = FString::Printf(TEXT("set_light %s needs a number from %g to %g."), Spec.Key, Spec.Min, Spec.Max);
				return false;
			case ELightValue::Colour:
			{
				float R = 0.f, G = 0.f, B = 0.f;
				if (Parts.Num() == 3 && ParseNumber(Parts[0], 0.f, 1.f, R) && ParseNumber(Parts[1], 0.f, 1.f, G) && ParseNumber(Parts[2], 0.f, 1.f, B))
				{
					Out.Colour = FLinearColor(R, G, B);
					return true;
				}
				OutError = TEXT("set_light color needs linear \"r,g,b\" with each from 0 to 1.");
				return false;
			}
			case ELightValue::Bool:
				Out.bFlag = Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1");
				if (Out.bFlag || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Value == TEXT("0")) return true;
				OutError = FString::Printf(TEXT("set_light %s takes true or false."), Spec.Key);
				return false;
			case ELightValue::Vector:
				if (ParseVector(Value, Out.Vector)) return true;
				OutError = FString::Printf(TEXT("set_light %s needs world \"x,y,z\" in centimetres."), Spec.Key);
				return false;
			case ELightValue::Rotator:
			{
				float Pitch = 0.f, Yaw = 0.f, Roll = 0.f;
				if (Parts.Num() == 3 && ParseNumber(Parts[0], -360.f, 360.f, Pitch) && ParseNumber(Parts[1], -360.f, 360.f, Yaw) && ParseNumber(Parts[2], -360.f, 360.f, Roll))
				{
					Out.Rotator = FRotator(Pitch, Yaw, Roll);
					return true;
				}
				OutError = TEXT("set_light rotation needs \"pitch,yaw,roll\" in degrees from -360 to 360; aim_at points it at a world \"x,y,z\" instead.");
				return false;
			}
			case ELightValue::Units:
			{
				const UEnum* Units = StaticEnum<ELightUnits>();
				TArray<FString> Names;
				for (int32 Index = 0; Index < Units->NumEnums() - (Units->ContainsExistingMax() ? 1 : 0); ++Index)
				{
					Names.Add(Units->GetNameStringByIndex(Index).ToLower());
					if (Names.Last() == Value.ToLower())
					{
						Out.Value = static_cast<float>(Units->GetValueByIndex(Index));
						return true;
					}
				}
				OutError = FString::Printf(TEXT("set_light intensity_units takes %s; the light keeps its brightness."), *FString::Join(Names, TEXT(", ")));
				return false;
			}
			case ELightValue::Asset:
			{
				if (Value.Equals(TEXT("none"), ESearchCase::IgnoreCase))
				{
					Out.Asset = nullptr;
					return true;
				}
				FString Path = Value;
				if (Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT(".")))
				{
					Path += TEXT(".") + FPackageName::GetShortName(Path);
				}
				UObject* Loaded = Path.StartsWith(TEXT("/")) ? FSoftObjectPath(Path).TryLoad() : nullptr;
				const bool bIes = FCString::Strcmp(Spec.Key, TEXT("ies_profile")) == 0;
				if (bIes ? Loaded && Loaded->IsA<UTextureLightProfile>() : IsLightFunction(Loaded))
				{
					Out.Asset = Loaded;
					return true;
				}
				OutError = bIes ? FString::Printf(TEXT("set_light ies_profile '%s' is not an IES light profile; pass its path, or none."), *Value.Left(128))
					: FString::Printf(TEXT("set_light light_function '%s' is not a material in the Light Function domain; pass its path, or none."), *Value.Left(128));
				return false;
			}
			}
			return false;
		}

		bool ParseOp(const FHyperAILightingOp& Op, FParsedOp& Out, FString& OutError)
		{
			if (Op.ActorLabel.Len() > MaxLabelChars || Op.Property.Len() > 128 || Op.Value.Len() > 128)
			{
				OutError = TEXT("actor_label, property or value is too long.");
				return false;
			}
			if (Op.Kind == TEXT("lock_exposure") || Op.Kind == TEXT("unlock_exposure"))
			{
				Out.Kind = TEXT("post_process");
				Out.bLock = Op.Kind == TEXT("lock_exposure");
				Out.bUnlock = !Out.bLock;
				if (!Op.Property.IsEmpty())
				{
					OutError = Op.Kind + TEXT(" takes no property.");
					return false;
				}
				if (Out.bUnlock ? !Op.Value.IsEmpty() : !ParseNumber(Op.Value, -10.f, 20.f, Out.Value))
				{
					OutError = Out.bUnlock ? FString(TEXT("unlock_exposure takes no value."))
						: FString(TEXT("lock_exposure needs value = the scene's EV100 from -10 to 20: about 15 for noon sun, 12 overcast, 8 bright interior, 4 dim interior, -2 moonlight."));
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("set_actor_material"))
			{
				Out.Kind = TEXT("actor_material");
				if (Op.ActorLabel.IsEmpty())
				{
					OutError = TEXT("set_actor_material needs actor_label: the placed actor whose mesh gets the material.");
					return false;
				}
				FString Slot = Op.Property;
				if (Slot.StartsWith(TEXT("component:")))
				{
					if (!Slot.RightChop(10).Split(TEXT("/"), &Out.ComponentName, &Slot))
					{
						Out.ComponentName = Slot.RightChop(10);
						Slot.Reset();
					}
				}
				if (!Slot.IsEmpty() && (!Slot.StartsWith(TEXT("slot:")) || Slot.Len() == 5))
				{
					OutError = TEXT("set_actor_material property is empty (slot 0), slot:<index or name>, or component:<name>/slot:<index or name>.");
					return false;
				}
				const FString SlotValue = Slot.IsEmpty() ? FString(TEXT("0")) : Slot.RightChop(5);
				if (SlotValue.IsNumeric())
				{
					Out.SlotIndex = FCString::Atoi(*SlotValue);
				}
				else
				{
					Out.SlotName = FName(*SlotValue);
				}
				FString MaterialPath = Op.Value.TrimStartAndEnd();
				if (MaterialPath.StartsWith(TEXT("/")) && !MaterialPath.Contains(TEXT(".")))
				{
					MaterialPath += TEXT(".") + FPackageName::GetShortName(MaterialPath);
				}
				Out.Material = MaterialPath.StartsWith(TEXT("/")) ? Cast<UMaterialInterface>(FSoftObjectPath(MaterialPath).TryLoad()) : nullptr;
				if (!Out.Material)
				{
					OutError = FString::Printf(TEXT("set_actor_material value '%s' is not a material or material instance; pass its path, e.g. /Game/Materials/M_Grid."), *Op.Value);
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("set_sky_system"))
			{
				Out.Kind = TEXT("sky_system");
				Out.SkyVariable = Op.Property.TrimStartAndEnd();
				Out.SkyValue = Op.Value;
				if (Out.SkyVariable.IsEmpty() || Op.Value.TrimStartAndEnd().IsEmpty())
				{
					OutError = TEXT("set_sky_system needs property, a variable of the sky by the name its Details panel shows (hyper_lighting_inspect lists them), and value.");
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("add_light"))
			{
				Out.Kind = TEXT("add_light");
				Out.LightMask = Op.Property == TEXT("point") ? PointMask : Op.Property == TEXT("spot") ? SpotMask : Op.Property == TEXT("rect") ? RectMask : 0;
				if (Op.ActorLabel.IsEmpty() || Op.ActorLabel != Op.ActorLabel.TrimStartAndEnd())
				{
					OutError = TEXT("add_light needs actor_label: the new light's label, such as Key_Window or Practical_Lamp_1.");
					return false;
				}
				if (!Out.LightMask)
				{
					OutError = TEXT("add_light property is the light's type: point, spot or rect.");
					return false;
				}
				if (!ParseVector(Op.Value, Out.Vector))
				{
					OutError = TEXT("add_light value is where it goes, as world \"x,y,z\" in centimetres.");
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("set_light"))
			{
				Out.Kind = TEXT("light");
				if (Op.ActorLabel.IsEmpty())
				{
					OutError = TEXT("set_light needs actor_label: the light, or the actor holding it (hyper_lighting_inspect lists them).");
					return false;
				}
				FString Key = Op.Property;
				if (Key.StartsWith(TEXT("component:")) && (!Key.RightChop(10).Split(TEXT("/"), &Out.ComponentName, &Key) || Out.ComponentName.IsEmpty()))
				{
					OutError = TEXT("set_light property is a setting, or component:<name>/<setting> for one light among several in an actor.");
					return false;
				}
				for (const FLightSpec& Spec : LightSpecs)
				{
					if (Key == Spec.Key) Out.LightSpec = &Spec;
				}
				if (!Out.LightSpec)
				{
					TArray<FString> Keys;
					for (const FLightSpec& Spec : LightSpecs)
					{
						Keys.Add(Spec.Key);
					}
					OutError = FString::Printf(TEXT("set_light has no setting '%s'; use one of %s."), *Key, *FString::Join(Keys, TEXT(", ")));
					return false;
				}
				return ParseLightValue(*Out.LightSpec, Op.Value, Out, OutError);
			}
			if (!Op.Kind.StartsWith(TEXT("set_")))
			{
				OutError = FString::Printf(TEXT("Unknown kind '%s'."), *Op.Kind);
				return false;
			}
			Out.Kind = Op.Kind.RightChop(4);
			if (!ClassOf(Out.Kind))
			{
				OutError = FString::Printf(TEXT("Unknown kind '%s': use set_sun, set_sky_light, set_fog, set_sky_atmosphere, set_post_process, lock_exposure, unlock_exposure, set_light, add_light, set_actor_material or set_sky_system."), *Op.Kind);
				return false;
			}
			for (const FPropertySpec& Spec : PropertySpecs)
			{
				if (Out.Kind == Spec.Kind && Op.Property == Spec.Key)
				{
					Out.Spec = &Spec;
				}
			}
			if (!Out.Spec)
			{
				TArray<FString> Keys;
				for (const FPropertySpec& Spec : PropertySpecs)
				{
					if (Out.Kind == Spec.Kind) Keys.Add(Spec.Key);
				}
				OutError = FString::Printf(TEXT("%s has no property '%s'; use one of %s."), *Op.Kind, *Op.Property, *FString::Join(Keys, TEXT(", ")));
				return false;
			}
			if (Out.Spec->bBool)
			{
				const FString Flag = Op.Value.TrimStartAndEnd();
				const bool bTrue = Flag.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Flag == TEXT("1");
				if (!bTrue && !Flag.Equals(TEXT("false"), ESearchCase::IgnoreCase) && Flag != TEXT("0"))
				{
					OutError = FString::Printf(TEXT("%s %s takes true or false."), *Op.Kind, *Op.Property);
					return false;
				}
				Out.Value = bTrue ? 1.f : 0.f;
				return true;
			}
			if (Out.Spec->bColour)
			{
				TArray<FString> Parts;
				Op.Value.ParseIntoArray(Parts, TEXT(","));
				float R = 0.f, G = 0.f, B = 0.f;
				if (Parts.Num() != 3 || !ParseNumber(Parts[0], Out.Spec->Min, Out.Spec->Max, R)
					|| !ParseNumber(Parts[1], Out.Spec->Min, Out.Spec->Max, G) || !ParseNumber(Parts[2], Out.Spec->Min, Out.Spec->Max, B))
				{
					OutError = FString::Printf(TEXT("%s %s needs linear \"r,g,b\" with each from %g to %g."), *Op.Kind, *Op.Property, Out.Spec->Min, Out.Spec->Max);
					return false;
				}
				Out.Colour = FLinearColor(R, G, B);
				return true;
			}
			if (!ParseNumber(Op.Value, Out.Spec->Min, Out.Spec->Max, Out.Value))
			{
				OutError = FString::Printf(TEXT("%s %s needs a number from %g to %g."), *Op.Kind, *Op.Property, Out.Spec->Min, Out.Spec->Max);
				return false;
			}
			return true;
		}

		/** The Details panel's path: PreEditChange records undo, PostEditChangeProperty refreshes rendering at any mobility. */
		template <typename TWrite>
		void Edit(UObject& Object, const FName PropertyName, TWrite&& Write)
		{
			FProperty* Property = FindFProperty<FProperty>(Object.GetClass(), PropertyName);
			Object.PreEditChange(Property);
			Write();
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Object.PostEditChangeProperty(Event);
		}

		void WriteOp(AActor& Actor, const FParsedOp& Op)
		{
			const FString Key = Op.Spec ? Op.Spec->Key : FString();
			if (ADirectionalLight* Sun = Cast<ADirectionalLight>(&Actor))
			{
				UDirectionalLightComponent& Light = *Sun->GetComponent();
				if (Key == TEXT("pitch") || Key == TEXT("yaw"))
				{
					FRotator Rotation = Actor.GetActorRotation();
					(Key == TEXT("pitch") ? Rotation.Pitch : Rotation.Yaw) = Op.Value;
					Actor.Modify();
					Actor.SetActorRotation(Rotation);
					Actor.PostEditMove(true);
				}
				else if (Key == TEXT("intensity")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponentBase, Intensity), [&] { Light.Intensity = Op.Value; });
				else if (Key == TEXT("temperature"))
				{
					Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponent, Temperature), [&]
					{
						Light.Temperature = Op.Value;
						Light.bUseTemperature = true;
					});
				}
				else if (Key == TEXT("color")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponentBase, LightColor), [&] { Light.LightColor = Op.Colour.ToFColor(true); });
				else if (Key == TEXT("source_angle")) Edit(Light, GET_MEMBER_NAME_CHECKED(UDirectionalLightComponent, LightSourceAngle), [&] { Light.LightSourceAngle = Op.Value; });
			}
			else if (ASkyLight* Sky = Cast<ASkyLight>(&Actor))
			{
				USkyLightComponent& Light = *Sky->GetLightComponent();
				if (Key == TEXT("intensity")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponentBase, Intensity), [&] { Light.Intensity = Op.Value; });
				else if (Key == TEXT("color")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponentBase, LightColor), [&] { Light.LightColor = Op.Colour.ToFColor(true); });
			}
			else if (AExponentialHeightFog* Fog = Cast<AExponentialHeightFog>(&Actor))
			{
				UExponentialHeightFogComponent& C = *Fog->GetComponent();
				if (Key == TEXT("density")) Edit(C, GET_MEMBER_NAME_CHECKED(UExponentialHeightFogComponent, FogDensity), [&] { C.FogDensity = Op.Value; });
				else if (Key == TEXT("height_falloff")) Edit(C, GET_MEMBER_NAME_CHECKED(UExponentialHeightFogComponent, FogHeightFalloff), [&] { C.FogHeightFalloff = Op.Value; });
				else if (Key == TEXT("start_distance")) Edit(C, GET_MEMBER_NAME_CHECKED(UExponentialHeightFogComponent, StartDistance), [&] { C.StartDistance = Op.Value; });
				else if (Key == TEXT("inscattering_color")) Edit(C, GET_MEMBER_NAME_CHECKED(UExponentialHeightFogComponent, FogInscatteringLuminance), [&] { C.FogInscatteringLuminance = Op.Colour; });
				else if (Key == TEXT("volumetric_fog")) Edit(C, GET_MEMBER_NAME_CHECKED(UExponentialHeightFogComponent, bEnableVolumetricFog), [&] { C.bEnableVolumetricFog = Op.Value > 0.5f; });
				else if (Key == TEXT("volumetric_scattering_distribution")) Edit(C, GET_MEMBER_NAME_CHECKED(UExponentialHeightFogComponent, VolumetricFogScatteringDistribution), [&] { C.VolumetricFogScatteringDistribution = Op.Value; });
				else if (Key == TEXT("volumetric_albedo")) Edit(C, GET_MEMBER_NAME_CHECKED(UExponentialHeightFogComponent, VolumetricFogAlbedo), [&] { C.VolumetricFogAlbedo = Op.Colour.ToFColor(true); });
				else if (Key == TEXT("volumetric_extinction_scale")) Edit(C, GET_MEMBER_NAME_CHECKED(UExponentialHeightFogComponent, VolumetricFogExtinctionScale), [&] { C.VolumetricFogExtinctionScale = Op.Value; });
			}
			else if (ASkyAtmosphere* Atmosphere = Cast<ASkyAtmosphere>(&Actor))
			{
				USkyAtmosphereComponent& C = *Atmosphere->GetComponent();
				if (Key == TEXT("rayleigh_scale")) Edit(C, GET_MEMBER_NAME_CHECKED(USkyAtmosphereComponent, RayleighScatteringScale), [&] { C.RayleighScatteringScale = Op.Value; });
				else if (Key == TEXT("mie_scale")) Edit(C, GET_MEMBER_NAME_CHECKED(USkyAtmosphereComponent, MieScatteringScale), [&] { C.MieScatteringScale = Op.Value; });
			}
			else if (APostProcessVolume* Volume = Cast<APostProcessVolume>(&Actor))
			{
				Edit(*Volume, GET_MEMBER_NAME_CHECKED(APostProcessVolume, Settings), [&]
				{
					FPostProcessSettings& S = Volume->Settings;
					if (Op.bLock)
					{
						// Manual metering with the physical camera off exposes for EV100 = -bias.
						S.bOverride_AutoExposureMethod = true;
						S.AutoExposureMethod = AEM_Manual;
						S.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
						S.AutoExposureApplyPhysicalCameraExposure = false;
						S.bOverride_AutoExposureBias = true;
						S.AutoExposureBias = -Op.Value;
					}
					else if (Op.bUnlock)
					{
						S.bOverride_AutoExposureMethod = false;
						S.bOverride_AutoExposureApplyPhysicalCameraExposure = false;
						S.bOverride_AutoExposureBias = false;
					}
					else if (Key == TEXT("exposure_bias")) { S.bOverride_AutoExposureBias = true; S.AutoExposureBias = Op.Value; }
					else if (Key == TEXT("white_temp")) { S.bOverride_WhiteTemp = true; S.WhiteTemp = Op.Value; }
					else if (Key == TEXT("saturation")) { S.bOverride_ColorSaturation = true; S.ColorSaturation = FVector4(Op.Value, Op.Value, Op.Value, 1.0); }
					else if (Key == TEXT("contrast")) { S.bOverride_ColorContrast = true; S.ColorContrast = FVector4(Op.Value, Op.Value, Op.Value, 1.0); }
					else if (Key == TEXT("gamma")) { S.bOverride_ColorGamma = true; S.ColorGamma = FVector4(Op.Value, Op.Value, Op.Value, 1.0); }
					else if (Key == TEXT("shadows_gain")) { S.bOverride_ColorGainShadows = true; S.ColorGainShadows = FVector4(Op.Colour.R, Op.Colour.G, Op.Colour.B, 1.0); }
					else if (Key == TEXT("shadows_offset")) { S.bOverride_ColorOffsetShadows = true; S.ColorOffsetShadows = FVector4(Op.Colour.R, Op.Colour.G, Op.Colour.B, 0.0); }
					else if (Key == TEXT("highlights_gain")) { S.bOverride_ColorGainHighlights = true; S.ColorGainHighlights = FVector4(Op.Colour.R, Op.Colour.G, Op.Colour.B, 1.0); }
				});
			}
		}

		/** The mesh component and slot a set_actor_material op names, on the one actor with that label. Never spawns. */
		bool ResolveMaterialSlot(UWorld& World, const FString& Label, const FParsedOp& Op, UMeshComponent*& OutMesh, int32& OutSlot, FString& OutError)
		{
			TArray<AActor*> Matches;
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (IsValid(*It) && !It->IsTemplate() && It->GetActorLabel() == Label) Matches.Add(*It);
			}
			if (Matches.Num() != 1)
			{
				OutError = Matches.IsEmpty() ? FString::Printf(TEXT("no actor is labelled '%s'."), *Label)
					: FString::Printf(TEXT("%d actors are labelled '%s'; give the one to change a unique label first."), Matches.Num(), *Label);
				return false;
			}
			TArray<UMeshComponent*> Meshes;
			Matches[0]->GetComponents(Meshes);
			// The root first, so an op without a component names the actor's main mesh.
			const USceneComponent* Root = Matches[0]->GetRootComponent();
			Meshes.StableSort([Root](const UMeshComponent& A, const UMeshComponent& B) { return &A == Root && &B != Root; });
			TArray<FString> Names;
			for (UMeshComponent* Mesh : Meshes)
			{
				Names.Add(Mesh->GetName());
				if (!Op.ComponentName.IsEmpty() && Mesh->GetName() != Op.ComponentName) continue;
				OutSlot = Op.SlotName.IsNone() ? Op.SlotIndex : Mesh->GetMaterialIndex(Op.SlotName);
				if (OutSlot >= 0 && OutSlot < Mesh->GetNumMaterials())
				{
					OutMesh = Mesh;
					return true;
				}
				TArray<FString> Slots;
				const TArray<FName> SlotNames = Mesh->GetMaterialSlotNames();
				for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index)
				{
					Slots.Add(FString::Printf(TEXT("%d '%s'"), Index, SlotNames.IsValidIndex(Index) ? *SlotNames[Index].ToString() : TEXT("")));
				}
				OutError = FString::Printf(TEXT("%s on '%s' has no slot %s; its slots are %s."), *Mesh->GetName(), *Label,
					Op.SlotName.IsNone() ? *FString::FromInt(Op.SlotIndex) : *Op.SlotName.ToString(),
					Slots.IsEmpty() ? TEXT("none (it has no mesh yet)") : *FString::Join(Slots, TEXT(", ")));
				return false;
			}
			OutError = Meshes.IsEmpty() ? FString::Printf(TEXT("'%s' has no mesh component to take a material."), *Label)
				: FString::Printf(TEXT("'%s' has no mesh component '%s'; it has %s."), *Label, *Op.ComponentName, *FString::Join(Names, TEXT(", ")));
			return false;
		}

		/** The sky a set_sky_system op edits and its checked value: the labelled one, else the first sky that has the variable. */
		bool ResolveSkyOp(UWorld& World, const FString& Label, const FParsedOp& Op, AActor*& OutActor, FProperty*& OutProperty,
			FString& OutImport, FString& OutError)
		{
			const TArray<AActor*> Skies = SkyActors(World);
			if (Skies.IsEmpty())
			{
				OutError = TEXT("the level has no Blueprint sky (Ultra Dynamic Sky or Weather, Sun Sky, Sky Sphere); light it with set_sun and the other kinds.");
				return false;
			}
			TArray<FString> Labels;
			for (AActor* Sky : Skies)
			{
				Labels.Add(FString::Printf(TEXT("'%s' (%s)"), *Sky->GetActorLabel(), SkySystemOf(Sky->GetClass())->Name));
				if (!Label.IsEmpty() && Sky->GetActorLabel() != Label) continue;
				FProperty* Property = FindSkyProperty(Sky->GetClass(), Op.SkyVariable);
				if (!Property)
				{
					if (!Label.IsEmpty())
					{
						OutError = FString::Printf(TEXT("'%s' has no variable '%s'; hyper_lighting_inspect lists the ones that move its look."), *Label, *Op.SkyVariable);
						return false;
					}
					// Weather variables live on the weather actor: try the next sky.
					continue;
				}
				FString Why;
				if (!IsEditableSkyProperty(*Property, Why) || !ParseSkyValue(*Property, *Sky, Op.SkyValue, OutImport, Why))
				{
					OutError = FString::Printf(TEXT("'%s' on '%s' %s."), *Op.SkyVariable, *Sky->GetActorLabel(), *Why);
					return false;
				}
				OutActor = Sky;
				OutProperty = Property;
				return true;
			}
			OutError = !Label.IsEmpty()
				? FString::Printf(TEXT("no sky is labelled '%s'; the skies are %s."), *Label, *FString::Join(Labels, TEXT(", ")))
				: FString::Printf(TEXT("no sky in the level has a variable '%s' (skies: %s); hyper_lighting_inspect lists their variables."), *Op.SkyVariable, *FString::Join(Labels, TEXT(", ")));
			return false;
		}

		AActor* SpawnKind(UWorld& World, const FString& Kind)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			// A new sun starts 50 degrees up so the scene is lit before the plan aims it.
			const FTransform Transform = Kind == TEXT("sun") ? FTransform(FRotator(-50.0, 0.0, 0.0)) : FTransform::Identity;
			AActor* Actor = World.SpawnActor(ClassOf(Kind), &Transform, Params);
			if (!Actor) return nullptr;
			Actor->SetActorLabel(SpawnLabelOf(Kind));
			if (APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor))
			{
				Volume->bUnbound = true;
			}
			return Actor;
		}

		uint8 MaskOf(const ULocalLightComponent& Light)
		{
			// A spot light is a point light too, so it is asked first.
			return Light.IsA<USpotLightComponent>() ? SpotMask : Light.IsA<URectLightComponent>() ? RectMask : PointMask;
		}

		const TCHAR* LightTypeOf(const uint8 Mask)
		{
			return Mask == SpotMask ? TEXT("spot") : Mask == RectMask ? TEXT("rect") : TEXT("point");
		}

		FString KindsIn(const uint8 Mask)
		{
			TArray<FString> Names;
			for (const uint8 Type : {uint8(PointMask), uint8(SpotMask), uint8(RectMask)})
			{
				if (Mask & Type) Names.Add(LightTypeOf(Type));
			}
			FString Joined = Names[0];
			for (int32 Index = 1; Index < Names.Num(); ++Index)
			{
				Joined += (Index + 1 == Names.Num() ? TEXT(" and ") : TEXT(", ")) + Names[Index];
			}
			return Joined + TEXT(" lights");
		}

		/** The actors labelled Label; a light's label is how every op names it. */
		TArray<AActor*> Labelled(UWorld& World, const FString& Label)
		{
			TArray<AActor*> Found;
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (IsValid(*It) && !It->IsTemplate() && It->GetActorLabel() == Label) Found.Add(*It);
			}
			return Found;
		}

		/** The local lights an actor holds: its own, or those among its components, in name order. */
		TArray<ULocalLightComponent*> LocalLightsOf(AActor& Actor)
		{
			TArray<ULocalLightComponent*> Lights;
			Actor.GetComponents(Lights);
			Lights.Sort([](const ULocalLightComponent& A, const ULocalLightComponent& B) { return A.GetName() < B.GetName(); });
			return Lights;
		}

		/** The light a set_light op changes, on the labelled actor; or, while validating, the type of one the plan adds first. */
		bool ResolveLight(UWorld& World, const FString& Label, const FParsedOp& Op, const TMap<FString, uint8>& Planned, AActor*& OutActor,
			ULocalLightComponent*& OutLight, uint8& OutMask, FString& OutError)
		{
			OutActor = nullptr;
			OutLight = nullptr;
			OutMask = 0;
			const TArray<AActor*> Actors = Labelled(World, Label);
			if (Actors.IsEmpty() && Planned.Contains(Label))
			{
				if (!Op.ComponentName.IsEmpty())
				{
					OutError = FString::Printf(TEXT("'%s' is a light this plan adds; it has no components to name."), *Label);
					return false;
				}
				OutMask = Planned[Label];
				return true;
			}
			if (Actors.Num() != 1)
			{
				OutError = Actors.IsEmpty() ? FString::Printf(TEXT("no actor is labelled '%s'; hyper_lighting_inspect lists the local lights by label."), *Label)
					: FString::Printf(TEXT("%d actors are labelled '%s'; give the light a unique label first."), Actors.Num(), *Label);
				return false;
			}
			const TArray<ULocalLightComponent*> Lights = LocalLightsOf(*Actors[0]);
			TArray<FString> Names;
			for (ULocalLightComponent* Light : Lights)
			{
				Names.Add(Light->GetName());
				if (!Op.ComponentName.IsEmpty() ? Light->GetName() == Op.ComponentName : Lights.Num() == 1)
				{
					OutActor = Actors[0];
					OutLight = Light;
					OutMask = MaskOf(*Light);
					return true;
				}
			}
			OutError = Lights.IsEmpty() ? FString::Printf(TEXT("'%s' has no point, spot or rect light; set_sun, set_sky_light and set_sky_system change the others."), *Label)
				: !Op.ComponentName.IsEmpty() ? FString::Printf(TEXT("'%s' has no light component '%s'; it has %s."), *Label, *Op.ComponentName, *FString::Join(Names, TEXT(", ")))
				: FString::Printf(TEXT("'%s' holds %d lights; name one as component:<name>/<setting>: %s."), *Label, Lights.Num(), *FString::Join(Names, TEXT(", ")));
			return false;
		}

		AActor* SpawnLight(UWorld& World, const uint8 Mask, const FVector& Location, const FString& Label)
		{
			UClass* Class = Mask == SpotMask ? ASpotLight::StaticClass() : Mask == RectMask ? ARectLight::StaticClass() : APointLight::StaticClass();
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			// Spot and rect lights start pointing down, so a new one lights what is under it until the plan aims it.
			const FTransform Transform(Mask == PointMask ? FRotator::ZeroRotator : FRotator(-90.0, 0.0, 0.0), Location);
			AActor* Actor = World.SpawnActor(Class, &Transform, Params);
			if (!Actor) return nullptr;
			Actor->SetActorLabel(Label);
			Actor->Tags.AddUnique(AddedLightTag);
			// Movable, so it lights the level at once; a static light would wait for a lighting build.
			if (ULightComponent* Light = CastChecked<ALight>(Actor)->GetLightComponent())
			{
				Light->SetMobility(EComponentMobility::Movable);
			}
			// In the project's default light units, as the editor's own placement does, and as bright as the engine's default.
			static const TConsoleVariableData<int32>* DefaultUnits = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.DefaultFeature.LightUnits"));
			ULocalLightComponent* Local = Cast<ULocalLightComponent>(CastChecked<ALight>(Actor)->GetLightComponent());
			if (Local && DefaultUnits)
			{
				const ELightUnits Units = static_cast<ELightUnits>(DefaultUnits->GetValueOnGameThread());
				const USpotLightComponent* Spot = Cast<USpotLightComponent>(Local);
				Local->Intensity *= ULocalLightComponent::GetUnitsConversionFactor(Local->IntensityUnits, Units, Spot ? Spot->GetCosHalfConeAngle() : -1.f);
				Local->IntensityUnits = Units;
			}
			return Actor;
		}

		void WriteLightOp(AActor& Actor, ULocalLightComponent& Light, const FParsedOp& Op)
		{
			const FString Key = Op.LightSpec->Key;
			if (Key == TEXT("location") || Key == TEXT("rotation") || Key == TEXT("aim_at"))
			{
				Actor.Modify();
				if (Key == TEXT("location"))
				{
					Actor.SetActorLocation(Op.Vector);
				}
				else
				{
					Actor.SetActorRotation(Key == TEXT("rotation") ? Op.Rotator : (Op.Vector - Actor.GetActorLocation()).Rotation());
				}
				Actor.PostEditMove(true);
				return;
			}
			UPointLightComponent* Point = Cast<UPointLightComponent>(&Light);
			USpotLightComponent* Spot = Cast<USpotLightComponent>(&Light);
			URectLightComponent* Rect = Cast<URectLightComponent>(&Light);
			const float V = Op.Value;
			if (Key == TEXT("intensity")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponentBase, Intensity), [&] { Light.Intensity = V; });
			else if (Key == TEXT("intensity_units"))
			{
				Edit(Light, GET_MEMBER_NAME_CHECKED(ULocalLightComponent, IntensityUnits), [&]
				{
					// As bright as before, in the new units: what the Details panel does when the units change.
					const float Brightness = Light.ComputeLightBrightness();
					Light.IntensityUnits = static_cast<ELightUnits>(FMath::RoundToInt(V));
					Light.SetLightBrightness(Brightness);
				});
			}
			else if (Key == TEXT("color")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponentBase, LightColor), [&] { Light.LightColor = Op.Colour.ToFColor(true); });
			else if (Key == TEXT("temperature"))
			{
				Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponent, Temperature), [&]
				{
					Light.Temperature = V;
					Light.bUseTemperature = true;
				});
			}
			else if (Key == TEXT("use_temperature")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponent, bUseTemperature), [&] { Light.bUseTemperature = Op.bFlag; });
			else if (Key == TEXT("attenuation_radius")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULocalLightComponent, AttenuationRadius), [&] { Light.AttenuationRadius = V; });
			else if (Key == TEXT("source_radius")) Edit(*Point, GET_MEMBER_NAME_CHECKED(UPointLightComponent, SourceRadius), [&] { Point->SourceRadius = V; });
			else if (Key == TEXT("soft_source_radius")) Edit(*Point, GET_MEMBER_NAME_CHECKED(UPointLightComponent, SoftSourceRadius), [&] { Point->SoftSourceRadius = V; });
			else if (Key == TEXT("source_length")) Edit(*Point, GET_MEMBER_NAME_CHECKED(UPointLightComponent, SourceLength), [&] { Point->SourceLength = V; });
			else if (Key == TEXT("inner_cone_angle")) Edit(*Spot, GET_MEMBER_NAME_CHECKED(USpotLightComponent, InnerConeAngle), [&] { Spot->InnerConeAngle = V; });
			else if (Key == TEXT("outer_cone_angle")) Edit(*Spot, GET_MEMBER_NAME_CHECKED(USpotLightComponent, OuterConeAngle), [&] { Spot->OuterConeAngle = V; });
			else if (Key == TEXT("source_width")) Edit(*Rect, GET_MEMBER_NAME_CHECKED(URectLightComponent, SourceWidth), [&] { Rect->SourceWidth = V; });
			else if (Key == TEXT("source_height")) Edit(*Rect, GET_MEMBER_NAME_CHECKED(URectLightComponent, SourceHeight), [&] { Rect->SourceHeight = V; });
			else if (Key == TEXT("barn_door_angle")) Edit(*Rect, GET_MEMBER_NAME_CHECKED(URectLightComponent, BarnDoorAngle), [&] { Rect->BarnDoorAngle = V; });
			else if (Key == TEXT("barn_door_length")) Edit(*Rect, GET_MEMBER_NAME_CHECKED(URectLightComponent, BarnDoorLength), [&] { Rect->BarnDoorLength = V; });
			else if (Key == TEXT("cast_shadows")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponentBase, CastShadows), [&] { Light.CastShadows = Op.bFlag; });
			else if (Key == TEXT("volumetric_scattering")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponentBase, VolumetricScatteringIntensity), [&] { Light.VolumetricScatteringIntensity = V; });
			else if (Key == TEXT("indirect_intensity")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponentBase, IndirectLightingIntensity), [&] { Light.IndirectLightingIntensity = V; });
			else if (Key == TEXT("specular_scale")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponent, SpecularScale), [&] { Light.SpecularScale = V; });
			else if (Key == TEXT("enabled")) Edit(Light, USceneComponent::GetVisiblePropertyName(), [&] { Light.SetVisibility(Op.bFlag); });
			else if (Key == TEXT("ies_profile")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponent, IESTexture), [&] { Light.IESTexture = Cast<UTextureLightProfile>(Op.Asset); });
			else if (Key == TEXT("use_ies_brightness")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponent, bUseIESBrightness), [&] { Light.bUseIESBrightness = Op.bFlag; });
			else if (Key == TEXT("light_function")) Edit(Light, GET_MEMBER_NAME_CHECKED(ULightComponent, LightFunctionMaterial), [&] { Light.LightFunctionMaterial = Cast<UMaterialInterface>(Op.Asset); });
		}

		void ReadLight(const AActor& Actor, const ULocalLightComponent& Light, TArray<FHyperAILightingProperty>& Out)
		{
			auto Add = [&Out](const TCHAR* Key, const FString& Value) { Out.Add({Key, Value}); };
			auto Flag = [](const bool bValue) { return bValue ? FString(TEXT("true")) : FString(TEXT("false")); };
			auto Triple = [](const double A, const double B, const double C) { return FString::Printf(TEXT("%.1f,%.1f,%.1f"), A, B, C); };
			if (!Actor.IsA<ALight>())
			{
				Add(TEXT("component"), Light.GetName());
			}
			Add(TEXT("intensity"), Number(Light.Intensity));
			Add(TEXT("intensity_units"), StaticEnum<ELightUnits>()->GetNameStringByValue(static_cast<int64>(Light.IntensityUnits)).ToLower());
			Add(TEXT("color"), Colour(FLinearColor(Light.LightColor)));
			Add(TEXT("temperature"), Number(Light.Temperature));
			Add(TEXT("use_temperature"), Flag(Light.bUseTemperature));
			Add(TEXT("attenuation_radius"), Number(Light.AttenuationRadius));
			if (const UPointLightComponent* Point = Cast<UPointLightComponent>(&Light))
			{
				Add(TEXT("source_radius"), Number(Point->SourceRadius));
				Add(TEXT("soft_source_radius"), Number(Point->SoftSourceRadius));
				Add(TEXT("source_length"), Number(Point->SourceLength));
			}
			if (const USpotLightComponent* Spot = Cast<USpotLightComponent>(&Light))
			{
				Add(TEXT("inner_cone_angle"), Number(Spot->InnerConeAngle));
				Add(TEXT("outer_cone_angle"), Number(Spot->OuterConeAngle));
			}
			if (const URectLightComponent* Rect = Cast<URectLightComponent>(&Light))
			{
				Add(TEXT("source_width"), Number(Rect->SourceWidth));
				Add(TEXT("source_height"), Number(Rect->SourceHeight));
				Add(TEXT("barn_door_angle"), Number(Rect->BarnDoorAngle));
				Add(TEXT("barn_door_length"), Number(Rect->BarnDoorLength));
			}
			Add(TEXT("cast_shadows"), Flag(Light.CastShadows));
			Add(TEXT("volumetric_scattering"), Number(Light.VolumetricScatteringIntensity));
			Add(TEXT("indirect_intensity"), Number(Light.IndirectLightingIntensity));
			Add(TEXT("specular_scale"), Number(Light.SpecularScale));
			Add(TEXT("enabled"), Flag(Light.IsVisible()));
			Add(TEXT("ies_profile"), Light.IESTexture ? Light.IESTexture->GetPathName() : FString(TEXT("none")));
			Add(TEXT("light_function"), Light.LightFunctionMaterial ? Light.LightFunctionMaterial->GetPathName() : FString(TEXT("none")));
			const FVector Location = Light.GetComponentLocation();
			Add(TEXT("location"), Triple(Location.X, Location.Y, Location.Z));
			if (MaskOf(Light) != PointMask)
			{
				const FRotator Rotation = Light.GetComponentRotation();
				Add(TEXT("rotation"), Triple(Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
			}
			Add(TEXT("mobility"), StaticEnum<EComponentMobility::Type>()->GetNameStringByValue(Light.Mobility));
			if (Actor.Tags.Contains(AddedLightTag))
			{
				Add(TEXT("added_by_hyperai"), TEXT("true"));
			}
		}

		/** Where the scene is looked at from, to list first the local lights that light what is seen. */
		struct FLightView
		{
			bool bValid = false;
			FVector Origin = FVector::ZeroVector;
			FVector Forward = FVector::ForwardVector;
			double HalfFovRadians = UE_HALF_PI * 0.5;
			FString Name;
		};

		FLightView FindView(UWorld& World, const FString& CameraLabel)
		{
			FLightView View;
			const FString Label = CameraLabel.IsEmpty() ? FString(TEXT("HyperAI_MatchCamera")) : CameraLabel;
			for (AActor* Actor : Labelled(World, Label))
			{
				if (const UCameraComponent* Camera = Actor->FindComponentByClass<UCameraComponent>())
				{
					View.bValid = true;
					View.Origin = Camera->GetComponentLocation();
					View.Forward = Camera->GetForwardVector();
					View.HalfFovRadians = FMath::DegreesToRadians(Camera->FieldOfView * 0.5);
					View.Name = Label;
					return View;
				}
			}
			// A camera that was named and is not there gives no view, so the caller can say so.
			const FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
			if (CameraLabel.IsEmpty() && Client && Client->IsPerspective() && Client->GetWorld() == &World)
			{
				View.bValid = true;
				View.Origin = Client->GetViewLocation();
				View.Forward = Client->GetViewRotation().Vector();
				View.HalfFovRadians = FMath::DegreesToRadians(Client->ViewFOV * 0.5);
				View.Name = TEXT("level_viewport");
			}
			return View;
		}

		/** Whether a light's sphere of reach overlaps the view: its cone, widened a quarter for the frame's corners. */
		bool InView(const FLightView& View, const FVector& Location, const double Radius)
		{
			const FVector ToLight = Location - View.Origin;
			const double Distance = ToLight.Size();
			if (Distance <= Radius) return true;
			const double Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(ToLight / Distance, View.Forward), -1.0, 1.0));
			return Angle - FMath::Asin(FMath::Clamp(Radius / Distance, 0.0, 1.0)) <= View.HalfFovRadians * 1.25;
		}

		bool RunOps(UWorld& World, const TArray<FHyperAILightingOp>& Ops, const bool bWrite, TArray<AActor*>& OutTouched, FString& OutError)
		{
			TMap<FString, bool> ManualLock;
			TSet<FString> PlannedSpawns;
			TMap<FString, uint8> PlannedLights;
			for (int32 Index = 0; Index < Ops.Num(); ++Index)
			{
				const FHyperAILightingOp& Op = Ops[Index];
				FParsedOp Parsed;
				FString Error;
				if (!ParseOp(Op, Parsed, Error))
				{
					OutError = FString::Printf(TEXT("Op %d: %s"), Index, *Error);
					return false;
				}
				if (Parsed.Kind == TEXT("actor_material"))
				{
					UMeshComponent* Mesh = nullptr;
					int32 Slot = INDEX_NONE;
					if (!ResolveMaterialSlot(World, Op.ActorLabel, Parsed, Mesh, Slot, Error))
					{
						OutError = FString::Printf(TEXT("Op %d: %s"), Index, *Error);
						return false;
					}
					if (bWrite)
					{
						// The Details panel's route, not a raw OverrideMaterials write: undo, render state and cleanup all follow.
						Edit(*Mesh, FName(TEXT("OverrideMaterials")), [&] { Mesh->SetMaterial(Slot, Parsed.Material); });
						OutTouched.AddUnique(Mesh->GetOwner());
					}
					continue;
				}
				if (Parsed.Kind == TEXT("sky_system"))
				{
					AActor* Sky = nullptr;
					FProperty* Property = nullptr;
					FString Import;
					if (!ResolveSkyOp(World, Op.ActorLabel, Parsed, Sky, Property, Import, Error))
					{
						OutError = FString::Printf(TEXT("Op %d: %s"), Index, *Error);
						return false;
					}
					if (bWrite)
					{
						// The Details panel's route: undo, then the sky's construction script redraws it.
						Edit(*Sky, Property->GetFName(), [&] { Property->ImportText_InContainer(*Import, Sky, Sky, PPF_None); });
						OutTouched.AddUnique(Sky);
					}
					continue;
				}
				if (Parsed.Kind == TEXT("add_light"))
				{
					if (PlannedLights.Contains(Op.ActorLabel) || !Labelled(World, Op.ActorLabel).IsEmpty())
					{
						OutError = FString::Printf(TEXT("Op %d: an actor is already labelled '%s'; give the new light its own label."), Index, *Op.ActorLabel);
						return false;
					}
					PlannedLights.Add(Op.ActorLabel, Parsed.LightMask);
					if (bWrite)
					{
						AActor* Added = SpawnLight(World, Parsed.LightMask, Parsed.Vector, Op.ActorLabel);
						if (!Added)
						{
							OutError = FString::Printf(TEXT("Op %d: the level refused a new %s light."), Index, LightTypeOf(Parsed.LightMask));
							return false;
						}
						OutTouched.AddUnique(Added);
					}
					continue;
				}
				if (Parsed.Kind == TEXT("light"))
				{
					// Local lights are the level's own, never a Blueprint sky's, so they change beside one too.
					AActor* Owner = nullptr;
					ULocalLightComponent* Light = nullptr;
					uint8 Mask = 0;
					if (!ResolveLight(World, Op.ActorLabel, Parsed, PlannedLights, Owner, Light, Mask, Error))
					{
						OutError = FString::Printf(TEXT("Op %d: %s"), Index, *Error);
						return false;
					}
					if (!(Mask & Parsed.LightSpec->Mask))
					{
						OutError = FString::Printf(TEXT("Op %d: '%s' is a %s light, and %s applies only to %s."), Index, *Op.ActorLabel, LightTypeOf(Mask),
							Parsed.LightSpec->Key, *KindsIn(Parsed.LightSpec->Mask));
						return false;
					}
					const bool bMoves = Parsed.LightSpec->Type == ELightValue::Vector || Parsed.LightSpec->Type == ELightValue::Rotator;
					if (bMoves && Owner && !Owner->IsA<ALight>())
					{
						OutError = FString::Printf(TEXT("Op %d: '%s' holds its lights inside; %s moves and aims only light actors, so move '%s' itself with the level tools."),
							Index, *Op.ActorLabel, Parsed.LightSpec->Key, *Op.ActorLabel);
						return false;
					}
					if (bWrite)
					{
						if (!Owner || !Light)
						{
							OutError = FString::Printf(TEXT("Op %d: '%s' was not there to change."), Index, *Op.ActorLabel);
							return false;
						}
						WriteLightOp(*Owner, *Light, Parsed);
						OutTouched.AddUnique(Owner);
					}
					continue;
				}
				if (Parsed.Kind == TEXT("sun") || Parsed.Kind == TEXT("sky_light") || Parsed.Kind == TEXT("fog") || Parsed.Kind == TEXT("sky_atmosphere"))
				{
					if (const AActor* Owner = LightOwningSkyActor(World))
					{
						const FSkySystem* System = SkySystemOf(Owner->GetClass());
						OutError = FString::Printf(TEXT("Op %d: this level's sky is %s ('%s'), which owns its sun, sky light, fog and atmosphere, so %s would fight it or add a second one. Use set_sky_system instead: %s."),
							Index, System->Name, *Owner->GetActorLabel(), *Op.Kind, System->Levers);
						return false;
					}
				}
				AActor* Actor = FindActor(World, Parsed.Kind, Op.ActorLabel);
				const bool bPlannedLabel = Op.ActorLabel == SpawnLabelOf(Parsed.Kind) && PlannedSpawns.Contains(Parsed.Kind);
				if (!Actor && !Op.ActorLabel.IsEmpty() && !bPlannedLabel)
				{
					OutError = FString::Printf(TEXT("Op %d: no %s actor is labelled '%s'; leave actor_label empty to use the first one, or add one."),
						Index, *Parsed.Kind, *Op.ActorLabel);
					return false;
				}
				if (!Actor && bWrite)
				{
					Actor = SpawnKind(World, Parsed.Kind);
					if (!Actor)
					{
						OutError = FString::Printf(TEXT("Op %d: the level refused a new %s actor."), Index, *Parsed.Kind);
						return false;
					}
				}
				if (!Actor) PlannedSpawns.Add(Parsed.Kind);
				if (Parsed.Kind == TEXT("post_process"))
				{
					const FString Key = Actor ? Actor->GetPathName() : TEXT("spawn");
					if (!ManualLock.Contains(Key))
					{
						ManualLock.Add(Key, Actor && IsManualLocked(*CastChecked<APostProcessVolume>(Actor)));
					}
					bool& bLocked = ManualLock[Key];
					if (Parsed.Spec && FCString::Strcmp(Parsed.Spec->Key, TEXT("exposure_bias")) == 0 && bLocked)
					{
						OutError = FString::Printf(TEXT("Op %d: exposure is locked there, so the bias holds the locked EV100; change it with lock_exposure instead."), Index);
						return false;
					}
					bLocked = Parsed.bLock || (bLocked && !Parsed.bUnlock);
				}
				if (bWrite)
				{
					WriteOp(*Actor, Parsed);
					OutTouched.AddUnique(Actor);
					// A sky that applies its own exposure would move a locked exposure again, so locking switches it off.
					for (const TPair<AActor*, FBoolProperty*>& Sky : (Parsed.bLock || Parsed.bUnlock) ? ExposureSkies(World) : TArray<TPair<AActor*, FBoolProperty*>>())
					{
						if (Sky.Value->GetPropertyValue_InContainer(Sky.Key) == Parsed.bLock)
						{
							Edit(*Sky.Key, Sky.Value->GetFName(), [&] { Sky.Value->SetPropertyValue_InContainer(Sky.Key, !Parsed.bLock); });
							OutTouched.AddUnique(Sky.Key);
						}
					}
				}
			}
			return true;
		}

		/** Linear value of each 8-bit sRGB code. */
		const float* SrgbToLinear()
		{
			static float Table[256];
			static bool bInit = false;
			if (!bInit)
			{
				for (int32 Code = 0; Code < 256; ++Code)
				{
					const float C = Code / 255.f;
					Table[Code] = C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
				}
				bInit = true;
			}
			return Table;
		}
	}

	FHyperAILightingImageMetrics MeasurePixels(const TConstArrayView<FColor> Pixels, const int32 Width, const int32 Height)
	{
		FHyperAILightingImageMetrics Metrics;
		Metrics.Width = Width;
		Metrics.Height = Height;
		const int64 Count = Pixels.Num();
		if (Width <= 0 || Height <= 0 || Count != static_cast<int64>(Width) * Height)
		{
			return Metrics;
		}
		const float* Linear = SrgbToLinear();
		const int64 Stride = FMath::Max<int64>(1, Count / MaxMeasuredSamples);
		TArray<float> Luminance;
		Luminance.Reserve(static_cast<int32>(Count / Stride + 1));
		TArray<double> Histogram;
		Histogram.SetNumZeroed(HistogramBins);
		double SumR = 0.0, SumG = 0.0, SumB = 0.0, Saturation = 0.0;
		int64 SaturationSamples = 0, Shadows = 0, Highlights = 0;
		for (int64 Index = 0; Index < Count; Index += Stride)
		{
			const FColor& Pixel = Pixels[Index];
			const float R = Linear[Pixel.R], G = Linear[Pixel.G], B = Linear[Pixel.B];
			const float Y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
			Luminance.Add(Y);
			// CIE L* scaled to 0-1: the brightness steps a viewer perceives as even.
			const float Lightness = Y <= 0.008856f ? 9.033f * Y : 1.16f * FMath::Pow(Y, 1.f / 3.f) - 0.16f;
			Histogram[FMath::Clamp(FMath::FloorToInt(Lightness * HistogramBins), 0, HistogramBins - 1)] += 1.0;
			const uint8 Max = FMath::Max3(Pixel.R, Pixel.G, Pixel.B);
			const uint8 Min = FMath::Min3(Pixel.R, Pixel.G, Pixel.B);
			Shadows += Lightness < 0.1f ? 1 : 0;
			Highlights += Max >= 250 ? 1 : 0;
			if (Max >= 13)
			{
				Saturation += static_cast<double>(Max - Min) / Max;
				++SaturationSamples;
			}
			// Colour temperature ignores black and clipped pixels, which carry no hue.
			if (Lightness >= 0.1f && Max < 250)
			{
				SumR += R;
				SumG += G;
				SumB += B;
			}
		}
		const int32 Samples = Luminance.Num();
		Luminance.Sort();
		auto Percentile = [&](const float Fraction) { return Luminance[FMath::Clamp(FMath::FloorToInt(Fraction * (Samples - 1)), 0, Samples - 1)]; };
		Metrics.LuminanceP5 = Percentile(0.05f);
		Metrics.LuminanceP50 = Percentile(0.5f);
		Metrics.LuminanceP95 = Percentile(0.95f);
		Metrics.ContrastRatio = (Metrics.LuminanceP95 + 0.002f) / (Metrics.LuminanceP5 + 0.002f);
		Metrics.MeanSaturation = SaturationSamples > 0 ? static_cast<float>(Saturation / SaturationSamples) : 0.f;
		Metrics.ShadowFraction = static_cast<float>(Shadows) / Samples;
		Metrics.HighlightFraction = static_cast<float>(Highlights) / Samples;
		// Average linear colour -> CIE xy -> McCamy's approximation of correlated colour temperature.
		const double X = 0.4124 * SumR + 0.3576 * SumG + 0.1805 * SumB;
		const double YSum = 0.2126 * SumR + 0.7152 * SumG + 0.0722 * SumB;
		const double Z = 0.0193 * SumR + 0.1192 * SumG + 0.9505 * SumB;
		if (X + YSum + Z > 0.0)
		{
			const double x = X / (X + YSum + Z);
			const double y = YSum / (X + YSum + Z);
			const double n = (x - 0.3320) / (0.1858 - y);
			Metrics.ColorTemperatureK = static_cast<float>(FMath::Clamp(449.0 * n * n * n + 3525.0 * n * n + 6823.3 * n + 5520.33, 1000.0, 40000.0));
		}
		for (const double Bin : Histogram)
		{
			Metrics.Histogram.Add(static_cast<float>(Bin / Samples));
		}
		Metrics.bValid = true;
		return Metrics;
	}

	float HistogramDistance(const TArray<float>& A, const TArray<float>& B)
	{
		if (A.Num() != B.Num() || A.Num() < 2) return 1.f;
		double CdfA = 0.0, CdfB = 0.0, Distance = 0.0;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			CdfA += A[Index];
			CdfB += B[Index];
			Distance += FMath::Abs(CdfA - CdfB);
		}
		return static_cast<float>(FMath::Clamp(Distance / (A.Num() - 1), 0.0, 1.0));
	}

	void CompareMetrics(const FHyperAILightingImageMetrics& Reference, const FHyperAILightingImageMetrics& Current,
		const bool bExposureLocked, const float MatchThreshold, FHyperAILightingCompareReport& Report)
	{
		Report.HistogramDistance = HistogramDistance(Reference.Histogram, Current.Histogram);
		Report.ExposureDeltaEV = FMath::Log2((Reference.LuminanceP50 + 0.0005f) / (Current.LuminanceP50 + 0.0005f));
		Report.ColorTemperatureDeltaK = Reference.ColorTemperatureK - Current.ColorTemperatureK;
		Report.ContrastRatioDelta = Reference.ContrastRatio - Current.ContrastRatio;
		Report.SaturationDelta = Reference.MeanSaturation - Current.MeanSaturation;
		Report.bMatched = Report.HistogramDistance <= MatchThreshold && FMath::Abs(Report.ExposureDeltaEV) <= 0.33f
			&& FMath::Abs(Report.ColorTemperatureDeltaK) <= 600.f && FMath::Abs(Report.SaturationDelta) <= 0.08f;

		TArray<TPair<float, FString>> Ranked;
		if (!bExposureLocked)
		{
			Ranked.Add({1000.f, TEXT("Auto exposure is on, so brightness here tracks the camera's adaptation, not the lights. lock_exposure first.")});
		}
		const float EV = Report.ExposureDeltaEV;
		if (FMath::Abs(EV) > 0.25f)
		{
			Ranked.Add({FMath::Abs(EV), FString::Printf(TEXT("%s by %.2f EV: %s, or scale sun and sky light intensity by %.2fx."),
				EV > 0.f ? TEXT("Brighten") : TEXT("Darken"), FMath::Abs(EV),
				bExposureLocked ? *FString::Printf(TEXT("lock_exposure with EV100 %s by %.2f"), EV > 0.f ? TEXT("lowered") : TEXT("raised"), FMath::Abs(EV))
					: *FString::Printf(TEXT("set_post_process exposure_bias %+.2f"), EV),
				FMath::Pow(2.f, EV))});
		}
		const float Kelvin = Report.ColorTemperatureDeltaK;
		if (FMath::Abs(Kelvin) > 300.f)
		{
			Ranked.Add({FMath::Abs(Kelvin) / 1000.f, FString::Printf(TEXT("%s: the reference averages about %.0f K against %.0f K here. %s"),
				Kelvin < 0.f ? TEXT("Warm the image") : TEXT("Cool the image"), Reference.ColorTemperatureK, Current.ColorTemperatureK,
				Kelvin < 0.f ? TEXT("Lower the sun temperature, add warmth to the sky light color, or raise post-process white_temp.")
					: TEXT("Raise the sun temperature, add blue to the sky light color, or lower post-process white_temp."))});
		}
		const float ContrastStops = FMath::Log2(FMath::Max(Reference.ContrastRatio, 1.f) / FMath::Max(Current.ContrastRatio, 1.f));
		if (FMath::Abs(ContrastStops) > 0.5f)
		{
			Ranked.Add({FMath::Abs(ContrastStops) / 2.f, FString::Printf(TEXT("%s: bright-to-dark ratio %.1f in the reference against %.1f here. %s"),
				ContrastStops > 0.f ? TEXT("Harder light") : TEXT("Softer light"), Reference.ContrastRatio, Current.ContrastRatio,
				ContrastStops > 0.f ? TEXT("Raise the sun against the sky light (lower sky light intensity), thin the fog, or raise post-process contrast.")
					: TEXT("Raise sky light intensity or fog density, lower the sun, or lower post-process contrast."))});
		}
		const float Saturation = Report.SaturationDelta;
		if (FMath::Abs(Saturation) > 0.05f)
		{
			Ranked.Add({FMath::Abs(Saturation) * 5.f, FString::Printf(TEXT("%s saturation about %.0f%%: set_post_process saturation, or %s."),
				Saturation > 0.f ? TEXT("Raise") : TEXT("Lower"), FMath::Abs(Saturation) / FMath::Max(Current.MeanSaturation, 0.05f) * 100.f,
				Saturation > 0.f ? TEXT("richer sun and sky colors") : TEXT("more neutral sun and sky colors"))});
		}
		const float Clipping = Current.HighlightFraction - Reference.HighlightFraction;
		if (Clipping > 0.03f)
		{
			Ranked.Add({Clipping * 10.f, FString::Printf(TEXT("%.0f%% of pixels clip to white against %.0f%% in the reference: lower the sun or exposure."),
				Current.HighlightFraction * 100.f, Reference.HighlightFraction * 100.f)});
		}
		Ranked.StableSort([](const TPair<float, FString>& A, const TPair<float, FString>& B) { return A.Key > B.Key; });
		for (const TPair<float, FString>& Entry : Ranked)
		{
			Report.Suggestions.Add(Entry.Value);
		}
		if (Report.Suggestions.IsEmpty())
		{
			Report.Suggestions.Add(Report.bMatched
				? TEXT("Matched: exposure, color temperature, contrast and saturation are all close. Save with apply_plan bSave when happy.")
				: TEXT("The averages agree but the brightness distribution differs; compare the images side by side and adjust fog or sky light for the midtones."));
		}
	}

	bool LoadProjectImage(const FString& Path, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
	{
		FString Full = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::ProjectDir() / Path : Path);
		FPaths::CollapseRelativeDirectories(Full);
		if (!FPaths::IsUnderDirectory(Full, FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())))
		{
			OutError = TEXT("Images must be inside the project folder; copy the reference in, for example to Saved/References.");
			return false;
		}
		const FString Extension = FPaths::GetExtension(Full).ToLower();
		if (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg") && Extension != TEXT("bmp") && Extension != TEXT("tga"))
		{
			OutError = TEXT("Use a PNG, JPG, BMP or TGA image.");
			return false;
		}
		const int64 Size = IFileManager::Get().FileSize(*Full);
		if (Size <= 0 || Size > MaxImageFileBytes)
		{
			OutError = Size <= 0 ? FString::Printf(TEXT("No image at %s."), *Full) : FString(TEXT("The image file is over 64 MB."));
			return false;
		}
		FImage Image;
		if (!FImageUtils::LoadImage(*Full, Image) || Image.SizeX <= 0 || Image.SizeY <= 0
			|| Image.SizeX > MaxImageDimension || Image.SizeY > MaxImageDimension)
		{
			OutError = FString::Printf(TEXT("%s could not be decoded, or is larger than %d pixels a side."), *Full, MaxImageDimension);
			return false;
		}
		Image.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<FColor> View = Image.AsBGRA8();
		OutPixels = TArray<FColor>(View.GetData(), static_cast<int32>(View.Num()));
		OutWidth = Image.SizeX;
		OutHeight = Image.SizeY;
		return true;
	}

	bool CaptureActiveViewport(TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
	{
		FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
		if (!Client || !Client->Viewport)
		{
			OutError = TEXT("No level viewport is open; open one, or pass current_image_path.");
			return false;
		}
		if (Client->GetViewMode() != VMI_Lit && Client->GetViewMode() != VMI_PathTracing)
		{
			OutError = TEXT("The level viewport is not in Lit view mode, so it does not show the lighting; switch it to Lit.");
			return false;
		}
		FViewport* Viewport = Client->Viewport;
		const FIntPoint Size = Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0 || static_cast<int64>(Size.X) * Size.Y > 16ll * 1024 * 1024)
		{
			OutError = TEXT("The level viewport has no drawable size, or is larger than 16 megapixels.");
			return false;
		}
		// Draw now so the measurement shows edits made since the viewport last redrew.
		Client->Invalidate();
		Viewport->Draw();
		if (!GetViewportScreenShot(Viewport, OutPixels) || OutPixels.Num() != Size.X * Size.Y)
		{
			OutError = TEXT("Reading the level viewport's pixels failed.");
			return false;
		}
		OutWidth = Size.X;
		OutHeight = Size.Y;
		return true;
	}

	bool WritePng(const FString& Path, const TConstArrayView<FColor> Pixels, const int32 Width, const int32 Height)
	{
		TArray<FColor> Opaque(Pixels.GetData(), Pixels.Num());
		for (FColor& Pixel : Opaque)
		{
			Pixel.A = 255;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Opaque.GetData(), Opaque.Num()), Png);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return !Png.IsEmpty() && FFileHelper::SaveArrayToFile(Png, *Path);
	}

	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	TArray<FHyperAILightingActorRecord> ReadActors(UWorld& World)
	{
		TArray<FHyperAILightingActorRecord> Records;
		for (const TCHAR* Kind : Kinds)
		{
			TArray<AActor*> Actors = ActorsOfKind(World, Kind);
			for (int32 Index = 0; Index < FMath::Min(Actors.Num(), MaxActorsPerKind); ++Index)
			{
				FHyperAILightingActorRecord& Record = Records.AddDefaulted_GetRef();
				Record.Kind = Kind;
				Record.Label = Actors[Index]->GetActorLabel();
				Record.Path = Actors[Index]->GetPathName();
				ReadProperties(*Actors[Index], Kind, Record.Properties);
			}
		}
		const TArray<AActor*> Skies = SkyActors(World);
		for (int32 Index = 0; Index < FMath::Min(Skies.Num(), MaxActorsPerKind); ++Index)
		{
			AActor& Sky = *Skies[Index];
			const FSkySystem& System = *SkySystemOf(Sky.GetClass());
			FHyperAILightingActorRecord& Record = Records.AddDefaulted_GetRef();
			Record.Kind = TEXT("sky_system");
			Record.Label = Sky.GetActorLabel();
			Record.Path = Sky.GetPathName();
			Record.Properties.Add({TEXT("system"), System.Name});
			Record.Properties.Add({TEXT("owns_lights"), System.bOwnsLights ? TEXT("true") : TEXT("false")});
			for (const TCHAR* Key : System.Keys)
			{
				FString Why;
				const FProperty* Property = FindSkyProperty(Sky.GetClass(), Key);
				if (Property && IsEditableSkyProperty(*Property, Why))
				{
					Record.Properties.Add({Key, SkyValueText(*Property, Sky)});
				}
			}
		}
		return Records;
	}

	TArray<FHyperAILightingActorRecord> ReadLocalLights(UWorld& World, const FString& CameraLabel, const FString& LabelFilter, const int32 Limit,
		int32& OutTotal, FString& OutView, const bool bUseView)
	{
		struct FEntry
		{
			AActor* Actor;
			ULocalLightComponent* Light;
			bool bInView;
			double Distance;
			FString Path;
		};
		const FLightView View = bUseView ? FindView(World, CameraLabel) : FLightView();
		OutView = View.bValid ? View.Name : FString();
		OutTotal = 0;
		TArray<FEntry> Entries;
		// Only the level's light components are walked, not every actor, so a large level stays quick.
		for (TObjectIterator<ULocalLightComponent> It; It; ++It)
		{
			ULocalLightComponent* Light = *It;
			AActor* Actor = IsValid(Light) && !Light->IsTemplate() && Light->GetWorld() == &World ? Light->GetOwner() : nullptr;
			if (!IsValid(Actor) || Actor->IsTemplate()) continue;
			++OutTotal;
			if (!LabelFilter.IsEmpty() && !Actor->GetActorLabel().Contains(LabelFilter)) continue;
			const FVector Location = Light->GetComponentLocation();
			Entries.Add({Actor, Light, View.bValid && InView(View, Location, Light->AttenuationRadius),
				View.bValid ? FVector::Dist(View.Origin, Location) : 0.0, Light->GetPathName()});
		}
		Entries.Sort([](const FEntry& A, const FEntry& B)
		{
			return A.bInView != B.bInView ? A.bInView : A.Distance != B.Distance ? A.Distance < B.Distance : A.Path < B.Path;
		});
		TArray<FHyperAILightingActorRecord> Records;
		for (int32 Index = 0; Index < FMath::Min(Entries.Num(), Limit); ++Index)
		{
			const FEntry& Entry = Entries[Index];
			FHyperAILightingActorRecord& Record = Records.AddDefaulted_GetRef();
			Record.Kind = FString(LightTypeOf(MaskOf(*Entry.Light))) + TEXT("_light");
			Record.Label = Entry.Actor->GetActorLabel();
			Record.Path = Entry.Path;
			ReadLight(*Entry.Actor, *Entry.Light, Record.Properties);
			if (View.bValid)
			{
				Record.Properties.Add({TEXT("in_view"), Entry.bInView ? TEXT("true") : TEXT("false")});
				Record.Properties.Add({TEXT("distance"), FString::Printf(TEXT("%.0f"), Entry.Distance)});
			}
		}
		return Records;
	}

	FString ComputeRevision(UWorld& World)
	{
		FString Canonical = TEXT("hyperai.lighting.revision.v1\n") + World.GetPathName() + TEXT("\n");
		TArray<FHyperAILightingActorRecord> Records = ReadActors(World);
		// Every local light, however few an inspection lists, and nothing that moves with a camera.
		int32 LocalTotal = 0;
		FString NoView;
		Records.Append(ReadLocalLights(World, FString(), FString(), MAX_int32, LocalTotal, NoView, /*bUseView=*/false));
		for (const FHyperAILightingActorRecord& Record : Records)
		{
			Canonical += Record.Kind + TEXT("|") + Record.Path + TEXT("|") + Record.Label + TEXT("\n");
			for (const FHyperAILightingProperty& Property : Record.Properties)
			{
				Canonical += Property.Key + TEXT("=") + Property.Value + TEXT("\n");
			}
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool IsExposureLocked(UWorld& World)
	{
		if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->ExposureSettings.bFixed)
		{
			return true;
		}
		for (const TPair<AActor*, FBoolProperty*>& Sky : ExposureSkies(World))
		{
			if (Sky.Value->GetPropertyValue_InContainer(Sky.Key))
			{
				return false;
			}
		}
		static IConsoleVariable* DefaultAutoExposure = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AutoExposure"));
		if (DefaultAutoExposure && DefaultAutoExposure->GetInt() == 0)
		{
			return true;
		}
		for (AActor* Actor : ActorsOfKind(World, TEXT("post_process")))
		{
			if (LocksExposure(*CastChecked<APostProcessVolume>(Actor)))
			{
				return true;
			}
		}
		return false;
	}

	FString LightOwningSky(UWorld& World)
	{
		const AActor* Sky = LightOwningSkyActor(World);
		return Sky ? FString::Printf(TEXT("%s ('%s')"), SkySystemOf(Sky->GetClass())->Name, *Sky->GetActorLabel()) : FString();
	}

	bool ValidateOps(UWorld& World, const TArray<FHyperAILightingOp>& Ops, FString& OutError)
	{
		TArray<AActor*> Unused;
		return RunOps(World, Ops, /*bWrite=*/false, Unused, OutError);
	}

	bool ApplyOps(UWorld& World, const TArray<FHyperAILightingOp>& Ops, TArray<AActor*>& OutTouched, FString& OutError)
	{
		if (!ValidateOps(World, Ops, OutError))
		{
			return false;
		}
		const FScopedTransaction Transaction(LOCTEXT("ApplyLightingPlan", "HyperAI: Apply Lighting Plan"));
		return RunOps(World, Ops, /*bWrite=*/true, OutTouched, OutError);
	}

	TArray<AActor*> ResolveTargets(UWorld& World, const TArray<FHyperAILightingOp>& Ops)
	{
		TArray<AActor*> Actors;
		for (const FHyperAILightingOp& Op : Ops)
		{
			FParsedOp Parsed;
			FString Error;
			if (!ParseOp(Op, Parsed, Error)) continue;
			UMeshComponent* Mesh = nullptr;
			int32 Slot = INDEX_NONE;
			if (Parsed.Kind == TEXT("actor_material"))
			{
				if (ResolveMaterialSlot(World, Op.ActorLabel, Parsed, Mesh, Slot, Error)) Actors.AddUnique(Mesh->GetOwner());
				continue;
			}
			if (Parsed.Kind == TEXT("sky_system"))
			{
				AActor* Sky = nullptr;
				FProperty* Property = nullptr;
				FString Import;
				if (ResolveSkyOp(World, Op.ActorLabel, Parsed, Sky, Property, Import, Error)) Actors.AddUnique(Sky);
				continue;
			}
			if (Parsed.Kind == TEXT("light") || Parsed.Kind == TEXT("add_light"))
			{
				const TArray<AActor*> Found = Labelled(World, Op.ActorLabel);
				if (Found.Num() == 1) Actors.AddUnique(Found[0]);
				continue;
			}
			if (Parsed.bLock || Parsed.bUnlock)
			{
				for (const TPair<AActor*, FBoolProperty*>& Sky : ExposureSkies(World)) Actors.AddUnique(Sky.Key);
			}
			if (AActor* Actor = FindActor(World, Parsed.Kind, Op.ActorLabel)) Actors.AddUnique(Actor);
		}
		return Actors;
	}

	bool CheckMaterialOps(UWorld& World, const TArray<FHyperAILightingOp>& Ops, FString& OutError)
	{
		// ponytail: the last op on a slot wins, so a plan setting one slot twice checks only the second material.
		TMap<TPair<UMeshComponent*, int32>, UMaterialInterface*> Expected;
		for (const FHyperAILightingOp& Op : Ops)
		{
			FParsedOp Parsed;
			UMeshComponent* Mesh = nullptr;
			int32 Slot = INDEX_NONE;
			if (!ParseOp(Op, Parsed, OutError) || Parsed.Kind != TEXT("actor_material")) continue;
			if (!ResolveMaterialSlot(World, Op.ActorLabel, Parsed, Mesh, Slot, OutError)) return false;
			Expected.Add(TPair<UMeshComponent*, int32>(Mesh, Slot), Parsed.Material);
		}
		for (const TPair<TPair<UMeshComponent*, int32>, UMaterialInterface*>& Entry : Expected)
		{
			if (Entry.Key.Key->GetMaterial(Entry.Key.Value) != Entry.Value)
			{
				OutError = FString::Printf(TEXT("%s slot %d does not hold %s."), *Entry.Key.Key->GetPathName(), Entry.Key.Value, *Entry.Value->GetPathName());
				return false;
			}
		}
		OutError.Reset();
		return true;
	}

	TArray<UPackage*> PackagesOf(const TArray<AActor*>& Actors)
	{
		TArray<UPackage*> Packages;
		for (const AActor* Actor : Actors)
		{
			// An actor's package is its own external package under One File Per Actor, else the level's.
			if (Actor && Actor->GetPackage()) Packages.AddUnique(Actor->GetPackage());
		}
		return Packages;
	}
}

#undef LOCTEXT_NAMESPACE
