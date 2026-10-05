// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Elements/PCGBoundsModifier.h"
#include "Elements/PCGCullPointsOutsideActorBounds.h"
#include "Elements/PCGDifferenceElement.h"
#include "Elements/PCGInnerIntersectionElement.h"
#include "Elements/PCGSplineSampler.h"
#include "Elements/PCGSurfaceSampler.h"
#include "Elements/PCGTypedGetter.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Grid/PCGPartitionActor.h"
#include "Helpers/PCGHelpers.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioPCGScatterToolset.h"
#include "HyperAIStudioSettings.h"
#include "JsonObjectConverter.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeSplineActor.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeSubsystem.h"
#include "LandscapeUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "ObjectTools.h"
#include "PCGComponent.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGManagedResource.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGVolume.h"
#include "PCGWorldActor.h"
#include "Tests/AutomationCommon.h"
#include "WorldPartition/WorldPartition.h"

namespace HyperAIStudio::PCGScatterTests
{
	using C = FHyperAIStudioPCGScatterContracts;

	FHyperAIPCGScatterOp Op(const TCHAR* Kind)
	{
		FHyperAIPCGScatterOp Out;
		Out.Kind = Kind;
		return Out;
	}

	FHyperAIPCGScatterPlanReport Run(TArray<FHyperAIPCGScatterOp> Ops, const bool bDryRun = false)
	{
		FHyperAIPCGScatterPlanRequest Plan;
		Plan.Operations = MoveTemp(Ops);
		Plan.bDryRun = bDryRun;
		return C::ApplyPlan(Plan);
	}

	/**
	 * A 63 m square landscape, flat on its first 32 m along X and rising at about 56 degrees beyond. Split, it is cut into
	 * World Partition streaming proxies, as hyper_landscape_apply_plan cuts new landscapes.
	 */
	ALandscape* MakeLandscape(UWorld& World, const FVector& Corner, const FString& Label, const bool bSplit)
	{
		constexpr int32 Size = 64;
		TArray<uint16> Heights;
		Heights.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				// At a Z scale of 100, 128 units are a meter: 192 units per meter across is a rise of 1.5.
				Heights[Y * Size + X] = static_cast<uint16>(32768 + (X < 32 ? 0 : (X - 32) * 192));
			}
		}
		ALandscape* Landscape = World.SpawnActor<ALandscape>(Corner, FRotator::ZeroRotator);
		if (!Landscape) return nullptr;
		Landscape->SetActorRelativeScale3D(FVector(100.0, 100.0, 100.0));
		TMap<FGuid, TArray<uint16>> HeightData;
		HeightData.Add(FGuid(), MoveTemp(Heights));
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> Layers;
		Layers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
		Landscape->Import(FGuid::NewGuid(), 0, 0, Size - 1, Size - 1, 1, 63, HeightData, TEXT(""), Layers, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
		Landscape->SetActorLabel(Label);
		ULandscapeSubsystem* Subsystem = World.GetSubsystem<ULandscapeSubsystem>();
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (bSplit && Info && Subsystem && Subsystem->IsGridBased()) Subsystem->ChangeGridSize(Info, 2);
		// Merge the edit layers now, so the ground PCG samples is the ground just imported.
		Landscape->ForceUpdateLayersContent();
		return Landscape;
	}

	ALandscape* FindLandscape(const FString& Label)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		for (TActorIterator<ALandscape> It(World); World && It; ++It)
		{
			if (It->GetActorLabel() == Label) return *It;
		}
		return nullptr;
	}

	/** A test landscape and the streaming proxies it was split into. */
	void DestroyLandscape(const FString& Label)
	{
		ALandscape* Landscape = FindLandscape(Label);
		if (!Landscape) return;
		TArray<ALandscapeProxy*> Proxies;
		if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
		{
			Info->ForEachLandscapeProxy([&Proxies](ALandscapeProxy* Proxy) { Proxies.Add(Proxy); return true; });
		}
		Proxies.AddUnique(Landscape);
		for (ALandscapeProxy* Proxy : Proxies) Proxy->GetWorld()->EditorDestroyActor(Proxy, /*bShouldModifyLevel=*/true);
	}

	/** Where the volume's instances stand, in the world. */
	TArray<FVector> Instances(const APCGVolume* Volume)
	{
		TArray<FVector> Out;
		if (!Volume) return Out;
		TArray<UInstancedStaticMeshComponent*> Components;
		Volume->GetComponents<UInstancedStaticMeshComponent>(Components);
		for (const UInstancedStaticMeshComponent* Component : Components)
		{
			for (int32 Index = 0; Component && Index < Component->GetInstanceCount(); ++Index)
			{
				FTransform Transform;
				if (Component->GetInstanceTransform(Index, Transform, /*bWorldSpace=*/true)) Out.Add(Transform.GetLocation());
			}
		}
		return Out;
	}

	APCGVolume* FindVolume(const FString& Label)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		for (TActorIterator<APCGVolume> It(World); World && It; ++It)
		{
			if (It->GetActorLabel() == Label) return *It;
		}
		return nullptr;
	}

	/** What the generation job said when it finished. */
	FString JobSaid(const FString& OperationId)
	{
		FString Detail;
		for (const FHyperAIStudioActivityEntry& Entry : FHyperAIStudioAgentActivityLog::GetSnapshot())
		{
			if (Entry.OperationId == OperationId && !Entry.Detail.IsEmpty()) Detail = Entry.Detail;
		}
		return Detail;
	}

	/** Shared across the test's latent steps. */
	struct FState
	{
		FString OperationId;
		/** The generation over a landscape kept whole, in a World Partition level. */
		FString WholeId;
		double Began = 0.0;
		int32 Flat = 0;
	};

	/** The ecosystem test's ground, in meters from its corner: 252 m square, a vertex a meter. */
	namespace Eco
	{
		constexpr int32 Size = 253;
		/** Meadow is painted west of this, Forest east. */
		constexpr double Middle = 126.0;
		/** A road across both, on the vertex rows from RoadFrom up to RoadTo. */
		constexpr double RoadFrom = 120.0;
		constexpr double RoadTo = 132.0;
		/**
		 * A landscape spline road across both too, along X at LaneY, LaneWidth meters to each side of its center and
		 * LaneFalloff more of shoulder; nothing is painted under it.
		 */
		constexpr double LaneY = 40.0;
		constexpr double LaneWidth = 3.0;
		constexpr double LaneFalloff = 2.0;
		/** A partition cell's edge crosses the flat forest here, along Y, when the level streams. */
		constexpr double SeamX = 190.0;
		/** A hut on the flat meadow at HutX, HutY: a 1 m cube HutScale times its size, kept HutMargin meters clear of. */
		constexpr double HutX = 60.0;
		constexpr double HutY = 90.0;
		constexpr double HutScale = 4.0;
		constexpr double HutMargin = 2.0;
		/** Flat up to GentleFrom, then 20 degrees up to SteepFrom, then 56 degrees. */
		constexpr double GentleFrom = 180.0;
		constexpr double SteepFrom = 220.0;

		const TCHAR* const TreeMesh = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
		const TCHAR* const BushMesh = TEXT("/Engine/BasicShapes/Sphere.Sphere");
		const TCHAR* const RockMesh = TEXT("/Engine/BasicShapes/Cube.Cube");
		const TCHAR* const GrassMesh = TEXT("/Engine/BasicShapes/Cone.Cone");
		const TCHAR* const PebbleMesh = TEXT("/Engine/BasicShapes/Plane.Plane");
		const TCHAR* const StoneMesh = TEXT("/Engine/EngineMeshes/Sphere.Sphere");
		const TCHAR* const FernMesh = TEXT("/Engine/EngineMeshes/Cylinder.Cylinder");

		/** Meters above the corner. */
		double Height(const double Y)
		{
			const double Gentle = FMath::Tan(FMath::DegreesToRadians(20.0)) * FMath::Clamp(Y - GentleFrom, 0.0, SteepFrom - GentleFrom);
			return Gentle + 1.5 * FMath::Max(Y - SteepFrom, 0.0);
		}
	}

	/**
	 * The ecosystem test's ground: Meadow, Forest and Road painted as Eco lays them out, its layer infos made in Folder,
	 * split into World Partition streaming proxies when the level streams.
	 */
	ALandscape* MakeEcosystemGround(UWorld& World, const FVector& Corner, const FString& Label, const FString& Folder)
	{
		constexpr int32 Size = Eco::Size;
		TArray<uint16> Heights;
		Heights.SetNumUninitialized(Size * Size);
		TArray<uint8> Meadow;
		TArray<uint8> Forest;
		TArray<uint8> Road;
		Meadow.SetNumZeroed(Size * Size);
		Forest.SetNumZeroed(Size * Size);
		Road.SetNumZeroed(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const int32 Index = Y * Size + X;
				// At a Z scale of 100, 128 units are a meter.
				Heights[Index] = static_cast<uint16>(FMath::RoundToInt(32768.0 + Eco::Height(Y) * 128.0));
				TArray<uint8>& Painted = Y >= Eco::RoadFrom && Y < Eco::RoadTo ? Road : (X < Eco::Middle ? Meadow : Forest);
				Painted[Index] = 255;
			}
		}
		ALandscape* Landscape = World.SpawnActor<ALandscape>(Corner, FRotator::ZeroRotator);
		if (!Landscape) return nullptr;
		Landscape->SetActorRelativeScale3D(FVector(100.0, 100.0, 100.0));
		TMap<FGuid, TArray<uint16>> HeightData;
		HeightData.Add(FGuid(), MoveTemp(Heights));
		TArray<ULandscapeLayerInfoObject*> Infos;
		TArray<FLandscapeImportLayerInfo> Imports;
		const TPair<const TCHAR*, TArray<uint8>*> Layers[] = {{TEXT("Meadow"), &Meadow}, {TEXT("Forest"), &Forest}, {TEXT("Road"), &Road}};
		for (const TPair<const TCHAR*, TArray<uint8>*>& Layer : Layers)
		{
			ULandscapeLayerInfoObject* Info = UE::Landscape::CreateTargetLayerInfo(FName(Layer.Key), Folder);
			if (!Info) continue;
			FLandscapeImportLayerInfo& Import = Imports.Emplace_GetRef(FName(Layer.Key));
			Import.LayerInfo = Info;
			Import.LayerData = MoveTemp(*Layer.Value);
			Infos.Add(Info);
		}
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> LayerData;
		LayerData.Add(FGuid(), MoveTemp(Imports));
		Landscape->Import(FGuid::NewGuid(), 0, 0, Size - 1, Size - 1, 1, 63, HeightData, TEXT(""), LayerData, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
		Landscape->SetActorLabel(Label);
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (!Info) return Landscape;
		Info->UpdateLayerInfoMap(Landscape);
		for (ULandscapeLayerInfoObject* LayerInfo : Infos)
		{
			Landscape->AddTargetLayer(LayerInfo->GetLayerName(), FLandscapeTargetLayerSettings(LayerInfo));
			const int32 Index = Info->GetLayerInfoIndex(LayerInfo->GetLayerName());
			if (Info->Layers.IsValidIndex(Index)) Info->Layers[Index].LayerInfoObj = LayerInfo;
		}
		ULandscapeSubsystem* Subsystem = World.GetSubsystem<ULandscapeSubsystem>();
		if (Subsystem && Subsystem->IsGridBased()) Subsystem->ChangeGridSize(Info, 2);
		Landscape->ForceUpdateLayersContent();
		return Landscape;
	}

	/**
	 * The ecosystem ground's lane: a straight landscape spline along X at Eco::LaneY, running 20 m past the ground at
	 * both ends so its ends play no part, kept as the landscape tool keeps a road (a spline actor labelled Label in World
	 * Partition, else the landscape's own splines). The ground is not reshaped: PCG reads the spline alone.
	 */
	bool MakeLane(UWorld& World, ALandscape& Landscape, const FVector& Corner, const FString& Label)
	{
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		if (!Info) return false;
		const FVector Ends[] = {Corner + FVector(-2000.0, Eco::LaneY * 100.0, 0.0), Corner + FVector((Eco::Size - 1 + 20) * 100.0, Eco::LaneY * 100.0, 0.0)};
		ILandscapeSplineInterface* Owner = nullptr;
		const ULandscapeSubsystem* Subsystem = World.GetSubsystem<ULandscapeSubsystem>();
		if (Subsystem && Subsystem->IsGridBased())
		{
			ALandscapeSplineActor* Actor = Info->CreateSplineActor(Ends[0]);
			if (Actor) Actor->SetActorLabel(Label);
			Owner = Actor;
		}
		else
		{
			Owner = Info->GetCurrentLevelLandscapeProxy(/*bRegistered=*/true);
		}
		if (!Owner) return false;
		if (!Owner->GetSplinesComponent()) Owner->CreateSplineComponent();
		ULandscapeSplinesComponent* Splines = Owner->GetSplinesComponent();
		if (!Splines) return false;
		const FTransform ToWorld = Splines->GetComponentTransform();
		ULandscapeSplineControlPoint* Controls[2] = {};
		for (int32 Index = 0; Index < 2; ++Index)
		{
			Controls[Index] = NewObject<ULandscapeSplineControlPoint>(Splines, NAME_None, RF_Transactional);
			Splines->GetControlPoints().Add(Controls[Index]);
			Controls[Index]->Location = ToWorld.InverseTransformPosition(Ends[Index]);
			Controls[Index]->Width = static_cast<float>(Eco::LaneWidth * 100.0);
			Controls[Index]->SideFalloff = static_cast<float>(Eco::LaneFalloff * 100.0);
		}
		ULandscapeSplineSegment* Segment = NewObject<ULandscapeSplineSegment>(Splines, NAME_None, RF_Transactional);
		Splines->GetSegments().Add(Segment);
		const float Length = static_cast<float>(FVector::Dist(Controls[0]->Location, Controls[1]->Location));
		for (int32 End = 0; End < 2; ++End)
		{
			Segment->Connections[End].ControlPoint = Controls[End];
			Segment->Connections[End].TangentLen = Length;
			Controls[End]->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, End));
		}
		Segment->AutoFlipTangents();
		for (ULandscapeSplineControlPoint* Control : Controls) Control->AutoCalcRotation(/*bAlwaysRotateForward=*/true);
		for (ULandscapeSplineControlPoint* Control : Controls) Control->UpdateSplinePoints();
		if (!Splines->IsRegistered()) Splines->RegisterComponent();
		return Segment->GetPoints().Num() > 1;
	}

	void DestroyLane(UWorld& World, const FString& Label)
	{
		for (TActorIterator<ALandscapeSplineActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel() == Label) World.EditorDestroyActor(*It, /*bShouldModifyLevel=*/true);
		}
	}

	/** One instance a generation made: where it stands and how it turns, the actor it is on, and where it stops drawing. */
	struct FPlaced
	{
		FTransform Transform;
		const AActor* Owner = nullptr;
		int32 EndCull = 0;
	};

	/** The actors a volume's generation puts meshes on: the volume, and the partition actors of its cells. */
	TArray<AActor*> OutputActors(APCGVolume* Volume)
	{
		TArray<AActor*> Actors;
		if (!Volume) return Actors;
		Actors.Add(Volume);
		const UPCGComponent* Component = Volume->PCGComponent;
		for (TActorIterator<APCGPartitionActor> It(Volume->GetWorld()); Component && It; ++It)
		{
			if (It->GetLocalComponent(Component)) Actors.Add(*It);
		}
		return Actors;
	}

	/** Every instance on these actors' mesh components, by mesh path. */
	TMap<FString, TArray<FPlaced>> Placed(const TArray<const UInstancedStaticMeshComponent*>& Components)
	{
		TMap<FString, TArray<FPlaced>> Out;
		for (const UInstancedStaticMeshComponent* Component : Components)
		{
			const UStaticMesh* Mesh = IsValid(Component) ? Component->GetStaticMesh() : nullptr;
			if (!Mesh || Component->GetInstanceCount() == 0) continue;
			TArray<FPlaced>& List = Out.FindOrAdd(Mesh->GetPathName());
			for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index)
			{
				FPlaced& Item = List.AddDefaulted_GetRef();
				Component->GetInstanceTransform(Index, Item.Transform, /*bWorldSpace=*/true);
				Item.Owner = Component->GetOwner();
				Item.EndCull = Component->InstanceEndCullDistance;
			}
		}
		return Out;
	}

	/** Every instanced mesh component on these actors. */
	TArray<const UInstancedStaticMeshComponent*> ComponentsOn(const TArray<AActor*>& Actors)
	{
		TArray<const UInstancedStaticMeshComponent*> Out;
		for (const AActor* Actor : Actors)
		{
			if (!IsValid(Actor)) continue;
			TArray<UInstancedStaticMeshComponent*> Components;
			Actor->GetComponents<UInstancedStaticMeshComponent>(Components);
			Out.Append(Components);
		}
		return Out;
	}

	/**
	 * What a volume's generation made, as PCG manages it for the volume and its local components. Partition actors are
	 * shared by every partitioned volume on the grid, so this is how a tool must count, and the test counts the same way.
	 */
	TArray<const UInstancedStaticMeshComponent*> Generated(APCGVolume* Volume)
	{
		TArray<const UInstancedStaticMeshComponent*> Out;
		const UPCGComponent* Component = Volume ? Volume->PCGComponent.Get() : nullptr;
		if (!Component) return Out;
		TArray<const UPCGComponent*> Sources = {Component};
		for (TActorIterator<APCGPartitionActor> It(Volume->GetWorld()); It; ++It)
		{
			if (const UPCGComponent* Local = It->GetLocalComponent(Component)) Sources.Add(Local);
		}
		for (const UPCGComponent* Source : Sources)
		{
			Source->ForEachConstManagedResource([&Out](const UPCGManagedResource* Resource)
			{
				const UPCGManagedISMComponent* Managed = Cast<UPCGManagedISMComponent>(Resource);
				if (const UInstancedStaticMeshComponent* Instanced = Managed ? Managed->GetComponent() : nullptr) Out.AddUnique(Instanced);
			});
		}
		return Out;
	}

	int32 CountPlaced(const TArray<const UInstancedStaticMeshComponent*>& Components)
	{
		int32 Count = 0;
		for (const TPair<FString, TArray<FPlaced>>& Pair : Placed(Components)) Count += Pair.Value.Num();
		return Count;
	}

	/** Where an instance stands on the ecosystem ground, in meters from its corner. */
	FVector2D OnGround(const FPlaced& Item, const FVector& Corner)
	{
		const FVector At = Item.Transform.GetLocation() - Corner;
		return FVector2D(At.X / 100.0, At.Y / 100.0);
	}

	int32 CountWhere(const TArray<FPlaced>& Items, const FVector& Corner, TFunctionRef<bool(const FVector2D&)> Where)
	{
		int32 Count = 0;
		for (const FPlaced& Item : Items)
		{
			if (Where(OnGround(Item, Corner))) ++Count;
		}
		return Count;
	}

	/** Degrees an instance's up axis leans from the vertical. */
	double LeanOf(const FPlaced& Item)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Item.Transform.GetRotation().GetUpVector().Z, -1.0, 1.0)));
	}

	/** The nearest of Others to Item, on any actor (species keep out of each other across a partition cell's edge too), in meters. */
	double Nearest(const FPlaced& Item, const TArray<FPlaced>& Others)
	{
		double Best = TNumericLimits<double>::Max();
		for (const FPlaced& Other : Others)
		{
			if (&Other == &Item) continue;
			Best = FMath::Min(Best, FVector::Dist2D(Item.Transform.GetLocation(), Other.Transform.GetLocation()) / 100.0);
		}
		return Best;
	}

	/** Pairs of an Item and one of Others on different actors (two partition cells) within Meters of each other. */
	int32 PairsAcrossCells(const TArray<FPlaced>& Items, const TArray<FPlaced>& Others, const double Meters)
	{
		int32 Pairs = 0;
		for (const FPlaced& Item : Items)
		{
			for (const FPlaced& Other : Others)
			{
				if (Other.Owner != Item.Owner && FVector::Dist2D(Item.Transform.GetLocation(), Other.Transform.GetLocation()) < Meters * 100.0) ++Pairs;
			}
		}
		return Pairs;
	}

	/** Instances standing where another of the same mesh stands, to the centimeter: made twice, by two cells. */
	int32 MadeTwice(const TArray<FPlaced>& Items)
	{
		TSet<FIntVector> Seen;
		int32 Twice = 0;
		for (const FPlaced& Item : Items)
		{
			const FVector At = Item.Transform.GetLocation();
			bool bSeen = false;
			Seen.Add(FIntVector(FMath::RoundToInt(At.X), FMath::RoundToInt(At.Y), FMath::RoundToInt(At.Z)), &bSeen);
			if (bSeen) ++Twice;
		}
		return Twice;
	}

	/** Meters from the hut's center along the farther axis: the distance its square box measures. */
	double FromHut(const FPlaced& Item, const FVector& Corner)
	{
		const FVector2D At = OnGround(Item, Corner) - FVector2D(Eco::HutX, Eco::HutY);
		return FMath::Max(FMath::Abs(At.X), FMath::Abs(At.Y));
	}

	/** Meters from the lane's center line to the nearest of Items, and how many stand within Band meters past From. */
	double FromLane(const TArray<FPlaced>& Items, const FVector& Corner, const double From = 0.0, const double Band = 0.0, int32* OutInBand = nullptr)
	{
		double Best = TNumericLimits<double>::Max();
		int32 InBand = 0;
		for (const FPlaced& Item : Items)
		{
			const double Away = FMath::Abs(OnGround(Item, Corner).Y - Eco::LaneY);
			Best = FMath::Min(Best, Away);
			if (Away >= From && Away < From + Band) ++InBand;
		}
		if (OutInBand) *OutInBand = InBand;
		return Best;
	}

	/** The node linked to a pin of Node, upstream when bInput, else downstream; null when none. */
	const UPCGNode* LinkedTo(const UPCGNode& Node, const FName Pin, const bool bInput)
	{
		const UPCGPin* Found = bInput ? Node.GetInputPin(Pin) : Node.GetOutputPin(Pin);
		const UPCGEdge* Edge = Found && !Found->Edges.IsEmpty() ? Found->Edges[0].Get() : nullptr;
		const UPCGPin* Other = Edge ? (bInput ? Edge->InputPin.Get() : Edge->OutputPin.Get()) : nullptr;
		return Other ? Other->Node.Get() : nullptr;
	}

	/** Every node of a graph whose settings are of type T. */
	template <typename T>
	TArray<const UPCGNode*> NodesOf(const UPCGGraph& Graph)
	{
		TArray<const UPCGNode*> Out;
		for (const UPCGNode* Node : Graph.GetNodes())
		{
			if (Node && Cast<T>(Node->GetSettings())) Out.Add(Node);
		}
		return Out;
	}

	/** A graph's node by its authored title. */
	const UPCGNode* Titled(const UPCGGraph& Graph, const FString& Title)
	{
		for (const UPCGNode* Node : Graph.GetNodes())
		{
			if (Node && Node->HasAuthoredTitle() && Node->NodeTitle.ToString() == Title) return Node;
		}
		return nullptr;
	}

	/** The share of the meadow's 2 m cells, clear of the roads and the edges, that have grass. */
	double GrassCoverage(const TArray<FPlaced>& Grass, const FVector& Corner)
	{
		TSet<FIntPoint> Grown;
		for (const FPlaced& Blade : Grass)
		{
			const FVector2D At = OnGround(Blade, Corner);
			Grown.Add(FIntPoint(FMath::FloorToInt(At.X / 2.0), FMath::FloorToInt(At.Y / 2.0)));
		}
		int32 Cells = 0;
		int32 Covered = 0;
		for (int32 X = 1; X < 61; ++X)
		{
			for (int32 Y = 1; Y < 108; ++Y)
			{
				// The painted road, and the lane with the 3 m kept off it.
				if ((Y >= 59 && Y < 67) || (Y >= 16 && Y < 24)) continue;
				++Cells;
				if (Grown.Contains(FIntPoint(X, Y))) ++Covered;
			}
		}
		return Cells > 0 ? static_cast<double>(Covered) / Cells : 0.0;
	}

	/** Shared across the ecosystem test's latent steps. */
	struct FEcosystemState
	{
		FString OperationId;
		double Began = 0.0;
		/** The actors the first generation put meshes on, to check the clear and clean up. */
		TArray<TWeakObjectPtr<AActor>> Outputs;
		int32 Trees = 0;
	};

	TArray<AActor*> Pinned(const TArray<TWeakObjectPtr<AActor>>& Actors)
	{
		TArray<AActor*> Out;
		for (const TWeakObjectPtr<AActor>& Actor : Actors)
		{
			if (Actor.IsValid()) Out.Add(Actor.Get());
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPCGScatterEndToEndTest,
	"HyperAIStudio.NativeTools.PCG.ScatterEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPCGScatterEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PCGScatterTests;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		AddInfo(TEXT("Skipped: no editor world."));
		return true;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/PCGScatter_") + Suffix;
	const FString Graph = Folder + TEXT("/PCG_Rocks");
	const FString VolumeLabel = TEXT("HyperAI_TestScatter_") + Suffix;
	const FString GroundLabel = TEXT("HyperAI_TestScatterGround_") + Suffix;
	const FString WholeLabel = TEXT("HyperAI_TestScatterWhole_") + Suffix;
	const FString WholeVolumeLabel = TEXT("HyperAI_TestScatterOverWhole_") + Suffix;
	// Far from the bench level; the slope starts 32 m along X from the corner.
	const FVector Corner(900000.0, 900000.0, 30000.0);
	const double SlopeStartsAt = Corner.X + 3200.0;
	if (!TestNotNull(TEXT("the test landscape is made"), MakeLandscape(*World, Corner, GroundLabel, /*bSplit=*/true))) return false;
	// A landscape kept whole and never saved is one PCG cannot see in a World Partition level; the tool names it.
	const ULandscapeSubsystem* LandscapeSubsystem = World->GetSubsystem<ULandscapeSubsystem>();
	const bool bGridBased = LandscapeSubsystem && LandscapeSubsystem->IsGridBased();
	const FVector WholeCorner = Corner + FVector(20000.0, 0.0, 0.0);
	if (bGridBased) MakeLandscape(*World, WholeCorner, WholeLabel, /*bSplit=*/false);
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	auto Cleanup = [World, VolumeLabel, WholeVolumeLabel, GroundLabel, WholeLabel, Folder]()
	{
		for (const FString& Label : {VolumeLabel, WholeVolumeLabel})
		{
			if (APCGVolume* Volume = FindVolume(Label)) World->EditorDestroyActor(Volume, /*bShouldModifyLevel=*/true);
		}
		DestroyLandscape(GroundLabel);
		DestroyLandscape(WholeLabel);
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

	FHyperAIPCGScatterOp MakeGraph = Op(TEXT("graph.create"));
	MakeGraph.Graph = Graph;
	FHyperAIPCGScatterMesh Rock;
	Rock.Mesh = TEXT("/Engine/BasicShapes/Cube");
	MakeGraph.Meshes = {Rock};
	MakeGraph.Density = 0.3f;
	MakeGraph.SlopeMax = 30.f;
	MakeGraph.Scale = FVector2D(0.2, 0.3);
	FHyperAIPCGScatterOp Place = Op(TEXT("volume.create"));
	Place.Graph = Graph;
	Place.Volume = VolumeLabel;
	Place.Center = Corner + FVector(3150.0, 3150.0, 0.0);
	Place.Size = FVector2D(60.0, 60.0);
	Place.Height = 200.f;

	// Refused before anything changes.
	FHyperAIPCGScatterOp Steep = MakeGraph;
	Steep.SlopeMax = 120.f;
	TestFalse(TEXT("a slope past 90 degrees is refused"), Run({Steep}, /*bDryRun=*/true).bOk);
	FHyperAIPCGScatterOp Empty = MakeGraph;
	Empty.Meshes.Reset();
	TestFalse(TEXT("a graph with nothing to scatter is refused"), Run({Empty}, /*bDryRun=*/true).bOk);
	TestFalse(TEXT("generating before the last op is refused"), Run({MakeGraph, Place, MakeGraph}, /*bDryRun=*/true).bOk);

	Settings->bRequireApprovalForAgentEdits = false;
	const FHyperAIPCGScatterPlanReport Started = Run({MakeGraph, Place});
	Settings->bRequireApprovalForAgentEdits = bApproval;
	if (!TestTrue(TEXT("the graph is built and the volume starts generating: ") + Started.Diagnostic + TEXT(" | ") + FString::Join(Started.Notes, TEXT(" | ")),
		Started.bOk && Started.Status == TEXT("generating")))
	{
		Cleanup();
		return false;
	}
	AddInfo(TEXT("Volumes: ") + FString::Join(C::Inspect(FHyperAIPCGScatterInspectRequest()).Volumes, TEXT(" | ")));
	FHyperAIPCGScatterInspectRequest Look;
	Look.Graph = Graph;
	const FString Described = FString::Join(C::Inspect(Look).Settings, TEXT(" | "));
	TestTrue(TEXT("inspect reads the graph's rules: ") + Described, Described.Contains(TEXT("Slope up to 30 degrees")) && Described.Contains(TEXT("Cube")));

	const TSharedRef<FState> State = MakeShared<FState>();
	State->OperationId = Started.OperationId;
	if (bGridBased)
	{
		FHyperAIPCGScatterOp OverWhole = Place;
		OverWhole.Volume = WholeVolumeLabel;
		OverWhole.Center = WholeCorner + FVector(3150.0, 3150.0, 0.0);
		Settings->bRequireApprovalForAgentEdits = false;
		const FHyperAIPCGScatterPlanReport Whole = Run({OverWhole});
		Settings->bRequireApprovalForAgentEdits = bApproval;
		TestTrue(TEXT("a volume over the landscape kept whole starts generating: ") + Whole.Diagnostic, Whole.bOk && Whole.Status == TEXT("generating"));
		State->WholeId = Whole.OperationId;
	}
	else
	{
		AddInfo(TEXT("Not a World Partition level: the landscape kept whole is not tried."));
	}
	State->Began = FPlatformTime::Seconds();
	// First generation: rocks on the flat half only.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, VolumeLabel, WholeLabel, SlopeStartsAt, Graph, Settings, bApproval]()
	{
		const bool bRunning = FHyperAIStudioAsyncJobHost::IsRunning(State->OperationId) || (!State->WholeId.IsEmpty() && FHyperAIStudioAsyncJobHost::IsRunning(State->WholeId));
		if (bRunning && FPlatformTime::Seconds() - State->Began < 120.0) return false;
		AddInfo(TEXT("First generation: ") + JobSaid(State->OperationId));
		if (!State->WholeId.IsEmpty())
		{
			const FString Said = JobSaid(State->WholeId);
			TestTrue(TEXT("the landscape kept whole is named, with what to do: ") + Said, Said.Contains(WholeLabel) && Said.Contains(TEXT("save it (bSave)")));
		}
		const TArray<FVector> Placed = Instances(FindVolume(VolumeLabel));
		const int32 OnSlope = Placed.FilterByPredicate([SlopeStartsAt](const FVector& At) { return At.X > SlopeStartsAt + 150.0; }).Num();
		State->Flat = Placed.Num();
		TestTrue(FString::Printf(TEXT("rocks are scattered on the flat half (%d)"), Placed.Num()), Placed.Num() > 50);
		TestEqual(TEXT("and none on the slope steeper than 30 degrees"), OnSlope, 0);
		// Any slope, regenerated.
		FHyperAIPCGScatterOp Loosen = Op(TEXT("graph.set"));
		Loosen.Graph = Graph;
		Loosen.SlopeMax = 0.f;
		FHyperAIPCGScatterOp Again = Op(TEXT("volume.generate"));
		Again.Volume = VolumeLabel;
		Settings->bRequireApprovalForAgentEdits = false;
		const FHyperAIPCGScatterPlanReport Regenerating = Run({Loosen, Again});
		Settings->bRequireApprovalForAgentEdits = bApproval;
		TestTrue(TEXT("the graph is loosened and the volume regenerates: ") + Regenerating.Diagnostic, Regenerating.bOk && Regenerating.Status == TEXT("generating"));
		State->OperationId = Regenerating.OperationId;
		State->Began = FPlatformTime::Seconds();
		return true;
	}));
	// Second generation: rocks up the slope too; then the volume is cleared.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, VolumeLabel, SlopeStartsAt, Settings, bApproval]()
	{
		if (FHyperAIStudioAsyncJobHost::IsRunning(State->OperationId) && FPlatformTime::Seconds() - State->Began < 120.0) return false;
		AddInfo(TEXT("Second generation: ") + JobSaid(State->OperationId));
		const TArray<FVector> Placed = Instances(FindVolume(VolumeLabel));
		const int32 OnSlope = Placed.FilterByPredicate([SlopeStartsAt](const FVector& At) { return At.X > SlopeStartsAt + 150.0; }).Num();
		TestTrue(FString::Printf(TEXT("with any slope allowed, rocks climb it too (%d of %d)"), OnSlope, Placed.Num()), OnSlope > 20 && Placed.Num() > State->Flat);
		FHyperAIPCGScatterOp Clear = Op(TEXT("volume.clear"));
		Clear.Volume = VolumeLabel;
		Settings->bRequireApprovalForAgentEdits = false;
		TestTrue(TEXT("the volume is cleared"), Run({Clear}).bOk);
		Settings->bRequireApprovalForAgentEdits = bApproval;
		State->Began = FPlatformTime::Seconds();
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, VolumeLabel, Cleanup]()
	{
		const int32 Left = Instances(FindVolume(VolumeLabel)).Num();
		if (Left > 0 && FPlatformTime::Seconds() - State->Began < 30.0) return false;
		TestEqual(TEXT("and no instance is left"), Left, 0);
		Cleanup();
		return true;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPCGEcosystemEndToEndTest,
	"HyperAIStudio.NativeTools.PCG.EcosystemEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioPCGEcosystemEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::PCGScatterTests;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		AddInfo(TEXT("Skipped: no editor world."));
		return true;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/PCGEcosystem_") + Suffix;
	const FString Graph = Folder + TEXT("/PCG_Wilds");
	const FString VolumeLabel = TEXT("HyperAI_TestEcosystem_") + Suffix;
	const FString GroundLabel = TEXT("HyperAI_TestEcosystemGround_") + Suffix;
	const FString LaneLabel = TEXT("HyperAI_TestEcosystemLane_") + Suffix;
	const ULandscapeSubsystem* LandscapeSubsystem = World->GetSubsystem<ULandscapeSubsystem>();
	const bool bPartitioned = LandscapeSubsystem && LandscapeSubsystem->IsGridBased() && World->GetWorldPartition();
	// Far from the bench level and from the scatter test's ground, placed so a partition cell's edge crosses the flat forest.
	const APCGWorldActor* PCGWorld = PCGHelpers::FindPCGWorldActor(World);
	const double Grid = static_cast<double>(PCGWorld ? PCGWorld->PartitionGridSize : GetDefault<APCGWorldActor>()->PartitionGridSize);
	const FVector Corner(FMath::CeilToDouble(1500000.0 / Grid) * Grid - Eco::SeamX * 100.0, 900000.0, 30000.0);
	ALandscape* Ground = MakeEcosystemGround(*World, Corner, GroundLabel, Folder);
	if (!TestNotNull(TEXT("the painted ground is made"), Ground)) return false;
	TestTrue(TEXT("a landscape spline lane crosses it"), MakeLane(*World, *Ground, Corner, LaneLabel));
	// A tagged hut, scaled up: the margin kept clear of it is in meters, whatever its scale.
	const FString HutLabel = TEXT("HyperAI_TestEcosystemHut_") + Suffix;
	const FString HutTag = TEXT("HyperAITestHut") + Suffix;
	AStaticMeshActor* Hut = World->SpawnActor<AStaticMeshActor>(Corner + FVector(Eco::HutX * 100.0, Eco::HutY * 100.0, 0.0), FRotator::ZeroRotator);
	if (Hut)
	{
		Hut->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Eco::RockMesh));
		Hut->SetActorScale3D(FVector(Eco::HutScale));
		Hut->Tags.Add(FName(*HutTag));
		Hut->SetActorLabel(HutLabel);
	}
	TestNotNull(TEXT("a hut stands on the meadow"), Hut);
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	const TSharedRef<FEcosystemState> State = MakeShared<FEcosystemState>();
	auto Cleanup = [World, VolumeLabel, GroundLabel, LaneLabel, HutLabel, Folder, State]()
	{
		if (APCGVolume* Volume = FindVolume(VolumeLabel)) World->EditorDestroyActor(Volume, /*bShouldModifyLevel=*/true);
		// The partition actors the generation made, once nothing generates into them.
		for (AActor* Actor : Pinned(State->Outputs))
		{
			const APCGPartitionActor* Cell = Cast<APCGPartitionActor>(Actor);
			if (Cell && !Cell->HasGraphInstances()) World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
		}
		for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
		{
			if (It->GetActorLabel() == HutLabel) World->EditorDestroyActor(*It, /*bShouldModifyLevel=*/true);
		}
		DestroyLane(*World, LaneLabel);
		DestroyLandscape(GroundLabel);
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

	auto Mesh = [](const TCHAR* Path)
	{
		FHyperAIPCGScatterMesh Out;
		Out.Mesh = Path;
		return Out;
	};
	FHyperAIPCGSpecies Trees;
	Trees.Name = TEXT("Trees");
	Trees.Meshes = {Mesh(Eco::TreeMesh)};
	Trees.Density = 0.01f;
	Trees.Footprint = 4.f;
	Trees.Clearance = 1.25f;
	Trees.CullDistance = 300.f;
	FHyperAIPCGSpecies Bushes;
	Bushes.Name = TEXT("Bushes");
	Bushes.Meshes = {Mesh(Eco::BushMesh)};
	Bushes.Density = 0.005f;
	Bushes.Footprint = 1.5f;
	Bushes.Cluster = 5;
	Bushes.ClusterRadius = 2.f;
	FHyperAIPCGSpecies Rocks;
	Rocks.Name = TEXT("Rocks");
	Rocks.Meshes = {Mesh(Eco::RockMesh)};
	Rocks.Density = 0.05f;
	Rocks.Scale = FVector2D(0.3, 0.5);
	Rocks.bAlignToGround = true;
	Rocks.Tilt = 10.f;
	Rocks.Sink = 20.f;
	Rocks.KeepOutOf = {TEXT("Trees")};
	// Small plants that grow under the trees but keep out of the bushes' clumps, so clumps at a cell's edge are tested.
	FHyperAIPCGSpecies Ferns;
	Ferns.Name = TEXT("Ferns");
	Ferns.Meshes = {Mesh(Eco::FernMesh)};
	Ferns.Density = 1.f;
	Ferns.Footprint = 0.5f;
	Ferns.Scale = FVector2D(0.4, 0.6);
	Ferns.KeepOutOf = {TEXT("Bushes")};
	FHyperAIPCGBiome Woods;
	Woods.Name = TEXT("Woods");
	Woods.Layer = TEXT("Forest");
	Woods.SlopeMax = 30.f;
	Woods.Species = {Trees, Bushes, Rocks, Ferns};
	FHyperAIPCGSpecies Grass;
	Grass.Name = TEXT("Grass");
	Grass.Meshes = {Mesh(Eco::GrassMesh)};
	Grass.Density = 1.f;
	Grass.Footprint = 0.5f;
	Grass.Patch = 20.f;
	Grass.Coverage = 0.4f;
	Grass.Scale = FVector2D(0.2, 0.3);
	FHyperAIPCGBiome Field;
	Field.Name = TEXT("Field");
	Field.Layer = TEXT("Meadow");
	Field.Species = {Grass};
	FHyperAIPCGSpecies Pebbles;
	Pebbles.Name = TEXT("Pebbles");
	Pebbles.Meshes = {Mesh(Eco::PebbleMesh)};
	Pebbles.Density = 0.05f;
	Pebbles.Scale = FVector2D(0.2, 0.2);
	FHyperAIPCGBiome Everywhere;
	Everywhere.Name = TEXT("Everywhere");
	Everywhere.Species = {Pebbles};
	FHyperAIPCGSpecies Stones;
	Stones.Name = TEXT("Stones");
	Stones.Meshes = {Mesh(Eco::StoneMesh)};
	Stones.Density = 0.2f;
	Stones.Scale = FVector2D(0.3, 0.6);
	FHyperAIPCGBiome Scree;
	Scree.Name = TEXT("Scree");
	// From 30 m above the flat ground up: only the steep slope's top.
	Scree.Height = FVector2D(Corner.Z / 100.0 + 30.0, Corner.Z / 100.0 + 100.0);
	Scree.Species = {Stones};

	FHyperAIPCGScatterOp Make = Op(TEXT("ecosystem.create"));
	Make.Graph = Graph;
	Make.Biomes = {Woods, Field, Everywhere, Scree};
	Make.AvoidLayers = {TEXT("Road")};
	Make.AvoidTags = {HutTag};
	Make.AvoidMargin = static_cast<float>(Eco::HutMargin);
	Make.RoadMargin = 3.f;
	FHyperAIPCGScatterOp Place = Op(TEXT("volume.create"));
	Place.Graph = Graph;
	Place.Volume = VolumeLabel;
	Place.Cover = GroundLabel;
	Place.Partitioned = bPartitioned ? TEXT("true") : TEXT("");

	// Refused before anything changes.
	FHyperAIPCGScatterOp Bad = Make;
	Bad.Biomes[0].Species[2].KeepOutOf = {TEXT("Grass")};
	TestFalse(TEXT("keeping out of a species that is not before it in its biome is refused"), Run({Bad}, /*bDryRun=*/true).bOk);
	Bad = Make;
	Bad.Biomes[1].Species[0].Patch = 1.f;
	TestFalse(TEXT("stands under 2 m are refused"), Run({Bad}, /*bDryRun=*/true).bOk);
	Bad = Make;
	Bad.Biomes[0].Species.Reset();
	TestFalse(TEXT("a biome with nothing growing is refused"), Run({Bad}, /*bDryRun=*/true).bOk);
	Bad = Make;
	Bad.Biomes[3].Height = FVector2D(400.0, 300.0);
	TestFalse(TEXT("a height band running down is refused"), Run({Bad}, /*bDryRun=*/true).bOk);
	for (const float Margin : {-1.f, 101.f})
	{
		Bad = Make;
		Bad.RoadMargin = Margin;
		const FHyperAIPCGScatterPlanReport Refused = Run({Bad}, /*bDryRun=*/true);
		TestTrue(FString::Printf(TEXT("a RoadMargin of %g is refused: %s"), Margin, *Refused.Diagnostic),
			!Refused.bOk && Refused.Diagnostic.Contains(TEXT("RoadMargin is 0 (off) to 100 meters.")));
	}
	for (const float Margin : {0.f, 5.f})
	{
		Bad = Make;
		Bad.RoadMargin = Margin;
		TestTrue(FString::Printf(TEXT("a RoadMargin of %g checks"), Margin), Run({Bad}, /*bDryRun=*/true).bOk);
	}
	FHyperAIPCGScatterOp Scatter = Op(TEXT("graph.create"));
	Scatter.Graph = Graph;
	Scatter.Meshes = {Mesh(Eco::RockMesh)};
	FHyperAIPCGScatterOp Rebuild = Make;
	Rebuild.Kind = TEXT("ecosystem.set");
	TestFalse(TEXT("rebuilding a scatter graph the same plan makes as an ecosystem is refused"), Run({Scatter, Rebuild}, /*bDryRun=*/true).bOk);
	FHyperAIPCGScatterOp Nowhere = Place;
	Nowhere.Cover = TEXT("HyperAI_NoSuchGround_") + Suffix;
	TestFalse(TEXT("covering a landscape that is not there is refused"), Run({Make, Nowhere}, /*bDryRun=*/true).bOk);
	FHyperAIPCGScatterOp Maybe = Place;
	Maybe.Partitioned = TEXT("maybe");
	TestFalse(TEXT("Partitioned is true or false"), Run({Make, Maybe}, /*bDryRun=*/true).bOk);
	const FHyperAIPCGScatterPlanReport Checked = Run({Make, Place}, /*bDryRun=*/true);
	TestTrue(TEXT("the plan itself checks: ") + Checked.Diagnostic, Checked.bOk);

	Settings->bRequireApprovalForAgentEdits = false;
	const FHyperAIPCGScatterPlanReport Started = Run({Make, Place});
	Settings->bRequireApprovalForAgentEdits = bApproval;
	if (!TestTrue(TEXT("the ecosystem is built and its volume starts generating: ") + Started.Diagnostic + TEXT(" | ") + FString::Join(Started.Notes, TEXT(" | ")),
		Started.bOk && Started.Status == TEXT("generating")))
	{
		Cleanup();
		return false;
	}
	AddInfo(FString::Join(Started.Notes, TEXT(" | ")));

	// Read back: a recipe per species, every node and edge, and the op that rebuilds it.
	FHyperAIPCGScatterInspectRequest Look;
	Look.Graph = Graph;
	const FHyperAIPCGScatterInspectReport Summary = C::Inspect(Look);
	const FString Recipes = FString::Join(Summary.Recipes, TEXT("\n"));
	AddInfo(Summary.Diagnostic + TEXT("\n") + Recipes);
	TestTrue(TEXT("inspect reads the ecosystem"), Summary.bOk);
	TestEqual(TEXT("a recipe for each species"), Summary.Recipes.Num(), 7);
	TestTrue(TEXT("the grass grows in stands"), Recipes.Contains(TEXT("Stands 20 m, 40%")));
	TestTrue(TEXT("the trees stop drawing at 300 m"), Recipes.Contains(TEXT("Cylinder x1 cull 240-300 m")));
	TestTrue(TEXT("the rocks keep out of the trees' room"), Recipes.Contains(TEXT("Differences: Room of Trees")));
	TestTrue(TEXT("every species keeps off the roads"), Recipes.Contains(TEXT("Differences: Roads + 3 m")));
	TestTrue(TEXT("the description names the biomes and the road margin"), Summary.Settings.Num() == 1 && Summary.Settings[0].Contains(TEXT("Woods on Forest"))
		&& Summary.Settings[0].Contains(TEXT("Keeps 3 m off landscape roads.")));
	TestTrue(TEXT("the plan says how far species sample past their cell"), Started.Notes.ContainsByPredicate([](const FString& Note)
	{
		return Note.StartsWith(TEXT("Partition seams")) && Note.Contains(TEXT("Woods: Trees 8 m")) && Note.Contains(TEXT("Woods: Bushes 7 m"));
	}));
	FHyperAIPCGScatterOp Spec;
	TestTrue(TEXT("the op that rebuilds it reads as one: ") + Summary.Ecosystem.Left(300), FJsonObjectConverter::JsonObjectStringToUStruct(Summary.Ecosystem, &Spec));
	TestEqual(TEXT("as ecosystem.set"), Spec.Kind, FString(TEXT("ecosystem.set")));
	TestTrue(TEXT("with every biome, species and setting"), Spec.Biomes.Num() == 4 && Spec.Biomes[0].Species.Num() == 4 && Spec.Biomes[0].Species[1].Cluster == 5
		&& Spec.Biomes[0].Species[2].bAlignToGround && Spec.AvoidLayers == TArray<FString>({TEXT("Road")}) && Spec.RoadMargin == 3.f);
	TestTrue(TEXT("and it checks as a plan"), Run({Spec}, /*bDryRun=*/true).bOk);

	// The roads: both kinds of landscape spline, sampled whole along their paved width, then widened by the margin.
	if (const UPCGGraph* Built = LoadObject<UPCGGraph>(nullptr, *(Graph + TEXT(".PCG_Wilds"))))
	{
		const TArray<const UPCGNode*> Getters = NodesOf<UPCGGetSplineSettings>(*Built);
		TSet<const UClass*> Owners;
		bool bComponents = true;
		for (const UPCGNode* Node : Getters)
		{
			const UPCGGetSplineSettings* Get = Cast<UPCGGetSplineSettings>(Node->GetSettings());
			Owners.Add(Get->ActorSelector.ActorSelectionClass.Get());
			bComponents &= Get->ActorSelector.ActorFilter == EPCGActorFilter::AllWorldActors && !Get->ActorSelector.bMustOverlapSelf
				&& Get->ComponentSelector.ComponentSelection == EPCGComponentSelection::ByClass && Get->ComponentSelector.ComponentSelectionClass.Get() == ULandscapeSplinesComponent::StaticClass();
		}
		TestTrue(TEXT("two spline getters, for spline actors and for landscapes' own splines, every one in the world"), Getters.Num() == 2
			&& Owners.Contains(ALandscapeSplineActor::StaticClass()) && Owners.Contains(ALandscapeProxy::StaticClass()) && bComponents);
		const TArray<const UPCGNode*> Samplers = NodesOf<UPCGSplineSamplerSettings>(*Built);
		const UPCGSplineSamplerSettings* Along = Samplers.Num() == 1 ? Cast<UPCGSplineSamplerSettings>(Samplers[0]->GetSettings()) : nullptr;
		TestTrue(TEXT("one spline sampler, every 2 m along the roads, whole"), Along && Along->SamplerParams.Dimension == EPCGSplineSamplingDimension::OnSpline
			&& Along->SamplerParams.Mode == EPCGSplineSamplingMode::Distance && Along->SamplerParams.DistanceIncrement == 200.f && Along->SamplerParams.bUnbounded);
		const UPCGNode* Paved = Along ? LinkedTo(*Samplers[0], PCGPinConstants::DefaultOutputLabel, false) : nullptr;
		const UPCGNode* Baked = Paved ? LinkedTo(*Paved, PCGPinConstants::DefaultOutputLabel, false) : nullptr;
		const UPCGNode* Widened = Baked ? LinkedTo(*Baked, PCGPinConstants::DefaultOutputLabel, false) : nullptr;
		const UPCGBoundsModifierSettings* Edge = Paved ? Cast<UPCGBoundsModifierSettings>(Paved->GetSettings()) : nullptr;
		const UPCGBoundsModifierSettings* Margin = Widened ? Cast<UPCGBoundsModifierSettings>(Widened->GetSettings()) : nullptr;
		TestTrue(TEXT("cut to the paved edge, the scale put in the bounds, then 3 m wider and a sample longer"), Edge && Margin && Baked
			&& Edge->Mode == EPCGBoundsModifierMode::Intersect && Edge->BoundsMin.Y == -1.0 && Edge->BoundsMax.Y == 1.0
			&& Baked->GetSettings()->GetClass()->GetName() == TEXT("PCGApplyScaleToBoundsSettings")
			&& Margin->Mode == EPCGBoundsModifierMode::Translate && Margin->BoundsMax == FVector(100.0, 300.0, 5000.0) && Margin->BoundsMin == FVector(-100.0, -300.0, -5000.0));
		const TArray<const UPCGNode*> Differences = NodesOf<UPCGDifferenceSettings>(*Built);
		int32 OffRoads = 0;
		for (const UPCGNode* Node : Differences)
		{
			const UPCGPin* Pin = Node->GetInputPin(PCGDifferenceConstants::DifferencesLabel);
			const bool bRoads = Pin && Pin->Edges.ContainsByPredicate([Widened](const UPCGEdge* Link) { return Link && Link->InputPin && Link->InputPin->Node.Get() == Widened; });
			if (bRoads && Cast<UPCGDifferenceSettings>(Node->GetSettings())->Mode == EPCGDifferenceMode::Discrete) ++OffRoads;
		}
		TestTrue(FString::Printf(TEXT("every species' difference takes the roads out, as points (%d of %d)"), OffRoads, Differences.Num()), Differences.Num() == 7 && OffRoads == 7);

		// Partition seams. Trees: footprint 4 m at up to 1.2x, room 1.25x; bushes 1.5 m clumped five over 2 m; rocks
		// keep out of the trees, ferns out of the bushes. Ferns' footprint reach 0.21 m + a bush's room 1.27 m makes the
		// bushes' Need 1.49 m, and its clump's reach 3.83 m + 1.27 m brings its halo to 7 m; Need 1.49 + 1.27 + the trees'
		// room 4.24 m is 7.0004 m, so the trees' is 8 m. Rocks and ferns, that nothing keeps out of and that have no clumps, have none.
		auto HaloOf = [Built](const TCHAR* Species) -> int32
		{
			const UPCGNode* Sampler = Titled(*Built, Species);
			const UPCGNode* Shape = Sampler ? LinkedTo(*Sampler, PCGSurfaceSamplerConstants::BoundingShapeLabel, true) : nullptr;
			if (!Shape || !Cast<UPCGInnerIntersectionSettings>(Shape->GetSettings())) return 0;
			const UPCGNode* Grown = LinkedTo(*Shape, PCGPinConstants::DefaultInputLabel, true);
			const UPCGBoundsModifierSettings* Halo = Grown ? Cast<UPCGBoundsModifierSettings>(Grown->GetSettings()) : nullptr;
			return Halo ? FMath::RoundToInt(Halo->BoundsMax.X / 100.0) : -1;
		};
		const int32 TreeHalo = HaloOf(TEXT("Woods: Trees"));
		const int32 BushHalo = HaloOf(TEXT("Woods: Bushes"));
		TestTrue(FString::Printf(TEXT("trees sample 8 m past their cell and the bushes' clumps 7 m (%d, %d)"), TreeHalo, BushHalo), TreeHalo == 8 && BushHalo == 7);
		const UPCGNode* RockSampler = Titled(*Built, TEXT("Woods: Rocks"));
		const UPCGNode* FernSampler = Titled(*Built, TEXT("Woods: Ferns"));
		TestTrue(TEXT("rocks and ferns sample their own cell, as the graph's input gives it"), RockSampler && FernSampler
			&& LinkedTo(*RockSampler, PCGSurfaceSamplerConstants::BoundingShapeLabel, true) == Built->GetInputNode()
			&& LinkedTo(*FernSampler, PCGSurfaceSamplerConstants::BoundingShapeLabel, true) == Built->GetInputNode());
		const TArray<const UPCGNode*> Culls = NodesOf<UPCGCullPointsOutsideActorBoundsSettings>(*Built);
		TestTrue(FString::Printf(TEXT("only the trees and bushes cull their halo before spawning (%d)"), Culls.Num()), Culls.Num() == 2
			&& Titled(*Built, TEXT("Woods: Trees in its cell")) && Titled(*Built, TEXT("Woods: Bushes in its cell")));
		const TArray<const UPCGNode*> Landscapes = NodesOf<UPCGGetLandscapeSettings>(*Built);
		TestTrue(TEXT("the ground is read past the landscape proxies a cell overlaps"), Landscapes.Num() == 1 && !Cast<UPCGGetLandscapeSettings>(Landscapes[0]->GetSettings())->ActorSelector.bMustOverlapSelf);
	}
	else
	{
		AddError(TEXT("The ecosystem graph would not load."));
	}
	// Without a road margin, or a species with a halo, neither roads nor cells are read.
	FHyperAIPCGScatterOp Plain = Op(TEXT("ecosystem.create"));
	Plain.Graph = Folder + TEXT("/PCG_Plain");
	Plain.Biomes = {Everywhere};
	Settings->bRequireApprovalForAgentEdits = false;
	const FHyperAIPCGScatterPlanReport PlainMade = Run({Plain});
	Settings->bRequireApprovalForAgentEdits = bApproval;
	const UPCGGraph* PlainGraph = PlainMade.bOk ? LoadObject<UPCGGraph>(nullptr, *(Plain.Graph + TEXT(".PCG_Plain"))) : nullptr;
	TestTrue(TEXT("an ecosystem with no road margin reads no splines, and its one species samples its cell: ") + PlainMade.Diagnostic, PlainGraph
		&& NodesOf<UPCGGetSplineSettings>(*PlainGraph).IsEmpty() && NodesOf<UPCGSplineSamplerSettings>(*PlainGraph).IsEmpty()
		&& NodesOf<UPCGCullPointsOutsideActorBoundsSettings>(*PlainGraph).IsEmpty() && NodesOf<UPCGInnerIntersectionSettings>(*PlainGraph).IsEmpty()
		&& !PlainMade.Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("Partition seams")); }));
	TestTrue(TEXT("the reading says it is an ecosystem"), Summary.Diagnostic.Contains(TEXT("an ecosystem this tool built")));
	// Its nodes are a scatter graph's kinds, many times over: graph.set must not reshape it.
	FHyperAIPCGScatterOp Reshape = Op(TEXT("graph.set"));
	Reshape.Graph = Graph;
	Reshape.Density = 0.5f;
	TestFalse(TEXT("graph.set will not reshape an ecosystem"), Run({Reshape}, /*bDryRun=*/true).bOk);
	Look.Detail = TEXT("full");
	const FHyperAIPCGScatterInspectReport Full = C::Inspect(Look);
	TestTrue(FString::Printf(TEXT("full detail lists every node and edge (%d, %d)"), Full.Nodes.Num(), Full.Edges.Num()), Full.Nodes.Num() >= 50 && Full.Edges.Num() >= 60 && !Full.bTruncated);
	Look.Detail = TEXT("nodes");
	Look.Filter = TEXT("Bushes");
	const FHyperAIPCGScatterInspectReport Bush = C::Inspect(Look);
	// The bushes' recipe, and the ferns' that keep out of the bushes' room.
	TestTrue(FString::Printf(TEXT("a filter narrows the reading to the bushes (%d recipes, %d nodes)"), Bush.Recipes.Num(), Bush.Nodes.Num()),
		Bush.Recipes.Num() == 2 && Bush.Nodes.Num() >= 3 && Bush.Nodes.Num() < 10);
	FHyperAIPCGScatterInspectRequest BadDetail;
	BadDetail.Graph = Graph;
	BadDetail.Detail = TEXT("everything");
	TestFalse(TEXT("an unknown Detail is refused"), C::Inspect(BadDetail).bOk);

	State->OperationId = Started.OperationId;
	State->Began = FPlatformTime::Seconds();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, VolumeLabel, Corner, Make, bPartitioned, Settings, bApproval]()
	{
		if (FHyperAIStudioAsyncJobHost::IsRunning(State->OperationId) && FPlatformTime::Seconds() - State->Began < 240.0) return false;
		AddInfo(FString::Printf(TEXT("Generated in %.1f s: %s"), FPlatformTime::Seconds() - State->Began, *JobSaid(State->OperationId)));
		const TArray<AActor*> Outputs = OutputActors(FindVolume(VolumeLabel));
		for (AActor* Actor : Outputs) State->Outputs.Add(Actor);
		if (bPartitioned) TestTrue(FString::Printf(TEXT("the volume generates in World Partition cells (%d actors)"), Outputs.Num()), Outputs.Num() > 1);
		const TArray<const UInstancedStaticMeshComponent*> Made = Generated(FindVolume(VolumeLabel));
		const TMap<FString, TArray<FPlaced>> All = Placed(Made);
		// Nothing else generates into these cells here, so what PCG manages for the volume is all they hold.
		TestEqual(TEXT("PCG manages every instance in the volume's cells"), CountPlaced(Made), CountPlaced(ComponentsOn(Outputs)));
		auto Of = [&All](const TCHAR* Mesh)
		{
			const TArray<FPlaced>* Found = All.Find(Mesh);
			return Found ? *Found : TArray<FPlaced>();
		};
		const TArray<FPlaced> Trees = Of(Eco::TreeMesh);
		const TArray<FPlaced> Bushes = Of(Eco::BushMesh);
		const TArray<FPlaced> Rocks = Of(Eco::RockMesh);
		const TArray<FPlaced> Grass = Of(Eco::GrassMesh);
		const TArray<FPlaced> Pebbles = Of(Eco::PebbleMesh);
		const TArray<FPlaced> Stones = Of(Eco::StoneMesh);
		const TArray<FPlaced> Ferns = Of(Eco::FernMesh);
		AddInfo(FString::Printf(TEXT("Trees %d, bushes %d, rocks %d, ferns %d, grass %d, pebbles %d, stones %d."), Trees.Num(), Bushes.Num(), Rocks.Num(), Ferns.Num(),
			Grass.Num(), Pebbles.Num(), Stones.Num()));
		State->Trees = Trees.Num();
		TestTrue(TEXT("trees grow"), Trees.Num() > 100);
		TestTrue(TEXT("bushes grow"), Bushes.Num() > 100);
		TestTrue(TEXT("rocks grow"), Rocks.Num() > 200);
		TestTrue(TEXT("ferns grow"), Ferns.Num() > 3000);
		TestTrue(TEXT("grass grows"), Grass.Num() > 3000);
		TestTrue(TEXT("pebbles lie about"), Pebbles.Num() > 1000);
		TestTrue(TEXT("stones lie on the heights"), Stones.Num() > 100);
		// Each cell spawns only what stands in it, so nothing is made twice where cells meet.
		int32 Twice = 0;
		for (const TPair<FString, TArray<FPlaced>>& Pair : All) Twice += MadeTwice(Pair.Value);
		TestEqual(TEXT("no instance is made twice"), Twice, 0);

		// Each biome on its paint layer; the pebbles' biome has none and lies on both.
		const auto West = [](const FVector2D& At) { return At.X < Eco::Middle - 0.5; };
		const auto East = [](const FVector2D& At) { return At.X > Eco::Middle + 0.5; };
		TestEqual(TEXT("no tree off the Forest layer"), CountWhere(Trees, Corner, West), 0);
		TestEqual(TEXT("no bush off the Forest layer"), CountWhere(Bushes, Corner, West), 0);
		TestEqual(TEXT("no rock off the Forest layer"), CountWhere(Rocks, Corner, West), 0);
		TestEqual(TEXT("no fern off the Forest layer"), CountWhere(Ferns, Corner, West), 0);
		TestEqual(TEXT("no grass off the Meadow layer"), CountWhere(Grass, Corner, East), 0);
		TestTrue(TEXT("pebbles on both layers"), CountWhere(Pebbles, Corner, West) > 300 && CountWhere(Pebbles, Corner, East) > 300);
		// Nothing on the road.
		const auto OnRoad = [](const FVector2D& At) { return At.Y > Eco::RoadFrom && At.Y < Eco::RoadTo - 1.0; };
		int32 Road = 0;
		for (const TArray<FPlaced>* List : {&Trees, &Bushes, &Rocks, &Ferns, &Grass, &Pebbles, &Stones}) Road += CountWhere(*List, Corner, OnRoad);
		TestEqual(TEXT("nothing grows on the road"), Road, 0);
		// Nothing within the hut's sides and the margin past them.
		int32 AtHut = 0;
		for (const TArray<FPlaced>* List : {&Trees, &Bushes, &Rocks, &Ferns, &Grass, &Pebbles, &Stones})
		{
			for (const FPlaced& Item : *List) AtHut += FromHut(Item, Corner) < 0.5 * Eco::HutScale + Eco::HutMargin ? 1 : 0;
		}
		TestEqual(TEXT("nothing grows within 2 m of the hut"), AtHut, 0);
		// Every footprint keeps 3 m from the lane's paved edge, 3 m from its center line: at least half the footprint at
		// its smallest scale past 6 m. Just past that, ground cover still grows.
		struct FLaneRule { const TCHAR* Name; const TArray<FPlaced>* Items; double HalfFootprint; };
		const FLaneRule Rules[] = {{TEXT("trees"), &Trees, 2.0 * 0.8}, {TEXT("bushes"), &Bushes, 0.75 * 0.8}, {TEXT("rocks"), &Rocks, 0.5 * 0.3},
			{TEXT("ferns"), &Ferns, 0.25 * 0.4}, {TEXT("grass"), &Grass, 0.25 * 0.2}, {TEXT("pebbles"), &Pebbles, 0.5 * 0.2}};
		TArray<FString> Margins;
		for (const FLaneRule& Rule : Rules)
		{
			const double Kept = Eco::LaneWidth + 3.0 + Rule.HalfFootprint;
			const double Closest = FromLane(*Rule.Items, Corner);
			Margins.Add(FString::Printf(TEXT("%s %.2f m"), Rule.Name, Closest));
			TestTrue(FString::Printf(TEXT("%s keep %.2f m from the lane's center (%.2f m)"), Rule.Name, Kept, Closest), Closest >= Kept - 0.05);
		}
		int32 GrassBeside = 0;
		int32 PebblesBeside = 0;
		FromLane(Grass, Corner, Eco::LaneWidth + 3.0 + 0.05, 4.0, &GrassBeside);
		FromLane(Pebbles, Corner, Eco::LaneWidth + 3.0 + 0.1, 4.0, &PebblesBeside);
		TestTrue(FString::Printf(TEXT("grass and pebbles grow in the 4 m beside the kept margin (%d, %d)"), GrassBeside, PebblesBeside), GrassBeside > 30 && PebblesBeside > 20);
		AddInfo(TEXT("Nearest to the lane's center: ") + FString::Join(Margins, TEXT(", ")));
		// The woods keep off ground steeper than 30 degrees; the field has no limit.
		const auto Steep = [](const FVector2D& At) { return At.Y > Eco::SteepFrom + 1.0; };
		TestEqual(TEXT("no tree on the steep slope"), CountWhere(Trees, Corner, Steep), 0);
		TestEqual(TEXT("no bush on the steep slope"), CountWhere(Bushes, Corner, Steep), 0);
		TestEqual(TEXT("no rock on the steep slope"), CountWhere(Rocks, Corner, Steep), 0);
		TestTrue(TEXT("grass climbs it"), CountWhere(Grass, Corner, [](const FVector2D& At) { return At.Y > Eco::SteepFrom + 5.0; }) > 50);
		// Stones only in the scree's height band.
		int32 OutOfBand = 0;
		for (const FPlaced& Stone : Stones)
		{
			const double Z = Stone.Transform.GetLocation().Z;
			if (Z < Corner.Z + 2950.0 || Z > Corner.Z + 10050.0) ++OutOfBand;
		}
		TestEqual(TEXT("no stone outside the scree's heights"), OutOfBand, 0);
		// On the 20 degree slope trees stand upright and rocks lean with it.
		const auto Gentle = [](const FVector2D& At) { return At.Y > Eco::GentleFrom + 3.0 && At.Y < Eco::SteepFrom - 3.0; };
		double TreeLean = 0.0;
		int32 SlopeTrees = 0;
		for (const FPlaced& Tree : Trees)
		{
			if (!Gentle(OnGround(Tree, Corner))) continue;
			TreeLean = FMath::Max(TreeLean, LeanOf(Tree));
			++SlopeTrees;
		}
		TestTrue(FString::Printf(TEXT("trees on the slope stand upright (%d, leaning at most %.1f degrees)"), SlopeTrees, TreeLean), SlopeTrees > 5 && TreeLean < 1.0);
		double SlopeLean = 0.0;
		double FlatLean = 0.0;
		int32 SlopeRocks = 0;
		int32 FlatRocks = 0;
		for (const FPlaced& Rock : Rocks)
		{
			const FVector2D At = OnGround(Rock, Corner);
			if (Gentle(At))
			{
				SlopeLean += LeanOf(Rock);
				++SlopeRocks;
			}
			else if (At.Y < 100.0)
			{
				FlatLean += LeanOf(Rock);
				++FlatRocks;
			}
		}
		SlopeLean /= FMath::Max(SlopeRocks, 1);
		FlatLean /= FMath::Max(FlatRocks, 1);
		TestTrue(FString::Printf(TEXT("rocks lean with the slope (%.1f degrees on average on it, %.1f on the flat)"), SlopeLean, FlatLean),
			SlopeRocks > 20 && FlatRocks > 20 && SlopeLean > 14.0 && SlopeLean < 27.0 && FlatLean < 10.0);
		// Later species keep out of the trees' room: their footprint and a quarter more.
		double BushToTree = TNumericLimits<double>::Max();
		for (const FPlaced& Bush : Bushes) BushToTree = FMath::Min(BushToTree, Nearest(Bush, Trees));
		double RockToTree = TNumericLimits<double>::Max();
		for (const FPlaced& Rock : Rocks) RockToTree = FMath::Min(RockToTree, Nearest(Rock, Trees));
		TestTrue(FString::Printf(TEXT("bushes and rocks keep 1.9 m from trees (%.2f m, %.2f m)"), BushToTree, RockToTree), BushToTree > 1.9 && RockToTree > 1.9);
		// Ferns keep out of the bushes' room, clump members across a cell's edge too: a bush's 0.6 m at its smallest and a
		// fern's 0.1 m. Across an edge each cell sees the other's bushes only through the halo.
		double FernToBush = TNumericLimits<double>::Max();
		for (const FPlaced& Fern : Ferns) FernToBush = FMath::Min(FernToBush, Nearest(Fern, Bushes));
		TestTrue(FString::Printf(TEXT("ferns keep 0.65 m from bushes (%.2f m)"), FernToBush), FernToBush > 0.65);
		const int32 FernsAcross = PairsAcrossCells(Ferns, Bushes, 2.0);
		const int32 RocksAcross = PairsAcrossCells(Rocks, Trees, 4.0);
		if (bPartitioned)
		{
			TestTrue(FString::Printf(TEXT("ferns stand within 2 m of bushes across a cell's edge, so the edge was tested (%d pairs; rocks within 4 m of trees, %d)"),
				FernsAcross, RocksAcross), FernsAcross > 5);
		}
		else
		{
			AddInfo(TEXT("Not a World Partition level: no cell edges to cross."));
		}
		// Bushes grow in clumps: most have two others close by, where an even spread at their density would give few.
		int32 Clumped = 0;
		for (const FPlaced& Bush : Bushes)
		{
			int32 Close = 0;
			for (const FPlaced& Other : Bushes)
			{
				if (&Other != &Bush && FVector::Dist2D(Bush.Transform.GetLocation(), Other.Transform.GetLocation()) < 350.0) ++Close;
			}
			if (Close >= 2) ++Clumped;
		}
		const double ClumpedShare = Bushes.IsEmpty() ? 0.0 : static_cast<double>(Clumped) / Bushes.Num();
		// Spread evenly at the bushes' density, about 7% would.
		TestTrue(FString::Printf(TEXT("bushes grow in clumps (%.0f%% have two others within 3.5 m)"), ClumpedShare * 100.0), ClumpedShare > 0.3);
		// Cull distances: the trees' 300 m, and none for the grass.
		TestTrue(TEXT("trees stop drawing at 300 m"), !Trees.IsEmpty() && Trees[0].EndCull == 30000);
		TestTrue(TEXT("grass has no cull distance"), !Grass.IsEmpty() && Grass[0].EndCull == 0);
		// Stands: about the share of the meadow asked for.
		const double Coverage = GrassCoverage(Grass, Corner);
		TestTrue(FString::Printf(TEXT("grass stands cover about 40%% of the meadow (%.0f%%)"), Coverage * 100.0), Coverage > 0.28 && Coverage < 0.58);
		AddInfo(FString::Printf(TEXT("Measured: grass covers %.0f%% of the meadow; %.0f%% of bushes in clumps; trees lean at most %.1f degrees on the slope, rocks %.1f there and %.1f on the flat; nearest bush %.2f m and rock %.2f m from a tree, fern %.2f m from a bush; %d fern-bush pairs within 2 m across cells."),
			Coverage * 100.0, ClumpedShare * 100.0, TreeLean, SlopeLean, FlatLean, BushToTree, RockToTree, FernToBush, FernsAcross));

		// Rebuilt: no rocks, and the grass everywhere on the meadow; then generated again.
		FHyperAIPCGScatterOp Rebuild = Make;
		Rebuild.Kind = TEXT("ecosystem.set");
		Rebuild.Biomes[0].Species.RemoveAt(2);
		Rebuild.Biomes[1].Species[0].Patch = 0.f;
		Rebuild.Biomes[1].Species[0].Coverage = 1.f;
		FHyperAIPCGScatterOp Again = Op(TEXT("volume.generate"));
		Again.Volume = VolumeLabel;
		Settings->bRequireApprovalForAgentEdits = false;
		const FHyperAIPCGScatterPlanReport Regenerating = Run({Rebuild, Again});
		Settings->bRequireApprovalForAgentEdits = bApproval;
		TestTrue(TEXT("the ecosystem is rebuilt and generates again: ") + Regenerating.Diagnostic, Regenerating.bOk && Regenerating.Status == TEXT("generating"));
		State->OperationId = Regenerating.OperationId;
		State->Began = FPlatformTime::Seconds();
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, VolumeLabel, Corner, Graph, Settings, bApproval]()
	{
		if (FHyperAIStudioAsyncJobHost::IsRunning(State->OperationId) && FPlatformTime::Seconds() - State->Began < 240.0) return false;
		AddInfo(TEXT("Regenerated: ") + JobSaid(State->OperationId));
		TArray<AActor*> Outputs = OutputActors(FindVolume(VolumeLabel));
		for (AActor* Actor : Outputs) State->Outputs.AddUnique(Actor);
		const TMap<FString, TArray<FPlaced>> All = Placed(Generated(FindVolume(VolumeLabel)));
		const TArray<FPlaced>* Trees = All.Find(Eco::TreeMesh);
		const TArray<FPlaced>* Grass = All.Find(Eco::GrassMesh);
		TestFalse(TEXT("the rocks are gone"), All.Contains(Eco::RockMesh));
		TestEqual(TEXT("the same trees grow again from the same seed"), Trees ? Trees->Num() : 0, State->Trees);
		const double Coverage = Grass ? GrassCoverage(*Grass, Corner) : 0.0;
		TestTrue(FString::Printf(TEXT("grass now covers the meadow (%.0f%%)"), Coverage * 100.0), Coverage > 0.85);
		// The grass, everywhere on the meadow now, keeps the hut's 2 m margin and grows just past it: the margin is not
		// grown fourfold with the hut's scale, to 8 m.
		const double HutKept = 0.5 * Eco::HutScale + Eco::HutMargin;
		int32 AtHut = 0;
		int32 BesideHut = 0;
		for (int32 Index = 0; Grass && Index < Grass->Num(); ++Index)
		{
			const double Away = FromHut((*Grass)[Index], Corner);
			AtHut += Away < HutKept ? 1 : 0;
			BesideHut += Away >= HutKept + 0.15 && Away < HutKept + 2.0 ? 1 : 0;
		}
		TestTrue(FString::Printf(TEXT("grass keeps 2 m from the hut and grows just past that (%d within, %d in the next 2 m)"), AtHut, BesideHut), AtHut == 0 && BesideHut > 20);
		FHyperAIPCGScatterInspectRequest Look;
		Look.Graph = Graph;
		const FHyperAIPCGScatterInspectReport Read = C::Inspect(Look);
		TestTrue(TEXT("inspect reads the rebuilt op"), Read.Recipes.Num() == 6 && !Read.Ecosystem.Contains(TEXT("Rocks")) && Read.Ecosystem.Contains(TEXT("Grass")));
		FHyperAIPCGScatterOp Clear = Op(TEXT("volume.clear"));
		Clear.Volume = VolumeLabel;
		Settings->bRequireApprovalForAgentEdits = false;
		const FHyperAIPCGScatterPlanReport Cleared = Run({Clear});
		Settings->bRequireApprovalForAgentEdits = bApproval;
		TestTrue(TEXT("the volume is cleared: ") + FString::Join(Cleared.Notes, TEXT(" | ")), Cleared.bOk);
		State->Began = FPlatformTime::Seconds();
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, Cleanup]()
	{
		const int32 Left = CountPlaced(ComponentsOn(Pinned(State->Outputs)));
		if (Left > 0 && FPlatformTime::Seconds() - State->Began < 30.0) return false;
		TestEqual(TEXT("and no instance is left in any cell"), Left, 0);
		Cleanup();
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
