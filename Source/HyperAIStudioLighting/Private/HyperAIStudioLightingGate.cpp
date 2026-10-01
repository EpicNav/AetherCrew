// Games by Hyper 2026.

#include "HyperAIStudioLightingGate.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxReflectionCaptureComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/MeshComponent.h"
#include "Components/ReflectionCaptureComponent.h"
#include "Components/SphereReflectionCaptureComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Engine/BoxReflectionCapture.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Light.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/RectLight.h"
#include "Engine/Scene.h"
#include "Engine/SkyLight.h"
#include "Engine/SphereReflectionCapture.h"
#include "Engine/SpotLight.h"
#include "Engine/TextureLightProfile.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioLightingLookDev.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/StringOutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RenderUtils.h"
#include "ScopedTransaction.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
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

		const TCHAR* const Kinds[] = {TEXT("sun"), TEXT("sky_light"), TEXT("fog"), TEXT("sky_atmosphere"), TEXT("clouds"), TEXT("post_process")};

		UClass* ClassOf(const FString& Kind)
		{
			if (Kind == TEXT("sun")) return ADirectionalLight::StaticClass();
			if (Kind == TEXT("sky_light")) return ASkyLight::StaticClass();
			if (Kind == TEXT("fog")) return AExponentialHeightFog::StaticClass();
			if (Kind == TEXT("sky_atmosphere")) return ASkyAtmosphere::StaticClass();
			if (Kind == TEXT("clouds")) return AVolumetricCloud::StaticClass();
			if (Kind == TEXT("post_process")) return APostProcessVolume::StaticClass();
			return nullptr;
		}

		FString SpawnLabelOf(const FString& Kind)
		{
			if (Kind == TEXT("sun")) return TEXT("HyperAI_Sun");
			if (Kind == TEXT("sky_light")) return TEXT("HyperAI_SkyLight");
			if (Kind == TEXT("fog")) return TEXT("HyperAI_HeightFog");
			if (Kind == TEXT("sky_atmosphere")) return TEXT("HyperAI_SkyAtmosphere");
			if (Kind == TEXT("clouds")) return TEXT("HyperAI_VolumetricCloud");
			return TEXT("HyperAI_PostProcess");
		}

		/** Kinds a Blueprint sky that owns its lights also owns: an edit to one would fight it or add a second. */
		bool IsSkyOwnedKind(const FString& Kind)
		{
			return Kind == TEXT("sun") || Kind == TEXT("sky_light") || Kind == TEXT("fog") || Kind == TEXT("sky_atmosphere") || Kind == TEXT("clouds");
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
		bool ParseSkyValue(const FProperty& Property, UObject& Owner, const FString& Text, FString& OutImport, FString& OutError)
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

		constexpr int32 MaxChangedDetails = 32;

		/**
		 * Where a stock actor keeps what its Details panel shows: its component, or a volume's post-process settings. Every
		 * setting there is a lever a lighting artist reaches for, so the ops take any of them by name beside their fixed keys.
		 */
		struct FDetails
		{
			/** What an edit is announced to: PreEditChange and PostEditChangeProperty. */
			UObject* Object = nullptr;
			/** Where the values live, and the class or struct declaring them. */
			void* Container = nullptr;
			const UStruct* Type = nullptr;
			/** The values nothing has changed: the component's archetype, or default post-process settings. */
			const void* Defaults = nullptr;
			bool bPostProcess = false;
		};

		const FPostProcessSettings& DefaultPostProcess()
		{
			static const FPostProcessSettings Settings;
			return Settings;
		}

		UActorComponent* ComponentOf(AActor& Actor)
		{
			if (ADirectionalLight* Sun = Cast<ADirectionalLight>(&Actor)) return Sun->GetComponent();
			if (ASkyLight* Sky = Cast<ASkyLight>(&Actor)) return Sky->GetLightComponent();
			if (AExponentialHeightFog* Fog = Cast<AExponentialHeightFog>(&Actor)) return Fog->GetComponent();
			if (ASkyAtmosphere* Atmosphere = Cast<ASkyAtmosphere>(&Actor)) return Atmosphere->GetComponent();
			if (AReflectionCapture* Capture = Cast<AReflectionCapture>(&Actor)) return Capture->GetCaptureComponent();
			return Actor.IsA<AVolumetricCloud>() ? Actor.FindComponentByClass<UVolumetricCloudComponent>() : nullptr;
		}

		FDetails DetailsOf(AActor& Actor)
		{
			FDetails Details;
			if (APostProcessVolume* Volume = Cast<APostProcessVolume>(&Actor))
			{
				Details.Object = Volume;
				Details.Container = &Volume->Settings;
				Details.Type = FPostProcessSettings::StaticStruct();
				Details.Defaults = &DefaultPostProcess();
				Details.bPostProcess = true;
			}
			else if (UActorComponent* Component = ComponentOf(Actor))
			{
				Details.Object = Component;
				Details.Container = Component;
				Details.Type = Component->GetClass();
				Details.Defaults = Component->GetArchetype();
			}
			return Details;
		}

		/** Letters and digits in lower case: how a name compares, however it is spaced or cased. */
		FString Plain(const FString& Name)
		{
			FString Out;
			for (const TCHAR Character : Name)
			{
				if (FChar::IsAlnum(Character)) Out.AppendChar(FChar::ToLower(Character));
			}
			return Out;
		}

		/** Settings the ops already own, so the Details path cannot route around the exposure lock. */
		bool IsOwnedByOps(const FString& Name)
		{
			return Name == TEXT("AutoExposureMethod") || Name == TEXT("AutoExposureBias") || Name == TEXT("AutoExposureApplyPhysicalCameraExposure")
				|| Name == TEXT("Mobility");
		}

		bool IsDetail(const FProperty& Property)
		{
			const FString Name = Property.GetName();
			return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance | CPF_Deprecated)
				&& Property.ArrayDim == 1 && !Name.StartsWith(TEXT("bOverride_")) && !IsOwnedByOps(Name);
		}

		/** A Details-panel setting by its C++ name or its label there, in any case, with or without spaces and underscores. */
		FProperty* FindDetail(const UStruct& Type, const FString& Name, FString& OutError)
		{
			const FString Wanted = Plain(Name);
			TArray<FProperty*> Matches;
			for (TFieldIterator<FProperty> It(&Type); It; ++It)
			{
				FProperty* Property = *It;
				if (!IsDetail(*Property)) continue;
				const FString Own = Property->GetName();
				if (Own.Equals(Name, ESearchCase::IgnoreCase))
				{
					return Property;
				}
				// A flag also answers to its name without the b: bEnableLightShaftBloom as enable_light_shaft_bloom.
				const FString Bare = Property->IsA<FBoolProperty>() && Own.Len() > 1 && Own[0] == TEXT('b') && FChar::IsUpper(Own[1]) ? Own.RightChop(1) : Own;
				if (Plain(Bare) == Wanted || Plain(Property->GetDisplayNameText().ToString()) == Wanted)
				{
					Matches.AddUnique(Property);
				}
			}
			if (Matches.Num() == 1)
			{
				return Matches[0];
			}
			TArray<FString> Names;
			for (const FProperty* Match : Matches)
			{
				Names.Add(Match->GetName());
			}
			OutError = Matches.IsEmpty() ? FString::Printf(TEXT("has no setting '%s' in its Details panel"), *Name.Left(64))
				: FString::Printf(TEXT("has several settings called '%s' (%s); name one exactly"), *Name.Left(64), *FString::Join(Names, TEXT(", ")));
			return nullptr;
		}

		/** Colours and vectors as the fixed keys take them, "r,g,b", in Unreal's text form; anything else as typed. */
		FString DetailText(const FProperty& Property, const FString& Value)
		{
			const FStructProperty* Struct = CastField<FStructProperty>(&Property);
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","));
			if (!Struct || Value.StartsWith(TEXT("(")) || Parts.Num() < 3 || Parts.Num() > 4)
			{
				return Value;
			}
			TArray<float> Numbers;
			for (const FString& Part : Parts)
			{
				float Number = 0.f;
				if (!LexTryParseString(Number, *Part.TrimStartAndEnd())) return Value;
				Numbers.Add(Number);
			}
			const float W = Numbers.Num() == 4 ? Numbers[3] : 1.f;
			const UScriptStruct* Type = Struct->Struct;
			if (Type == TBaseStructure<FLinearColor>::Get())
			{
				return FString::Printf(TEXT("(R=%g,G=%g,B=%g,A=%g)"), Numbers[0], Numbers[1], Numbers[2], W);
			}
			if (Type == TBaseStructure<FColor>::Get())
			{
				// Written in linear colour like every other colour here; kept as sRGB bytes.
				const FColor Bytes = FLinearColor(Numbers[0], Numbers[1], Numbers[2]).ToFColor(true);
				return FString::Printf(TEXT("(R=%d,G=%d,B=%d,A=255)"), Bytes.R, Bytes.G, Bytes.B);
			}
			if (Type == TBaseStructure<FVector4>::Get())
			{
				return FString::Printf(TEXT("(X=%g,Y=%g,Z=%g,W=%g)"), Numbers[0], Numbers[1], Numbers[2], W);
			}
			if (Type == TBaseStructure<FVector>::Get() && Numbers.Num() == 3)
			{
				return FString::Printf(TEXT("(X=%g,Y=%g,Z=%g)"), Numbers[0], Numbers[1], Numbers[2]);
			}
			return Value;
		}

		/** The C++ names the fixed keys already show, so a changed setting is not listed twice. */
		const TSet<FString>& ShownByKeys()
		{
			static const TSet<FString> Names = {TEXT("Intensity"), TEXT("Temperature"), TEXT("bUseTemperature"), TEXT("LightColor"), TEXT("LightSourceAngle"),
				TEXT("bAtmosphereSunLight"), TEXT("bRealTimeCapture"), TEXT("SourceType"), TEXT("FogDensity"), TEXT("FogHeightFalloff"), TEXT("StartDistance"),
				TEXT("FogInscatteringLuminance"), TEXT("bEnableVolumetricFog"), TEXT("VolumetricFogScatteringDistribution"), TEXT("VolumetricFogAlbedo"),
				TEXT("VolumetricFogExtinctionScale"), TEXT("RayleighScatteringScale"), TEXT("MieScatteringScale"), TEXT("WhiteTemp"), TEXT("ColorSaturation"),
				TEXT("ColorContrast"), TEXT("ColorGamma"), TEXT("ColorGainShadows"), TEXT("ColorOffsetShadows"), TEXT("ColorGainHighlights"),
				TEXT("LayerBottomAltitude"), TEXT("LayerHeight"), TEXT("Material")};
			return Names;
		}

		/** Settings changed from their defaults beyond the fixed keys: what a plan or a person set through the Details panel. */
		void AddChangedDetails(AActor& Actor, TArray<FHyperAILightingProperty>& Out)
		{
			const FDetails Details = DetailsOf(Actor);
			if (!Details.Type || !Details.Defaults)
			{
				return;
			}
			int32 Added = 0;
			for (TFieldIterator<FProperty> It(Details.Type); It && Added < MaxChangedDetails; ++It)
			{
				FProperty* Property = *It;
				const FString Name = Property->GetName();
				// Where a component sits and whether it renders are the level's business, not its light's.
				const UStruct* Owner = Property->GetOwnerStruct();
				if (!IsDetail(*Property) || Property->HasAnyPropertyFlags(CPF_Transient) || ShownByKeys().Contains(Name)
					|| Owner == USceneComponent::StaticClass() || Owner == UActorComponent::StaticClass() || Owner == UPrimitiveComponent::StaticClass())
				{
					continue;
				}
				bool bChanged = false;
				if (Details.bPostProcess)
				{
					const FBoolProperty* Override = FindFProperty<FBoolProperty>(FPostProcessSettings::StaticStruct(), FName(*(TEXT("bOverride_") + Name)));
					bChanged = Override && Override->GetPropertyValue_InContainer(Details.Container);
				}
				else
				{
					bChanged = !Property->Identical_InContainer(Details.Container, Details.Defaults);
				}
				if (!bChanged) continue;
				FString Text;
				Property->ExportTextItem_InContainer(Text, Details.Container, nullptr, Details.Object, PPF_None);
				Out.Add({Name, Text.Len() > 160 ? Text.Left(160) + TEXT("...") : Text});
				++Added;
			}
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
			else if (const UVolumetricCloudComponent* Cloud = Actor.IsA<AVolumetricCloud>() ? Actor.FindComponentByClass<UVolumetricCloudComponent>() : nullptr)
			{
				Add(TEXT("layer_bottom_altitude"), Number(Cloud->LayerBottomAltitude));
				Add(TEXT("layer_height"), Number(Cloud->LayerHeight));
				// A soft reference the component loads when it registers, so this only looks it up.
				const UMaterialInterface* Material = Cloud->Material.LoadSynchronous();
				Add(TEXT("material"), Material ? Material->GetPathName() : FString(TEXT("none")));
				// The engine's cloud material's two strongest levers, when the clouds' material has them.
				for (const TCHAR* Parameter : {TEXT("Cloud_GlobalCoverage"), TEXT("Cloud_GlobalDensity")})
				{
					float Value = 0.f;
					if (Material && Material->GetScalarParameterValue(FHashedMaterialParameterInfo(FName(Parameter)), Value))
					{
						Out.Add({FString(TEXT("material:")) + Parameter, Number(Value)});
					}
				}
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
			AddChangedDetails(Actor, Out);
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
		/** Reflection captures add_reflection_capture places. */
		const FName AddedCaptureTag(TEXT("HyperAI.AddedReflectionCapture"));

		enum ECaptureShape : uint8
		{
			NoShape = 0,
			SphereShape = 1,
			BoxShape = 2,
			PlaneShape = 3,
		};

		/** set_actor_lighting's settings: how a placed mesh takes part in the level's light, by its Details-panel flag. */
		struct FActorLightingSpec
		{
			const TCHAR* Key;
			const TCHAR* Property;
		};

		const FActorLightingSpec ActorLightingSpecs[] = {
			{TEXT("cast_shadow"), TEXT("CastShadow")},
			// Lumen leaves small meshes out of its scene, so a small glowing one stops lighting its surroundings off screen.
			{TEXT("emissive_light_source"), TEXT("bEmissiveLightSource")},
			{TEXT("affect_indirect_lighting"), TEXT("bAffectDynamicIndirectLighting")},
			{TEXT("affect_distance_field_lighting"), TEXT("bAffectDistanceFieldLighting")},
			{TEXT("visible_in_reflection_captures"), TEXT("bVisibleInReflectionCaptures")},
			{TEXT("visible_in_ray_tracing"), TEXT("bVisibleInRayTracing")},
		};

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
			// A stock kind's setting beyond the fixed keys: any its Details panel shows, found by name when the plan runs.
			FString DetailName;
			FString DetailValue;
			// set_material_parameter and set_actor_material_parameter: the parameter on the slot's material, and whether the
			// actor's slot alone changes.
			FString ParameterName;
			bool bActorOnly = false;
			// set_actor_lighting: the mesh flag.
			const FActorLightingSpec* ActorLighting = nullptr;
			// add_reflection_capture: its shape. set_reflection_capture: location or box_extent, else a Details setting.
			uint8 CaptureShape = NoShape;
			FString CaptureKey;
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
			if (Op.Kind == TEXT("set_actor_shadow"))
			{
				// A blockout shape standing in for foliage, cloth or glass would block light the finished asset lets through.
				Out.Kind = TEXT("actor_shadow");
				FString Component = Op.Property.TrimStartAndEnd();
				if (Op.ActorLabel.IsEmpty() || (!Component.IsEmpty() && !Component.StartsWith(TEXT("component:"))))
				{
					OutError = TEXT("set_actor_shadow needs actor_label, a placed actor; property is empty for all its meshes or component:<name> for one; value true or false.");
					return false;
				}
				Out.ComponentName = Component.RightChop(10);
				const FString Flag = Op.Value.TrimStartAndEnd();
				Out.bFlag = Flag.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Flag == TEXT("1");
				if (!Out.bFlag && !Flag.Equals(TEXT("false"), ESearchCase::IgnoreCase) && Flag != TEXT("0"))
				{
					OutError = TEXT("set_actor_shadow takes true (it casts shadows) or false (light passes it).");
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("set_material_parameter") || Op.Kind == TEXT("set_actor_material_parameter"))
			{
				Out.Kind = TEXT("material_parameter");
				Out.bActorOnly = Op.Kind == TEXT("set_actor_material_parameter");
				const FString Grammar = Op.Kind + TEXT(" property is [component:<name>/][slot:<index or name>/]<parameter>, such as Roughness or slot:1/BaseColor; value is a number, linear \"r,g,b[,a]\", a texture's path, or default.");
				if (Op.ActorLabel.IsEmpty())
				{
					OutError = Op.Kind + TEXT(" needs actor_label: a placed actor showing the material (hyper_lighting_inspect lists them with each material in view).");
					return false;
				}
				FString Rest = Op.Property.TrimStartAndEnd();
				if (Rest.StartsWith(TEXT("component:")) && (!Rest.RightChop(10).Split(TEXT("/"), &Out.ComponentName, &Rest) || Out.ComponentName.IsEmpty()))
				{
					OutError = Grammar;
					return false;
				}
				if (Rest.StartsWith(TEXT("slot:")))
				{
					FString SlotValue;
					if (!Rest.RightChop(5).Split(TEXT("/"), &SlotValue, &Rest) || SlotValue.IsEmpty())
					{
						OutError = Grammar;
						return false;
					}
					if (SlotValue.IsNumeric())
					{
						Out.SlotIndex = FCString::Atoi(*SlotValue);
					}
					else
					{
						Out.SlotName = FName(*SlotValue);
					}
				}
				Out.ParameterName = Rest.TrimStartAndEnd();
				Out.DetailValue = Op.Value.TrimStartAndEnd();
				if (Out.ParameterName.IsEmpty() || Out.DetailValue.IsEmpty())
				{
					OutError = Grammar;
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("set_actor_lighting"))
			{
				Out.Kind = TEXT("actor_lighting");
				FString Key = Op.Property.TrimStartAndEnd();
				if (Op.ActorLabel.IsEmpty() || (Key.StartsWith(TEXT("component:")) && (!Key.RightChop(10).Split(TEXT("/"), &Out.ComponentName, &Key) || Out.ComponentName.IsEmpty())))
				{
					OutError = TEXT("set_actor_lighting needs actor_label, a placed actor, and property [component:<name>/]<setting>; value true or false.");
					return false;
				}
				TArray<FString> Keys;
				for (const FActorLightingSpec& Spec : ActorLightingSpecs)
				{
					Keys.Add(Spec.Key);
					Out.ActorLighting = Key == Spec.Key ? &Spec : Out.ActorLighting;
				}
				if (!Out.ActorLighting)
				{
					OutError = FString::Printf(TEXT("set_actor_lighting has no setting '%s'; use one of %s."), *Key.Left(64), *FString::Join(Keys, TEXT(", ")));
					return false;
				}
				const FString Flag = Op.Value.TrimStartAndEnd();
				Out.bFlag = Flag.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Flag == TEXT("1");
				if (!Out.bFlag && !Flag.Equals(TEXT("false"), ESearchCase::IgnoreCase) && Flag != TEXT("0"))
				{
					OutError = FString::Printf(TEXT("set_actor_lighting %s takes true or false."), Out.ActorLighting->Key);
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("add_reflection_capture"))
			{
				Out.Kind = TEXT("add_reflection_capture");
				Out.CaptureShape = Op.Property == TEXT("sphere") ? SphereShape : Op.Property == TEXT("box") ? BoxShape : NoShape;
				if (Op.ActorLabel.IsEmpty() || Op.ActorLabel != Op.ActorLabel.TrimStartAndEnd())
				{
					OutError = TEXT("add_reflection_capture needs actor_label: the new capture's label, such as Capture_Hall.");
					return false;
				}
				if (Out.CaptureShape == NoShape)
				{
					OutError = TEXT("add_reflection_capture property is its shape: sphere or box.");
					return false;
				}
				if (!ParseVector(Op.Value, Out.Vector))
				{
					OutError = TEXT("add_reflection_capture value is where it goes, as world \"x,y,z\" in centimetres.");
					return false;
				}
				return true;
			}
			if (Op.Kind == TEXT("set_reflection_capture"))
			{
				Out.Kind = TEXT("reflection_capture");
				Out.CaptureKey = Op.Property.TrimStartAndEnd();
				if (Op.ActorLabel.IsEmpty() || Out.CaptureKey.IsEmpty() || Op.Value.TrimStartAndEnd().IsEmpty())
				{
					OutError = TEXT("set_reflection_capture needs actor_label, the capture; property location, box_extent or a setting its Details panel shows, such as Brightness or InfluenceRadius; and value.");
					return false;
				}
				if (Out.CaptureKey == TEXT("location"))
				{
					if (ParseVector(Op.Value, Out.Vector)) return true;
					OutError = TEXT("set_reflection_capture location needs world \"x,y,z\" in centimetres.");
					return false;
				}
				if (Out.CaptureKey == TEXT("box_extent"))
				{
					TArray<FString> Parts;
					Op.Value.ParseIntoArray(Parts, TEXT(","));
					float X = 0.f, Y = 0.f, Z = 0.f;
					if (Parts.Num() == 3 && ParseNumber(Parts[0], 10.f, 1000000.f, X) && ParseNumber(Parts[1], 10.f, 1000000.f, Y) && ParseNumber(Parts[2], 10.f, 1000000.f, Z))
					{
						Out.Vector = FVector(X, Y, Z);
						return true;
					}
					OutError = TEXT("set_reflection_capture box_extent needs the box's half sizes \"x,y,z\" in centimetres, each from 10 to 1000000.");
					return false;
				}
				Out.DetailName = Out.CaptureKey;
				Out.DetailValue = Op.Value.TrimStartAndEnd();
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
				OutError = FString::Printf(TEXT("Unknown kind '%s': use set_sun, set_sky_light, set_fog, set_sky_atmosphere, set_clouds, set_post_process, lock_exposure, unlock_exposure, set_light, add_light, set_actor_material, set_material_parameter, set_actor_material_parameter, set_actor_shadow, set_actor_lighting, add_reflection_capture, set_reflection_capture or set_sky_system."), *Op.Kind);
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
				// Beyond the fixed keys, any setting the actor's Details panel shows: checked against the actor when the plan runs.
				Out.DetailName = Op.Property.TrimStartAndEnd();
				Out.DetailValue = Op.Value.TrimStartAndEnd();
				if (Out.DetailName.IsEmpty() || Out.DetailValue.IsEmpty())
				{
					TArray<FString> Keys;
					for (const FPropertySpec& Spec : PropertySpecs)
					{
						if (Out.Kind == Spec.Kind) Keys.Add(Spec.Key);
					}
					OutError = FString::Printf(TEXT("%s needs property and value: %s%sor any setting its Details panel shows, by name, such as %s."), *Op.Kind,
						*FString::Join(Keys, TEXT(", ")), Keys.IsEmpty() ? TEXT("") : TEXT(", "),
						Out.Kind == TEXT("sun") ? TEXT("BloomScale or light_shaft_bloom") : Out.Kind == TEXT("sky_light") ? TEXT("MinOcclusion or OcclusionTint")
						: Out.Kind == TEXT("fog") ? TEXT("DirectionalInscatteringExponent or SecondFogData") : Out.Kind == TEXT("clouds") ? TEXT("LayerBottomAltitude, or material:Cloud_GlobalCoverage")
						: Out.Kind == TEXT("post_process") ? TEXT("BloomIntensity, LocalExposureShadowContrastScale or color_grading_lut") : TEXT("MieAnisotropy"));
					return false;
				}
				return true;
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

		/** A Details-panel setting's value from an op, checked as the panel would take it; "default" lifts a post-process override. */
		bool ResolveDetail(AActor& Actor, const FParsedOp& Op, FDetails& OutDetails, FProperty*& OutProperty, FString& OutImport, bool& bOutDefault,
			FString& OutError)
		{
			OutDetails = DetailsOf(Actor);
			bOutDefault = false;
			if (!OutDetails.Type)
			{
				OutError = FString::Printf(TEXT("'%s' has no settings these ops change"), *Actor.GetActorLabel());
				return false;
			}
			OutProperty = FindDetail(*OutDetails.Type, Op.DetailName, OutError);
			if (!OutProperty)
			{
				return false;
			}
			if (OutDetails.bPostProcess && Op.DetailValue.Equals(TEXT("default"), ESearchCase::IgnoreCase))
			{
				bOutDefault = true;
				return true;
			}
			const FString Text = DetailText(*OutProperty, Op.DetailValue);
			// An asset's path loads it first, so the panel's own parsing finds it.
			if (CastField<FObjectPropertyBase>(OutProperty) && Text.StartsWith(TEXT("/")))
			{
				FString Path = Text;
				if (!Path.Contains(TEXT(".")))
				{
					Path += TEXT(".") + FPackageName::GetShortName(Path);
				}
				FSoftObjectPath(Path).TryLoad();
			}
			FString Why;
			if (!ParseSkyValue(*OutProperty, *OutDetails.Object, Text, OutImport, Why))
			{
				OutError = FString::Printf(TEXT("%s %s"), *OutProperty->GetName(), *Why);
				return false;
			}
			return true;
		}

		void WriteDetail(const FDetails& Details, FProperty& Property, const FString& Import, const bool bDefault)
		{
			// A post-process setting counts only while its override is on, as its Details-panel checkbox is.
			FBoolProperty* Override = Details.bPostProcess
				? FindFProperty<FBoolProperty>(FPostProcessSettings::StaticStruct(), FName(*(TEXT("bOverride_") + Property.GetName()))) : nullptr;
			Edit(*Details.Object, Details.bPostProcess ? GET_MEMBER_NAME_CHECKED(APostProcessVolume, Settings) : Property.GetFName(), [&]
			{
				if (!bDefault)
				{
					Property.ImportText_InContainer(*Import, Details.Container, Details.Object, PPF_None);
				}
				if (Override)
				{
					Override->SetPropertyValue_InContainer(Details.Container, !bDefault);
				}
			});
		}

		/** A 256 x 16 LUT image as the texture Unreal's colour grading takes: made, or refreshed, under MadeAssetsRoot/LUTs. */
		UTexture2D* MakeLutTexture(const FString& ImagePath, const TArray<FColor>& Strip, FString& OutError)
		{
			FString Stem;
			for (const TCHAR Character : FPaths::GetBaseFilename(ImagePath))
			{
				Stem.AppendChar(FChar::IsAlnum(Character) && Character < 128 ? Character : TEXT('_'));
			}
			const FString Name = TEXT("LUT_") + Stem.Left(80);
			const FString PackageName = FString(MadeAssetsRoot) + TEXT("LUTs/") + Name;
			UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *(PackageName + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
			const bool bNew = Texture == nullptr;
			if (bNew)
			{
				UPackage* Package = CreatePackage(*PackageName);
				Texture = Package ? NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional) : nullptr;
				if (!Texture)
				{
					OutError = FString::Printf(TEXT("the LUT texture %s could not be made"), *PackageName);
					return nullptr;
				}
			}
			Texture->Modify();
			Texture->PreEditChange(nullptr);
			Texture->Source.Init(Lut::StripWidth, Lut::StripHeight, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(Strip.GetData()));
			// What the engine asks of a colour-grading LUT: no mips, no sRGB decode, uncompressed and clamped.
			Texture->LODGroup = TEXTUREGROUP_ColorLookupTable;
			Texture->MipGenSettings = TMGS_NoMipmaps;
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_VectorDisplacementmap;
			Texture->AddressX = TA_Clamp;
			Texture->AddressY = TA_Clamp;
			Texture->PostEditChange();
			// Built now, so the next capture shows the grade rather than the texture still compiling.
			FTextureCompilingManager::Get().FinishCompilation({static_cast<UTexture*>(Texture)});
			if (bNew)
			{
				FAssetRegistryModule::AssetCreated(Texture);
			}
			Texture->MarkPackageDirty();
			return Texture;
		}

		/**
		 * set_post_process color_grading_lut: a 256 x 16 LUT image inside the project, as hyper_image_compare's fit_lut writes
		 * one, imported as a LUT texture; a LUT texture's path; or none. It applies at full intensity: a fitted LUT already holds
		 * the old grade at its old strength, and a later ColorGradingIntensity op softens it.
		 */
		bool LutOp(APostProcessVolume& Volume, const FString& Value, const bool bWrite, FString& OutError)
		{
			const bool bNone = Value.Equals(TEXT("none"), ESearchCase::IgnoreCase);
			const bool bImage = !bNone && !Value.StartsWith(TEXT("/"));
			UTexture* Texture = nullptr;
			TArray<FColor> Strip;
			if (bImage)
			{
				int32 Width = 0;
				int32 Height = 0;
				if (!LoadProjectImage(Value, Strip, Width, Height, OutError))
				{
					return false;
				}
				if (Width != Lut::StripWidth || Height != Lut::StripHeight)
				{
					OutError = FString::Printf(TEXT("color_grading_lut %s is %d x %d; a LUT is 256 x 16, as hyper_image_compare's fit_lut writes"), *Value, Width, Height);
					return false;
				}
			}
			else if (!bNone)
			{
				FString Path = Value;
				if (!Path.Contains(TEXT(".")))
				{
					Path += TEXT(".") + FPackageName::GetShortName(Path);
				}
				Texture = Cast<UTexture>(FSoftObjectPath(Path).TryLoad());
				if (!Texture)
				{
					OutError = FString::Printf(TEXT("color_grading_lut '%s' is not a texture; pass a LUT image inside the project, a LUT texture's path, or none"), *Value.Left(128));
					return false;
				}
			}
			if (!bWrite)
			{
				return true;
			}
			if (bImage && !(Texture = MakeLutTexture(Value, Strip, OutError)))
			{
				return false;
			}
			Edit(Volume, GET_MEMBER_NAME_CHECKED(APostProcessVolume, Settings), [&]
			{
				FPostProcessSettings& S = Volume.Settings;
				S.bOverride_ColorGradingLUT = !bNone;
				S.ColorGradingLUT = bNone ? nullptr : Texture;
				S.bOverride_ColorGradingIntensity = !bNone;
				S.ColorGradingIntensity = 1.f;
			});
			return true;
		}

		/** The clouds' own material instance: made from their material, under MadeAssetsRoot/Clouds, the first time a plan changes one of its parameters. */
		UMaterialInstanceConstant* OwnCloudMaterial(UVolumetricCloudComponent& Cloud, UWorld& World, FString& OutError)
		{
			UMaterialInterface* Current = Cloud.Material.LoadSynchronous();
			const FString Folder = FString(MadeAssetsRoot) + TEXT("Clouds/");
			if (UMaterialInstanceConstant* Own = Cast<UMaterialInstanceConstant>(Current); Own && Own->GetPathName().StartsWith(Folder))
			{
				return Own;
			}
			const FString Base = TEXT("MI_Clouds_") + FPackageName::GetShortName(World.GetOutermost()->GetName());
			FString Name = Base;
			for (int32 Suffix = 2; FindPackage(nullptr, *(Folder + Name)) || FPackageName::DoesPackageExist(Folder + Name); ++Suffix)
			{
				Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
			}
			UPackage* Package = CreatePackage(*(Folder + Name));
			UMaterialInstanceConstant* Instance = Package ? NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional) : nullptr;
			if (!Instance)
			{
				OutError = TEXT("the clouds' material instance could not be made");
				return nullptr;
			}
			Instance->SetParentEditorOnly(Current);
			Instance->PostEditChange();
			FAssetRegistryModule::AssetCreated(Instance);
			Instance->MarkPackageDirty();
			// As the Details panel sets it: the component registers again around the edit and loads the new material.
			Edit(Cloud, FName(TEXT("Material")), [&] { Cloud.Material = Instance; });
			return Instance;
		}

		/** set_clouds material:<parameter>: a number, or "r,g,b[,a]" for a colour, on the clouds' own material instance. */
		bool CloudParameterOp(UWorld& World, AActor& Actor, const FString& Parameter, const FString& Value, const bool bWrite, FString& OutError)
		{
			UVolumetricCloudComponent* Cloud = Actor.FindComponentByClass<UVolumetricCloudComponent>();
			UMaterialInterface* Material = Cloud ? Cloud->Material.LoadSynchronous() : nullptr;
			if (!Material)
			{
				OutError = TEXT("the clouds have no material to set a parameter on");
				return false;
			}
			const FHashedMaterialParameterInfo Info{FName(*Parameter)};
			float Scalar = 0.f;
			FLinearColor Colour = FLinearColor::White;
			const bool bScalar = Material->GetScalarParameterValue(Info, Scalar);
			if (!bScalar && !Material->GetVectorParameterValue(Info, Colour))
			{
				OutError = FString::Printf(TEXT("material %s has no parameter '%s'; the engine's clouds take Cloud_GlobalCoverage, Cloud_GlobalDensity, Layout_CloudGlobalScale and Cloud_AlbedoColor"),
					*Material->GetName(), *Parameter.Left(64));
				return false;
			}
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","));
			if (bScalar ? !LexTryParseString(Scalar, *Value) || !FMath::IsFinite(Scalar)
				: (Parts.Num() < 3 || Parts.Num() > 4 || !LexTryParseString(Colour.R, *Parts[0].TrimStartAndEnd()) || !LexTryParseString(Colour.G, *Parts[1].TrimStartAndEnd())
					|| !LexTryParseString(Colour.B, *Parts[2].TrimStartAndEnd()) || (Parts.Num() == 4 && !LexTryParseString(Colour.A, *Parts[3].TrimStartAndEnd()))))
			{
				OutError = FString::Printf(TEXT("material:%s takes %s"), *Parameter, bScalar ? TEXT("a number") : TEXT("\"r,g,b\" or \"r,g,b,a\""));
				return false;
			}
			if (!bWrite)
			{
				return true;
			}
			UMaterialInstanceConstant* Instance = OwnCloudMaterial(*Cloud, World, OutError);
			if (!Instance)
			{
				return false;
			}
			Instance->Modify();
			if (bScalar)
			{
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(*Parameter)), Scalar);
			}
			else
			{
				Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(FName(*Parameter)), Colour);
			}
			Instance->PostEditChange();
			return true;
		}

		/** A setting beyond the fixed keys: the grade's LUT, a parameter of the clouds' material, or any Details-panel setting. */
		bool DetailOp(UWorld& World, AActor& Actor, const FParsedOp& Op, const bool bWrite, FString& OutError)
		{
			if (APostProcessVolume* Volume = Cast<APostProcessVolume>(&Actor); Volume && Plain(Op.DetailName) == TEXT("colorgradinglut"))
			{
				return LutOp(*Volume, Op.DetailValue, bWrite, OutError);
			}
			if (Actor.IsA<AVolumetricCloud>() && Op.DetailName.StartsWith(TEXT("material:")))
			{
				return CloudParameterOp(World, Actor, Op.DetailName.RightChop(9), Op.DetailValue, bWrite, OutError);
			}
			FDetails Details;
			FProperty* Property = nullptr;
			FString Import;
			bool bDefault = false;
			if (!ResolveDetail(Actor, Op, Details, Property, Import, bDefault, OutError))
			{
				return false;
			}
			if (bWrite)
			{
				WriteDetail(Details, *Property, Import, bDefault);
			}
			return true;
		}

		/** The meshes a set_actor_shadow or set_actor_lighting op names: all of the labelled actor's, or its one component by name. */
		bool ResolveShadowMeshes(UWorld& World, const FString& Label, const FParsedOp& Op, TArray<UMeshComponent*>& OutMeshes, FString& OutError)
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
			TArray<FString> Names;
			for (UMeshComponent* Mesh : Meshes)
			{
				Names.Add(Mesh->GetName());
				if (Op.ComponentName.IsEmpty() || Mesh->GetName() == Op.ComponentName) OutMeshes.Add(Mesh);
			}
			if (OutMeshes.IsEmpty())
			{
				OutError = Meshes.IsEmpty() ? FString::Printf(TEXT("'%s' has no mesh, so it neither casts shadows nor takes light."), *Label)
					: FString::Printf(TEXT("'%s' has no mesh component '%s'; it has %s."), *Label, *Op.ComponentName, *FString::Join(Names, TEXT(", ")));
				return false;
			}
			return true;
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

		uint8 ShapeOf(const AReflectionCapture& Capture)
		{
			return Capture.IsA<ABoxReflectionCapture>() ? BoxShape : Capture.IsA<ASphereReflectionCapture>() ? SphereShape : PlaneShape;
		}

		const TCHAR* ShapeName(const uint8 Shape)
		{
			return Shape == BoxShape ? TEXT("box") : Shape == SphereShape ? TEXT("sphere") : TEXT("plane");
		}

		/** Reflection captures in the level, in path order. */
		TArray<AReflectionCapture*> ReflectionCaptures(UWorld& World)
		{
			TArray<AReflectionCapture*> Captures;
			for (TActorIterator<AReflectionCapture> It(&World); It; ++It)
			{
				if (IsValid(*It) && !It->IsTemplate()) Captures.Add(*It);
			}
			Captures.Sort([](const AReflectionCapture& A, const AReflectionCapture& B) { return A.GetPathName() < B.GetPathName(); });
			return Captures;
		}

		AActor* SpawnCapture(UWorld& World, const uint8 Shape, const FVector& Location, const FString& Label)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			UClass* Class = Shape == BoxShape ? ABoxReflectionCapture::StaticClass() : ASphereReflectionCapture::StaticClass();
			AActor* Actor = World.SpawnActor(Class, &Location, nullptr, Params);
			if (!Actor) return nullptr;
			Actor->SetActorLabel(Label);
			Actor->Tags.AddUnique(AddedCaptureTag);
			// A box's scale is its half size in centimetres; the editor's own placement starts it at 10 by 10 by 4 metres.
			Actor->SetActorScale3D(Shape == BoxShape ? FVector(1000.0, 1000.0, 400.0) : FVector::OneVector);
			CastChecked<AReflectionCapture>(Actor)->GetCaptureComponent()->MarkDirtyForRecapture();
			return Actor;
		}

		void ReadCapture(const AReflectionCapture& Capture, TArray<FHyperAILightingProperty>& Out)
		{
			auto Add = [&Out](const TCHAR* Key, const FString& Value) { Out.Add({Key, Value}); };
			const UReflectionCaptureComponent* Component = Capture.GetCaptureComponent();
			const FVector Location = Capture.GetActorLocation();
			Add(TEXT("shape"), ShapeName(ShapeOf(Capture)));
			Add(TEXT("location"), FString::Printf(TEXT("%.1f,%.1f,%.1f"), Location.X, Location.Y, Location.Z));
			if (const USphereReflectionCaptureComponent* Sphere = Cast<USphereReflectionCaptureComponent>(Component))
			{
				Add(TEXT("InfluenceRadius"), Number(Sphere->InfluenceRadius));
			}
			if (const UBoxReflectionCaptureComponent* Box = Cast<UBoxReflectionCaptureComponent>(Component))
			{
				const FVector Extent = Capture.GetActorScale3D();
				Add(TEXT("box_extent"), FString::Printf(TEXT("%.1f,%.1f,%.1f"), Extent.X, Extent.Y, Extent.Z));
				Add(TEXT("BoxTransitionDistance"), Number(Box->BoxTransitionDistance));
			}
			if (Component)
			{
				Add(TEXT("Brightness"), Number(Component->Brightness));
				Add(TEXT("source"), Component->ReflectionSourceType == EReflectionSourceType::CapturedScene ? TEXT("captured_scene") : TEXT("specified_cubemap"));
			}
			if (Capture.Tags.Contains(AddedCaptureTag))
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
			TMap<FString, uint8> PlannedCaptures;
			// While validating, what an earlier set_actor_material puts in a slot, so a later parameter op is checked against it.
			TMap<TPair<UMeshComponent*, int32>, UMaterialInterface*> PlannedMaterials;
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
					PlannedMaterials.Add(TPair<UMeshComponent*, int32>(Mesh, Slot), Parsed.Material);
					if (bWrite)
					{
						// The Details panel's route, not a raw OverrideMaterials write: undo, render state and cleanup all follow.
						Edit(*Mesh, FName(TEXT("OverrideMaterials")), [&] { Mesh->SetMaterial(Slot, Parsed.Material); });
						OutTouched.AddUnique(Mesh->GetOwner());
					}
					continue;
				}
				if (Parsed.Kind == TEXT("material_parameter"))
				{
					UMeshComponent* Mesh = nullptr;
					int32 Slot = INDEX_NONE;
					if (!ResolveMaterialSlot(World, Op.ActorLabel, Parsed, Mesh, Slot, Error))
					{
						OutError = FString::Printf(TEXT("Op %d: %s"), Index, *Error);
						return false;
					}
					UMaterialInterface* const* Planned = bWrite ? nullptr : PlannedMaterials.Find(TPair<UMeshComponent*, int32>(Mesh, Slot));
					if (!LookDev::SetParameter(World, *Mesh, Slot, Parsed.bActorOnly, Parsed.ParameterName, Parsed.DetailValue, Planned ? *Planned : nullptr, bWrite,
						OutTouched, Error))
					{
						OutError = FString::Printf(TEXT("Op %d: %s"), Index, *Error);
						return false;
					}
					continue;
				}
				if (Parsed.Kind == TEXT("actor_lighting"))
				{
					TArray<UMeshComponent*> Meshes;
					if (!ResolveShadowMeshes(World, Op.ActorLabel, Parsed, Meshes, Error))
					{
						OutError = FString::Printf(TEXT("Op %d: %s"), Index, *Error);
						return false;
					}
					if (bWrite)
					{
						FBoolProperty* Flag = FindFProperty<FBoolProperty>(UPrimitiveComponent::StaticClass(), Parsed.ActorLighting->Property);
						for (UMeshComponent* Mesh : Meshes)
						{
							// The Details panel's route: undo, then the mesh registers again with the new flag.
							Edit(*Mesh, Flag->GetFName(), [&] { Flag->SetPropertyValue_InContainer(Mesh, Parsed.bFlag); });
						}
						OutTouched.AddUnique(Meshes[0]->GetOwner());
					}
					continue;
				}
				if (Parsed.Kind == TEXT("add_reflection_capture"))
				{
					if (PlannedCaptures.Contains(Op.ActorLabel) || PlannedLights.Contains(Op.ActorLabel) || !Labelled(World, Op.ActorLabel).IsEmpty())
					{
						OutError = FString::Printf(TEXT("Op %d: an actor is already labelled '%s'; give the new capture its own label."), Index, *Op.ActorLabel);
						return false;
					}
					PlannedCaptures.Add(Op.ActorLabel, Parsed.CaptureShape);
					if (bWrite)
					{
						AActor* Added = SpawnCapture(World, Parsed.CaptureShape, Parsed.Vector, Op.ActorLabel);
						if (!Added)
						{
							OutError = FString::Printf(TEXT("Op %d: the level refused a new %s reflection capture."), Index, ShapeName(Parsed.CaptureShape));
							return false;
						}
						OutTouched.AddUnique(Added);
					}
					continue;
				}
				if (Parsed.Kind == TEXT("reflection_capture"))
				{
					const TArray<AActor*> Found = Labelled(World, Op.ActorLabel);
					AReflectionCapture* Capture = Found.Num() == 1 ? Cast<AReflectionCapture>(Found[0]) : nullptr;
					const uint8* PlannedShape = Found.IsEmpty() ? PlannedCaptures.Find(Op.ActorLabel) : nullptr;
					if (!Capture && !PlannedShape)
					{
						OutError = FString::Printf(TEXT("Op %d: %s"), Index, Found.Num() > 1
							? *FString::Printf(TEXT("%d actors are labelled '%s'; give the capture a unique label first."), Found.Num(), *Op.ActorLabel)
							: *FString::Printf(TEXT("no reflection capture is labelled '%s'; hyper_lighting_inspect lists them, and add_reflection_capture places one."), *Op.ActorLabel));
						return false;
					}
					const uint8 Shape = Capture ? ShapeOf(*Capture) : *PlannedShape;
					if (Parsed.CaptureKey == TEXT("box_extent") && Shape != BoxShape)
					{
						OutError = FString::Printf(TEXT("Op %d: '%s' is a %s capture, and box_extent sizes only a box; a sphere's reach is InfluenceRadius."), Index, *Op.ActorLabel, ShapeName(Shape));
						return false;
					}
					if (!Parsed.DetailName.IsEmpty())
					{
						// Checked on the capture, or, before the plan adds it, on its class's default, which has the same settings.
						AActor& Target = Capture ? static_cast<AActor&>(*Capture) : Shape == BoxShape ? static_cast<AActor&>(*GetMutableDefault<ABoxReflectionCapture>())
							: static_cast<AActor&>(*GetMutableDefault<ASphereReflectionCapture>());
						if (!DetailOp(World, Target, Parsed, bWrite, Error))
						{
							OutError = FString::Printf(TEXT("Op %d: set_reflection_capture %s."), Index, *Error);
							return false;
						}
					}
					if (bWrite)
					{
						if (!Capture)
						{
							OutError = FString::Printf(TEXT("Op %d: '%s' was not there to change."), Index, *Op.ActorLabel);
							return false;
						}
						if (Parsed.DetailName.IsEmpty())
						{
							Capture->Modify();
							if (Parsed.CaptureKey == TEXT("location"))
							{
								Capture->SetActorLocation(Parsed.Vector);
							}
							else
							{
								Capture->SetActorScale3D(Parsed.Vector);
							}
							Capture->PostEditMove(true);
						}
						// Captured again on the next editor tick, so the next capture of the view shows it.
						Capture->GetCaptureComponent()->MarkDirtyForRecapture();
						OutTouched.AddUnique(Capture);
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
				if (Parsed.Kind == TEXT("actor_shadow"))
				{
					TArray<UMeshComponent*> Meshes;
					if (!ResolveShadowMeshes(World, Op.ActorLabel, Parsed, Meshes, Error))
					{
						OutError = FString::Printf(TEXT("Op %d: %s"), Index, *Error);
						return false;
					}
					if (bWrite)
					{
						for (UMeshComponent* Mesh : Meshes)
						{
							Edit(*Mesh, FName(TEXT("CastShadow")), [&] { Mesh->SetCastShadow(Parsed.bFlag); });
						}
						OutTouched.AddUnique(Meshes[0]->GetOwner());
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
				if (IsSkyOwnedKind(Parsed.Kind))
				{
					if (const AActor* Owner = LightOwningSkyActor(World))
					{
						const FSkySystem* System = SkySystemOf(Owner->GetClass());
						OutError = FString::Printf(TEXT("Op %d: this level's sky is %s ('%s'), which owns its sun, sky light, fog, atmosphere and clouds, so %s would fight it or add a second one. Use set_sky_system instead: %s."),
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
				if (!Parsed.DetailName.IsEmpty())
				{
					// Checked on the actor, or, before the plan adds it, on its class's default, which has the same settings.
					AActor& Target = Actor ? *Actor : *ClassOf(Parsed.Kind)->GetDefaultObject<AActor>();
					if (!DetailOp(World, Target, Parsed, bWrite, Error))
					{
						OutError = FString::Printf(TEXT("Op %d: %s %s."), Index, *Op.Kind, *Error);
						return false;
					}
					if (bWrite)
					{
						OutTouched.AddUnique(Actor);
					}
					continue;
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
		const TArray<AReflectionCapture*> Captures = ReflectionCaptures(World);
		for (int32 Index = 0; Index < FMath::Min(Captures.Num(), MaxActorsPerKind); ++Index)
		{
			FHyperAILightingActorRecord& Record = Records.AddDefaulted_GetRef();
			Record.Kind = TEXT("reflection_capture");
			Record.Label = Captures[Index]->GetActorLabel();
			Record.Path = Captures[Index]->GetPathName();
			ReadCapture(*Captures[Index], Record.Properties);
		}
		return Records;
	}

	TArray<FHyperAILightingProperty> ReadRendering(UWorld& World)
	{
		auto Setting = [](const TCHAR* Name)
		{
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			return Variable ? Variable->GetInt() : 0;
		};
		auto Flag = [](const bool bValue) { return bValue ? FString(TEXT("true")) : FString(TEXT("false")); };
		int32 Gi = Setting(TEXT("r.DynamicGlobalIlluminationMethod"));
		int32 Reflections = Setting(TEXT("r.ReflectionMethod"));
		float MaxRoughness = DefaultPostProcess().LumenMaxRoughnessToTraceReflections;
		bool bFrontLayer = false;
		// The level's unbound volumes override the project, the highest priority last.
		TArray<AActor*> Volumes = ActorsOfKind(World, TEXT("post_process"));
		Volumes.StableSort([](const AActor& A, const AActor& B) { return CastChecked<APostProcessVolume>(&A)->Priority < CastChecked<APostProcessVolume>(&B)->Priority; });
		for (const AActor* Actor : Volumes)
		{
			const APostProcessVolume& Volume = *CastChecked<APostProcessVolume>(Actor);
			if (!Volume.bEnabled || Volume.BlendWeight <= 0.f) continue;
			const FPostProcessSettings& S = Volume.Settings;
			Gi = S.bOverride_DynamicGlobalIlluminationMethod ? static_cast<int32>(S.DynamicGlobalIlluminationMethod) : Gi;
			Reflections = S.bOverride_ReflectionMethod ? static_cast<int32>(S.ReflectionMethod) : Reflections;
			MaxRoughness = S.bOverride_LumenMaxRoughnessToTraceReflections ? S.LumenMaxRoughnessToTraceReflections : MaxRoughness;
			bFrontLayer = S.bOverride_LumenFrontLayerTranslucencyReflections ? static_cast<bool>(S.LumenFrontLayerTranslucencyReflections) : bFrontLayer;
		}
		const TCHAR* const GiNames[] = {TEXT("none"), TEXT("lumen"), TEXT("screen_space"), TEXT("plugin")};
		const TCHAR* const ReflectionNames[] = {TEXT("none"), TEXT("lumen"), TEXT("screen_space")};
		const bool bHardware = Setting(TEXT("r.Lumen.HardwareRayTracing")) != 0 && IsRayTracingEnabled();
		const int32 Captures = ReflectionCaptures(World).Num();
		TArray<FHyperAILightingProperty> Out;
		Out.Add({TEXT("gi_method"), GiNames[FMath::Clamp(Gi, 0, 3)]});
		Out.Add({TEXT("reflection_method"), ReflectionNames[FMath::Clamp(Reflections, 0, 2)]});
		Out.Add({TEXT("lumen_tracing"), bHardware ? TEXT("hardware") : TEXT("software")});
		Out.Add({TEXT("lumen_max_roughness_to_trace"), Number(MaxRoughness)});
		Out.Add({TEXT("mesh_distance_fields"), Flag(Setting(TEXT("r.GenerateMeshDistanceFields")) != 0)});
		Out.Add({TEXT("substrate"), Flag(Setting(TEXT("r.Substrate")) != 0)});
		Out.Add({TEXT("virtual_shadow_maps"), Flag(Setting(TEXT("r.Shadow.Virtual.Enable")) != 0)});
		Out.Add({TEXT("megalights"), Flag(Setting(TEXT("r.MegaLights.EnableForProject")) != 0)});
		Out.Add({TEXT("static_lighting"), Flag(Setting(TEXT("r.AllowStaticLighting")) != 0)});
		Out.Add({TEXT("translucency_front_layer_reflections"), Flag(Setting(TEXT("r.Lumen.TranslucencyReflections.FrontLayer.EnableForProject")) != 0 || bFrontLayer)});
		Out.Add({TEXT("reflection_captures"), FString::FromInt(Captures)});
		// What that means for a match, in a lighting artist's words.
		if (Reflections == 1)
		{
			Out.Add({TEXT("note"), FString::Printf(TEXT("Lumen reflections: surfaces smoother than roughness %s reflect their surroundings, rougher ones a blur; reflection captures add little. %s"),
				*Number(MaxRoughness), bHardware ? TEXT("Hardware tracing reflects detail from off screen too.")
					: TEXT("Software tracing reflects what is off screen only coarsely, from distance fields: a mirror shows sharp detail only of what is on screen."))});
		}
		else
		{
			Out.Add({TEXT("note"), Captures == 0
				? FString(TEXT("No Lumen reflections and no reflection captures: shiny surfaces reflect only the sky and what is on screen. add_reflection_capture fills a room or street."))
				: FString(TEXT("No Lumen reflections: reflection captures and screen-space reflections give surfaces their reflections, so place captures where reflective surfaces are."))});
		}
		if (Gi == 1 && Setting(TEXT("r.GenerateMeshDistanceFields")) == 0 && !bHardware)
		{
			Out.Add({TEXT("note"), TEXT("Software Lumen without mesh distance fields traces almost nothing: bounce light and reflections fall back to the sky.")});
		}
		return Out;
	}

	TArray<FHyperAILightingMaterialRecord> ReadMaterials(UWorld& World, const FString& CameraLabel, const int32 Limit, int32& OutTotal)
	{
		OutTotal = 0;
		const FLightView View = FindView(World, CameraLabel);
		if (!View.bValid || Limit <= 0)
		{
			return {};
		}
		bool bLumenReflections = false;
		float MaxRoughness = DefaultPostProcess().LumenMaxRoughnessToTraceReflections;
		for (const FHyperAILightingProperty& Fact : ReadRendering(World))
		{
			bLumenReflections = Fact.Key == TEXT("reflection_method") ? Fact.Value == TEXT("lumen") : bLumenReflections;
			MaxRoughness = Fact.Key == TEXT("lumen_max_roughness_to_trace") ? FCString::Atof(*Fact.Value) : MaxRoughness;
		}
		return LookDev::ReadMaterials(World, View.Origin, [&View](const FVector& Center, const double Radius) { return InView(View, Center, Radius); },
			Limit, bLumenReflections, MaxRoughness, OutTotal);
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
		// The surfaces a plan tuned: the level's own material instances and what they hold, as a hash so they add little.
		Canonical += TEXT("look_dev=") + FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(LookDev::RevisionText(World)) + TEXT("\n");
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
			if (Parsed.Kind == TEXT("material_parameter"))
			{
				if (!ResolveMaterialSlot(World, Op.ActorLabel, Parsed, Mesh, Slot, Error)) continue;
				Actors.AddUnique(Mesh->GetOwner());
				// A level-wide change reaches every actor showing the instance; those it changed are unsaved, the rest saved before.
				if (const UMaterialInstanceConstant* Instance = LookDev::InstanceFor(World, *Mesh, Slot, Parsed.bActorOnly))
				{
					for (AActor* User : LookDev::UnsavedUsers(World, *Instance)) Actors.AddUnique(User);
				}
				continue;
			}
			if (Parsed.Kind == TEXT("light") || Parsed.Kind == TEXT("add_light") || Parsed.Kind == TEXT("actor_shadow") || Parsed.Kind == TEXT("actor_lighting")
				|| Parsed.Kind == TEXT("add_reflection_capture") || Parsed.Kind == TEXT("reflection_capture"))
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
		// The last value a plan gave each instance's parameter, as the op wrote it.
		struct FParameterCheck
		{
			UMeshComponent* Mesh = nullptr;
			int32 Slot = INDEX_NONE;
			bool bActorOnly = false;
			FString Value;
		};
		TMap<TPair<UMaterialInstanceConstant*, FString>, FParameterCheck> Parameters;
		for (const FHyperAILightingOp& Op : Ops)
		{
			FParsedOp Parsed;
			UMeshComponent* Mesh = nullptr;
			int32 Slot = INDEX_NONE;
			if (!ParseOp(Op, Parsed, OutError) || (Parsed.Kind != TEXT("actor_material") && Parsed.Kind != TEXT("material_parameter"))) continue;
			if (!ResolveMaterialSlot(World, Op.ActorLabel, Parsed, Mesh, Slot, OutError)) return false;
			if (Parsed.Kind == TEXT("actor_material"))
			{
				Expected.Add(TPair<UMeshComponent*, int32>(Mesh, Slot), Parsed.Material);
				continue;
			}
			UMaterialInstanceConstant* Instance = LookDev::InstanceFor(World, *Mesh, Slot, Parsed.bActorOnly);
			Parameters.Add(TPair<UMaterialInstanceConstant*, FString>(Instance, Parsed.ParameterName.ToLower()),
				FParameterCheck{Mesh, Slot, Parsed.bActorOnly, Parsed.DetailValue});
		}
		for (const TPair<TPair<UMeshComponent*, int32>, UMaterialInterface*>& Entry : Expected)
		{
			if (Entry.Key.Key->GetMaterial(Entry.Key.Value) != Entry.Value)
			{
				OutError = FString::Printf(TEXT("%s slot %d does not hold %s."), *Entry.Key.Key->GetPathName(), Entry.Key.Value, *Entry.Value->GetPathName());
				return false;
			}
		}
		for (const TPair<TPair<UMaterialInstanceConstant*, FString>, FParameterCheck>& Entry : Parameters)
		{
			if (!LookDev::Holds(Entry.Key.Key, Entry.Key.Value, Entry.Value.Value))
			{
				OutError = FString::Printf(TEXT("%s slot %d shows no look-dev instance holding %s = %s."), *Entry.Value.Mesh->GetPathName(), Entry.Value.Slot,
					*Entry.Key.Value, *Entry.Value.Value);
				return false;
			}
		}
		OutError.Reset();
		return true;
	}

	TArray<UPackage*> PackagesOf(const TArray<AActor*>& Actors)
	{
		TArray<UPackage*> Packages;
		// An asset a plan made for the level, still unsaved: saved with it, or the level would name an asset that is not there.
		auto AddMade = [&Packages](const UObject* Asset)
		{
			UPackage* Package = Asset ? Asset->GetPackage() : nullptr;
			if (Package && Package->IsDirty() && Package->GetName().StartsWith(MadeAssetsRoot)) Packages.AddUnique(Package);
		};
		for (const AActor* Actor : Actors)
		{
			// An actor's package is its own external package under One File Per Actor, else the level's.
			if (Actor && Actor->GetPackage()) Packages.AddUnique(Actor->GetPackage());
			if (const APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor))
			{
				AddMade(Volume->Settings.ColorGradingLUT.Get());
			}
			if (const UVolumetricCloudComponent* Cloud = Actor && Actor->IsA<AVolumetricCloud>() ? Actor->FindComponentByClass<UVolumetricCloudComponent>() : nullptr)
			{
				AddMade(Cloud->Material.Get());
			}
			if (Actor)
			{
				LookDev::AddUnsaved(*Actor, Packages);
			}
		}
		return Packages;
	}

	bool ReadLevelLut(UWorld& World, Lut::FTable& OutTable, FString& OutPath)
	{
		OutTable = Lut::Identity();
		OutPath.Reset();
		for (AActor* Actor : ActorsOfKind(World, TEXT("post_process")))
		{
			const APostProcessVolume* Volume = CastChecked<APostProcessVolume>(Actor);
			const FPostProcessSettings& S = Volume->Settings;
			UTexture2D* Texture = Cast<UTexture2D>(S.ColorGradingLUT.Get());
			FImage Image;
			if (!Volume->bEnabled || Volume->BlendWeight <= 0.f || !S.bOverride_ColorGradingLUT || !Texture || !Texture->Source.IsValid()
				|| !Texture->Source.GetMipImage(Image, 0))
			{
				continue;
			}
			// The bytes as stored: a LUT keeps sRGB values in a texture that does not decode them.
			FImage Stored;
			Image.CopyTo(Stored, ERawImageFormat::BGRA8, EGammaSpace::Linear);
			const TArrayView64<FColor> Pixels = Stored.AsBGRA8();
			Lut::FTable Table;
			if (!Lut::FromStrip(TConstArrayView<FColor>(Pixels.GetData(), static_cast<int32>(Pixels.Num())), Stored.SizeX, Stored.SizeY, Table))
			{
				continue;
			}
			OutTable = Lut::Weaken(Table, S.bOverride_ColorGradingIntensity ? S.ColorGradingIntensity : 1.f);
			OutPath = Texture->GetPathName();
			return true;
		}
		return false;
	}
}

#undef LOCTEXT_NAMESPACE
