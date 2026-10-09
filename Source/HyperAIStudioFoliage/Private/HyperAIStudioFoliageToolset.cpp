// Games by Hyper 2026.

#include "HyperAIStudioFoliageToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/BrushComponent.h"
#include "Components/ModelComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "FoliageHelper.h"
#include "FoliageInstancedStaticMeshComponent.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeInfo.h"
#include "MaterialShared.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioFoliage, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioFoliage"

FString FHyperAIStudioFoliageContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioFoliage.HyperAIStudioFoliageToolset");
}

TArray<FString> FHyperAIStudioFoliageContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioFoliageContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::FoliageTools::Private
{
	using C = FHyperAIStudioFoliageContracts;

	/** Traces start this far above a spot and end this far below it, cm. */
	constexpr double TraceReach = 1000000.0;
	/** Unreal's foliage Density counts instances per 1000 x 1000 cm. */
	constexpr double DensityArea = 1000000.0;
	/** Unreal's default foliage Height band, which means no limit. */
	constexpr float NoHeightLimit = 262143.f;

	FString KindOf(const FHyperAIFoliageOp& Op)
	{
		return Op.Kind.TrimStartAndEnd().ToLower();
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	ALandscape* FindLandscape(UWorld& World, const FString& Label)
	{
		for (TActorIterator<ALandscape> It(&World); It; ++It)
		{
			if (It->GetActorLabel().Equals(Label.TrimStartAndEnd(), ESearchCase::IgnoreCase)) return *It;
		}
		return nullptr;
	}

	template <typename T>
	T* Load(const FString& Path)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		// The load makes an FName of each part, and the package it looked for: past 1023 characters, or with a double
		// slash, either stops the editor.
		if (!FHyperAIStudioNames::IsObjectPath(Trimmed, TEXT("Path"))) return nullptr;
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** The package of an asset reference: /Game/F/FT_A.FT_A and /Game/F/FT_A name the same asset. */
	FString PackageOf(const FString& Reference)
	{
		FString Path = Reference.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
		return Path;
	}

	/** A foliage type asset by path, or by name when exactly one asset has it. */
	UFoliageType* FindType(const FString& Reference, FString& OutError)
	{
		const FString Wanted = PackageOf(Reference);
		if (Wanted.IsEmpty())
		{
			OutError = TEXT("Name each foliage type by its asset path or name.");
			return nullptr;
		}
		if (Wanted.StartsWith(TEXT("/")))
		{
			UFoliageType* Type = Load<UFoliageType>(Wanted);
			if (!Type) OutError = FString::Printf(TEXT("%s is not a foliage type asset; foliage_type.create makes one."), *Wanted);
			return Type;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UFoliageType::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
		TArray<FString> Matches;
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString().Equals(Wanted, ESearchCase::IgnoreCase)) Matches.Add(Asset.GetObjectPathString());
		}
		if (Matches.Num() == 1) return Load<UFoliageType>(Matches[0]);
		OutError = Matches.IsEmpty()
			? FString::Printf(TEXT("No foliage type asset is named %s; foliage_type.create makes one."), *Wanted)
			: FString::Printf(TEXT("%d foliage types are named %s (%s); give the path."), Matches.Num(), *Wanted, *FString::Join(Matches, TEXT(", ")));
		return nullptr;
	}

	FString NameOf(const UFoliageType& Type)
	{
		if (Type.IsAsset()) return Type.GetPathName();
		const UObject* Source = Type.GetSource();
		return FString::Printf(TEXT("%s (local to its foliage actor)"), Source ? *Source->GetName() : *Type.GetName());
	}

	bool IsSetting(const FProperty& Property)
	{
		return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_DisableEditOnInstance);
	}

	/** An editable property of the type by its name, any case, with or without a bool's leading b. */
	FProperty* FindSetting(const UClass& Class, const FString& Name)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		for (TFieldIterator<FProperty> It(&Class); It; ++It)
		{
			if (IsSetting(**It) && (It->GetName().Equals(Wanted, ESearchCase::IgnoreCase) || It->GetName().Equals(TEXT("b") + Wanted, ESearchCase::IgnoreCase)))
			{
				return *It;
			}
		}
		return nullptr;
	}

	FString SettingNames(const UClass& Class)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(&Class); It; ++It)
		{
			if (IsSetting(**It)) Names.Add(It->GetName());
		}
		return FString::Join(Names, TEXT(", "));
	}

	FString ExportValue(const FProperty& Property, const void* Container, UObject* Owner)
	{
		// The container as its own delta: against null, a value equal to its type's zero exports as nothing.
		FString Value;
		Property.ExportText_InContainer(0, Value, Container, Container, Owner, PPF_None);
		return Value.Len() > 300 ? Value.Left(300) + TEXT("...") : Value;
	}

	/**
	 * Each setting written into the type in Unreal's text form; false, with why, at the first that does not take. With
	 * bNotify the type hears of each change as the details panel would tell it, so foliage already placed follows it.
	 */
	bool ApplySettings(UFoliageType& Type, const TArray<FHyperAIFoliageSetting>& Settings, const bool bNotify, FString& OutError)
	{
		for (const FHyperAIFoliageSetting& Setting : Settings)
		{
			FProperty* Property = FindSetting(*Type.GetClass(), Setting.Name);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("A foliage type has no setting '%s'; its settings are %s."), *Setting.Name, *SettingNames(*Type.GetClass()));
				return false;
			}
			const FString Before = ExportValue(*Property, &Type, &Type);
			// The import makes names of the text, and loads an asset it names (making the package when nothing is there).
			if (!FHyperAIStudioNames::CanImportText(*Property, Setting.Value, *Setting.Name, &OutError)) return false;
			if (bNotify) Type.PreEditChange(Property);
			const TCHAR* Rest = Property->ImportText_InContainer(*Setting.Value, &Type, &Type, PPF_None);
			if (bNotify)
			{
				FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
				Type.PostEditChangeProperty(Event);
			}
			if (!Rest || !FString(Rest).TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not a value for %s (%s); it reads %s."), *Setting.Value, *Property->GetName(), *Property->GetCPPType(), *Before);
				return false;
			}
		}
		if (!bNotify) Type.UpdateBounds();
		return true;
	}

	TArray<FString> ExportSettings(UFoliageType& Type)
	{
		TArray<FString> Lines;
		for (TFieldIterator<FProperty> It(Type.GetClass()); It; ++It)
		{
			if (IsSetting(**It)) Lines.Add(FString::Printf(TEXT("%s = %s"), *It->GetName(), *ExportValue(**It, &Type, &Type)));
		}
		return Lines;
	}

	/** The op's settings, led by its Mesh when it names one. */
	TArray<FHyperAIFoliageSetting> SettingsOf(const FHyperAIFoliageOp& Op, const UStaticMesh* Mesh)
	{
		TArray<FHyperAIFoliageSetting> All;
		if (Mesh)
		{
			FHyperAIFoliageSetting& Setting = All.AddDefaulted_GetRef();
			Setting.Name = TEXT("Mesh");
			Setting.Value = Mesh->GetPathName();
		}
		All.Append(Op.Settings);
		return All;
	}

	FString Range(const FFloatInterval& Interval, const TCHAR* Unit)
	{
		return FString::Printf(TEXT("%g to %g%s"), Interval.Min, Interval.Max, Unit);
	}

	FString Summary(const UFoliageType& Type)
	{
		TArray<FString> Parts;
		Parts.Add(FString::Printf(TEXT("Density %g per 100 m2"), Type.Density));
		if (Type.Radius > 0.f) Parts.Add(FString::Printf(TEXT("Radius %g cm"), Type.Radius));
		Parts.Add(FString::Printf(TEXT("%s scale %s"), *UEnum::GetValueAsString(Type.Scaling).RightChop(FString(TEXT("EFoliageScaling::")).Len()), *Range(Type.ScaleX, TEXT(""))));
		Parts.Add(Type.AlignToNormal ? FString::Printf(TEXT("AlignToNormal up to %g deg"), Type.AlignMaxAngle) : FString(TEXT("upright")));
		if (Type.RandomYaw) Parts.Add(TEXT("RandomYaw"));
		if (Type.RandomPitchAngle > 0.f) Parts.Add(FString::Printf(TEXT("RandomPitchAngle %g"), Type.RandomPitchAngle));
		Parts.Add(TEXT("GroundSlopeAngle ") + Range(Type.GroundSlopeAngle, TEXT(" deg")));
		if (Type.Height.Min > -NoHeightLimit || Type.Height.Max < NoHeightLimit) Parts.Add(TEXT("Height ") + Range(Type.Height, TEXT(" cm")));
		if (!FMath::IsNearlyZero(Type.ZOffset.Min) || !FMath::IsNearlyZero(Type.ZOffset.Max)) Parts.Add(TEXT("ZOffset ") + Range(Type.ZOffset, TEXT(" cm")));
		auto Layers = [](const TArray<FName>& Names)
		{
			TArray<FString> Out;
			for (const FName& Name : Names)
			{
				if (Name != NAME_None) Out.Add(Name.ToString());
			}
			return FString::Join(Out, TEXT(", "));
		};
		if (!Layers(Type.LandscapeLayers).IsEmpty())
		{
			Parts.Add(FString::Printf(TEXT("LandscapeLayers %s (MinimumLayerWeight %g)"), *Layers(Type.LandscapeLayers), Type.MinimumLayerWeight));
		}
		if (!Layers(Type.ExclusionLandscapeLayers).IsEmpty()) Parts.Add(TEXT("ExclusionLandscapeLayers ") + Layers(Type.ExclusionLandscapeLayers));
		if (Type.CollisionWithWorld) Parts.Add(TEXT("CollisionWithWorld"));
		Parts.Add(Type.CullDistance.Max > 0 ? FString::Printf(TEXT("CullDistance %d to %d cm"), Type.CullDistance.Min, Type.CullDistance.Max) : FString(TEXT("never culled")));
		Parts.Add(TEXT("collision ") + Type.BodyInstance.GetCollisionProfileName().ToString());
		if (!Type.CastShadow) Parts.Add(TEXT("no shadows"));
		return FString::Join(Parts, TEXT(", "));
	}

	/** Where a scatter or an erase works: a circle when Radius is above 0, otherwise a rectangle. Distances in cm. */
	struct FRegion
	{
		FVector2D Center = FVector2D::ZeroVector;
		double Radius = 0.0;
		FVector2D Half = FVector2D::ZeroVector;
		FString Name;

		bool IsCircle() const { return Radius > 0.0; }
		double Area() const { return IsCircle() ? UE_DOUBLE_PI * Radius * Radius : 4.0 * Half.X * Half.Y; }

		FBox Bounds(const double Grow = 0.0) const
		{
			const FVector2D Reach = (IsCircle() ? FVector2D(Radius, Radius) : Half) + FVector2D(Grow, Grow);
			return FBox(FVector(Center - Reach, -TraceReach), FVector(Center + Reach, TraceReach));
		}

		bool Contains(const FVector2D& Point) const
		{
			return IsCircle() ? FVector2D::DistSquared(Point, Center) <= Radius * Radius
				: FMath::Abs(Point.X - Center.X) <= Half.X && FMath::Abs(Point.Y - Center.Y) <= Half.Y;
		}

		/** A spot drawn evenly over the region. */
		FVector2D Sample(FRandomStream& Stream) const
		{
			if (IsCircle())
			{
				const double Distance = Radius * FMath::Sqrt(Stream.FRand());
				const double Angle = Stream.FRand() * 2.0 * UE_DOUBLE_PI;
				return Center + FVector2D(Distance * FMath::Cos(Angle), Distance * FMath::Sin(Angle));
			}
			return Center + FVector2D((Stream.FRand() * 2.0 - 1.0) * Half.X, (Stream.FRand() * 2.0 - 1.0) * Half.Y);
		}
	};

	bool ResolveRegion(UWorld& World, const FHyperAIFoliageOp& Op, FRegion& Out, FString& OutError)
	{
		Out.Center = FVector2D(Op.Center);
		if (Op.Radius > 0.f)
		{
			Out.Radius = Op.Radius * 100.0;
			Out.Name = FString::Printf(TEXT("%g m around (%.0f, %.0f)"), Op.Radius, Op.Center.X, Op.Center.Y);
			return true;
		}
		if (Op.Size.X > 0.0 && Op.Size.Y > 0.0)
		{
			Out.Half = Op.Size * 50.0;
			Out.Name = FString::Printf(TEXT("%g x %g m at (%.0f, %.0f)"), Op.Size.X, Op.Size.Y, Op.Center.X, Op.Center.Y);
			return true;
		}
		if (!Op.Landscape.TrimStartAndEnd().IsEmpty())
		{
			const ALandscape* Landscape = FindLandscape(World, Op.Landscape);
			const ULandscapeInfo* Info = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
			const FBox Bounds = Info ? Info->GetLoadedBounds() : FBox(ForceInit);
			if (!Bounds.IsValid)
			{
				OutError = FString::Printf(TEXT("No loaded landscape is labelled %s."), *Op.Landscape);
				return false;
			}
			Out.Center = FVector2D(Bounds.GetCenter());
			Out.Half = FVector2D(Bounds.GetExtent());
			Out.Name = TEXT("landscape ") + Landscape->GetActorLabel();
			return true;
		}
		OutError = TEXT("Give the region: Center and Radius (a circle), Center and Size (a rectangle), or a Landscape.");
		return false;
	}

	struct FSurfaces
	{
		bool bLandscape = true;
		bool bStaticMesh = false;
		bool bBSP = false;
		bool bFoliage = false;
		bool bTranslucent = false;
	};

	bool ParseSurfaces(const TArray<FString>& Names, FSurfaces& Out, FString& OutError)
	{
		if (Names.IsEmpty()) return true;
		Out.bLandscape = false;
		for (const FString& Raw : Names)
		{
			const FString Name = Raw.TrimStartAndEnd().ToLower().Replace(TEXT(" "), TEXT("_"));
			if (Name == TEXT("landscape")) Out.bLandscape = true;
			else if (Name == TEXT("static_mesh") || Name == TEXT("staticmesh") || Name == TEXT("mesh")) Out.bStaticMesh = true;
			else if (Name == TEXT("bsp") || Name == TEXT("brush")) Out.bBSP = true;
			else if (Name == TEXT("foliage")) Out.bFoliage = true;
			else if (Name == TEXT("translucent")) Out.bTranslucent = true;
			else
			{
				OutError = FString::Printf(TEXT("'%s' is not a surface: landscape, static_mesh, bsp, foliage, translucent."), *Raw);
				return false;
			}
		}
		if (!Out.bLandscape && !Out.bStaticMesh && !Out.bBSP && !Out.bFoliage)
		{
			OutError = TEXT("Surfaces names nothing to stand on: landscape, static_mesh, bsp or foliage.");
			return false;
		}
		return true;
	}

	bool IsFoliage(const UPrimitiveComponent& Component)
	{
		return Component.IsA<UFoliageInstancedStaticMeshComponent>() || (Component.GetOwner() && FFoliageHelper::IsOwnedByFoliage(Component.GetOwner()));
	}

	/** Epic's paint filter: whether foliage may stand on the component. */
	bool Allows(const FSurfaces& Surfaces, const UPrimitiveComponent* Component)
	{
		if (!Component) return false;
		const bool bFoliage = IsFoliage(*Component);
		bool bAllowed = (Surfaces.bLandscape && Component->IsA<ULandscapeHeightfieldCollisionComponent>())
			|| (Surfaces.bStaticMesh && Component->IsA<UStaticMeshComponent>() && !bFoliage)
			|| (Surfaces.bBSP && (Component->IsA<UBrushComponent>() || Component->IsA<UModelComponent>()))
			|| (Surfaces.bFoliage && bFoliage);
		if (bAllowed && !Surfaces.bTranslucent)
		{
			const UMaterialInterface* Material = Component->GetMaterial(0);
			bAllowed = !(Material && IsTranslucentBlendMode(*Material));
		}
		return bAllowed;
	}

	/** Anything solid overhead that is not foliage: a roof, a bridge, a road laid on the ground. */
	bool IsCover(const UPrimitiveComponent* Component)
	{
		return Component && !IsFoliage(*Component)
			&& (Component->IsA<ULandscapeHeightfieldCollisionComponent>() || Component->IsA<UStaticMeshComponent>() || Component->IsA<UModelComponent>());
	}

	enum class EGround : uint8 { Found, None, Covered };

	FDesiredFoliageInstance DesiredAt(const FVector2D& Where, const UFoliageType& Type)
	{
		return FDesiredFoliageInstance(FVector(Where, TraceReach), FVector(Where, -TraceReach), &Type);
	}

	/** The surface a spot's instance would stand on, traced down as Epic's paint tool traces. */
	EGround Probe(const UWorld& World, const UFoliageType& Type, const FVector2D& Where, const FSurfaces& Surfaces, const bool bUnderCover, FHitResult& OutHit)
	{
		static const FName TraceTag(TEXT("HyperAIFoliage"));
		const FDesiredFoliageInstance Desired = DesiredAt(Where, Type);
		if (!AInstancedFoliageActor::FoliageTrace(&World, OutHit, Desired, TraceTag, /*InbReturnFaceIndex=*/true,
			[&Surfaces](const UPrimitiveComponent* Component) { return Allows(Surfaces, Component); }, /*bAverageNormal=*/true))
		{
			return EGround::None;
		}
		FHitResult Top;
		if (!bUnderCover && AInstancedFoliageActor::FoliageTrace(&World, Top, Desired, TraceTag, /*InbReturnFaceIndex=*/false,
			[](const UPrimitiveComponent* Component) { return IsCover(Component); })
			&& Top.GetComponent() != OutHit.GetComponent() && Top.ImpactPoint.Z > OutHit.ImpactPoint.Z + 1.0)
		{
			return EGround::Covered;
		}
		return EGround::Found;
	}

	using FLayerCache = TMap<FName, TMap<ULandscapeComponent*, TArray<uint8>>>;

	/** The strongest of the named paint layers where a landscape was hit; false off landscapes or when no layer is named. */
	bool LayerWeight(const FHitResult& Hit, const TArray<FName>& Layers, FLayerCache& Cache, float& OutWeight)
	{
		if (!Layers.ContainsByPredicate([](const FName& Name) { return Name != NAME_None; })) return false;
		const ULandscapeHeightfieldCollisionComponent* Collision = Cast<ULandscapeHeightfieldCollisionComponent>(Hit.GetComponent());
		ULandscapeComponent* Render = Collision ? Collision->GetRenderComponent() : nullptr;
		const ULandscapeInfo* Info = Render ? Render->GetLandscapeInfo() : nullptr;
		if (!Info) return false;
		float Strongest = 0.f;
		for (const FName& Layer : Layers)
		{
			if (Layer == NAME_None) continue;
			TArray<uint8>& LayerCache = Cache.FindOrAdd(Layer).FindOrAdd(Render);
			Strongest = FMath::Max(Strongest, Render->GetLayerWeightAtLocation(Hit.ImpactPoint, Info->GetLayerInfoByName(Layer), &LayerCache));
		}
		OutWeight = Strongest;
		return true;
	}

	bool IsWithinSlope(const double NormalZ, const FFloatInterval& Slope)
	{
		// Epic's paint check, with its small tolerance for normals not quite of unit length.
		const double Steepest = FMath::Cos(FMath::DegreesToRadians(Slope.Max));
		const double Flattest = FMath::Cos(FMath::DegreesToRadians(Slope.Min));
		return !(Steepest > NormalZ + UE_SMALL_NUMBER || Flattest < NormalZ - UE_SMALL_NUMBER);
	}

	/** Instance locations in a grid, to keep a type's Radius between them. */
	class FSpacing
	{
	public:
		explicit FSpacing(const double InRadius) : Radius(InRadius), Cell(FMath::Max(InRadius, 50.0)) {}

		bool IsClear(const FVector& Point) const
		{
			if (Radius <= 0.0) return true;
			const FIntPoint Key = KeyOf(Point);
			for (int32 X = -1; X <= 1; ++X)
			{
				for (int32 Y = -1; Y <= 1; ++Y)
				{
					const TArray<FVector>* Points = Cells.Find(Key + FIntPoint(X, Y));
					if (Points && Points->ContainsByPredicate([this, &Point](const FVector& Other) { return FVector::DistSquared(Other, Point) < Radius * Radius; }))
					{
						return false;
					}
				}
			}
			return true;
		}

		void Add(const FVector& Point)
		{
			if (Radius > 0.0) Cells.FindOrAdd(KeyOf(Point)).Add(Point);
		}

	private:
		FIntPoint KeyOf(const FVector& Point) const
		{
			return FIntPoint(FMath::FloorToInt32(Point.X / Cell), FMath::FloorToInt32(Point.Y / Cell));
		}

		double Radius;
		double Cell;
		TMap<FIntPoint, TArray<FVector>> Cells;
	};

	/** Why the spots a scatter or a placement tried did or did not take an instance. */
	struct FTally
	{
		int32 Tried = 0;
		int32 Existing = 0;
		int32 Placed = 0;
		int32 NoGround = 0;
		int32 Covered = 0;
		int32 Height = 0;
		int32 Slope = 0;
		int32 Layer = 0;
		int32 Crowded = 0;
		int32 Clearing = 0;
		int32 Blocked = 0;
	};

	FString Describe(const UFoliageType& Type, const FTally& Tally, const FSurfaces& Surfaces, const bool bScatter)
	{
		TArray<FString> Away;
		auto Count = [&Away](const int32 Number, const TCHAR* Why)
		{
			if (Number > 0) Away.Add(FString::Printf(TEXT("%d %s"), Number, Why));
		};
		Count(Tally.NoGround, TEXT("found nothing to stand on"));
		Count(Tally.Covered, TEXT("were under cover"));
		Count(Tally.Height, TEXT("were outside its Height"));
		Count(Tally.Slope, TEXT("were steeper than its GroundSlopeAngle"));
		Count(Tally.Layer, TEXT("were off its landscape layers"));
		Count(Tally.Crowded, TEXT("were within its Radius of another"));
		Count(Tally.Clearing, TEXT("were in a clearing"));
		Count(Tally.Blocked, TEXT("met the world (CollisionWithWorld)"));
		FString Note = bScatter
			? FString::Printf(TEXT("%s: %d placed from %d spots tried"), *Type.GetName(), Tally.Placed, Tally.Tried)
			: FString::Printf(TEXT("%s: %d placed at %d points"), *Type.GetName(), Tally.Placed, Tally.Tried);
		if (Tally.Existing > 0) Note += FString::Printf(TEXT(", %d there already"), Tally.Existing);
		if (!Away.IsEmpty()) Note += TEXT("; turned away: ") + FString::Join(Away, TEXT(", "));
		if (Tally.Placed == 0 && Tally.NoGround * 2 >= Tally.Tried && Tally.NoGround > 0)
		{
			Note += Surfaces.bStaticMesh || Surfaces.bBSP
				? TEXT(". The region has no surface under it that Surfaces allows")
				: TEXT(". The region has no landscape under it; add static_mesh or bsp to Surfaces to stand on meshes");
		}
		return Note + TEXT(".");
	}

	/** Instances into the foliage actor that owns each spot (the level's, or a World Partition cell's), made where missing. */
	int32 Spawn(UWorld& World, const UFoliageType& Type, const TArray<FFoliageInstance>& Placed)
	{
		TMap<AInstancedFoliageActor*, TArray<const FFoliageInstance*>> PerActor;
		for (const FFoliageInstance& Instance : Placed)
		{
			ULevel* Level = Instance.BaseComponent ? Instance.BaseComponent->GetComponentLevel() : World.GetCurrentLevel();
			if (AInstancedFoliageActor* Actor = AInstancedFoliageActor::Get(&World, /*bCreateIfNone=*/true, Level, Instance.Location))
			{
				PerActor.FindOrAdd(Actor).Add(&Instance);
			}
		}
		int32 Added = 0;
		for (TPair<AInstancedFoliageActor*, TArray<const FFoliageInstance*>>& Pair : PerActor)
		{
			FFoliageInfo* Info = nullptr;
			const UFoliageType* Used = Pair.Key->AddFoliageType(&Type, &Info);
			if (!Used || !Info) continue;
			Info->AddInstances(Used, Pair.Value);
			Info->Refresh(/*Async=*/true, /*Force=*/false);
			Added += Pair.Value.Num();
		}
		return Added;
	}

	/** One type over a region: spots drawn at its density, each kept only where the type's own rules and the op's allow. */
	FTally Scatter(UWorld& World, const UFoliageType& Type, const FHyperAIFoliageOp& Op, const FRegion& Region, const FSurfaces& Surfaces, const int32 Salt)
	{
		FTally Tally;
		const double Density = Op.Density > 0.f ? Op.Density : Type.Density;
		Tally.Tried = FMath::RoundToInt32(Region.Area() / DensityArea * Density);
		FSpacing Taken(Type.Radius);
		// What stands in the region already counts toward the density and keeps its spacing.
		const FBox Near = Region.Bounds(Type.Radius);
		for (TActorIterator<AInstancedFoliageActor> It(&World); It; ++It)
		{
			const FFoliageInfo* Info = It->FindInfo(&Type);
			if (!Info) continue;
			for (const FFoliageInstance& Instance : Info->Instances)
			{
				if (!Near.IsInsideOrOn(Instance.Location)) continue;
				Taken.Add(Instance.Location);
				if (Region.Contains(FVector2D(Instance.Location))) ++Tally.Existing;
			}
		}
		FRandomStream Stream(static_cast<int32>(static_cast<uint32>(Op.Seed) * 7919u + static_cast<uint32>(Salt)));
		FLayerCache Cache;
		TArray<FPotentialInstance> Spots;
		int32 Eligible = 0;
		for (int32 Index = 0; Index < Tally.Tried; ++Index)
		{
			const FVector2D Where = Region.Sample(Stream);
			const float Roll = Stream.FRand();
			if (Op.Clearings.ContainsByPredicate([&Where](const FHyperAIFoliageClearing& Clearing)
				{
					return FVector2D::DistSquared(Where, FVector2D(Clearing.Center)) < FMath::Square(Clearing.Radius * 100.0);
				}))
			{
				++Tally.Clearing;
				continue;
			}
			FHitResult Hit;
			const EGround Ground = Probe(World, Type, Where, Surfaces, Op.bUnderCover, Hit);
			if (Ground != EGround::Found)
			{
				++(Ground == EGround::None ? Tally.NoGround : Tally.Covered);
				continue;
			}
			if (!Type.Height.Contains(static_cast<float>(Hit.ImpactPoint.Z)))
			{
				++Tally.Height;
				continue;
			}
			if (!IsWithinSlope(Hit.ImpactNormal.Z, Type.GroundSlopeAngle))
			{
				++Tally.Slope;
				continue;
			}
			// Epic's layer test: thinned at random in proportion to a weak layer, refused on an excluded one.
			float Weight = 1.f;
			float Excluded = 0.f;
			if ((LayerWeight(Hit, Type.LandscapeLayers, Cache, Weight) && Weight < FMath::Max(UE_SMALL_NUMBER, FMath::Max(Type.MinimumLayerWeight, Roll)))
				|| (LayerWeight(Hit, Type.ExclusionLandscapeLayers, Cache, Excluded) && Excluded >= FMath::Max(UE_SMALL_NUMBER, Type.MinimumExclusionLayerWeight)))
			{
				++Tally.Layer;
				continue;
			}
			++Eligible;
			if (!Taken.IsClear(Hit.ImpactPoint))
			{
				++Tally.Crowded;
				continue;
			}
			Taken.Add(Hit.ImpactPoint);
			Spots.Emplace(Hit.ImpactPoint, Hit.ImpactNormal, Hit.GetComponent(), Weight, DesiredAt(Where, Type));
		}
		// The region holds one instance per eligible spot at this density: top it up to that and no further.
		const int32 Wanted = FMath::Clamp(Eligible - Tally.Existing, 0, Spots.Num());
		TArray<FFoliageInstance> Placed;
		Placed.Reserve(Wanted);
		for (int32 Index = 0; Index < Wanted; ++Index)
		{
			FFoliageInstance Instance;
			if (!Spots[Index].PlaceInstance(&World, &Type, Instance))
			{
				++Tally.Blocked;
				continue;
			}
			Instance.BaseComponent = Spots[Index].HitComponent;
			Placed.Add(MoveTemp(Instance));
		}
		Tally.Placed = Spawn(World, Type, Placed);
		return Tally;
	}

	/** One instance per point, dropped onto the surface below; the type's scale, rotation and alignment still apply. */
	FTally Place(UWorld& World, const UFoliageType& Type, const FHyperAIFoliageOp& Op, const FSurfaces& Surfaces)
	{
		FTally Tally;
		Tally.Tried = Op.Points.Num();
		TArray<FFoliageInstance> Placed;
		for (const FVector& Point : Op.Points)
		{
			FHitResult Hit;
			const EGround Ground = Probe(World, Type, FVector2D(Point), Surfaces, Op.bUnderCover, Hit);
			if (Ground != EGround::Found)
			{
				++(Ground == EGround::None ? Tally.NoGround : Tally.Covered);
				continue;
			}
			FPotentialInstance Spot(Hit.ImpactPoint, Hit.ImpactNormal, Hit.GetComponent(), 1.f, DesiredAt(FVector2D(Point), Type));
			FFoliageInstance Instance;
			if (!Spot.PlaceInstance(&World, &Type, Instance))
			{
				++Tally.Blocked;
				continue;
			}
			Instance.BaseComponent = Spot.HitComponent;
			Placed.Add(MoveTemp(Instance));
		}
		Tally.Placed = Spawn(World, Type, Placed);
		return Tally;
	}

	/** The region's instances of the types (every type when none is named), all or a random share of them. */
	int32 Erase(UWorld& World, const TArray<UFoliageType*>& Types, const FRegion& Region, const float Fraction, const int32 Seed)
	{
		FRandomStream Stream(Seed);
		const FBox Bounds = Region.Bounds();
		int32 Removed = 0;
		for (TActorIterator<AInstancedFoliageActor> It(&World); It; ++It)
		{
			It->ForEachFoliageInfo([&Types, &Region, &Bounds, &Stream, &Removed, Fraction](UFoliageType* Type, FFoliageInfo& Info)
			{
				if (!Types.IsEmpty() && !Types.Contains(Type)) return true;
				TArray<int32> Inside;
				for (int32 Index = 0; Index < Info.Instances.Num(); ++Index)
				{
					const FVector& Location = Info.Instances[Index].Location;
					if (Bounds.IsInsideOrOn(Location) && Region.Contains(FVector2D(Location))) Inside.Add(Index);
				}
				if (Fraction < 1.f)
				{
					for (int32 Index = Inside.Num() - 1; Index > 0; --Index)
					{
						Inside.Swap(Index, Stream.RandRange(0, Index));
					}
					Inside.SetNum(FMath::RoundToInt32(Inside.Num() * Fraction));
				}
				if (!Inside.IsEmpty())
				{
					Info.RemoveInstances(Inside, /*RebuildFoliageTree=*/true);
					Removed += Inside.Num();
				}
				return true;
			});
		}
		return Removed;
	}

	/** The type out of every loaded foliage actor, its instances with it. */
	int32 RemoveType(UWorld& World, UFoliageType* Type)
	{
		int32 Removed = 0;
		for (TActorIterator<AInstancedFoliageActor> It(&World); It; ++It)
		{
			if (const FFoliageInfo* Info = It->FindInfo(Type))
			{
				Removed += Info->Instances.Num();
				UFoliageType* ToRemove = Type;
				It->RemoveFoliageType(&ToRemove, 1);
			}
		}
		return Removed;
	}

	/** A type as the plan will have left it by then: made or changed earlier in the plan, or an asset. */
	UFoliageType* Resolve(const TMap<FString, UFoliageType*>& Planned, const FString& Reference, FString& OutError)
	{
		const FString Wanted = PackageOf(Reference).ToLower();
		if (UFoliageType* const* Scratch = Planned.Find(Wanted)) return *Scratch;
		for (const TPair<FString, UFoliageType*>& Pair : Planned)
		{
			if (FPackageName::GetShortName(Pair.Key) == Wanted) return Pair.Value;
		}
		return FindType(Reference, OutError);
	}

	/** The key a type is planned under: its package path, lower case. */
	FString PlanKey(const TMap<FString, UFoliageType*>& Planned, const UFoliageType& Type)
	{
		for (const TPair<FString, UFoliageType*>& Pair : Planned)
		{
			if (Pair.Value == &Type) return Pair.Key;
		}
		return Type.GetOutermost()->GetName().ToLower();
	}

	bool Check(UWorld& World, const FHyperAIFoliagePlanRequest& Request, FString& OutError)
	{
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		// Types made or changed so far, as scratch copies holding the plan's settings, by lower-case package path.
		TMap<FString, UFoliageType*> Planned;
		int64 Spots = 0;
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIFoliageOp& Op = Request.Operations[Index];
			const FString Kind = KindOf(Op);
			auto Fail = [&OutError, &Op, Index](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Kind, *Message);
				return false;
			};
			FString Why;
			UStaticMesh* Mesh = nullptr;
			if (!Op.Mesh.TrimStartAndEnd().IsEmpty())
			{
				Mesh = Load<UStaticMesh>(Op.Mesh);
				if (!Mesh) return Fail(FString::Printf(TEXT("%s is not a static mesh."), *Op.Mesh));
			}
			if (Kind == TEXT("foliage_type.create"))
			{
				if (Op.Types.Num() != 1) return Fail(TEXT("Name the one new foliage type in Types, as /Game/Folder/FT_Name."));
				const FString Path = PackageOf(Op.Types[0]);
				if (!Path.StartsWith(TEXT("/Game/")) || !FHyperAIStudioNames::IsPackagePath(Path, nullptr))
				{
					return Fail(TEXT("A new foliage type goes under /Game, as /Game/Folder/FT_Name."));
				}
				FString Ignored;
				// Free in memory too: an unsaved asset there of another class would stop the editor when the type is made.
				if (Planned.Contains(Path.ToLower()) || FindType(Path, Ignored) || !FHyperAIStudioNames::IsFreePackage(Path))
				{
					return Fail(FString::Printf(TEXT("%s exists already; foliage_type.set changes it."), *Path));
				}
				if (!Mesh) return Fail(TEXT("Give the static Mesh the type places."));
				UFoliageType_InstancedStaticMesh* Scratch = NewObject<UFoliageType_InstancedStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
				if (!ApplySettings(*Scratch, SettingsOf(Op, Mesh), /*bNotify=*/false, Why)) return Fail(Why);
				Planned.Add(Path.ToLower(), Scratch);
				continue;
			}
			TArray<UFoliageType*> Types;
			for (const FString& Reference : Op.Types)
			{
				UFoliageType* Type = Resolve(Planned, Reference, Why);
				if (!Type) return Fail(Why);
				Types.AddUnique(Type);
			}
			if (Kind == TEXT("foliage_type.set"))
			{
				if (Types.Num() != 1) return Fail(TEXT("Name the one foliage type to change in Types."));
				if (Op.Settings.IsEmpty() && !Mesh) return Fail(TEXT("Give the Settings to change, or a new Mesh."));
				if (Mesh && !Types[0]->IsA<UFoliageType_InstancedStaticMesh>()) return Fail(TEXT("Only a static mesh foliage type takes a Mesh."));
				UFoliageType* Scratch = NewObject<UFoliageType>(GetTransientPackage(), Types[0]->GetClass(), NAME_None, RF_Transient, Types[0]);
				if (!ApplySettings(*Scratch, SettingsOf(Op, Mesh), /*bNotify=*/false, Why)) return Fail(Why);
				Planned.Add(PlanKey(Planned, *Types[0]), Scratch);
			}
			else if (Kind == TEXT("foliage_type.remove"))
			{
				if (Types.IsEmpty()) return Fail(TEXT("Name the Types to take out of the level."));
			}
			else if (Kind == TEXT("foliage.scatter") || Kind == TEXT("foliage.place") || Kind == TEXT("foliage.erase"))
			{
				FSurfaces Surfaces;
				if (!ParseSurfaces(Op.Surfaces, Surfaces, Why)) return Fail(Why);
				if (Kind != TEXT("foliage.erase"))
				{
					if (Types.IsEmpty()) return Fail(TEXT("Name the foliage Types to place."));
					for (const UFoliageType* Type : Types)
					{
						const UFoliageType_InstancedStaticMesh* Placing = Cast<UFoliageType_InstancedStaticMesh>(Type);
						if (!Placing || !Placing->GetStaticMesh())
						{
							return Fail(FString::Printf(TEXT("%s places no static mesh; foliage_type.set gives it one."), *NameOf(*Type)));
						}
					}
				}
				if (Kind == TEXT("foliage.place"))
				{
					if (Types.Num() != 1) return Fail(TEXT("foliage.place places one type; use one op per type."));
					if (Op.Points.IsEmpty()) return Fail(TEXT("Give the Points to place at."));
					Spots += Op.Points.Num();
				}
				else
				{
					FRegion Region;
					if (!ResolveRegion(World, Op, Region, Why)) return Fail(Why);
					if (Kind == TEXT("foliage.erase"))
					{
						if (!(Op.Fraction > 0.f && Op.Fraction <= 1.f)) return Fail(TEXT("Fraction is above 0 and at most 1."));
					}
					else
					{
						if (!(Op.Density >= 0.f)) return Fail(TEXT("Density is 0 (each type's own) or more."));
						if (Op.Clearings.ContainsByPredicate([](const FHyperAIFoliageClearing& Clearing) { return !(Clearing.Radius > 0.f); }))
						{
							return Fail(TEXT("Each clearing has a Radius above 0, in meters."));
						}
						for (const UFoliageType* Type : Types)
						{
							const double Density = Op.Density > 0.f ? Op.Density : Type->Density;
							if (!(Density > 0.0)) return Fail(FString::Printf(TEXT("%s has Density 0; give the op or the type a Density."), *NameOf(*Type)));
							Spots += FMath::CeilToInt64(Region.Area() / DensityArea * Density);
						}
					}
				}
				if (Spots > C::MaxSpots)
				{
					return Fail(FString::Printf(TEXT("The plan would try %lld spots and the most is %d; lower the Density or split the region."), Spots, C::MaxSpots));
				}
			}
			else
			{
				return Fail(TEXT("Unknown op; see hyper_foliage_apply_plan's description."));
			}
		}
		return true;
	}

	/** One op, inside the plan's undo step. */
	bool Apply(UWorld& World, const FHyperAIFoliageOp& Op, FHyperAIFoliagePlanReport& Report, TArray<UObject*>& Assets, FString& OutError)
	{
		const FString Kind = KindOf(Op);
		UStaticMesh* Mesh = Op.Mesh.TrimStartAndEnd().IsEmpty() ? nullptr : Load<UStaticMesh>(Op.Mesh);
		if (Kind == TEXT("foliage_type.create"))
		{
			const FString Path = PackageOf(Op.Types[0]);
			UPackage* Package = CreatePackage(*Path);
			UFoliageType_InstancedStaticMesh* Type = NewObject<UFoliageType_InstancedStaticMesh>(Package, *FPackageName::GetShortName(Path),
				RF_Public | RF_Standalone | RF_Transactional);
			// A new asset takes its settings outside the undo step: undo leaves it whole rather than blank.
			if (!ApplySettings(*Type, SettingsOf(Op, Mesh), /*bNotify=*/false, OutError)) return false;
			FAssetRegistryModule::AssetCreated(Type);
			Package->MarkPackageDirty();
			Assets.AddUnique(Type);
			Report.Notes.Add(FString::Printf(TEXT("%s made: %s."), *Type->GetPathName(), *Summary(*Type)));
			return true;
		}
		TArray<UFoliageType*> Types;
		for (const FString& Reference : Op.Types)
		{
			UFoliageType* Type = FindType(Reference, OutError);
			if (!Type) return false;
			Types.AddUnique(Type);
		}
		if (Kind == TEXT("foliage_type.set"))
		{
			UFoliageType* Type = Types[0];
			Type->Modify();
			if (!ApplySettings(*Type, SettingsOf(Op, Mesh), /*bNotify=*/true, OutError)) return false;
			if (Type->IsAsset()) Assets.AddUnique(Type);
			Report.Notes.Add(FString::Printf(TEXT("%s set: %s."), *NameOf(*Type), *Summary(*Type)));
			return true;
		}
		if (Kind == TEXT("foliage_type.remove"))
		{
			for (UFoliageType* Type : Types)
			{
				const int32 Removed = RemoveType(World, Type);
				Report.Removed += Removed;
				Report.Notes.Add(FString::Printf(TEXT("%s taken out of the level with its %d instance(s)."), *Type->GetName(), Removed));
			}
			return true;
		}
		FSurfaces Surfaces;
		if (!ParseSurfaces(Op.Surfaces, Surfaces, OutError)) return false;
		if (Kind == TEXT("foliage.place"))
		{
			const FTally Tally = Place(World, *Types[0], Op, Surfaces);
			Report.Placed += Tally.Placed;
			Report.Notes.Add(Describe(*Types[0], Tally, Surfaces, /*bScatter=*/false));
			return true;
		}
		FRegion Region;
		if (!ResolveRegion(World, Op, Region, OutError)) return false;
		if (Kind == TEXT("foliage.erase"))
		{
			const int32 Removed = Erase(World, Types, Region, Op.Fraction, Op.Seed);
			Report.Removed += Removed;
			Report.Notes.Add(FString::Printf(TEXT("%d instance(s) erased in %s."), Removed, *Region.Name));
			return true;
		}
		for (int32 Index = 0; Index < Types.Num(); ++Index)
		{
			const FTally Tally = Scatter(World, *Types[Index], Op, Region, Surfaces, Index);
			Report.Placed += Tally.Placed;
			Report.Notes.Add(Describe(*Types[Index], Tally, Surfaces, /*bScatter=*/true) + TEXT(" Region: ") + Region.Name + TEXT("."));
		}
		return true;
	}

	/** The plan for real: every op as one undo step, then the types it made or changed saved, and the level if asked. */
	FHyperAIFoliagePlanReport Execute(const FHyperAIFoliagePlanRequest& Request, const FString& OperationId)
	{
		FHyperAIFoliagePlanReport Report;
		Report.OperationId = OperationId;
		UWorld* World = EditorWorld();
		if (!World)
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("No level is open in the editor.");
			return Report;
		}
		TArray<UObject*> Assets;
		{
			const FScopedTransaction Transaction(LOCTEXT("FoliagePlan", "HyperAI: Edit Foliage"));
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				FString Error;
				if (!Apply(*World, Request.Operations[Index], Report, Assets, Error))
				{
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Request.Operations[Index].Kind, *Error,
						Report.Applied > 0 ? TEXT(" The operations before it stay applied, unsaved; Edit > Undo takes the plan back.") : TEXT(""));
					return Report;
				}
				++Report.Applied;
			}
		}
		for (UObject* Asset : Assets)
		{
			FString Error;
			if (!FHyperAIStudioSourceControl::SaveAsset(*Asset, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Asset->GetPathName());
		}
		if (Request.bSave)
		{
			if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/false))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = TEXT("The level could not be saved; the foliage is changed in memory.");
				return Report;
			}
			Report.Saved.Add(World->GetOutermost()->GetName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied: %d instance(s) placed, %d removed%s."), Report.Applied, Report.Placed, Report.Removed,
			Request.bSave ? TEXT(", saved") : TEXT("; the level is unsaved"));
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIFoliagePlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Notes.IsEmpty() ? FString() : Done.Notes[0].Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
}

FHyperAIFoliageInspectReport FHyperAIStudioFoliageContracts::Inspect(const FHyperAIFoliageInspectRequest& Request)
{
	using namespace HyperAIStudio::FoliageTools::Private;
	FHyperAIFoliageInspectReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	UWorld* World = EditorWorld();
	if (!World)
	{
		Report.Diagnostic = TEXT("No level is open in the editor.");
		return Report;
	}
	Report.bWorldPartition = World->IsPartitionedWorld();
	TArray<UFoliageType*> Wanted;
	for (const FString& Reference : Request.Types)
	{
		FString Error;
		UFoliageType* Type = FindType(Reference, Error);
		if (!Type)
		{
			Report.Diagnostic = Error;
			return Report;
		}
		Wanted.AddUnique(Type);
	}
	struct FUse
	{
		int32 Instances = 0;
		int32 Actors = 0;
		FBox Bounds = FBox(ForceInit);
	};
	TMap<UFoliageType*, FUse> Uses;
	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		++Report.FoliageActors;
		It->ForEachFoliageInfo([&Uses, &Wanted](UFoliageType* Type, FFoliageInfo& Info)
		{
			if (Type && (Wanted.IsEmpty() || Wanted.Contains(Type)))
			{
				FUse& Use = Uses.FindOrAdd(Type);
				Use.Instances += Info.Instances.Num();
				++Use.Actors;
				for (const FFoliageInstance& Instance : Info.Instances)
				{
					Use.Bounds += Instance.Location;
				}
			}
			return true;
		});
	}
	for (UFoliageType* Type : Wanted)
	{
		Uses.FindOrAdd(Type);
	}
	for (const TPair<UFoliageType*, FUse>& Pair : Uses)
	{
		FHyperAIFoliageTypeInfo& Info = Report.Types.AddDefaulted_GetRef();
		Info.Type = NameOf(*Pair.Key);
		const UObject* Source = Pair.Key->GetSource();
		Info.Mesh = Source ? Source->GetPathName() : FString();
		Info.Instances = Pair.Value.Instances;
		Info.FoliageActors = Pair.Value.Actors;
		if (Pair.Value.Bounds.IsValid)
		{
			Info.Min = Pair.Value.Bounds.Min;
			Info.Max = Pair.Value.Bounds.Max;
		}
		Info.Summary = Summary(*Pair.Key);
		if (Request.bSettings) Info.Settings = ExportSettings(*Pair.Key);
		Report.Instances += Info.Instances;
	}
	Report.Types.Sort([](const FHyperAIFoliageTypeInfo& A, const FHyperAIFoliageTypeInfo& B) { return A.Type < B.Type; });
	const FString Folder = Request.Folder.TrimStartAndEnd().IsEmpty() ? FString(TEXT("/Game")) : Request.Folder.TrimStartAndEnd();
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(UFoliageType::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
	int32 Unlisted = 0;
	for (const FAssetData& Asset : Assets)
	{
		const FString Path = Asset.GetObjectPathString();
		if (!Path.StartsWith(Folder + TEXT("/")) || Report.Types.ContainsByPredicate([&Path](const FHyperAIFoliageTypeInfo& Info) { return Info.Type == Path; }))
		{
			continue;
		}
		if (Report.Assets.Num() < 50) Report.Assets.Add(Path);
		else ++Unlisted;
	}
	Report.Assets.Sort();
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%d foliage type(s) with %d instance(s) in %d foliage actor(s)%s; %d unused foliage type asset(s) under %s%s."),
		Report.Types.Num(), Report.Instances, Report.FoliageActors, Report.bWorldPartition ? TEXT(" (loaded cells, World Partition)") : TEXT(""),
		Report.Assets.Num() + Unlisted, *Folder, Unlisted > 0 ? *FString::Printf(TEXT(", %d not listed"), Unlisted) : TEXT(""));
	return Report;
}

FHyperAIFoliagePlanReport FHyperAIStudioFoliageContracts::ApplyPlan(const FHyperAIFoliagePlanRequest& Request)
{
	using namespace HyperAIStudio::FoliageTools::Private;
	FHyperAIFoliagePlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("foliage-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Value")})) return Report;
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
		Report.Diagnostic = TEXT("This level has never been saved, so there is nowhere to save the foliage; save the level first or leave bSave off.");
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
		Summary.VariantId = TEXT("foliage.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.Operations[0].Types.IsEmpty() ? FString(TEXT("level foliage")) : Request.Operations[0].Types[0];
		Summary.Effects.Add(FString::Printf(TEXT("Edit foliage: %d operation(s), one undo step%s"), Request.Operations.Num(), Request.bSave ? TEXT(", then the level saved") : TEXT("")));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAIFoliageOp& Op = Request.Operations[Index];
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s"), *Op.Kind, Op.Types.IsEmpty() ? TEXT("(every type)") : *FString::Join(Op.Types, TEXT(", "))));
			for (const FString& Type : Op.Types)
			{
				Summary.Touches.AddUnique(Type);
			}
		}
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				// The editor may have moved on while the plan waited (an asset deleted, a path taken, the level closed),
				// so it is checked again before anything changes.
				FString Problem;
				UWorld* Now = EditorWorld();
				if (!Now || !Check(*Now, Request, Problem))
				{
					FHyperAIFoliagePlanReport Refused;
					Refused.OperationId = OperationId;
					Refused.Status = TEXT("invalid_request");
					Refused.Diagnostic = Now ? TEXT("The editor changed while the plan waited for approval: ") + Problem : FString(TEXT("No level is open in the editor; nothing was changed."));
					Record(OperationId, Refused);
					return;
				}
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

FHyperAIFoliageInspectReport UHyperAIStudioFoliageToolset::hyper_foliage_inspect(const FHyperAIFoliageInspectRequest& Request)
{
	return FHyperAIStudioFoliageContracts::Inspect(Request);
}

FHyperAIFoliagePlanReport UHyperAIStudioFoliageToolset::hyper_foliage_apply_plan(const FHyperAIFoliagePlanRequest& Request)
{
	return FHyperAIStudioFoliageContracts::ApplyPlan(Request);
}

void FHyperAIStudioFoliageRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioFoliageRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioFoliageRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioFoliageToolset::StaticClass(),
			FHyperAIStudioFoliageContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioFoliage, Warning, TEXT("Could not unregister the foliage toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioFoliageRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioFoliageContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioFoliage, Verbose, TEXT("The foliage source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioFoliageToolset::StaticClass(),
		FHyperAIStudioFoliageContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioFoliage, Error, TEXT("Foliage cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
