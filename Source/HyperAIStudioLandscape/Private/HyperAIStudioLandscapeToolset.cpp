// Games by Hyper 2026.

#include "HyperAIStudioLandscapeToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSourceControl.h"
#include "ImageCore.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeSplineActor.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeSubsystem.h"
#include "LandscapeUtils.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLandscape, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioLandscape"

FString FHyperAIStudioLandscapeContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioLandscape.HyperAIStudioLandscapeToolset");
}

TArray<FString> FHyperAIStudioLandscapeContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioLandscapeContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::LandscapeTools::Private
{
	using C = FHyperAIStudioLandscapeContracts;

	/** Unreal's recommended layout for most sizes: 63-quad sections, two a side per component. */
	constexpr int32 QuadsPerSection = 63;
	constexpr int32 SectionsPerComponent = 2;
	constexpr int32 QuadsPerComponent = QuadsPerSection * SectionsPerComponent;
	/** Landscape heights are stored with 128 units per cm of the Z scale: world Z = (H - 32768) / 128 * ScaleZ. */
	constexpr double HeightUnitsPerScaledCm = 128.0;
	const TCHAR* const DefaultLayerInfoFolder = TEXT("/Game/Landscape/LayerInfos");

	FString KindOf(const FHyperAILandscapeOp& Op)
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
		if (Trimmed.IsEmpty()) return nullptr;
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	bool IsIdentifier(const FString& Name)
	{
		if (Name.IsEmpty() || Name.Len() > 64 || !(FChar::IsAlpha(Name[0]) || Name[0] == TEXT('_'))) return false;
		for (const TCHAR Char : Name)
		{
			if (!FChar::IsAlnum(Char) && Char != TEXT('_')) return false;
		}
		return true;
	}

	/** Vertices a side for a size in meters at a resolution in cm: a whole number of components, plus one. */
	int32 VerticesFor(const float SizeMeters, const float ResolutionCm)
	{
		const int32 Components = FMath::Max(1, FMath::RoundToInt(SizeMeters * 100.f / ResolutionCm / QuadsPerComponent));
		return Components * QuadsPerComponent + 1;
	}

	/** Fractal noise in -1..1 at a point in meters. */
	double Fbm(const FHyperAILandscapeNoise& Noise, const double Xm, const double Ym)
	{
		double Sum = 0.0;
		double Norm = 0.0;
		double Amplitude = 1.0;
		double Frequency = 1.0 / FMath::Max(1.0, static_cast<double>(Noise.Wavelength));
		const FVector2D Offset(Noise.Seed * 17.137, Noise.Seed * 31.713);
		for (int32 Octave = 0; Octave < FMath::Clamp(Noise.Octaves, 1, 8); ++Octave)
		{
			double Value = FMath::PerlinNoise2D(FVector2D(Xm * Frequency, Ym * Frequency) + Offset * (Octave + 1));
			if (Noise.bRidged)
			{
				Value = 1.0 - 2.0 * FMath::Abs(Value);
			}
			Sum += Value * Amplitude;
			Norm += Amplitude;
			Amplitude *= 0.5;
			Frequency *= 2.0;
		}
		return Norm > 0.0 ? Sum / Norm : 0.0;
	}

	/** Height units a meter is on this landscape (its Z scale). */
	double UnitsPerMeter(const ALandscapeProxy& Landscape)
	{
		return 100.0 * HeightUnitsPerScaledCm / FMath::Max(0.001, Landscape.GetActorScale3D().Z);
	}

	/** A world height in meters as a stored height, and back; the landscape is taken as unrotated in pitch and roll. */
	double StoredFromMeters(const ALandscapeProxy& Landscape, const double Meters)
	{
		return 32768.0 + (Meters * 100.0 - Landscape.GetActorLocation().Z) / Landscape.GetActorScale3D().Z * HeightUnitsPerScaledCm;
	}

	double MetersFromStored(const ALandscapeProxy& Landscape, const double Stored)
	{
		return (Landscape.GetActorLocation().Z + (Stored - 32768.0) / HeightUnitsPerScaledCm * Landscape.GetActorScale3D().Z) / 100.0;
	}

	uint16 ToStored(const double Value)
	{
		return static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(Value), 0, 65535));
	}

	/** A 16-bit grayscale read of a texture's source, resampled to Size x Size. */
	bool ReadHeightmap(UTexture2D& Texture, const int32 Size, TArray<uint16>& OutHeights, FString& OutError)
	{
		FImage Source;
		if (!Texture.Source.IsValid() || !Texture.Source.GetMipImage(Source, 0, 0, 0))
		{
			OutError = FString::Printf(TEXT("%s has no source image to read heights from."), *Texture.GetName());
			return false;
		}
		FImage Gray;
		Source.CopyTo(Gray, ERawImageFormat::G16, EGammaSpace::Linear);
		const TArrayView64<uint16> Pixels = Gray.AsG16();
		const int32 Width = Gray.SizeX;
		const int32 HeightPixels = Gray.SizeY;
		OutHeights.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			const double V = Size > 1 ? static_cast<double>(Y) * (HeightPixels - 1) / (Size - 1) : 0.0;
			const int32 V0 = FMath::FloorToInt(V);
			const int32 V1 = FMath::Min(V0 + 1, HeightPixels - 1);
			for (int32 X = 0; X < Size; ++X)
			{
				const double U = Size > 1 ? static_cast<double>(X) * (Width - 1) / (Size - 1) : 0.0;
				const int32 U0 = FMath::FloorToInt(U);
				const int32 U1 = FMath::Min(U0 + 1, Width - 1);
				const double Top = FMath::Lerp<double>(Pixels[static_cast<int64>(V0) * Width + U0], Pixels[static_cast<int64>(V0) * Width + U1], U - U0);
				const double Bottom = FMath::Lerp<double>(Pixels[static_cast<int64>(V1) * Width + U0], Pixels[static_cast<int64>(V1) * Width + U1], U - U0);
				OutHeights[Y * Size + X] = ToStored(FMath::Lerp(Top, Bottom, V - V0));
			}
		}
		return true;
	}

	/** An existing layer info asset for the layer name in Folder, or a new one when bMake. */
	ULandscapeLayerInfoObject* FindOrMakeLayerInfo(const FString& Folder, const FString& Name, const bool bMake)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Folder), Assets, /*bRecursive=*/false);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath != ULandscapeLayerInfoObject::StaticClass()->GetClassPathName()) continue;
			ULandscapeLayerInfoObject* Info = Cast<ULandscapeLayerInfoObject>(Asset.GetAsset());
			if (Info && Info->GetLayerName().ToString().Equals(Name, ESearchCase::IgnoreCase)) return Info;
		}
		return bMake ? UE::Landscape::CreateTargetLayerInfo(FName(*Name), Folder) : nullptr;
	}

	/** The edit layer to change: by name, or the first. */
	bool EditLayerGuid(const ALandscape& Landscape, const FString& Name, FGuid& OutGuid, FString& OutError)
	{
		const TArray<const ULandscapeEditLayerBase*> Layers = Landscape.GetEditLayersConst();
		for (const ULandscapeEditLayerBase* Layer : Layers)
		{
			if (Layer && (Name.IsEmpty() || Layer->GetName().ToString().Equals(Name, ESearchCase::IgnoreCase)))
			{
				OutGuid = Layer->GetGuid();
				return true;
			}
		}
		OutError = Name.IsEmpty() ? FString(TEXT("The landscape has no edit layer.")) : FString::Printf(TEXT("The landscape has no edit layer '%s'."), *Name);
		return false;
	}

	/** Where a world point falls in the landscape's vertex grid. */
	FVector2D ToVertex(const ALandscapeProxy& Landscape, const FVector& World)
	{
		const FVector Local = Landscape.GetActorTransform().InverseTransformPosition(World);
		return FVector2D(Local.X, Local.Y);
	}

	/** 1 inside Inner, easing to 0 at Inner + Fade. */
	double Falloff(const double Distance, const double Inner, const double Fade)
	{
		if (Distance <= Inner) return 1.0;
		if (Fade <= 0.0 || Distance >= Inner + Fade) return 0.0;
		const double T = 1.0 - (Distance - Inner) / Fade;
		return T * T * (3.0 - 2.0 * T);
	}

	/** 1 inside [Min, Max], easing to 0 over Fade outside it. */
	double Band(const double Value, const double Min, const double Max, const double Fade)
	{
		if (Value < Min) return Falloff(Min - Value, 0.0, Fade);
		if (Value > Max) return Falloff(Value - Max, 0.0, Fade);
		return 1.0;
	}

	/** A rule's band: both bounds 0 means anything; a Max at or below Min means no upper bound (Ceiling). */
	double RuleBand(const double Value, const float Min, const float Max, const float Fade, const double Ceiling)
	{
		if (Min == 0.f && Max == 0.f) return 1.0;
		return Band(Value, Min, Max > Min ? static_cast<double>(Max) : Ceiling, Fade);
	}

	struct FRegion
	{
		int32 X1 = 0;
		int32 Y1 = 0;
		int32 X2 = 0;
		int32 Y2 = 0;
		int32 Width() const { return X2 - X1 + 1; }
		int32 Height() const { return Y2 - Y1 + 1; }
	};

	/** The vertices within Reach (vertex units) of the segment A-B, clipped to the landscape; false when none. */
	bool RegionAround(const ULandscapeInfo& Info, const FVector2D& A, const FVector2D& B, const double Reach, FRegion& Out)
	{
		int32 MinX = 0;
		int32 MinY = 0;
		int32 MaxX = 0;
		int32 MaxY = 0;
		if (!Info.GetLandscapeExtent(MinX, MinY, MaxX, MaxY)) return false;
		Out.X1 = FMath::Max(MinX, FMath::FloorToInt(FMath::Min(A.X, B.X) - Reach));
		Out.Y1 = FMath::Max(MinY, FMath::FloorToInt(FMath::Min(A.Y, B.Y) - Reach));
		Out.X2 = FMath::Min(MaxX, FMath::CeilToInt(FMath::Max(A.X, B.X) + Reach));
		Out.Y2 = FMath::Min(MaxY, FMath::CeilToInt(FMath::Max(A.Y, B.Y) + Reach));
		return Out.X1 <= Out.X2 && Out.Y1 <= Out.Y2;
	}

	FString Sculpt(ALandscape& Landscape, const FHyperAILandscapeOp& Op, const FGuid& Layer)
	{
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		const double Resolution = Landscape.GetActorScale3D().X;
		const FString Mode = Op.Mode.TrimStartAndEnd().ToLower();
		const FVector2D A = ToVertex(Landscape, Op.Center);
		const FVector2D B = Mode == TEXT("ramp") ? ToVertex(Landscape, Op.To) : A;
		const double Inner = (Mode == TEXT("ramp") ? Op.Width * 0.5 : Op.Radius) * 100.0 / Resolution;
		const double Fade = Op.Falloff * 100.0 / Resolution;
		FRegion Region;
		if (!RegionAround(*Info, A, B, Inner + Fade + 2.0, Region))
		{
			return TEXT("The sculpt region is outside the landscape.");
		}
		FLandscapeEditDataInterface Edit(Info, Layer);
		TArray<uint16> Heights;
		Heights.SetNumZeroed(Region.Width() * Region.Height());
		Edit.GetHeightDataFast(Region.X1, Region.Y1, Region.X2, Region.Y2, Heights.GetData(), 0);
		const TArray<uint16> Before = Heights;
		auto At = [&Before, &Region](const int32 X, const int32 Y)
		{
			return static_cast<double>(Before[(FMath::Clamp(Y, Region.Y1, Region.Y2) - Region.Y1) * Region.Width() + FMath::Clamp(X, Region.X1, Region.X2) - Region.X1]);
		};
		const double PerMeter = UnitsPerMeter(Landscape);
		const double StartHeight = At(FMath::RoundToInt(A.X), FMath::RoundToInt(A.Y));
		const double EndHeight = At(FMath::RoundToInt(B.X), FMath::RoundToInt(B.Y));
		const FVector2D Segment = B - A;
		const double SegmentLength = Segment.Size();
		for (int32 Y = Region.Y1; Y <= Region.Y2; ++Y)
		{
			for (int32 X = Region.X1; X <= Region.X2; ++X)
			{
				const FVector2D P(X, Y);
				double T = 0.0;
				if (SegmentLength > KINDA_SMALL_NUMBER)
				{
					T = FMath::Clamp(FVector2D::DotProduct(P - A, Segment) / (SegmentLength * SegmentLength), 0.0, 1.0);
				}
				const double Distance = FVector2D::Distance(P, A + Segment * T);
				const double Weight = Falloff(Distance, Inner, Fade);
				if (Weight <= 0.0) continue;
				const double Current = At(X, Y);
				double Target = Current;
				if (Mode == TEXT("raise") || Mode == TEXT("lower"))
				{
					Target = Current + (Mode == TEXT("raise") ? 1.0 : -1.0) * Op.Amount * PerMeter;
				}
				else if (Mode == TEXT("flatten"))
				{
					Target = StoredFromMeters(Landscape, Op.Height);
				}
				else if (Mode == TEXT("smooth"))
				{
					double Sum = 0.0;
					for (int32 DY = -2; DY <= 2; ++DY)
					{
						for (int32 DX = -2; DX <= 2; ++DX) Sum += At(X + DX, Y + DY);
					}
					Target = FMath::Lerp(Current, Sum / 25.0, static_cast<double>(FMath::Clamp(Op.Strength, 0.f, 1.f)));
				}
				else if (Mode == TEXT("noise"))
				{
					const double Xm = X * Resolution / 100.0;
					const double Ym = Y * Resolution / 100.0;
					Target = Current + Fbm(Op.Noise, Xm, Ym) * Op.Amount * 0.5 * PerMeter;
				}
				else if (Mode == TEXT("terrace"))
				{
					const double Step = FMath::Max(0.01, static_cast<double>(Op.Amount));
					const double Meters = MetersFromStored(Landscape, Current);
					const double Floor = FMath::FloorToDouble(Meters / Step) * Step;
					// A short riser at each step's back edge, so the terraces read as walkable shelves.
					const double Rise = FMath::SmoothStep(0.85, 1.0, (Meters - Floor) / Step);
					Target = StoredFromMeters(Landscape, Floor + Rise * Step);
				}
				else if (Mode == TEXT("ramp"))
				{
					Target = FMath::Lerp(StartHeight, EndHeight, T);
				}
				Heights[(Y - Region.Y1) * Region.Width() + X - Region.X1] = ToStored(FMath::Lerp(Current, Target, Weight));
			}
		}
		Edit.SetHeightData(Region.X1, Region.Y1, Region.X2, Region.Y2, Heights.GetData(), 0, /*InCalcNormals=*/true);
		Edit.Flush();
		return FString();
	}

	/** Final heights (meters) over a region, for painting by height and slope. */
	TArray<double> FinalHeights(ALandscape& Landscape, const FRegion& Region)
	{
		FLandscapeEditDataInterface Final(Landscape.GetLandscapeInfo(), FGuid());
		TArray<uint16> Stored;
		Stored.SetNumZeroed(Region.Width() * Region.Height());
		Final.GetHeightDataFast(Region.X1, Region.Y1, Region.X2, Region.Y2, Stored.GetData(), 0);
		TArray<double> Meters;
		Meters.SetNumUninitialized(Stored.Num());
		for (int32 Index = 0; Index < Stored.Num(); ++Index) Meters[Index] = MetersFromStored(Landscape, Stored[Index]);
		return Meters;
	}

	/** The landscape's roads and other splines, as segments in its vertex grid. */
	TArray<TPair<FVector2D, FVector2D>> SplineSegments(const ALandscape& Landscape, const ULandscapeInfo& Info)
	{
		TSet<ULandscapeSplinesComponent*> Components;
		Info.ForEachLandscapeProxy([&Components](ALandscapeProxy* Proxy)
		{
			if (ULandscapeSplinesComponent* Splines = Proxy->GetSplinesComponent()) Components.Add(Splines);
			return true;
		});
		// World Partition keeps each road in a spline actor of its own.
		Info.ForAllSplineActors([&Components](TScriptInterface<ILandscapeSplineInterface> Owner)
		{
			if (Owner && Owner->GetSplinesComponent()) Components.Add(Owner->GetSplinesComponent());
		});
		TArray<TPair<FVector2D, FVector2D>> Segments;
		for (ULandscapeSplinesComponent* Splines : Components)
		{
			const FTransform ToWorld = Splines->GetComponentTransform();
			for (const ULandscapeSplineSegment* Segment : Splines->GetSegments())
			{
				if (!Segment) continue;
				const TArray<FLandscapeSplineInterpPoint>& Points = Segment->GetPoints();
				for (int32 Index = 1; Index < Points.Num(); ++Index)
				{
					Segments.Emplace(ToVertex(Landscape, ToWorld.TransformPosition(Points[Index - 1].Center)), ToVertex(Landscape, ToWorld.TransformPosition(Points[Index].Center)));
				}
			}
		}
		return Segments;
	}

	double DistanceToSegment(const FVector2D& Point, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D Along = B - A;
		const double LengthSquared = Along.SizeSquared();
		const double T = LengthSquared > 0.0 ? FMath::Clamp(FVector2D::DotProduct(Point - A, Along) / LengthSquared, 0.0, 1.0) : 0.0;
		return FVector2D::Distance(Point, A + Along * T);
	}

	/** Each vertex's distance in vertex units to the nearest segment, Reach where none is nearer. */
	TArray<float> DistanceField(const TArray<TPair<FVector2D, FVector2D>>& Segments, const FRegion& Region, const double Reach)
	{
		const int32 W = Region.Width();
		const int32 H = Region.Height();
		TArray<float> Out;
		Out.Init(static_cast<float>(Reach), W * H);
		// Each segment goes in every cell within Reach of it, so a vertex needs only its own cell's segments.
		const double Cell = FMath::Max(Reach, 8.0);
		TMap<FIntPoint, TArray<int32>> Cells;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FVector2D& A = Segments[Index].Key;
			const FVector2D& B = Segments[Index].Value;
			const int32 X1 = FMath::FloorToInt((FMath::Min(A.X, B.X) - Reach) / Cell);
			const int32 X2 = FMath::FloorToInt((FMath::Max(A.X, B.X) + Reach) / Cell);
			const int32 Y1 = FMath::FloorToInt((FMath::Min(A.Y, B.Y) - Reach) / Cell);
			const int32 Y2 = FMath::FloorToInt((FMath::Max(A.Y, B.Y) + Reach) / Cell);
			for (int32 Y = Y1; Y <= Y2; ++Y)
			{
				for (int32 X = X1; X <= X2; ++X) Cells.FindOrAdd(FIntPoint(X, Y)).Add(Index);
			}
		}
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const FVector2D Point(Region.X1 + X, Region.Y1 + Y);
				const TArray<int32>* Near = Cells.Find(FIntPoint(FMath::FloorToInt(Point.X / Cell), FMath::FloorToInt(Point.Y / Cell)));
				if (!Near) continue;
				double Best = Reach;
				for (const int32 Index : *Near) Best = FMath::Min(Best, DistanceToSegment(Point, Segments[Index].Key, Segments[Index].Value));
				Out[Y * W + X] = static_cast<float>(Best);
			}
		}
		return Out;
	}

	/** How far either side of a patch's edge, in noise units, the layer eases in. */
	constexpr double PatchFade = 0.04;

	/** A rule's patch noise over the region, and the level Coverage of it lies above; empty when the rule has no patches. */
	TArray<float> PatchNoise(const FHyperAILandscapePaintRule& Rule, const FRegion& Region, const double MetersPerVertex, double& OutCut)
	{
		TArray<float> Noise;
		OutCut = 0.0;
		if (!(Rule.Patch > 0.f) || Rule.Coverage >= 1.f) return Noise;
		FHyperAILandscapeNoise Patches;
		Patches.Wavelength = Rule.Patch;
		Patches.Octaves = 4;
		Patches.Seed = Rule.Seed != 0 ? Rule.Seed : static_cast<int32>(GetTypeHash(Rule.Layer.TrimStartAndEnd().ToLower()) & 0xffff) + 1;
		const int32 W = Region.Width();
		const int32 H = Region.Height();
		Noise.SetNumUninitialized(W * H);
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				Noise[Y * W + X] = static_cast<float>(Fbm(Patches, (Region.X1 + X) * MetersPerVertex, (Region.Y1 + Y) * MetersPerVertex));
			}
		}
		// The cut is measured on this ground, so the patches cover what was asked whatever the noise's spread here.
		TArray<float> Sample;
		const int32 Stride = FMath::Max(1, Noise.Num() / 65536);
		for (int32 Index = 0; Index < Noise.Num(); Index += Stride) Sample.Add(Noise[Index]);
		Sample.Sort();
		OutCut = Sample[FMath::Clamp(FMath::FloorToInt((1.0 - Rule.Coverage) * Sample.Num()), 0, Sample.Num() - 1)];
		return Noise;
	}

	FString Paint(ALandscape& Landscape, const FHyperAILandscapeOp& Op, const FGuid& Layer)
	{
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		const double Resolution = Landscape.GetActorScale3D().X;
		FRegion Region;
		const bool bCircle = Op.Radius > 0.f && !Op.Center.IsZero();
		const FVector2D Center = ToVertex(Landscape, Op.Center);
		const double Inner = Op.Radius * 100.0 / Resolution;
		const double Fade = Op.Falloff * 100.0 / Resolution;
		if (bCircle)
		{
			if (!RegionAround(*Info, Center, Center, Inner + Fade + 1.0, Region)) return TEXT("The paint region is outside the landscape.");
		}
		else if (!Info->GetLandscapeExtent(Region.X1, Region.Y1, Region.X2, Region.Y2))
		{
			return TEXT("The landscape has no extent.");
		}
		const TArray<double> Heights = FinalHeights(Landscape, Region);
		const int32 W = Region.Width();
		const int32 H = Region.Height();
		auto HeightAt = [&Heights, W, H](const int32 X, const int32 Y) { return Heights[FMath::Clamp(Y, 0, H - 1) * W + FMath::Clamp(X, 0, W - 1)]; };
		// Every layer the landscape has gets a weight here, so the painted region sums to full coverage.
		TArray<ULandscapeLayerInfoObject*> LayerInfos;
		for (const FLandscapeInfoLayerSettings& Settings : Info->Layers)
		{
			if (Settings.LayerInfoObj) LayerInfos.AddUnique(Settings.LayerInfoObj);
		}
		TArray<int32> RuleLayer;
		for (const FHyperAILandscapePaintRule& Rule : Op.Rules)
		{
			ULandscapeLayerInfoObject* RuleInfo = Info->GetLayerInfoByName(FName(*Rule.Layer));
			const int32 Index = LayerInfos.IndexOfByKey(RuleInfo);
			if (Index == INDEX_NONE) return FString::Printf(TEXT("The landscape has no paint layer %s with a layer info."), *Rule.Layer);
			RuleLayer.Add(Index);
		}
		TArray<TArray<uint8>> Weights;
		Weights.SetNum(LayerInfos.Num());
		FLandscapeEditDataInterface Edit(Info, Layer);
		for (int32 LayerIndex = 0; LayerIndex < LayerInfos.Num(); ++LayerIndex)
		{
			Weights[LayerIndex].SetNumZeroed(W * H);
			Edit.GetWeightDataFast(LayerInfos[LayerIndex], Region.X1, Region.Y1, Region.X2, Region.Y2, Weights[LayerIndex].GetData(), 0);
		}
		FHyperAILandscapeNoise Breakup;
		Breakup.Wavelength = 20.f;
		Breakup.Octaves = 3;
		const double MetersPerVertex = Resolution / 100.0;
		TArray<TArray<float>> Patches;
		TArray<double> PatchCuts;
		Patches.SetNum(Op.Rules.Num());
		PatchCuts.SetNumZeroed(Op.Rules.Num());
		double Reach = 0.0;
		for (int32 RuleIndex = 0; RuleIndex < Op.Rules.Num(); ++RuleIndex)
		{
			const FHyperAILandscapePaintRule& Rule = Op.Rules[RuleIndex];
			Patches[RuleIndex] = PatchNoise(Rule, Region, MetersPerVertex, PatchCuts[RuleIndex]);
			if (Rule.DistanceMin > 0.f || Rule.DistanceMax > 0.f) Reach = FMath::Max(Reach, FMath::Max(Rule.DistanceMin, Rule.DistanceMax) + Rule.DistanceFade + 1.0);
		}
		// Meters from the landscape's roads, as far out as any rule looks.
		TArray<float> Distance;
		if (Reach > 0.0)
		{
			const TArray<TPair<FVector2D, FVector2D>> Segments = SplineSegments(Landscape, *Info);
			if (Segments.IsEmpty()) return TEXT("The landscape has no roads or other splines to measure distance from; lay one with landscape.road first.");
			Distance = DistanceField(Segments, Region, Reach / MetersPerVertex);
			for (float& Meters : Distance) Meters *= static_cast<float>(MetersPerVertex);
		}
		TArray<double> Computed;
		Computed.SetNum(LayerInfos.Num());
		for (int32 Y = 0; Y < H; ++Y)
		{
			for (int32 X = 0; X < W; ++X)
			{
				const double Mask = bCircle ? Falloff(FVector2D::Distance(FVector2D(Region.X1 + X, Region.Y1 + Y), Center), Inner, Fade) : 1.0;
				if (Mask <= 0.0) continue;
				const double Height = HeightAt(X, Y);
				const double DZDX = (HeightAt(X + 1, Y) - HeightAt(X - 1, Y)) / (2.0 * Resolution / 100.0);
				const double DZDY = (HeightAt(X, Y + 1) - HeightAt(X, Y - 1)) / (2.0 * Resolution / 100.0);
				const double Slope = FMath::RadiansToDegrees(FMath::Atan(FMath::Sqrt(DZDX * DZDX + DZDY * DZDY)));
				const double Noise = Fbm(Breakup, (Region.X1 + X) * Resolution / 100.0, (Region.Y1 + Y) * Resolution / 100.0);
				for (double& Value : Computed) Value = 0.0;
				// Later rules win: each takes its share of what the rules after it left, and the first takes the rest.
				double Remaining = 1.0;
				for (int32 RuleIndex = Op.Rules.Num() - 1; RuleIndex >= 0; --RuleIndex)
				{
					const FHyperAILandscapePaintRule& Rule = Op.Rules[RuleIndex];
					const double Jitter = Noise * Rule.Breakup;
					double Coverage = FMath::Clamp(static_cast<double>(Rule.Strength), 0.0, 1.0)
						* RuleBand(Height + Jitter * Rule.HeightFade, Rule.HeightMin, Rule.HeightMax, Rule.HeightFade, TNumericLimits<double>::Max())
						* RuleBand(Slope + Jitter * Rule.SlopeFade, Rule.SlopeMin, Rule.SlopeMax, Rule.SlopeFade, 90.0);
					if (!Distance.IsEmpty())
					{
						Coverage *= RuleBand(Distance[Y * W + X] + Jitter * Rule.DistanceFade, Rule.DistanceMin, Rule.DistanceMax, Rule.DistanceFade, TNumericLimits<double>::Max());
					}
					if (!Patches[RuleIndex].IsEmpty())
					{
						Coverage *= FMath::SmoothStep(PatchCuts[RuleIndex] - PatchFade, PatchCuts[RuleIndex] + PatchFade, Patches[RuleIndex][Y * W + X] + Jitter * PatchFade);
					}
					const double Share = RuleIndex == 0 ? Remaining : Remaining * Coverage;
					Computed[RuleLayer[RuleIndex]] += Share;
					Remaining -= Share;
				}
				const int32 Index = Y * W + X;
				for (int32 LayerIndex = 0; LayerIndex < LayerInfos.Num(); ++LayerIndex)
				{
					const double Old = Weights[LayerIndex][Index] / 255.0;
					Weights[LayerIndex][Index] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(255.0 * FMath::Lerp(Old, Computed[LayerIndex], Mask)), 0, 255));
				}
			}
		}
		for (int32 LayerIndex = 0; LayerIndex < LayerInfos.Num(); ++LayerIndex)
		{
			Edit.SetAlphaData(LayerInfos[LayerIndex], Region.X1, Region.Y1, Region.X2, Region.Y2, Weights[LayerIndex].GetData(), 0);
		}
		Edit.Flush();
		return FString();
	}

	FHyperAILandscapeInfo Describe(ALandscape& Landscape)
	{
		FHyperAILandscapeInfo Out;
		Out.Label = Landscape.GetActorLabel();
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		if (!Info) return Out;
		int32 MinX = 0;
		int32 MinY = 0;
		int32 MaxX = 0;
		int32 MaxY = 0;
		Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY);
		Out.VerticesX = MaxX - MinX + 1;
		Out.VerticesY = MaxY - MinY + 1;
		Out.Resolution = Landscape.GetActorScale3D().X;
		Out.Components = Info->XYtoComponentMap.Num();
		Out.QuadsPerSection = Info->SubsectionSizeQuads;
		Out.SectionsPerComponent = Info->ComponentNumSubsections;
		Out.HeightRange = 512.f * Landscape.GetActorScale3D().Z / 100.f;
		FBox Bounds(ForceInit);
		int32 Proxies = 0;
		Info->ForEachLandscapeProxy([&Bounds, &Proxies](ALandscapeProxy* Proxy)
		{
			Bounds += Proxy->GetComponentsBoundingBox(/*bNonColliding=*/true);
			++Proxies;
			return true;
		});
		Out.StreamingProxies = FMath::Max(0, Proxies - 1);
		if (Bounds.IsValid)
		{
			Out.Center = Bounds.GetCenter();
			Out.Extent = Bounds.GetExtent();
			Out.HeightMin = Bounds.Min.Z / 100.f;
			Out.HeightMax = Bounds.Max.Z / 100.f;
		}
		Out.Material = Landscape.LandscapeMaterial ? Landscape.LandscapeMaterial->GetPathName() : FString(TEXT("none"));
		// Coverage is measured on landscapes up to 2049 vertices a side; larger ones would read tens of megabytes.
		const bool bMeasure = Out.VerticesX * Out.VerticesY <= 2049 * 2049;
		FLandscapeEditDataInterface Final(Info, FGuid());
		for (const FLandscapeInfoLayerSettings& Settings : Info->Layers)
		{
			FString Line = Settings.LayerName.ToString() + TEXT(" (") + (Settings.LayerInfoObj ? Settings.LayerInfoObj->GetPathName() : FString(TEXT("no layer info"))) + TEXT(")");
			if (bMeasure && Settings.LayerInfoObj)
			{
				TArray<uint8> Weights;
				Weights.SetNumZeroed(Out.VerticesX * Out.VerticesY);
				Final.GetWeightDataFast(Settings.LayerInfoObj, MinX, MinY, MaxX, MaxY, Weights.GetData(), 0);
				uint64 Sum = 0;
				for (const uint8 Weight : Weights) Sum += Weight;
				Line += FString::Printf(TEXT(": %.0f%%"), Weights.IsEmpty() ? 0.0 : 100.0 * Sum / (255.0 * Weights.Num()));
			}
			Out.PaintLayers.Add(Line);
		}
		for (const ULandscapeEditLayerBase* Layer : Landscape.GetEditLayersConst())
		{
			if (Layer) Out.EditLayers.Add(Layer->GetName().ToString());
		}
		return Out;
	}

	FString Summarize(const FHyperAILandscapeInfo& Info)
	{
		return FString::Printf(TEXT("%s: %dx%d vertices at %.0f cm (%.0f m a side), ground %.1f to %.1f m of %.0f m room; material %s; paint layers %s; edit layers %s"),
			*Info.Label, Info.VerticesX, Info.VerticesY, Info.Resolution, (Info.VerticesX - 1) * Info.Resolution / 100.f, Info.HeightMin, Info.HeightMax,
			Info.HeightRange, *Info.Material, Info.PaintLayers.IsEmpty() ? TEXT("none") : *FString::Join(Info.PaintLayers, TEXT(", ")),
			Info.EditLayers.IsEmpty() ? TEXT("none") : *FString::Join(Info.EditLayers, TEXT(", ")));
	}

	/** The ground's height (cm) under a world XY, from whichever of the landscape's proxies holds it. */
	TOptional<double> GroundAt(ULandscapeInfo& Info, const FVector& Where)
	{
		TOptional<float> Height;
		Info.ForEachLandscapeProxy([&Height, &Where](ALandscapeProxy* Proxy)
		{
			Height = Proxy->GetHeightAtLocation(Where, EHeightfieldSource::Editor);
			return !Height.IsSet();
		});
		return Height.IsSet() ? TOptional<double>(Height.GetValue()) : TOptional<double>();
	}

	/**
	 * A road as the landscape spline tool makes one: control points on the ground joined by segments, each point turned
	 * along the road so it curves smoothly, then rendered into the landscape's splines edit layer.
	 */
	bool BuildRoad(UWorld& World, ALandscape& Landscape, ULandscapeInfo& Info, const FHyperAILandscapeOp& Op, TArray<FString>& OutNotes, FString& OutError)
	{
		TArray<FVector> Points;
		for (const FVector& Point : Op.Points)
		{
			const TOptional<double> Ground = GroundAt(Info, Point);
			if (!Ground.IsSet())
			{
				OutError = FString::Printf(TEXT("(%.0f, %.0f) is off the landscape."), Point.X, Point.Y);
				return false;
			}
			Points.Add(FVector(Point.X, Point.Y, Ground.GetValue()));
		}
		// Splines shape the ground only through a splines edit layer.
		if (!Landscape.FindLayerOfTypeConst(ULandscapeEditLayerSplines::StaticClass()))
		{
			if (Landscape.CreateLayer(FName(TEXT("Splines")), ULandscapeEditLayerSplines::StaticClass()) == INDEX_NONE)
			{
				OutError = TEXT("The landscape would not take a splines edit layer (it may have reached its edit layer limit).");
				return false;
			}
			OutNotes.Add(FString::Printf(TEXT("%s: a Splines edit layer was added for its roads."), *Landscape.GetActorLabel()));
		}
		// World Partition keeps each road in its own spline actor; otherwise the road joins the landscape's splines.
		ILandscapeSplineInterface* Owner = nullptr;
		const ULandscapeSubsystem* Subsystem = World.GetSubsystem<ULandscapeSubsystem>();
		if (Subsystem && Subsystem->IsGridBased())
		{
			ALandscapeSplineActor* Actor = Info.CreateSplineActor(Points[0]);
			if (Actor && !Op.Road.TrimStartAndEnd().IsEmpty()) Actor->SetActorLabel(Op.Road.TrimStartAndEnd());
			Owner = Actor;
		}
		else
		{
			Owner = Info.GetCurrentLevelLandscapeProxy(/*bRegistered=*/true);
		}
		if (!Owner || !Owner->IsSplineOwnerValid())
		{
			OutError = TEXT("The landscape has nowhere to keep a road's spline.");
			return false;
		}
		if (!Owner->GetSplinesComponent()) Owner->CreateSplineComponent();
		ULandscapeSplinesComponent* Splines = Owner->GetSplinesComponent();
		if (!Splines)
		{
			OutError = TEXT("The landscape would not make a splines component.");
			return false;
		}
		Splines->Modify();
		UStaticMesh* Mesh = Load<UStaticMesh>(Op.Mesh);
		const FName LayerName = Op.Layer.TrimStartAndEnd().IsEmpty() ? NAME_None : FName(*Op.Layer.TrimStartAndEnd());
		const FTransform SplinesToWorld = Splines->GetComponentTransform();
		TArray<ULandscapeSplineControlPoint*> Controls;
		for (const FVector& Point : Points)
		{
			ULandscapeSplineControlPoint* Control = NewObject<ULandscapeSplineControlPoint>(Splines, NAME_None, RF_Transactional);
			Splines->GetControlPoints().Add(Control);
			Control->Location = SplinesToWorld.InverseTransformPosition(Point);
			// Width is the half-width, and falloffs are in cm.
			Control->Width = Op.Width * 50.f;
			Control->SideFalloff = Op.Falloff * 100.f;
			Control->EndFalloff = Op.Falloff * 100.f;
			Control->LayerName = LayerName;
			Control->bRaiseTerrain = true;
			Control->bLowerTerrain = true;
			Controls.Add(Control);
		}
		const int32 Joins = Op.bClosed ? Controls.Num() : Controls.Num() - 1;
		for (int32 Index = 0; Index < Joins; ++Index)
		{
			ULandscapeSplineControlPoint* Start = Controls[Index];
			ULandscapeSplineControlPoint* End = Controls[(Index + 1) % Controls.Num()];
			ULandscapeSplineSegment* Segment = NewObject<ULandscapeSplineSegment>(Splines, NAME_None, RF_Transactional);
			Splines->GetSegments().Add(Segment);
			Segment->Connections[0].ControlPoint = Start;
			Segment->Connections[1].ControlPoint = End;
			const float Length = static_cast<float>(FVector::Dist(Start->Location, End->Location));
			Segment->Connections[0].TangentLen = Length;
			Segment->Connections[1].TangentLen = Length;
			Segment->LayerName = LayerName;
			Segment->bRaiseTerrain = true;
			Segment->bLowerTerrain = true;
			if (Mesh)
			{
				FLandscapeSplineMeshEntry& Entry = Segment->SplineMeshes.AddDefaulted_GetRef();
				Entry.Mesh = Mesh;
				Entry.bScaleToWidth = true;
			}
			Start->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 0));
			End->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 1));
			Segment->AutoFlipTangents();
		}
		for (ULandscapeSplineControlPoint* Control : Controls)
		{
			Control->AutoCalcRotation(/*bAlwaysRotateForward=*/true);
		}
		for (ULandscapeSplineControlPoint* Control : Controls)
		{
			Control->UpdateSplinePoints();
		}
		if (!Splines->IsRegistered()) Splines->RegisterComponent();
		else Splines->MarkRenderStateDirty();
		// Render the road into the splines layer and merge it now, so the report and the next op read the result.
		Landscape.RequestSplineLayerUpdate();
		Landscape.ForceUpdateLayersContent();
		OutNotes.Add(FString::Printf(TEXT("%s: road%s through %d points, %g m wide%s%s."), *Landscape.GetActorLabel(),
			Op.Road.IsEmpty() ? TEXT("") : *(TEXT(" ") + Op.Road), Points.Num(), Op.Width, LayerName.IsNone() ? TEXT("") : *(TEXT(", painted ") + LayerName.ToString()),
			Mesh ? *(TEXT(", ") + Mesh->GetName() + TEXT(" along it")) : TEXT("")));
		return true;
	}

	/** What a plan knows of each landscape it names: its paint layers and edit layers, as they are or will be. */
	struct FPlanned
	{
		TSet<FString> PaintLayers;
		TSet<FString> EditLayers;
	};

	/** Every op checked before anything changes. */
	bool Check(UWorld& World, const FHyperAILandscapePlanRequest& Request, FString& OutError)
	{
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		TMap<FString, FPlanned> Plan;
		auto Track = [&World, &Plan](const FString& Label) -> FPlanned*
		{
			const FString Key = Label.TrimStartAndEnd().ToLower();
			if (FPlanned* Planned = Plan.Find(Key)) return Planned;
			ALandscape* Landscape = FindLandscape(World, Label);
			ULandscapeInfo* Info = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
			if (!Info) return nullptr;
			FPlanned Known;
			for (const FLandscapeInfoLayerSettings& Settings : Info->Layers)
			{
				if (Settings.LayerInfoObj) Known.PaintLayers.Add(Settings.LayerName.ToString().ToLower());
			}
			for (const ULandscapeEditLayerBase* Layer : Landscape->GetEditLayersConst())
			{
				if (Layer) Known.EditLayers.Add(Layer->GetName().ToString().ToLower());
			}
			return &Plan.Add(Key, MoveTemp(Known));
		};
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAILandscapeOp& Op = Request.Operations[Index];
			const FString Kind = KindOf(Op);
			auto Fail = [&OutError, &Op, Index](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Kind, *Message);
				return false;
			};
			if (Op.Landscape.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("Name the Landscape (its label)."));
			if (Kind == TEXT("landscape.create"))
			{
				if (FindLandscape(World, Op.Landscape) || Plan.Contains(Op.Landscape.TrimStartAndEnd().ToLower()))
				{
					return Fail(FString::Printf(TEXT("A landscape is labelled %s already."), *Op.Landscape));
				}
				if (!(Op.Resolution >= 10.f && Op.Resolution <= 1000.f) || !(Op.Size >= 10.f) || !(Op.HeightRange >= 1.f && Op.HeightRange <= 20000.f))
				{
					return Fail(TEXT("Resolution is 10 to 1000 cm, Size at least 10 m, HeightRange 1 to 20000 m."));
				}
				if (VerticesFor(Op.Size, Op.Resolution) > C::MaxVertices)
				{
					return Fail(FString::Printf(TEXT("%.0f m at %.0f cm is %d vertices a side; the most is %d. Use a coarser Resolution or a smaller Size."),
						Op.Size, Op.Resolution, VerticesFor(Op.Size, Op.Resolution), C::MaxVertices));
				}
				if (!Op.Heightmap.IsEmpty() && !Load<UTexture2D>(Op.Heightmap))
				{
					return Fail(FString::Printf(TEXT("%s is not a texture."), *Op.Heightmap));
				}
				if (!Op.Material.IsEmpty() && !Load<UMaterialInterface>(Op.Material))
				{
					return Fail(FString::Printf(TEXT("%s is not a material."), *Op.Material));
				}
				if (!Op.LayerInfoFolder.IsEmpty() && !Op.LayerInfoFolder.StartsWith(TEXT("/Game/")))
				{
					return Fail(TEXT("LayerInfoFolder is a folder under /Game."));
				}
				FPlanned Made;
				for (const FString& Layer : Op.PaintLayers)
				{
					if (!IsIdentifier(Layer)) return Fail(FString::Printf(TEXT("'%s' is not a layer name: letters, digits and _."), *Layer));
					Made.PaintLayers.Add(Layer.ToLower());
				}
				Made.EditLayers.Add(TEXT("layer"));
				Plan.Add(Op.Landscape.TrimStartAndEnd().ToLower(), MoveTemp(Made));
				continue;
			}
			FPlanned* Landscape = Track(Op.Landscape);
			if (!Landscape) return Fail(FString::Printf(TEXT("No landscape is labelled %s."), *Op.Landscape));
			auto KnowsEditLayer = [Landscape, &Op]()
			{
				return Op.EditLayer.IsEmpty() || Landscape->EditLayers.Contains(Op.EditLayer.TrimStartAndEnd().ToLower());
			};
			if (Kind == TEXT("landscape.sculpt"))
			{
				const FString Mode = Op.Mode.TrimStartAndEnd().ToLower();
				static const TSet<FString> Modes = {TEXT("raise"), TEXT("lower"), TEXT("flatten"), TEXT("smooth"), TEXT("noise"), TEXT("terrace"), TEXT("ramp")};
				if (!Modes.Contains(Mode)) return Fail(TEXT("Mode is raise, lower, flatten, smooth, noise, terrace or ramp."));
				if (!(Op.Radius > 0.f) || !(Op.Falloff >= 0.f) || (Mode == TEXT("ramp") && !(Op.Width > 0.f)))
				{
					return Fail(TEXT("Radius (or a ramp's Width) above 0 and Falloff 0 or more, in meters."));
				}
				if ((Mode == TEXT("terrace") || Mode == TEXT("noise") || Mode == TEXT("raise") || Mode == TEXT("lower")) && !(Op.Amount > 0.f))
				{
					return Fail(TEXT("Amount, in meters, above 0."));
				}
				if (!KnowsEditLayer()) return Fail(FString::Printf(TEXT("The landscape has no edit layer %s."), *Op.EditLayer));
			}
			else if (Kind == TEXT("landscape.set_material"))
			{
				if (!Load<UMaterialInterface>(Op.Material)) return Fail(FString::Printf(TEXT("%s is not a material."), *Op.Material));
			}
			else if (Kind == TEXT("landscape.add_paint_layer"))
			{
				if (Op.PaintLayers.IsEmpty()) return Fail(TEXT("Name the PaintLayers to add."));
				for (const FString& Layer : Op.PaintLayers)
				{
					if (!IsIdentifier(Layer)) return Fail(FString::Printf(TEXT("'%s' is not a layer name: letters, digits and _."), *Layer));
					if (Landscape->PaintLayers.Contains(Layer.ToLower())) return Fail(FString::Printf(TEXT("The landscape has paint layer %s already."), *Layer));
					Landscape->PaintLayers.Add(Layer.ToLower());
				}
			}
			else if (Kind == TEXT("landscape.paint"))
			{
				if (Op.Rules.IsEmpty()) return Fail(TEXT("Give the Rules to paint by."));
				for (const FHyperAILandscapePaintRule& Rule : Op.Rules)
				{
					if (!Landscape->PaintLayers.Contains(Rule.Layer.TrimStartAndEnd().ToLower()))
					{
						return Fail(FString::Printf(TEXT("The landscape has no paint layer '%s'; add_paint_layer adds one."), *Rule.Layer));
					}
					if (Rule.Patch < 0.f || (Rule.Patch > 0.f && Rule.Patch < 5.f) || Rule.Patch > 10000.f) return Fail(TEXT("A rule's Patch is 0 (none) or 5 to 10000 meters."));
					if (Rule.Coverage < 0.05f || Rule.Coverage > 1.f) return Fail(TEXT("A rule's Coverage is 0.05 to 1."));
					if (Rule.DistanceMin < 0.f || Rule.DistanceMax < 0.f || Rule.DistanceFade < 0.f || FMath::Max(Rule.DistanceMin, Rule.DistanceMax) + Rule.DistanceFade > 2000.f)
					{
						return Fail(TEXT("A rule's DistanceMin, DistanceMax and DistanceFade are 0 or more, and reach at most 2000 m."));
					}
				}
				if (!KnowsEditLayer()) return Fail(FString::Printf(TEXT("The landscape has no edit layer %s."), *Op.EditLayer));
			}
			else if (Kind == TEXT("landscape.add_edit_layer"))
			{
				if (!IsIdentifier(Op.EditLayer)) return Fail(TEXT("Name the EditLayer: letters, digits and _."));
				if (Landscape->EditLayers.Contains(Op.EditLayer.ToLower())) return Fail(FString::Printf(TEXT("The landscape has edit layer %s already."), *Op.EditLayer));
				Landscape->EditLayers.Add(Op.EditLayer.ToLower());
			}
			else if (Kind == TEXT("landscape.road"))
			{
				if (Op.Points.Num() < (Op.bClosed ? 3 : 2) || Op.Points.Num() > 256) return Fail(TEXT("A road runs through 2 to 256 Points (3 or more when bClosed)."));
				if (!(Op.Width > 0.f) || !(Op.Falloff >= 0.f)) return Fail(TEXT("Width above 0 and Falloff 0 or more, in meters."));
				if (!Op.Layer.IsEmpty() && !Landscape->PaintLayers.Contains(Op.Layer.TrimStartAndEnd().ToLower()))
				{
					return Fail(FString::Printf(TEXT("The landscape has no paint layer '%s'; add_paint_layer adds one."), *Op.Layer));
				}
				if (!Op.Mesh.IsEmpty() && !Load<UStaticMesh>(Op.Mesh)) return Fail(FString::Printf(TEXT("%s is not a static mesh."), *Op.Mesh));
			}
			else
			{
				return Fail(TEXT("Unknown op; see hyper_landscape_apply_plan's description."));
			}
		}
		return true;
	}

	/** One op, inside the plan's undo step. */
	bool Apply(UWorld& World, const FHyperAILandscapeOp& Op, TArray<ALandscape*>& Touched, TArray<UObject*>& Assets, TArray<FString>& OutNotes, FString& OutError)
	{
		const FString Kind = KindOf(Op);
		const FString Folder = Op.LayerInfoFolder.IsEmpty() ? FString(DefaultLayerInfoFolder) : Op.LayerInfoFolder;
		auto MakeLayerInfos = [&Folder, &Assets](const TArray<FString>& Names)
		{
			TArray<ULandscapeLayerInfoObject*> Infos;
			for (const FString& Name : Names)
			{
				ULandscapeLayerInfoObject* Info = FindOrMakeLayerInfo(Folder, Name, /*bMake=*/true);
				Infos.Add(Info);
				Assets.AddUnique(Info);
			}
			return Infos;
		};
		if (Kind == TEXT("landscape.create"))
		{
			const int32 Size = VerticesFor(Op.Size, Op.Resolution);
			TArray<uint16> Heights;
			if (UTexture2D* Heightmap = Load<UTexture2D>(Op.Heightmap))
			{
				if (!ReadHeightmap(*Heightmap, Size, Heights, OutError)) return false;
			}
			else
			{
				Heights.Init(32768, Size * Size);
				if (Op.Noise.Amplitude > 0.f)
				{
					const double PerMeter = 100.0 * HeightUnitsPerScaledCm / (Op.HeightRange * 100.0 / 512.0);
					for (int32 Y = 0; Y < Size; ++Y)
					{
						for (int32 X = 0; X < Size; ++X)
						{
							const double Meters = Fbm(Op.Noise, X * Op.Resolution / 100.0, Y * Op.Resolution / 100.0) * Op.Noise.Amplitude * 0.5;
							Heights[Y * Size + X] = ToStored(32768.0 + Meters * PerMeter);
						}
					}
				}
			}
			const TArray<ULandscapeLayerInfoObject*> LayerInfos = MakeLayerInfos(Op.PaintLayers);
			const double Quads = Size - 1;
			const FVector Scale(Op.Resolution, Op.Resolution, Op.HeightRange * 100.0 / 512.0);
			ALandscape* Landscape = World.SpawnActor<ALandscape>(Op.Location - FVector(Quads * Op.Resolution * 0.5, Quads * Op.Resolution * 0.5, 0.0), FRotator::ZeroRotator);
			if (!Landscape)
			{
				OutError = TEXT("The level refused a new landscape.");
				return false;
			}
			Landscape->LandscapeMaterial = Load<UMaterialInterface>(Op.Material);
			Landscape->SetActorRelativeScale3D(Scale);
			Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((Size * Size) / (2048 * 2048) + 1), static_cast<uint32>(2));
			TMap<FGuid, TArray<uint16>> HeightData;
			HeightData.Add(FGuid(), MoveTemp(Heights));
			TArray<FLandscapeImportLayerInfo> ImportLayers;
			for (int32 Index = 0; Index < LayerInfos.Num(); ++Index)
			{
				FLandscapeImportLayerInfo& Layer = ImportLayers.Emplace_GetRef(LayerInfos[Index]->GetLayerName());
				Layer.LayerInfo = LayerInfos[Index];
				// The first layer covers the ground until paint rules say otherwise.
				if (Index == 0) Layer.LayerData.Init(255, Size * Size);
			}
			TMap<FGuid, TArray<FLandscapeImportLayerInfo>> LayerData;
			LayerData.Add(FGuid(), MoveTemp(ImportLayers));
			Landscape->Import(FGuid::NewGuid(), 0, 0, Size - 1, Size - 1, SectionsPerComponent, QuadsPerSection, HeightData, TEXT(""), LayerData,
				ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
			Landscape->SetActorLabel(Op.Landscape.TrimStartAndEnd());
			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			if (!Info)
			{
				OutError = TEXT("The landscape imported without its info.");
				return false;
			}
			Info->UpdateLayerInfoMap(Landscape);
			for (ULandscapeLayerInfoObject* LayerInfo : LayerInfos)
			{
				Landscape->AddTargetLayer(LayerInfo->GetLayerName(), FLandscapeTargetLayerSettings(LayerInfo));
				const int32 LayerIndex = Info->GetLayerInfoIndex(LayerInfo->GetLayerName());
				if (Info->Layers.IsValidIndex(LayerIndex)) Info->Layers[LayerIndex].LayerInfoObj = LayerInfo;
			}
			ULandscapeSubsystem* Subsystem = World.GetSubsystem<ULandscapeSubsystem>();
			if (Subsystem && Subsystem->IsGridBased())
			{
				// World Partition streams landscapes in proxies of 2 x 2 components, as the editor's own new landscapes do.
				Subsystem->ChangeGridSize(Info, 2);
				OutNotes.Add(FString::Printf(TEXT("%s: split into World Partition streaming proxies."), *Op.Landscape));
			}
			if (!Landscape->LandscapeMaterial)
			{
				OutNotes.Add(FString::Printf(TEXT("%s has no material yet; set_material gives it one whose layer blend names its paint layers."), *Op.Landscape));
			}
			Touched.AddUnique(Landscape);
			return true;
		}
		ALandscape* Landscape = FindLandscape(World, Op.Landscape);
		ULandscapeInfo* Info = Landscape ? Landscape->GetLandscapeInfo() : nullptr;
		if (!Info)
		{
			OutError = FString::Printf(TEXT("No landscape is labelled %s."), *Op.Landscape);
			return false;
		}
		Touched.AddUnique(Landscape);
		Landscape->Modify();
		if (Kind == TEXT("landscape.set_material"))
		{
			Landscape->LandscapeMaterial = Load<UMaterialInterface>(Op.Material);
			Landscape->PostEditChange();
			return true;
		}
		if (Kind == TEXT("landscape.add_paint_layer"))
		{
			for (ULandscapeLayerInfoObject* LayerInfo : MakeLayerInfos(Op.PaintLayers))
			{
				Landscape->AddTargetLayer(LayerInfo->GetLayerName(), FLandscapeTargetLayerSettings(LayerInfo));
				Info->UpdateLayerInfoMap(Landscape);
				const int32 LayerIndex = Info->GetLayerInfoIndex(LayerInfo->GetLayerName());
				if (Info->Layers.IsValidIndex(LayerIndex)) Info->Layers[LayerIndex].LayerInfoObj = LayerInfo;
			}
			return true;
		}
		if (Kind == TEXT("landscape.add_edit_layer"))
		{
			if (Landscape->CreateLayer(FName(*Op.EditLayer)) == INDEX_NONE)
			{
				OutError = TEXT("The landscape would not take another edit layer (it may have reached its limit).");
				return false;
			}
			return true;
		}
		if (Kind == TEXT("landscape.road"))
		{
			return BuildRoad(World, *Landscape, *Info, Op, OutNotes, OutError);
		}
		FGuid Layer;
		if (!EditLayerGuid(*Landscape, Op.EditLayer, Layer, OutError)) return false;
		FString Problem;
		{
			FScopedSetLandscapeEditingLayer Editing(Landscape, Layer);
			Problem = Kind == TEXT("landscape.sculpt") ? Sculpt(*Landscape, Op, Layer) : Paint(*Landscape, Op, Layer);
		}
		if (!Problem.IsEmpty())
		{
			OutError = Problem;
			return false;
		}
		// Merge the edit layers now, so the report and the next op read the result.
		Landscape->ForceUpdateLayersContent();
		return true;
	}

	/** The plan for real: every op as one undo step, the merged result read back, then saved as asked. */
	FHyperAILandscapePlanReport Execute(const FHyperAILandscapePlanRequest& Request, const FString& OperationId)
	{
		FHyperAILandscapePlanReport Report;
		Report.OperationId = OperationId;
		UWorld* World = EditorWorld();
		if (!World)
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("No level is open in the editor.");
			return Report;
		}
		TArray<ALandscape*> Touched;
		TArray<UObject*> Assets;
		{
			const FScopedTransaction Transaction(LOCTEXT("LandscapePlan", "HyperAI: Edit Landscapes"));
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				FString Error;
				if (!Apply(*World, Request.Operations[Index], Touched, Assets, Report.Notes, Error))
				{
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Request.Operations[Index].Kind, *Error,
						Report.Applied > 0 ? TEXT(" The operations before it stay applied, unsaved; Edit > Undo takes the plan back.") : TEXT(""));
					return Report;
				}
				++Report.Applied;
			}
		}
		for (ALandscape* Landscape : Touched)
		{
			Report.Made.Add(Summarize(Describe(*Landscape)));
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
				Report.Diagnostic = TEXT("The level could not be saved; the landscapes are changed in memory.");
				return Report;
			}
			Report.Saved.Add(World->GetOutermost()->GetName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied%s."), Report.Applied, Request.bSave ? TEXT(" and saved") : TEXT("; the level is unsaved"));
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAILandscapePlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Made.IsEmpty() ? FString() : Done.Made[0].Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
}

FHyperAILandscapeInspectReport FHyperAIStudioLandscapeContracts::Inspect(const FHyperAILandscapeInspectRequest& Request)
{
	using namespace HyperAIStudio::LandscapeTools::Private;
	FHyperAILandscapeInspectReport Report;
	UWorld* World = EditorWorld();
	if (!World)
	{
		Report.Diagnostic = TEXT("No level is open in the editor.");
		return Report;
	}
	const ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>();
	Report.bWorldPartition = Subsystem && Subsystem->IsGridBased();
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (!Request.Landscapes.IsEmpty() && !Request.Landscapes.ContainsByPredicate([&It](const FString& Label) { return It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase); }))
		{
			continue;
		}
		Report.Landscapes.Add(Describe(**It));
	}
	if (!Request.Landscapes.IsEmpty() && Report.Landscapes.Num() != Request.Landscapes.Num())
	{
		Report.Diagnostic = TEXT("Not every named landscape is in the open level.");
		return Report;
	}
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%d landscape(s)%s."), Report.Landscapes.Num(), Report.bWorldPartition ? TEXT(", World Partition") : TEXT(""));
	return Report;
}

FHyperAILandscapePlanReport FHyperAIStudioLandscapeContracts::ApplyPlan(const FHyperAILandscapePlanRequest& Request)
{
	using namespace HyperAIStudio::LandscapeTools::Private;
	FHyperAILandscapePlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("landscape-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
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
		Report.Diagnostic = TEXT("This level has never been saved, so there is nowhere to save the landscape; save the level first or leave bSave off.");
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
		Summary.VariantId = TEXT("landscape.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.Operations[0].Landscape;
		Summary.Effects.Add(FString::Printf(TEXT("Edit landscapes: %d operation(s), one undo step%s"), Request.Operations.Num(), Request.bSave ? TEXT(", then the level saved") : TEXT("")));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAILandscapeOp& Op = Request.Operations[Index];
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s%s"), *Op.Kind, *Op.Landscape, Op.Mode.IsEmpty() ? TEXT("") : *(TEXT(" ") + Op.Mode)));
			Summary.Touches.AddUnique(Op.Landscape);
		}
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

FHyperAILandscapeInspectReport UHyperAIStudioLandscapeToolset::hyper_landscape_inspect(const FHyperAILandscapeInspectRequest& Request)
{
	return FHyperAIStudioLandscapeContracts::Inspect(Request);
}

FHyperAILandscapePlanReport UHyperAIStudioLandscapeToolset::hyper_landscape_apply_plan(const FHyperAILandscapePlanRequest& Request)
{
	return FHyperAIStudioLandscapeContracts::ApplyPlan(Request);
}

void FHyperAIStudioLandscapeRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioLandscapeRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioLandscapeRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioLandscapeToolset::StaticClass(),
			FHyperAIStudioLandscapeContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioLandscape, Warning, TEXT("Could not unregister the landscape toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioLandscapeRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioLandscapeContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioLandscape, Verbose, TEXT("The landscape source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioLandscapeToolset::StaticClass(),
		FHyperAIStudioLandscapeContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioLandscape, Error, TEXT("Landscape cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
