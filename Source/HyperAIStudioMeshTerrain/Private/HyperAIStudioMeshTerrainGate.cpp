// Games by Hyper 2026.

#include "HyperAIStudioMeshTerrainGate.h"

#include "Components/SplineComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "MeshPartition.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionModifierActor.h"
#include "MeshPartitionModifierComponent.h"
#include "MeshPartitionRectangleGenerator.h"
#include "Misc/StringOutputDevice.h"
#include "Modifiers/MeshPartitionMeshProvider.h"
#include "Modifiers/MeshPartitionNoiseModifier.h"
#include "Modifiers/MeshPartitionRemeshModifier.h"
#include "Modifiers/MeshPartitionSplineModifier.h"
#include "Modifiers/MeshPartitionTexturePatchModifier.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioMeshTerrain"

namespace HyperAIStudio::MeshTerrain::Gate
{
	namespace MP = UE::MeshPartition;

	namespace
	{
		constexpr double MinSize = 1000.0;
		constexpr double MaxSize = 200000.0;
		constexpr double MinSpacing = 25.0;
		constexpr double MaxSpacing = 1000.0;
		constexpr double DefaultSpacing = 100.0;
		/** Quads per side; a bigger terrain takes a coarser spacing. */
		constexpr int32 MaxResolution = 2048;
		/** What Mesh Terrain Mode's rectangle tool uses to split a terrain into sections. */
		constexpr int32 MaxTrianglesPerSection = 500 * 500 * 2;
		constexpr int32 MaxPoints = 64;
		constexpr int32 MaxProperties = 32;
		constexpr int32 MaxValueCharacters = 200;
		constexpr int32 MaxSettingsCharacters = 1500;

		struct FModifierType
		{
			const TCHAR* Name;
			UClass* Class;
		};

		TArray<FModifierType> ModifierTypes()
		{
			return {
				{TEXT("noise"), MP::UNoiseModifier::StaticClass()},
				{TEXT("remesh"), MP::URemeshModifier::StaticClass()},
				{TEXT("texture_patch"), MP::UTexturePatchModifier::StaticClass()},
				{TEXT("spline"), MP::USplineModifier::StaticClass()}};
		}

		UClass* ClassForType(const FString& Type)
		{
			for (const FModifierType& Entry : ModifierTypes())
			{
				if (Type.Equals(Entry.Name, ESearchCase::IgnoreCase))
				{
					return Entry.Class;
				}
			}
			return nullptr;
		}

		FString TypeOf(const MP::UModifierComponent& Modifier)
		{
			for (const FModifierType& Entry : ModifierTypes())
			{
				if (Modifier.IsA(Entry.Class))
				{
					return Entry.Name;
				}
			}
			return Modifier.GetClass()->GetName();
		}

		FString Vector(const FVector& Value)
		{
			return FString::Printf(TEXT("%.0f,%.0f,%.0f"), Value.X, Value.Y, Value.Z);
		}

		bool ParseNumbers(const FString& Text, const int32 Count, TArray<double>& Out)
		{
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() != Count)
			{
				return false;
			}
			Out.Reset();
			for (const FString& Raw : Parts)
			{
				const FString Part = Raw.TrimStartAndEnd();
				if (Part.IsEmpty() || !Part.IsNumeric())
				{
					return false;
				}
				const double Value = FCString::Atod(*Part);
				if (!FMath::IsFinite(Value) || FMath::Abs(Value) > 1.0e7)
				{
					return false;
				}
				Out.Add(Value);
			}
			return true;
		}

		bool ParseVector(const FString& Text, FVector& Out)
		{
			TArray<double> Values;
			if (!ParseNumbers(Text, 3, Values))
			{
				return false;
			}
			Out = FVector(Values[0], Values[1], Values[2]);
			return true;
		}

		bool ParseRotator(const FString& Text, FRotator& Out)
		{
			TArray<double> Values;
			if (!ParseNumbers(Text, 3, Values))
			{
				return false;
			}
			Out = FRotator(Values[0], Values[1], Values[2]);
			return true;
		}

		bool ParseScale(const FString& Text, FVector& Out)
		{
			return ParseVector(Text, Out) && Out.X > 0.001 && Out.Y > 0.001 && Out.Z > 0.001 && Out.GetMax() <= 10000.0;
		}

		bool IsValidName(const FString& Name)
		{
			if (Name.IsEmpty() || Name.Len() > 48)
			{
				return false;
			}
			for (const TCHAR Character : Name)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
				{
					return false;
				}
			}
			return true;
		}

		TArray<MP::AMeshPartition*> Terrains(UWorld& World)
		{
			TArray<MP::AMeshPartition*> Out;
			for (TActorIterator<MP::AMeshPartition> It(&World); It; ++It)
			{
				if (IsValid(*It))
				{
					Out.Add(*It);
				}
			}
			Out.Sort([](const MP::AMeshPartition& A, const MP::AMeshPartition& B) { return A.GetActorLabel() < B.GetActorLabel(); });
			return Out;
		}

		/** Every modifier component in the level, base mesh sections included. */
		TArray<MP::UModifierComponent*> AllModifiers(UWorld& World)
		{
			TArray<MP::UModifierComponent*> Out;
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				TArray<MP::UModifierComponent*> Components;
				It->GetComponents(Components);
				for (MP::UModifierComponent* Component : Components)
				{
					if (IsValid(Component))
					{
						Out.Add(Component);
					}
				}
			}
			return Out;
		}

		bool IsBase(const MP::UModifierComponent& Modifier)
		{
			return Modifier.IsA<MP::UMeshProviderModifier>();
		}

		/** The modifiers agents shape terrain with, by their actor's label. Base sections are the terrain itself. */
		TArray<MP::UModifierComponent*> Modifiers(UWorld& World)
		{
			TArray<MP::UModifierComponent*> Out = AllModifiers(World).FilterByPredicate(
				[](const MP::UModifierComponent* Modifier) { return !IsBase(*Modifier); });
			Out.Sort([](const MP::UModifierComponent& A, const MP::UModifierComponent& B)
			{
				return A.GetOwner()->GetActorLabel() < B.GetOwner()->GetActorLabel();
			});
			return Out;
		}

		MP::AMeshPartition* FindTerrain(UWorld& World, const FString& Name)
		{
			for (MP::AMeshPartition* Terrain : Terrains(World))
			{
				if (Terrain->GetActorLabel().Equals(Name, ESearchCase::IgnoreCase))
				{
					return Terrain;
				}
			}
			return nullptr;
		}

		MP::UModifierComponent* FindModifier(UWorld& World, const FString& Name)
		{
			for (MP::UModifierComponent* Modifier : Modifiers(World))
			{
				if (Modifier->GetOwner()->GetActorLabel().Equals(Name, ESearchCase::IgnoreCase))
				{
					return Modifier;
				}
			}
			return nullptr;
		}

		MP::UMeshPartitionEditorComponent* EditorComponentOf(const MP::AMeshPartition& Terrain)
		{
			return Cast<MP::UMeshPartitionEditorComponent>(Terrain.GetMeshPartitionComponent());
		}

		bool IsEditable(const FProperty& Property)
		{
			return Property.HasAnyPropertyFlags(CPF_Edit)
				&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_Transient | CPF_DisableEditOnInstance)
				&& !Property.GetName().StartsWith(TEXT("bDraw"));
		}

		/** The instanced settings object a property holds, such as a texture patch's height input. */
		UObject* InstancedObject(const FProperty& Property, const UObject& Owner)
		{
			const FObjectProperty* Object = CastField<FObjectProperty>(&Property);
			return Object && Object->HasAnyPropertyFlags(CPF_InstancedReference) ? Object->GetObjectPropertyValue_InContainer(&Owner) : nullptr;
		}

		FString ExportValue(const FProperty& Property, const UObject& Owner)
		{
			FString Value;
			Property.ExportText_InContainer(0, Value, &Owner, &Owner, const_cast<UObject*>(&Owner), PPF_None);
			return Value.Len() > MaxValueCharacters ? Value.Left(MaxValueCharacters) + TEXT("...") : Value;
		}

		/** Editable settings declared by the modifier's own type, and one level into its instanced settings objects. */
		FString Settings(const MP::UModifierComponent& Modifier)
		{
			TArray<FString> Pairs;
			for (TFieldIterator<FProperty> It(Modifier.GetClass()); It; ++It)
			{
				const UClass* Owner = It->GetOwnerClass();
				if (!IsEditable(**It) || !Owner || !Owner->IsChildOf(MP::UModifierComponent::StaticClass())
					|| Owner == MP::UModifierComponent::StaticClass())
				{
					continue;
				}
				if (const UObject* Inner = InstancedObject(**It, Modifier))
				{
					for (TFieldIterator<FProperty> InnerIt(Inner->GetClass()); InnerIt; ++InnerIt)
					{
						if (IsEditable(**InnerIt))
						{
							Pairs.Add(It->GetName() + TEXT(".") + InnerIt->GetName() + TEXT("=") + ExportValue(**InnerIt, *Inner));
						}
					}
					continue;
				}
				if (It->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference))
				{
					continue;
				}
				Pairs.Add(It->GetName() + TEXT("=") + ExportValue(**It, Modifier));
			}
			const FString Joined = FString::Join(Pairs, TEXT("; "));
			return Joined.Len() > MaxSettingsCharacters ? Joined.Left(MaxSettingsCharacters) + TEXT("...") : Joined;
		}

		/**
		 * Resolves "Name" or "Object.Name" to an editable setting of a modifier type, on Target (an instance, or the class
		 * default object when validating a plan).
		 */
		bool ResolveProperty(UObject& Target, const FString& Path, FProperty*& OutProperty, UObject*& OutOwner, FString& OutError)
		{
			FString Head = Path;
			FString Tail;
			Path.Split(TEXT("."), &Head, &Tail);
			if (Head.IsEmpty() || Tail.Contains(TEXT(".")))
			{
				OutError = FString::Printf(TEXT("\"%s\" is not a setting name; use Name or Object.Name."), *Path);
				return false;
			}
			FProperty* Property = FindFProperty<FProperty>(Target.GetClass(), *Head);
			const UClass* Owner = Property ? Property->GetOwnerClass() : nullptr;
			if (!Property || !IsEditable(*Property) || !Owner || !Owner->IsChildOf(MP::UModifierComponent::StaticClass())
				|| Owner == MP::UModifierComponent::StaticClass())
			{
				OutError = FString::Printf(TEXT("%s has no editable setting %s; hyper_mesh_terrain_inspect lists them."), *Target.GetClass()->GetName(), *Head);
				return false;
			}
			if (Tail.IsEmpty())
			{
				if (InstancedObject(*Property, Target) || Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference))
				{
					OutError = FString::Printf(TEXT("%s holds settings of its own; set them as %s.Name."), *Head, *Head);
					return false;
				}
				OutProperty = Property;
				OutOwner = &Target;
				return true;
			}
			UObject* Inner = InstancedObject(*Property, Target);
			FProperty* InnerProperty = Inner ? FindFProperty<FProperty>(Inner->GetClass(), *Tail) : nullptr;
			if (!InnerProperty || !IsEditable(*InnerProperty))
			{
				OutError = FString::Printf(TEXT("%s has no editable setting %s."), *Head, *Tail);
				return false;
			}
			OutProperty = InnerProperty;
			OutOwner = Inner;
			return true;
		}

		bool ImportValue(const FProperty& Property, const FString& Value, UObject* Owner, void* Destination, const FString& Path, FString& OutError)
		{
			FStringOutputDevice Errors;
			const TCHAR* End = Property.ImportText_Direct(*Value, Destination, Owner, PPF_None, &Errors);
			if (!End || !FString(End).TrimStartAndEnd().IsEmpty() || !Errors.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s does not take \"%s\"%s"), *Path, *Value.Left(MaxValueCharacters),
					Errors.IsEmpty() ? TEXT(".") : *(TEXT(": ") + Errors.TrimStartAndEnd()));
				return false;
			}
			return true;
		}

		/** Checks the settings against a modifier type without changing anything. */
		bool CheckProperties(UObject& Target, const FString& Text, FString& OutError)
		{
			TArray<TPair<FString, FString>> Pairs;
			if (!ParseProperties(Text, Pairs, OutError))
			{
				return false;
			}
			for (const TPair<FString, FString>& Pair : Pairs)
			{
				FProperty* Property = nullptr;
				UObject* Owner = nullptr;
				if (!ResolveProperty(Target, Pair.Key, Property, Owner, OutError))
				{
					return false;
				}
				void* Scratch = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
				Property->InitializeValue(Scratch);
				const bool bImported = ImportValue(*Property, Pair.Value, Owner, Scratch, Pair.Key, OutError);
				Property->DestroyValue(Scratch);
				FMemory::Free(Scratch);
				if (!bImported)
				{
					return false;
				}
			}
			return true;
		}

		bool SetProperties(MP::UModifierComponent& Modifier, const FString& Text, FString& OutError)
		{
			TArray<TPair<FString, FString>> Pairs;
			if (!ParseProperties(Text, Pairs, OutError))
			{
				return false;
			}
			for (const TPair<FString, FString>& Pair : Pairs)
			{
				FProperty* Property = nullptr;
				UObject* Owner = nullptr;
				if (!ResolveProperty(Modifier, Pair.Key, Property, Owner, OutError))
				{
					return false;
				}
				Owner->Modify();
				if (!ImportValue(*Property, Pair.Value, Owner, Property->ContainerPtrToValuePtr<void>(Owner), Pair.Key, OutError))
				{
					return false;
				}
				FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
				Owner->PostEditChangeProperty(Event);
			}
			return true;
		}

		USplineComponent* SplineOf(const MP::UModifierComponent& Modifier)
		{
			return Modifier.GetOwner() ? Modifier.GetOwner()->FindComponentByClass<USplineComponent>() : nullptr;
		}

		void SetPoints(USplineComponent& Spline, const TArray<FVector>& Points)
		{
			Spline.Modify();
			Spline.ClearSplinePoints(false);
			for (int32 Index = 0; Index < Points.Num(); ++Index)
			{
				Spline.AddPoint(FSplinePoint(static_cast<float>(Index), Points[Index]), false);
			}
			Spline.UpdateSpline();
		}

		/** What the plan's ops see: names that exist at that point, and each modifier's class. */
		struct FPlanState
		{
			TSet<FString> Terrains;
			TMap<FString, UClass*> Modifiers;

			bool Taken(const FString& Name) const
			{
				return Terrains.Contains(Name.ToLower()) || Modifiers.Contains(Name.ToLower());
			}
		};

		bool ResolveTerrainName(const FPlanState& State, const FString& Requested, FString& OutError)
		{
			if (Requested.IsEmpty())
			{
				if (State.Terrains.Num() != 1)
				{
					OutError = State.Terrains.IsEmpty() ? TEXT("The level has no Mesh Terrain; create_terrain first.")
						: TEXT("The level has several terrains; name one in Terrain.");
					return false;
				}
				return true;
			}
			if (!State.Terrains.Contains(Requested.ToLower()))
			{
				OutError = FString::Printf(TEXT("There is no terrain named %s."), *Requested);
				return false;
			}
			return true;
		}

		MP::AMeshPartition* ResolveTerrain(UWorld& World, const FString& Requested)
		{
			if (!Requested.IsEmpty())
			{
				return FindTerrain(World, Requested);
			}
			const TArray<MP::AMeshPartition*> All = Terrains(World);
			return All.Num() == 1 ? All[0] : nullptr;
		}

		bool ParseSize(const FString& Text, double& OutWidth, double& OutDepth)
		{
			TArray<double> Values;
			if (!ParseNumbers(Text, 2, Values))
			{
				return false;
			}
			OutWidth = Values[0];
			OutDepth = Values[1];
			return OutWidth >= MinSize && OutWidth <= MaxSize && OutDepth >= MinSize && OutDepth <= MaxSize;
		}

		bool ApplyOp(UWorld& World, const FHyperAIMeshTerrainOp& Op, TSet<MP::AMeshPartition*>& Touched, FString& OutError)
		{
			const FString Kind = Op.Kind.ToLower();
			if (Kind == TEXT("create_terrain"))
			{
				FVector Location;
				double Width = 0.0;
				double Depth = 0.0;
				ParseVector(Op.Location, Location);
				ParseSize(Op.Size, Width, Depth);
				const double Spacing = Op.Spacing > 0.f ? Op.Spacing : DefaultSpacing;
				MP::FMegaMeshRectangleGeneratorParams Params;
				Params.MeshResolution = FInt32Point(FMath::Max(1, FMath::RoundToInt(Width / Spacing)), FMath::Max(1, FMath::RoundToInt(Depth / Spacing)));
				Params.MeshSize = FVector2D(Width, Depth);
				Params.SectionsResolution = MP::FRectangleGeneratorUtils::ComputeSectionResolution(Params.MeshResolution, MaxTrianglesPerSection);
				Params.bSaveAndUnload = false;
				Params.LocationVolumesResolution = FInt32Point(0, 0);
				MP::FMegaMeshRectangleGenerator Generator(Params, World, FTransform(Location));
				if (!Generator.Generate(nullptr) || !Generator.GetMegaMesh())
				{
					OutError = Generator.GetErrorText().IsEmpty() ? FString(TEXT("Mesh Partition did not create the terrain."))
						: Generator.GetErrorText().ToString();
					return false;
				}
				Generator.GetMegaMesh()->SetActorLabel(Op.Name);
				Touched.Add(Generator.GetMegaMesh());
				return true;
			}
			if (Kind == TEXT("add_modifier"))
			{
				UClass* Class = ClassForType(Op.Type);
				MP::AMeshPartition* Terrain = ResolveTerrain(World, Op.Terrain);
				FVector Location;
				if (!Class || !Terrain || !ParseVector(Op.Location, Location))
				{
					OutError = TEXT("The terrain or type went away after planning.");
					return false;
				}
				FRotator Rotation = FRotator::ZeroRotator;
				FVector Scale = FVector::OneVector;
				if (!Op.Rotation.IsEmpty()) ParseRotator(Op.Rotation, Rotation);
				if (!Op.Scale.IsEmpty()) ParseScale(Op.Scale, Scale);
				FActorSpawnParameters Spawn;
				Spawn.ObjectFlags |= RF_Transactional;
				MP::AModifierActor* Actor = World.SpawnActor<MP::AModifierActor>(Location, Rotation, Spawn);
				if (!Actor)
				{
					OutError = TEXT("The modifier actor could not be spawned.");
					return false;
				}
				Actor->SetActorLabel(Op.Name);
				Actor->SetActorScale3D(Scale);
				MP::UModifierComponent* Modifier = NewObject<MP::UModifierComponent>(Actor, Class, NAME_None, RF_Transactional);
				Actor->AddInstanceComponent(Modifier);
				Modifier->SetupAttachment(Actor->GetRootComponent());
				Modifier->RegisterComponent();
				if (MP::USplineModifier* SplineModifier = Cast<MP::USplineModifier>(Modifier))
				{
					TArray<FVector> Points;
					ParsePoints(Op.Points, Points, OutError);
					USplineComponent* Spline = NewObject<USplineComponent>(Actor, NAME_None, RF_Transactional);
					Actor->AddInstanceComponent(Spline);
					Spline->SetupAttachment(Actor->GetRootComponent());
					Spline->RegisterComponent();
					SetPoints(*Spline, Points);
					SplineModifier->SetSplineComponent(Spline, false);
				}
				if (!Op.Properties.IsEmpty() && !SetProperties(*Modifier, Op.Properties, OutError))
				{
					return false;
				}
				// The order Mesh Terrain Mode's place-modifier tool uses.
				Modifier->Modify(true);
				Modifier->SetAffectedMeshPartition(Terrain);
				Modifier->PostEditChange();
				Actor->Modifier = Modifier;
				Touched.Add(Terrain);
				return true;
			}
			MP::UModifierComponent* Modifier = FindModifier(World, Op.Name);
			if (!Modifier)
			{
				OutError = TEXT("The modifier went away after planning.");
				return false;
			}
			AActor* Actor = Modifier->GetOwner();
			if (MP::AMeshPartition* Terrain = Modifier->GetAffectedMeshPartition())
			{
				Touched.Add(Terrain);
			}
			if (Kind == TEXT("remove_modifier"))
			{
				Actor->Modify();
				if (!World.EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true))
				{
					OutError = TEXT("The modifier actor could not be deleted.");
					return false;
				}
				return true;
			}
			// set_modifier
			Actor->Modify();
			Modifier->Modify();
			FVector Vector3;
			FRotator Rotation;
			bool bMoved = false;
			if (!Op.Location.IsEmpty() && ParseVector(Op.Location, Vector3))
			{
				Actor->SetActorLocation(Vector3);
				bMoved = true;
			}
			if (!Op.Rotation.IsEmpty() && ParseRotator(Op.Rotation, Rotation))
			{
				Actor->SetActorRotation(Rotation);
				bMoved = true;
			}
			if (!Op.Scale.IsEmpty() && ParseScale(Op.Scale, Vector3))
			{
				Actor->SetActorScale3D(Vector3);
				bMoved = true;
			}
			if (bMoved)
			{
				Actor->PostEditMove(true);
			}
			if (!Op.Points.IsEmpty())
			{
				TArray<FVector> Points;
				USplineComponent* Spline = SplineOf(*Modifier);
				MP::USplineModifier* SplineModifier = Cast<MP::USplineModifier>(Modifier);
				if (!Spline || !SplineModifier || !ParsePoints(Op.Points, Points, OutError))
				{
					if (OutError.IsEmpty()) OutError = TEXT("Only a spline modifier with its spline takes points.");
					return false;
				}
				SetPoints(*Spline, Points);
				SplineModifier->SetSplineComponent(Spline, false);
			}
			if (!Op.Properties.IsEmpty() && !SetProperties(*Modifier, Op.Properties, OutError))
			{
				return false;
			}
			Modifier->PostEditChange();
			return true;
		}
	}

	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	bool ParseProperties(const FString& Text, TArray<TPair<FString, FString>>& OutPairs, FString& OutError)
	{
		OutPairs.Reset();
		TArray<FString> Pieces;
		FString Current;
		int32 Depth = 0;
		bool bQuoted = false;
		for (const TCHAR Character : Text)
		{
			if (Character == TEXT('"'))
			{
				bQuoted = !bQuoted;
			}
			else if (!bQuoted && (Character == TEXT('(') || Character == TEXT('[') || Character == TEXT('{')))
			{
				++Depth;
			}
			else if (!bQuoted && (Character == TEXT(')') || Character == TEXT(']') || Character == TEXT('}')))
			{
				--Depth;
			}
			if (Character == TEXT(';') && Depth == 0 && !bQuoted)
			{
				Pieces.Add(Current);
				Current.Reset();
				continue;
			}
			Current.AppendChar(Character);
		}
		Pieces.Add(Current);
		if (Depth != 0 || bQuoted)
		{
			OutError = TEXT("Properties have an unclosed bracket or quote.");
			return false;
		}
		for (const FString& Raw : Pieces)
		{
			const FString Piece = Raw.TrimStartAndEnd();
			if (Piece.IsEmpty())
			{
				continue;
			}
			FString Name;
			FString Value;
			if (!Piece.Split(TEXT("="), &Name, &Value) || Name.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("\"%s\" is not Name=Value."), *Piece.Left(MaxValueCharacters));
				return false;
			}
			OutPairs.Emplace(Name.TrimStartAndEnd(), Value.TrimStartAndEnd());
		}
		if (OutPairs.Num() > MaxProperties)
		{
			OutError = FString::Printf(TEXT("At most %d settings per op."), MaxProperties);
			return false;
		}
		return true;
	}

	bool ParsePoints(const FString& Text, TArray<FVector>& OutPoints, FString& OutError)
	{
		OutPoints.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(";"));
		for (const FString& Part : Parts)
		{
			FVector Point;
			if (!ParseVector(Part, Point))
			{
				OutError = FString::Printf(TEXT("\"%s\" is not a point \"x,y,z\"."), *Part.TrimStartAndEnd().Left(64));
				return false;
			}
			OutPoints.Add(Point);
		}
		if (OutPoints.Num() < 2 || OutPoints.Num() > MaxPoints)
		{
			OutError = FString::Printf(TEXT("A spline takes 2 to %d points."), MaxPoints);
			return false;
		}
		return true;
	}

	void Read(UWorld& World, TArray<FHyperAIMeshTerrain>& OutTerrains, TArray<FHyperAIMeshTerrainModifier>& OutModifiers)
	{
		const TArray<MP::UModifierComponent*> All = AllModifiers(World);
		for (MP::AMeshPartition* Terrain : Terrains(World))
		{
			FHyperAIMeshTerrain& Record = OutTerrains.AddDefaulted_GetRef();
			Record.Name = Terrain->GetActorLabel();
			Record.ActorPath = Terrain->GetPathName();
			Record.Definition = Terrain->GetMeshPartitionDefinition() ? Terrain->GetMeshPartitionDefinition()->GetPathName() : FString();
			Record.Location = Vector(Terrain->GetActorLocation());
			FBox Bounds(ForceInit);
			for (const MP::UModifierComponent* Modifier : All)
			{
				if (Modifier->GetAffectedMeshPartition() != Terrain)
				{
					continue;
				}
				if (IsBase(*Modifier))
				{
					++Record.SectionCount;
					Bounds += Modifier->Bounds.GetBox();
				}
				else
				{
					++Record.ModifierCount;
				}
			}
			Record.Size = Bounds.IsValid ? Vector(Bounds.GetSize()) : FString();
			MP::UMeshPartitionEditorComponent* Editor = EditorComponentOf(*Terrain);
			Record.bBuilding = Editor && Editor->IsAnyPreviewSectionBuildActive();
		}
		for (const MP::UModifierComponent* Modifier : Modifiers(World))
		{
			FHyperAIMeshTerrainModifier& Record = OutModifiers.AddDefaulted_GetRef();
			const AActor* Owner = Modifier->GetOwner();
			Record.Name = Owner->GetActorLabel();
			Record.Type = TypeOf(*Modifier);
			const MP::AMeshPartition* Terrain = Modifier->GetAffectedMeshPartition();
			Record.Terrain = Terrain ? Terrain->GetActorLabel() : FString();
			Record.Location = Vector(Modifier->GetComponentLocation());
			const FRotator Rotation = Modifier->GetComponentRotation();
			Record.Rotation = FString::Printf(TEXT("%.1f,%.1f,%.1f"), Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
			const FVector Scale = Modifier->GetComponentScale();
			Record.Scale = FString::Printf(TEXT("%.3g,%.3g,%.3g"), Scale.X, Scale.Y, Scale.Z);
			Record.bDisabled = Modifier->GetIsDisabledFlag();
			Record.Settings = Settings(*Modifier);
			if (const USplineComponent* Spline = SplineOf(*Modifier))
			{
				TArray<FString> Points;
				for (int32 Index = 0; Index < Spline->GetNumberOfSplinePoints(); ++Index)
				{
					Points.Add(Vector(Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::Local)));
				}
				Record.Points = FString::Join(Points, TEXT("; "));
			}
		}
	}

	FString ComputeRevision(UWorld& World)
	{
		TArray<FHyperAIMeshTerrain> TerrainRecords;
		TArray<FHyperAIMeshTerrainModifier> ModifierRecords;
		Read(World, TerrainRecords, ModifierRecords);
		// Record by record, so a level with many modifiers stays within the hashing bound.
		FString Canonical = TEXT("hyperai.mesh_terrain.revision.v1\n") + World.GetPathName() + TEXT("\n");
		for (const FHyperAIMeshTerrain& Terrain : TerrainRecords)
		{
			Canonical += FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(FString::Join(TArray<FString>{
				Terrain.Name, Terrain.ActorPath, Terrain.Definition, Terrain.Location, Terrain.Size, FString::FromInt(Terrain.SectionCount)}, TEXT("|")))
				+ TEXT("\n");
		}
		for (const FHyperAIMeshTerrainModifier& Modifier : ModifierRecords)
		{
			Canonical += FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(FString::Join(TArray<FString>{
				Modifier.Name, Modifier.Type, Modifier.Terrain, Modifier.Location, Modifier.Rotation, Modifier.Scale,
				Modifier.bDisabled ? TEXT("disabled") : TEXT("enabled"), Modifier.Settings, Modifier.Points}, TEXT("|")))
				+ TEXT("\n");
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	bool ValidateOps(UWorld& World, const TArray<FHyperAIMeshTerrainOp>& Ops, TArray<FString>& OutNotes, FString& OutError)
	{
		FPlanState State;
		for (const MP::AMeshPartition* Terrain : Terrains(World))
		{
			State.Terrains.Add(Terrain->GetActorLabel().ToLower());
		}
		for (const MP::UModifierComponent* Modifier : Modifiers(World))
		{
			State.Modifiers.Add(Modifier->GetOwner()->GetActorLabel().ToLower(), Modifier->GetClass());
		}
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			const FHyperAIMeshTerrainOp& Op = Ops[Index];
			const FString Kind = Op.Kind.ToLower();
			auto Fail = [&](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Op %d (%s %s): %s"), Index + 1, *Op.Kind, *Op.Name, *Message);
				return false;
			};
			if (!IsValidName(Op.Name))
			{
				return Fail(TEXT("Name must be 1-48 letters, digits, _ or -."));
			}
			FVector Vector3;
			FRotator Rotation;
			if (!Op.Rotation.IsEmpty() && !ParseRotator(Op.Rotation, Rotation))
			{
				return Fail(TEXT("Rotation must be \"pitch,yaw,roll\" in degrees."));
			}
			if (!Op.Scale.IsEmpty() && !ParseScale(Op.Scale, Vector3))
			{
				return Fail(TEXT("Scale must be \"x,y,z\", each above 0.001."));
			}
			if (Kind == TEXT("create_terrain"))
			{
				double Width = 0.0;
				double Depth = 0.0;
				const double Spacing = Op.Spacing > 0.f ? Op.Spacing : DefaultSpacing;
				if (State.Taken(Op.Name))
				{
					return Fail(TEXT("That name is taken."));
				}
				if (!ParseVector(Op.Location, Vector3))
				{
					return Fail(TEXT("Location must be \"x,y,z\" in centimetres."));
				}
				if (!ParseSize(Op.Size, Width, Depth))
				{
					return Fail(FString::Printf(TEXT("Size must be \"width,depth\", each %.0f-%.0f cm."), MinSize, MaxSize));
				}
				if (Spacing < MinSpacing || Spacing > MaxSpacing)
				{
					return Fail(FString::Printf(TEXT("Spacing must be %.0f-%.0f cm."), MinSpacing, MaxSpacing));
				}
				const int32 QuadsX = FMath::RoundToInt(Width / Spacing);
				const int32 QuadsY = FMath::RoundToInt(Depth / Spacing);
				if (QuadsX > MaxResolution || QuadsY > MaxResolution)
				{
					return Fail(FString::Printf(TEXT("That is %dx%d quads; at most %d per side, so use a spacing of at least %.0f cm."),
						QuadsX, QuadsY, MaxResolution, FMath::Max(Width, Depth) / MaxResolution));
				}
				OutNotes.Add(FString::Printf(TEXT("%s: %.0f x %.0f m at %.0f cm spacing, %dx%d quads."), *Op.Name, Width / 100.0,
					Depth / 100.0, Spacing, QuadsX, QuadsY));
				State.Terrains.Add(Op.Name.ToLower());
				continue;
			}
			if (Kind == TEXT("add_modifier"))
			{
				UClass* Class = ClassForType(Op.Type);
				if (State.Taken(Op.Name))
				{
					return Fail(TEXT("That name is taken."));
				}
				if (!Class)
				{
					return Fail(TEXT("Type must be noise, remesh, texture_patch or spline."));
				}
				FString Error;
				if (!ResolveTerrainName(State, Op.Terrain, Error))
				{
					return Fail(Error);
				}
				if (!ParseVector(Op.Location, Vector3))
				{
					return Fail(TEXT("Location must be \"x,y,z\" in centimetres."));
				}
				const bool bSpline = Class == MP::USplineModifier::StaticClass();
				TArray<FVector> Points;
				if (bSpline != !Op.Points.IsEmpty())
				{
					return Fail(bSpline ? TEXT("A spline needs Points.") : TEXT("Only a spline takes Points."));
				}
				if (bSpline && !ParsePoints(Op.Points, Points, Error))
				{
					return Fail(Error);
				}
				if (!Op.Properties.IsEmpty() && !CheckProperties(*Class->GetDefaultObject(), Op.Properties, Error))
				{
					return Fail(Error);
				}
				State.Modifiers.Add(Op.Name.ToLower(), Class);
				continue;
			}
			UClass* const* Existing = State.Modifiers.Find(Op.Name.ToLower());
			if (Kind == TEXT("remove_modifier"))
			{
				if (!Existing)
				{
					return Fail(TEXT("There is no modifier by that name."));
				}
				State.Modifiers.Remove(Op.Name.ToLower());
				continue;
			}
			if (Kind != TEXT("set_modifier"))
			{
				return Fail(TEXT("Kind must be create_terrain, add_modifier, set_modifier or remove_modifier."));
			}
			if (!Existing)
			{
				return Fail(TEXT("There is no modifier by that name."));
			}
			if (Op.Location.IsEmpty() && Op.Rotation.IsEmpty() && Op.Scale.IsEmpty() && Op.Properties.IsEmpty() && Op.Points.IsEmpty())
			{
				return Fail(TEXT("Nothing to change: give Location, Rotation, Scale, Properties or Points."));
			}
			if (!Op.Location.IsEmpty() && !ParseVector(Op.Location, Vector3))
			{
				return Fail(TEXT("Location must be \"x,y,z\" in centimetres."));
			}
			FString Error;
			TArray<FVector> Points;
			if (!Op.Points.IsEmpty() && (!(*Existing)->IsChildOf(MP::USplineModifier::StaticClass()) || !ParsePoints(Op.Points, Points, Error)))
			{
				return Fail(Error.IsEmpty() ? TEXT("Only a spline takes Points.") : Error);
			}
			// A modifier that exists now is checked against itself, so its own settings objects count.
			UObject* Target = (*Existing)->GetDefaultObject();
			if (MP::UModifierComponent* Live = FindModifier(World, Op.Name))
			{
				Target = Live;
			}
			if (!Op.Properties.IsEmpty() && !CheckProperties(*Target, Op.Properties, Error))
			{
				return Fail(Error);
			}
		}
		return true;
	}

	bool ApplyOps(UWorld& World, const TArray<FHyperAIMeshTerrainOp>& Ops, int32& OutChanged, FString& OutError)
	{
		OutChanged = 0;
		const FScopedTransaction Transaction(LOCTEXT("MeshTerrainEdit", "HyperAI: Mesh Terrain edit"));
		TSet<MP::AMeshPartition*> Touched;
		for (const FHyperAIMeshTerrainOp& Op : Ops)
		{
			FString Error;
			if (!ApplyOp(World, Op, Touched, Error))
			{
				OutError = FString::Printf(TEXT("%s %s: %s"), *Op.Kind, *Op.Name, *Error);
				return false;
			}
			++OutChanged;
		}
		for (MP::AMeshPartition* Terrain : Touched)
		{
			if (MP::UMeshPartitionEditorComponent* Editor = IsValid(Terrain) ? EditorComponentOf(*Terrain) : nullptr)
			{
				Editor->OnModifierAssigned();
			}
		}
		return true;
	}

	bool IsBuilding(UWorld& World)
	{
		for (const MP::AMeshPartition* Terrain : Terrains(World))
		{
			MP::UMeshPartitionEditorComponent* Editor = EditorComponentOf(*Terrain);
			if (Editor && Editor->IsAnyPreviewSectionBuildActive())
			{
				return true;
			}
		}
		return false;
	}

	void Check(UWorld& World, FHyperAIMeshTerrainValidateReport& Report)
	{
		auto Add = [&Report](const TCHAR* Code, const TCHAR* Severity, const FString& Subject, const FString& Message)
		{
			FHyperAIMeshTerrainIssue& Issue = Report.Issues.AddDefaulted_GetRef();
			Issue.Code = Code;
			Issue.Severity = Severity;
			Issue.Subject = Subject;
			Issue.Message = Message;
			(FCString::Strcmp(Severity, TEXT("error")) == 0 ? Report.ErrorCount : Report.WarningCount)++;
		};
		TArray<FHyperAIMeshTerrain> TerrainRecords;
		TArray<FHyperAIMeshTerrainModifier> ModifierRecords;
		Read(World, TerrainRecords, ModifierRecords);
		Report.TerrainCount = TerrainRecords.Num();
		for (const FHyperAIMeshTerrain& Terrain : TerrainRecords)
		{
			if (Terrain.Definition.IsEmpty())
			{
				Add(TEXT("no_definition"), TEXT("error"), Terrain.Name, TEXT("The terrain has no Mesh Partition Definition, so it has no material or build settings."));
			}
			if (Terrain.SectionCount == 0)
			{
				Add(TEXT("no_base_mesh"), TEXT("error"), Terrain.Name, TEXT("The terrain has no base mesh sections; modifiers have nothing to shape."));
			}
			if (Terrain.bBuilding)
			{
				Report.bBuilding = true;
				Add(TEXT("still_building"), TEXT("warning"), Terrain.Name, TEXT("Preview sections are still rebuilding; check again when they finish."));
			}
		}
		for (const MP::UModifierComponent* Modifier : Modifiers(World))
		{
			const FString Name = Modifier->GetOwner()->GetActorLabel();
			if (!Modifier->GetAffectedMeshPartition())
			{
				Add(TEXT("unbound_modifier"), TEXT("error"), Name, TEXT("The modifier affects no terrain; set_modifier cannot fix this, remove it and add it again."));
			}
			if (Modifier->GetIsDisabledFlag())
			{
				Add(TEXT("disabled_modifier"), TEXT("warning"), Name, TEXT("The modifier is disabled and changes nothing."));
			}
			if (Modifier->IsA<MP::UTexturePatchModifier>())
			{
				const FProperty* Height = FindFProperty<FProperty>(Modifier->GetClass(), TEXT("HeightChannel"));
				const UObject* Input = Height ? InstancedObject(*Height, *Modifier) : nullptr;
				const FObjectProperty* Texture = Input ? FindFProperty<FObjectProperty>(Input->GetClass(), TEXT("TextureAsset")) : nullptr;
				if (!Texture || !Texture->GetObjectPropertyValue_InContainer(Input))
				{
					Add(TEXT("texture_missing"), TEXT("error"), Name, TEXT("The texture patch has no height texture: set HeightChannel.TextureAsset."));
				}
			}
			if (Modifier->IsA<MP::USplineModifier>())
			{
				const USplineComponent* Spline = SplineOf(*Modifier);
				if (!Spline || Spline->GetNumberOfSplinePoints() < 2)
				{
					Add(TEXT("spline_missing"), TEXT("error"), Name, TEXT("The spline modifier has no spline with two or more points."));
				}
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
