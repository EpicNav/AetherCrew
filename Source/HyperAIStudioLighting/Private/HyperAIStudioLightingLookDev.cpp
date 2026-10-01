// Games by Hyper 2026.

#include "HyperAIStudioLightingLookDev.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/MeshComponent.h"
#include "Engine/Texture.h"
#include "GameFramework/Actor.h"
#include "HyperAIStudioLightingGate.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameters.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

namespace HyperAIStudio::Lighting::LookDev
{
	namespace
	{
		constexpr int32 MaxHops = 16;
		constexpr int32 MaxListedParameters = 24;
		constexpr int32 MaxNamedParameters = 40;
		constexpr int32 MaxNotes = 3;
		constexpr int32 MaxActorsListed = 3;
		constexpr int32 MaxNameSuffix = 64;

		enum class EParameter : uint8
		{
			None,
			Scalar,
			Vector,
			Texture,
			Switch
		};

		const TPair<EMaterialParameterType, EParameter> ParameterTypes[] = {
			{EMaterialParameterType::Scalar, EParameter::Scalar},
			{EMaterialParameterType::Vector, EParameter::Vector},
			{EMaterialParameterType::Texture, EParameter::Texture},
			{EMaterialParameterType::StaticSwitch, EParameter::Switch}};

		struct FValue
		{
			bool bDefault = false;
			float Scalar = 0.f;
			FLinearColor Colour = FLinearColor::Black;
			/** The colour gave its alpha; otherwise the instance keeps the one it has. */
			bool bAlpha = false;
			UTexture* Texture = nullptr;
		};

		/** Letters, digits and underscores: a name an asset can take. */
		FString Safe(const FString& Name)
		{
			FString Out;
			for (const TCHAR Character : Name)
			{
				Out.AppendChar(FChar::IsAlnum(Character) && Character < 128 ? Character : TEXT('_'));
			}
			return Out.Left(64);
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

		bool IsUnder(const UObject* Object, const FString& Folder)
		{
			if (!Object)
			{
				return false;
			}
			const FNameBuilder Package(Object->GetOutermost()->GetFName());
			return Package.ToView().StartsWith(Folder);
		}

		bool IsActorInstance(const UMaterialInterface* Material, const FString& Folder)
		{
			return Material && Material->IsA<UMaterialInstanceConstant>() && IsUnder(Material, Folder + TEXT("Actors/"));
		}

		bool IsLevelInstance(const UMaterialInterface* Material, const FString& Folder)
		{
			return Material && Material->IsA<UMaterialInstanceConstant>() && IsUnder(Material, Folder) && !IsActorInstance(Material, Folder);
		}

		UMaterialInterface* ParentOf(const UMaterialInterface* Material)
		{
			const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material);
			return Instance ? Instance->Parent.Get() : nullptr;
		}

		/** What a slot's material is made from, past this level's look-dev instances. */
		UMaterialInterface* SourceOf(UMaterialInterface* Material, const FString& Folder)
		{
			for (int32 Hop = 0; Hop < MaxHops && Material && Material->IsA<UMaterialInstanceConstant>() && IsUnder(Material, Folder); ++Hop)
			{
				Material = ParentOf(Material);
			}
			return Material;
		}

		/** Whether Material is Instance, or made from it. */
		bool MadeFrom(const UMaterialInterface* Material, const UMaterialInterface& Instance)
		{
			for (int32 Hop = 0; Hop < MaxHops && Material; ++Hop)
			{
				if (Material == &Instance) return true;
				Material = ParentOf(Material);
			}
			return false;
		}

		/** Every mesh of an actor placed in the level. */
		void ForEachLevelMesh(const UWorld& World, TFunctionRef<void(UMeshComponent&, AActor&)> Visit)
		{
			for (TObjectIterator<UMeshComponent> It; It; ++It)
			{
				UMeshComponent* Mesh = *It;
				if (!IsValid(Mesh) || Mesh->IsTemplate() || !Mesh->IsRegistered() || Mesh->GetWorld() != &World) continue;
				AActor* Owner = Mesh->GetOwner();
				if (IsValid(Owner) && !Owner->IsTemplate()) Visit(*Mesh, *Owner);
			}
		}

		/** Meshes that keep a material given to them: foliage, PCG and construction scripts make theirs again from their own settings. */
		bool IsReassignable(const UMeshComponent& Mesh, const AActor& Owner)
		{
			static const FName PcgTag(TEXT("PCG Generated Component"));
			static const FName FoliageActor(TEXT("InstancedFoliageActor"));
			if (Mesh.CreationMethod == EComponentCreationMethod::UserConstructionScript || Mesh.ComponentHasTag(PcgTag))
			{
				return false;
			}
			for (const UClass* Class = Owner.GetClass(); Class; Class = Class->GetSuperClass())
			{
				if (Class->GetFName() == FoliageActor) return false;
			}
			return true;
		}

		/** As the Details panel gives a slot a material: PreEditChange records undo, PostEditChangeProperty redraws it. */
		void Assign(UMeshComponent& Mesh, const int32 Slot, UMaterialInterface* Material)
		{
			FProperty* Property = FindFProperty<FProperty>(UMeshComponent::StaticClass(), TEXT("OverrideMaterials"));
			Mesh.PreEditChange(Property);
			Mesh.SetMaterial(Slot, Material);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Mesh.PostEditChangeProperty(Event);
		}

		/** A parameter of Material by name, in any case, then however it is spaced: its type, and its own name. */
		EParameter FindParameter(const UMaterialInterface& Material, const FString& Name, FName& OutName)
		{
			const FString Wanted = Plain(Name);
			EParameter Loose = EParameter::None;
			FName LooseName;
			int32 LooseMatches = 0;
			for (const TPair<EMaterialParameterType, EParameter>& Type : ParameterTypes)
			{
				TArray<FMaterialParameterInfo> Infos;
				TArray<FGuid> Ids;
				Material.GetAllParameterInfoOfType(Type.Key, Infos, Ids);
				for (const FMaterialParameterInfo& Info : Infos)
				{
					if (Info.Association != EMaterialParameterAssociation::GlobalParameter) continue;
					const FString Own = Info.Name.ToString();
					if (Own.Equals(Name, ESearchCase::IgnoreCase))
					{
						OutName = Info.Name;
						return Type.Value;
					}
					if (!Wanted.IsEmpty() && Plain(Own) == Wanted)
					{
						Loose = Type.Value;
						LooseName = Info.Name;
						++LooseMatches;
					}
				}
			}
			if (LooseMatches == 1)
			{
				OutName = LooseName;
				return Loose;
			}
			return EParameter::None;
		}

		FString ParameterNames(const UMaterialInterface& Material)
		{
			TArray<FString> Names;
			for (const TPair<EMaterialParameterType, EParameter>& Type : ParameterTypes)
			{
				if (Type.Value == EParameter::Switch) continue;
				TArray<FMaterialParameterInfo> Infos;
				TArray<FGuid> Ids;
				Material.GetAllParameterInfoOfType(Type.Key, Infos, Ids);
				for (const FMaterialParameterInfo& Info : Infos)
				{
					if (Info.Association == EMaterialParameterAssociation::GlobalParameter) Names.AddUnique(Info.Name.ToString());
				}
			}
			Names.Sort();
			const int32 More = Names.Num() - MaxNamedParameters;
			if (More > 0)
			{
				Names.SetNum(MaxNamedParameters);
				Names.Add(FString::Printf(TEXT("and %d more"), More));
			}
			return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
		}

		bool ParseValue(const EParameter Type, const FString& Text, FValue& Out, FString& OutError)
		{
			const FString Value = Text.TrimStartAndEnd();
			if (Value.Equals(TEXT("default"), ESearchCase::IgnoreCase))
			{
				Out.bDefault = true;
				return true;
			}
			if (Type == EParameter::Scalar)
			{
				if (LexTryParseString(Out.Scalar, *Value) && FMath::IsFinite(Out.Scalar) && FMath::Abs(Out.Scalar) <= 1000000.f)
				{
					return true;
				}
				OutError = TEXT("takes a number, or default");
				return false;
			}
			if (Type == EParameter::Vector)
			{
				TArray<FString> Parts;
				Value.ParseIntoArray(Parts, TEXT(","));
				float Channels[4] = {0.f, 0.f, 0.f, 1.f};
				bool bOk = Parts.Num() == 3 || Parts.Num() == 4;
				for (int32 Index = 0; bOk && Index < Parts.Num(); ++Index)
				{
					bOk = LexTryParseString(Channels[Index], *Parts[Index].TrimStartAndEnd()) && FMath::IsFinite(Channels[Index]) && FMath::Abs(Channels[Index]) <= 100000.f;
				}
				if (!bOk)
				{
					OutError = TEXT("takes a linear colour \"r,g,b\" or \"r,g,b,a\", or default");
					return false;
				}
				Out.Colour = FLinearColor(Channels[0], Channels[1], Channels[2], Channels[3]);
				Out.bAlpha = Parts.Num() == 4;
				return true;
			}
			FString Path = Value;
			if (Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT(".")))
			{
				Path += TEXT(".") + FPackageName::GetShortName(Path);
			}
			Out.Texture = Path.StartsWith(TEXT("/")) ? Cast<UTexture>(FSoftObjectPath(Path).TryLoad()) : nullptr;
			if (!Out.Texture)
			{
				OutError = FString::Printf(TEXT("takes a texture's path, or default; '%s' is not a texture"), *Value.Left(128));
				return false;
			}
			return true;
		}

		/** Whether an instance sets the parameter itself, rather than taking its parent's value. */
		template <typename TValue>
		bool IsOwn(const TValue& Entry, const FName Name)
		{
			return Entry.ParameterInfo.Name == Name && Entry.ParameterInfo.Association == EMaterialParameterAssociation::GlobalParameter;
		}

		void Write(UMaterialInstanceConstant& Instance, const EParameter Type, const FName Name, const FValue& Value)
		{
			const FMaterialParameterInfo Info(Name);
			if (Value.bDefault)
			{
				Instance.ScalarParameterValues.RemoveAll([Name](const FScalarParameterValue& Entry) { return IsOwn(Entry, Name); });
				Instance.VectorParameterValues.RemoveAll([Name](const FVectorParameterValue& Entry) { return IsOwn(Entry, Name); });
				Instance.TextureParameterValues.RemoveAll([Name](const FTextureParameterValue& Entry) { return IsOwn(Entry, Name); });
				return;
			}
			if (Type == EParameter::Scalar)
			{
				Instance.SetScalarParameterValueEditorOnly(Info, Value.Scalar);
			}
			else if (Type == EParameter::Vector)
			{
				FLinearColor Colour = Value.Colour;
				FLinearColor Now = FLinearColor::White;
				if (!Value.bAlpha && Instance.GetVectorParameterValue(FHashedMaterialParameterInfo(Name), Now))
				{
					Colour.A = Now.A;
				}
				Instance.SetVectorParameterValueEditorOnly(Info, Colour);
			}
			else
			{
				Instance.SetTextureParameterValueEditorOnly(Info, Value.Texture);
			}
		}

		UMaterialInstanceConstant* MakeInstance(const FString& Folder, const FString& Name, UMaterialInterface& Parent, FString& OutError)
		{
			UPackage* Package = CreatePackage(*(Folder + Name));
			UMaterialInstanceConstant* Instance = Package ? NewObject<UMaterialInstanceConstant>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional) : nullptr;
			if (!Instance)
			{
				OutError = FString::Printf(TEXT("the material instance %s%s could not be made"), *Folder, *Name);
				return nullptr;
			}
			Instance->SetParentEditorOnly(&Parent);
			Instance->PostEditChange();
			FAssetRegistryModule::AssetCreated(Instance);
			Instance->MarkPackageDirty();
			return Instance;
		}

		/** Whether a package name is free: nothing loaded holds it, and no file does. */
		bool IsFree(const FString& PackageName)
		{
			return !FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName);
		}

		/**
		 * The level's instance of Source, by its name: MI_<source>, or MI_<source>_2 and on where another material's took the
		 * name first. Null with OutFreeName set to the first free name when there is none.
		 */
		UMaterialInstanceConstant* FindLevelInstance(const FString& Folder, const UMaterialInterface& Source, FString* OutFreeName = nullptr)
		{
			const FString Base = TEXT("MI_") + Safe(Source.GetName());
			for (int32 Suffix = 1; Suffix <= MaxNameSuffix; ++Suffix)
			{
				const FString Name = Suffix == 1 ? Base : FString::Printf(TEXT("%s_%d"), *Base, Suffix);
				const FString PackageName = Folder + Name;
				if (IsFree(PackageName))
				{
					if (OutFreeName) *OutFreeName = Name;
					return nullptr;
				}
				const FString Path = PackageName + TEXT(".") + Name;
				UMaterialInstanceConstant* Existing = FindObject<UMaterialInstanceConstant>(nullptr, *Path);
				if (!Existing && FPackageName::DoesPackageExist(PackageName))
				{
					Existing = LoadObject<UMaterialInstanceConstant>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
				}
				if (IsValid(Existing) && Existing->Parent == &Source)
				{
					return Existing;
				}
			}
			return nullptr;
		}

		/** The level's instance of the material a slot shows: found, or made; every slot in the level showing the material shows it. */
		UMaterialInstanceConstant* OwnLevelInstance(UWorld& World, UMaterialInterface& Current, TArray<AActor*>& OutTouched, FString& OutError)
		{
			const FString Folder = FolderFor(World);
			if (IsLevelInstance(&Current, Folder))
			{
				return CastChecked<UMaterialInstanceConstant>(&Current);
			}
			UMaterialInterface* Source = SourceOf(&Current, Folder);
			if (!Source)
			{
				OutError = FString::Printf(TEXT("%s is made from no material"), *Current.GetName());
				return nullptr;
			}
			FString FreeName;
			UMaterialInstanceConstant* Instance = FindLevelInstance(Folder, *Source, &FreeName);
			if (!Instance)
			{
				if (FreeName.IsEmpty())
				{
					OutError = FString::Printf(TEXT("there is no free name for the level's instance of %s under %s"), *Source->GetName(), *Folder);
					return nullptr;
				}
				Instance = MakeInstance(Folder, FreeName, *Source, OutError);
				if (!Instance)
				{
					return nullptr;
				}
			}
			// Modular pieces stay alike: every slot showing the material shows the instance, and an actor's own instance of
			// the material is made from it, so it keeps what it changed and takes the rest from the level's.
			ForEachLevelMesh(World, [&](UMeshComponent& Mesh, AActor& Owner)
			{
				if (!IsReassignable(Mesh, Owner)) return;
				for (int32 Index = 0; Index < Mesh.GetNumMaterials(); ++Index)
				{
					UMaterialInterface* Shown = Mesh.GetMaterial(Index);
					if (Shown == Source)
					{
						Assign(Mesh, Index, Instance);
						OutTouched.AddUnique(&Owner);
					}
					else if (IsActorInstance(Shown, Folder) && ParentOf(Shown) == Source)
					{
						UMaterialInstanceConstant* Own = CastChecked<UMaterialInstanceConstant>(Shown);
						Own->Modify();
						Own->SetParentEditorOnly(Instance);
						Own->PostEditChange();
						OutTouched.AddUnique(&Owner);
					}
				}
			});
			return Instance;
		}

		/** One actor's own instance for a slot: found, or made from what the slot shows and given to that slot alone. */
		UMaterialInstanceConstant* OwnActorInstance(UWorld& World, UMeshComponent& Mesh, const int32 Slot, UMaterialInterface& Current,
			TArray<AActor*>& OutTouched, FString& OutError)
		{
			const FString Folder = FolderFor(World);
			if (IsActorInstance(&Current, Folder))
			{
				return CastChecked<UMaterialInstanceConstant>(&Current);
			}
			AActor* Owner = Mesh.GetOwner();
			const FString Actors = Folder + TEXT("Actors/");
			const FString Base = FString::Printf(TEXT("MI_%s_%d"), *Safe(Owner->GetActorLabel()), Slot);
			FString Name = Base;
			for (int32 Suffix = 2; !IsFree(Actors + Name); ++Suffix)
			{
				if (Suffix > MaxNameSuffix)
				{
					OutError = FString::Printf(TEXT("there is no free name for an instance of %s under %s"), *Base, *Actors);
					return nullptr;
				}
				Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
			}
			UMaterialInstanceConstant* Instance = MakeInstance(Actors, Name, Current, OutError);
			if (Instance)
			{
				Assign(Mesh, Slot, Instance);
				OutTouched.AddUnique(Owner);
			}
			return Instance;
		}

		FString Snake(const FString& Name)
		{
			FString Out;
			for (int32 Index = 0; Index < Name.Len(); ++Index)
			{
				if (FChar::IsUpper(Name[Index]) && Index > 0 && !FChar::IsUpper(Name[Index - 1]))
				{
					Out.AppendChar(TEXT('_'));
				}
				Out.AppendChar(FChar::ToLower(Name[Index]));
			}
			return Out;
		}

		FString BlendName(const EBlendMode Mode)
		{
			FString Name = StaticEnum<EBlendMode>()->GetNameStringByValue(Mode);
			Name.RemoveFromStart(TEXT("BLEND_"));
			return Snake(Name);
		}

		FString ShadingName(const FMaterialShadingModelField Models)
		{
			TArray<FString> Names;
			for (int32 Index = 0; Index < MSM_NUM; ++Index)
			{
				const EMaterialShadingModel Model = static_cast<EMaterialShadingModel>(Index);
				if (!Models.HasShadingModel(Model)) continue;
				FString Name = StaticEnum<EMaterialShadingModel>()->GetNameStringByValue(Model);
				Name.RemoveFromStart(TEXT("MSM_"));
				Names.Add(Name == TEXT("Strata") ? FString(TEXT("substrate")) : Snake(Name));
			}
			return FString::Join(Names, TEXT("+"));
		}

		FString KindOf(const UMaterialInterface& Material, const FString& Folder)
		{
			if (Material.IsA<UMaterialInstanceDynamic>()) return TEXT("dynamic");
			if (IsActorInstance(&Material, Folder)) return TEXT("look_dev_actor");
			if (IsLevelInstance(&Material, Folder)) return TEXT("look_dev");
			return Material.IsA<UMaterialInstance>() ? TEXT("material_instance") : TEXT("material");
		}

		/** The parameters a lighting artist reaches for first: how rough, metallic, reflective, bright and coloured a surface is. */
		bool IsKeyParameter(const FString& PlainName)
		{
			for (const TCHAR* Word : {TEXT("rough"), TEXT("metal"), TEXT("spec"), TEXT("emiss"), TEXT("glow"), TEXT("color"), TEXT("colour"), TEXT("albedo"),
				TEXT("tint"), TEXT("base"), TEXT("opacity"), TEXT("normal"), TEXT("occlusion"), TEXT("wet"), TEXT("gloss"), TEXT("reflect"), TEXT("fresnel"),
				TEXT("bright"), TEXT("intensity")})
			{
				if (PlainName.Contains(Word)) return true;
			}
			return false;
		}

		/** A parameter that is the surface's value itself, rather than a map, multiplier or range that remaps one. */
		bool IsValueOf(const FString& PlainName, const TCHAR* Word)
		{
			if (!PlainName.Contains(Word))
			{
				return false;
			}
			for (const TCHAR* Remap : {TEXT("map"), TEXT("tex"), TEXT("mult"), TEXT("scale"), TEXT("contrast"), TEXT("min"), TEXT("max"), TEXT("power"),
				TEXT("variation"), TEXT("strength"), TEXT("amount"), TEXT("offset"), TEXT("tint"), TEXT("color"), TEXT("colour"), TEXT("intensity"), TEXT("blend")})
			{
				if (PlainName.Contains(Remap)) return false;
			}
			return true;
		}

		struct FParameters
		{
			TArray<TPair<FString, float>> Scalars;
			TArray<TPair<FString, FLinearColor>> Colours;
		};

		void ReadParameters(const UMaterialInterface& Material, TArray<FHyperAILightingProperty>& Out, FParameters& OutValues)
		{
			struct FEntry
			{
				bool bKey;
				FString Name;
				FString Value;
			};
			TArray<FEntry> Entries;
			for (const EMaterialParameterType Type : {EMaterialParameterType::Scalar, EMaterialParameterType::Vector, EMaterialParameterType::Texture})
			{
				TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters;
				Material.GetAllParametersOfType(Type, Parameters);
				for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Parameter : Parameters)
				{
					if (Parameter.Key.Association != EMaterialParameterAssociation::GlobalParameter) continue;
					const FString Name = Parameter.Key.Name.ToString();
					const FMaterialParameterValue& Value = Parameter.Value.Value;
					FString Text;
					if (Type == EMaterialParameterType::Scalar)
					{
						Text = FString::Printf(TEXT("%.4g"), Value.AsScalar());
						OutValues.Scalars.Add({Name, Value.AsScalar()});
					}
					else if (Type == EMaterialParameterType::Vector)
					{
						const FLinearColor Colour = Value.AsLinearColor();
						Text = FString::Printf(TEXT("%.3g,%.3g,%.3g,%.3g"), Colour.R, Colour.G, Colour.B, Colour.A);
						OutValues.Colours.Add({Name, Colour});
					}
					else
					{
						const UObject* Texture = Value.AsTextureObject();
						Text = Texture ? Texture->GetPathName() : FString(TEXT("none"));
					}
					Entries.Add({IsKeyParameter(Plain(Name)), Name, Text});
				}
			}
			Entries.Sort([](const FEntry& A, const FEntry& B) { return A.bKey != B.bKey ? A.bKey : A.Name < B.Name; });
			for (int32 Index = 0; Index < Entries.Num() && Index < MaxListedParameters; ++Index)
			{
				Out.Add({Entries[Index].Name, Entries[Index].Value});
			}
			if (Entries.Num() > MaxListedParameters)
			{
				Out.Add({TEXT("more"), FString::Printf(TEXT("%d more parameters; the material ops take any of them by name"), Entries.Num() - MaxListedParameters)});
			}
		}

		/** What in a material's setup or values keeps it from taking light as the real surface would, by its parameters' usual names. */
		TArray<FString> NotesFor(const UMaterialInterface& Material, const FHyperAILightingMaterialRecord& Record, const FParameters& Values,
			const bool bLumenReflections, const float MaxRoughness)
		{
			TArray<FString> Notes;
			const UMaterial* Base = Material.GetMaterial();
			if (Record.Kind == TEXT("dynamic"))
			{
				Notes.Add(TEXT("Made by its actor's construction script as the actor is built, so a change here would not last: change the material that Blueprint uses."));
			}
			if (Base && Base->MaterialDomain == MD_Surface && Record.ShadingModel == TEXT("unlit"))
			{
				Notes.Add(TEXT("Unlit: no light or shadow reaches it, only its emissive shows. A surface the reference shows lit needs a lit material (set_actor_material)."));
			}
			if (Record.BlendMode.StartsWith(TEXT("translucent")) || Record.BlendMode == TEXT("additive"))
			{
				Notes.Add(TEXT("Translucent: under Lumen it reflects sharply only with High Quality Translucency Reflections (the front layer), otherwise a blur; its opacity and refraction set most of its look."));
			}
			float Metallic = 0.f;
			for (const TPair<FString, float>& Scalar : Values.Scalars)
			{
				Metallic = IsValueOf(Plain(Scalar.Key), TEXT("metal")) ? Scalar.Value : Metallic;
			}
			for (const TPair<FString, float>& Scalar : Values.Scalars)
			{
				const FString Name = Plain(Scalar.Key);
				if (IsValueOf(Name, TEXT("rough")) && bLumenReflections && Scalar.Value > MaxRoughness)
				{
					Notes.Add(FString::Printf(TEXT("%s %.2f is above Lumen's reflection trace limit (%.2f), so it reflects only a blur of its surroundings. Lower it where the reference shows crisp reflections (wet, polished or glossy), or raise post-process LumenMaxRoughnessToTraceReflections."),
						*Scalar.Key, Scalar.Value, MaxRoughness));
				}
				else if (IsValueOf(Name, TEXT("spec")) && Scalar.Value < 0.2f)
				{
					Notes.Add(FString::Printf(TEXT("%s %.2f dulls its reflections: 0.5 (4%% reflectance) suits nearly every non-metal."), *Scalar.Key, Scalar.Value));
				}
				else if (IsValueOf(Name, TEXT("metal")) && Scalar.Value > 0.1f && Scalar.Value < 0.9f)
				{
					Notes.Add(FString::Printf(TEXT("%s %.2f is neither metal nor not: real surfaces are 0 or 1, with values between only where dirt or rust covers metal."),
						*Scalar.Key, Scalar.Value));
				}
			}
			for (const TPair<FString, FLinearColor>& Colour : Values.Colours)
			{
				const FString Name = Plain(Colour.Key);
				const bool bAlbedo = Name == TEXT("basecolor") || Name == TEXT("basecolour") || Name == TEXT("albedo") || Name == TEXT("albedocolor")
					|| Name == TEXT("color") || Name == TEXT("colour") || Name == TEXT("diffuse") || Name == TEXT("diffusecolor");
				if (!bAlbedo || Metallic > 0.5f) continue;
				const float Luminance = 0.2126f * Colour.Value.R + 0.7152f * Colour.Value.G + 0.0722f * Colour.Value.B;
				if (Luminance > 0.9f)
				{
					Notes.Add(FString::Printf(TEXT("%s is brighter than fresh snow (luminance %.2f against 0.81): lit, it glares and washes out. Non-metals sit between charcoal (0.02) and snow (0.81)."),
						*Colour.Key, Luminance));
				}
				else if (Luminance < 0.015f)
				{
					Notes.Add(FString::Printf(TEXT("%s is darker than charcoal (luminance %.3f against 0.02): it reads as a hole and bounces no light."), *Colour.Key, Luminance));
				}
			}
			if (Notes.Num() > MaxNotes)
			{
				Notes.SetNum(MaxNotes);
			}
			return Notes;
		}

		/** How the material ops' property names this slot: component:<name>/ only where the actor's first mesh is another. */
		FString SlotOf(const UMeshComponent& Mesh, const AActor& Owner, const int32 Slot)
		{
			TArray<UMeshComponent*> Meshes;
			Owner.GetComponents(Meshes);
			return Meshes.Num() > 1 && &Mesh != Owner.GetRootComponent()
				? FString::Printf(TEXT("component:%s/slot:%d"), *Mesh.GetName(), Slot) : FString::Printf(TEXT("slot:%d"), Slot);
		}
	}

	FString FolderFor(const UWorld& World)
	{
		return FString(Gate::MadeAssetsRoot) + TEXT("LookDev/") + Safe(FPackageName::GetShortName(World.GetOutermost()->GetName())) + TEXT("/");
	}

	bool SetParameter(UWorld& World, UMeshComponent& Mesh, const int32 Slot, const bool bActorOnly, const FString& Parameter, const FString& Value,
		UMaterialInterface* Shows, const bool bWrite, TArray<AActor*>& OutTouched, FString& OutError)
	{
		AActor* Owner = Mesh.GetOwner();
		const FString Where = FString::Printf(TEXT("slot %d of %s on '%s'"), Slot, *Mesh.GetName(), Owner ? *Owner->GetActorLabel() : TEXT(""));
		UMaterialInterface* Current = Shows ? Shows : Mesh.GetMaterial(Slot);
		if (!Owner || !Current)
		{
			OutError = Where + TEXT(" shows no material.");
			return false;
		}
		if (Current->IsA<UMaterialInstanceDynamic>() || Current->HasAnyFlags(RF_Transient) || Current->GetOutermost() == GetTransientPackage())
		{
			OutError = FString::Printf(TEXT("%s shows %s, which the actor makes as it is built (a dynamic instance), so a change here would not last; change the material its Blueprint uses."),
				*Where, *Current->GetName());
			return false;
		}
		if (!IsReassignable(Mesh, *Owner))
		{
			OutError = FString::Printf(TEXT("%s is made by %s from its own settings, which would put the old material back; change the material there."), *Where,
				Mesh.CreationMethod == EComponentCreationMethod::UserConstructionScript ? TEXT("the actor's construction script") : TEXT("foliage or PCG"));
			return false;
		}
		FName Name;
		const EParameter Type = FindParameter(*Current, Parameter, Name);
		if (Type == EParameter::None)
		{
			OutError = FString::Printf(TEXT("%s shows %s, which has no parameter '%s'; its parameters are %s. A value the material fixes needs a parameter first (hyper_material_apply_plan), or give the slot another material with set_actor_material."),
				*Where, *Current->GetName(), *Parameter.Left(64), *ParameterNames(*Current));
			return false;
		}
		if (Type == EParameter::Switch)
		{
			OutError = FString::Printf(TEXT("'%s' of %s is a static switch, which rebuilds the material's shaders; switch it on a material instance with hyper_material_apply_plan."),
				*Name.ToString(), *Current->GetName());
			return false;
		}
		FValue Parsed;
		FString Why;
		if (!ParseValue(Type, Value, Parsed, Why))
		{
			OutError = FString::Printf(TEXT("'%s' of %s %s."), *Name.ToString(), *Current->GetName(), *Why);
			return false;
		}
		if (!bWrite)
		{
			return true;
		}
		UMaterialInstanceConstant* Instance = nullptr;
		if (Parsed.bDefault)
		{
			// Nothing to drop where no instance holds a change yet.
			Instance = InstanceFor(World, Mesh, Slot, bActorOnly);
			if (!Instance)
			{
				return true;
			}
		}
		else
		{
			Instance = bActorOnly ? OwnActorInstance(World, Mesh, Slot, *Current, OutTouched, OutError) : OwnLevelInstance(World, *Current, OutTouched, OutError);
			if (!Instance)
			{
				return false;
			}
		}
		Instance->Modify();
		Write(*Instance, Type, Name, Parsed);
		Instance->PostEditChange();
		Instance->MarkPackageDirty();
		OutTouched.AddUnique(Owner);
		return true;
	}

	UMaterialInstanceConstant* InstanceFor(UWorld& World, UMeshComponent& Mesh, const int32 Slot, const bool bActorOnly)
	{
		const FString Folder = FolderFor(World);
		UMaterialInterface* Shown = Mesh.GetMaterial(Slot);
		if (bActorOnly)
		{
			return IsActorInstance(Shown, Folder) ? Cast<UMaterialInstanceConstant>(Shown) : nullptr;
		}
		if (IsLevelInstance(Shown, Folder))
		{
			return Cast<UMaterialInstanceConstant>(Shown);
		}
		const UMaterialInterface* Source = SourceOf(Shown, Folder);
		return Source ? FindLevelInstance(Folder, *Source) : nullptr;
	}

	bool Holds(const UMaterialInstanceConstant* Instance, const FString& Parameter, const FString& Value)
	{
		FValue Parsed;
		FString Error;
		if (!Instance)
		{
			return ParseValue(EParameter::Scalar, Value, Parsed, Error) && Parsed.bDefault;
		}
		FName Name;
		const EParameter Type = FindParameter(*Instance, Parameter, Name);
		if (Type == EParameter::None || Type == EParameter::Switch || !ParseValue(Type, Value, Parsed, Error))
		{
			return false;
		}
		const FScalarParameterValue* Scalar = Instance->ScalarParameterValues.FindByPredicate([Name](const FScalarParameterValue& Entry) { return IsOwn(Entry, Name); });
		const FVectorParameterValue* Vector = Instance->VectorParameterValues.FindByPredicate([Name](const FVectorParameterValue& Entry) { return IsOwn(Entry, Name); });
		const FTextureParameterValue* Texture = Instance->TextureParameterValues.FindByPredicate([Name](const FTextureParameterValue& Entry) { return IsOwn(Entry, Name); });
		if (Parsed.bDefault)
		{
			return !Scalar && !Vector && !Texture;
		}
		if (Type == EParameter::Scalar)
		{
			return Scalar && FMath::IsNearlyEqual(Scalar->ParameterValue, Parsed.Scalar, 0.0001f * FMath::Max(1.f, FMath::Abs(Parsed.Scalar)));
		}
		if (Type == EParameter::Vector)
		{
			const float Tolerance = 0.0001f * FMath::Max(1.f, Parsed.Colour.GetMax());
			return Vector && FMath::IsNearlyEqual(Vector->ParameterValue.R, Parsed.Colour.R, Tolerance) && FMath::IsNearlyEqual(Vector->ParameterValue.G, Parsed.Colour.G, Tolerance)
				&& FMath::IsNearlyEqual(Vector->ParameterValue.B, Parsed.Colour.B, Tolerance) && (!Parsed.bAlpha || FMath::IsNearlyEqual(Vector->ParameterValue.A, Parsed.Colour.A, Tolerance));
		}
		return Texture && Texture->ParameterValue == Parsed.Texture;
	}

	TArray<AActor*> UnsavedUsers(UWorld& World, const UMaterialInterface& Instance)
	{
		TArray<AActor*> Users;
		ForEachLevelMesh(World, [&](UMeshComponent& Mesh, AActor& Owner)
		{
			const UPackage* Package = Owner.GetPackage();
			if (!Package || !Package->IsDirty() || Users.Contains(&Owner)) return;
			for (int32 Index = 0; Index < Mesh.GetNumMaterials(); ++Index)
			{
				if (MadeFrom(Mesh.GetMaterial(Index), Instance))
				{
					Users.Add(&Owner);
					return;
				}
			}
		});
		return Users;
	}

	void AddUnsaved(const AActor& Actor, TArray<UPackage*>& OutPackages)
	{
		const FString Root = FString(Gate::MadeAssetsRoot) + TEXT("LookDev/");
		TArray<UMeshComponent*> Meshes;
		Actor.GetComponents(Meshes);
		for (const UMeshComponent* Mesh : Meshes)
		{
			for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index)
			{
				const UMaterialInterface* Material = Mesh->GetMaterial(Index);
				for (int32 Hop = 0; Hop < MaxHops && Material && IsUnder(Material, Root); ++Hop)
				{
					UPackage* Package = Material->GetPackage();
					if (Package && Package->IsDirty()) OutPackages.AddUnique(Package);
					Material = ParentOf(Material);
				}
			}
		}
	}

	FString RevisionText(const UWorld& World)
	{
		const FString Folder = FolderFor(World);
		TArray<FString> Lines;
		for (TObjectIterator<UMaterialInstanceConstant> It; It; ++It)
		{
			const UMaterialInstanceConstant* Instance = *It;
			if (!IsValid(Instance) || !IsUnder(Instance, Folder)) continue;
			FString Line = Instance->GetPathName() + TEXT("<") + (Instance->Parent ? Instance->Parent->GetPathName() : FString());
			for (const FScalarParameterValue& Entry : Instance->ScalarParameterValues)
			{
				Line += FString::Printf(TEXT("|%s=%.6g"), *Entry.ParameterInfo.Name.ToString(), Entry.ParameterValue);
			}
			for (const FVectorParameterValue& Entry : Instance->VectorParameterValues)
			{
				Line += FString::Printf(TEXT("|%s=%s"), *Entry.ParameterInfo.Name.ToString(), *Entry.ParameterValue.ToString());
			}
			for (const FTextureParameterValue& Entry : Instance->TextureParameterValues)
			{
				Line += FString::Printf(TEXT("|%s=%s"), *Entry.ParameterInfo.Name.ToString(), Entry.ParameterValue ? *Entry.ParameterValue->GetPathName() : TEXT("none"));
			}
			Lines.Add(MoveTemp(Line));
		}
		Lines.Sort();
		return FString::Join(Lines, TEXT("\n"));
	}

	TArray<FHyperAILightingMaterialRecord> ReadMaterials(UWorld& World, const FVector& Origin, const TFunctionRef<bool(const FVector&, double)> InView,
		const int32 Limit, const bool bLumenReflections, const float MaxRoughnessToTrace, int32& OutTotal)
	{
		struct FSeen
		{
			double Weight = 0.0;
			/** Every placed actor showing it, in view or not: what a level-wide change reaches. */
			TSet<const AActor*> Actors;
			TMap<const AActor*, double> Nearest;
			FString Slot;
			double SlotDistance = TNumericLimits<double>::Max();
		};
		TMap<UMaterialInterface*, FSeen> Seen;
		ForEachLevelMesh(World, [&](UMeshComponent& Mesh, AActor& Owner)
		{
			const int32 Slots = Mesh.GetNumMaterials();
			if (Slots <= 0 || Mesh.IsEditorOnly())
			{
				return;
			}
			const FBoxSphereBounds& Bounds = Mesh.Bounds;
			const bool bShown = Mesh.IsVisibleInEditor() && !Mesh.bHiddenInGame && !Owner.IsHiddenEd() && InView(Bounds.Origin, Bounds.SphereRadius);
			const double Distance = FVector::Dist(Origin, Bounds.Origin);
			// Roughly the share of the view a sphere that size covers from there: 1 once the view is inside it.
			const double Weight = bShown ? FMath::Square(FMath::Min(1.0, Bounds.SphereRadius / FMath::Max(Distance, 1.0))) / Slots : 0.0;
			for (int32 Index = 0; Index < Slots; ++Index)
			{
				UMaterialInterface* Material = Mesh.GetMaterial(Index);
				if (!Material) continue;
				FSeen& Entry = Seen.FindOrAdd(Material);
				Entry.Actors.Add(&Owner);
				if (!bShown) continue;
				Entry.Weight += Weight;
				double& Near = Entry.Nearest.FindOrAdd(&Owner, Distance);
				Near = FMath::Min(Near, Distance);
				if (Distance < Entry.SlotDistance)
				{
					Entry.SlotDistance = Distance;
					Entry.Slot = SlotOf(Mesh, Owner, Index);
				}
			}
		});
		double Total = 0.0;
		TArray<TPair<double, UMaterialInterface*>> Ranked;
		for (const TPair<UMaterialInterface*, FSeen>& Entry : Seen)
		{
			if (Entry.Value.Weight > 0.0)
			{
				Ranked.Add({Entry.Value.Weight, Entry.Key});
				Total += Entry.Value.Weight;
			}
		}
		OutTotal = Ranked.Num();
		Ranked.Sort([](const TPair<double, UMaterialInterface*>& A, const TPair<double, UMaterialInterface*>& B)
		{
			return A.Key != B.Key ? A.Key > B.Key : A.Value->GetPathName() < B.Value->GetPathName();
		});
		const FString Folder = FolderFor(World);
		TArray<FHyperAILightingMaterialRecord> Records;
		for (int32 Index = 0; Index < Ranked.Num() && Index < Limit; ++Index)
		{
			UMaterialInterface& Material = *Ranked[Index].Value;
			const FSeen& Entry = Seen.FindChecked(&Material);
			FHyperAILightingMaterialRecord& Record = Records.AddDefaulted_GetRef();
			Record.Path = Material.GetPathName();
			Record.Kind = KindOf(Material, Folder);
			const UMaterial* Base = Material.GetMaterial();
			Record.BaseMaterial = Base ? Base->GetPathName() : FString();
			Record.BlendMode = BlendName(Material.GetBlendMode());
			Record.ShadingModel = ShadingName(Material.GetShadingModels());
			Record.bTwoSided = Material.IsTwoSided();
			Record.ViewShare = Total > 0.0 ? static_cast<float>(Entry.Weight / Total) : 0.f;
			Record.ActorCount = Entry.Actors.Num();
			TArray<TPair<double, const AActor*>> Nearest;
			for (const TPair<const AActor*, double>& Near : Entry.Nearest)
			{
				Nearest.Add({Near.Value, Near.Key});
			}
			Nearest.Sort([](const TPair<double, const AActor*>& A, const TPair<double, const AActor*>& B) { return A.Key < B.Key; });
			for (int32 Actor = 0; Actor < Nearest.Num() && Actor < MaxActorsListed; ++Actor)
			{
				Record.Actors.Add(Nearest[Actor].Value->GetActorLabel());
			}
			Record.Slot = Entry.Slot;
			FParameters Values;
			ReadParameters(Material, Record.Parameters, Values);
			Record.Notes = NotesFor(Material, Record, Values, bLumenReflections, MaxRoughnessToTrace);
		}
		return Records;
	}
}
