// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Grid/PCGPartitionActor.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioPCGScatterToolset.h"
#include "HyperAIStudioSettings.h"
#include "JsonObjectConverter.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeSubsystem.h"
#include "LandscapeUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "ObjectTools.h"
#include "PCGComponent.h"
#include "PCGManagedResource.h"
#include "PCGVolume.h"
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
		/** Flat up to GentleFrom, then 20 degrees up to SteepFrom, then 56 degrees. */
		constexpr double GentleFrom = 180.0;
		constexpr double SteepFrom = 220.0;

		const TCHAR* const TreeMesh = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
		const TCHAR* const BushMesh = TEXT("/Engine/BasicShapes/Sphere.Sphere");
		const TCHAR* const RockMesh = TEXT("/Engine/BasicShapes/Cube.Cube");
		const TCHAR* const GrassMesh = TEXT("/Engine/BasicShapes/Cone.Cone");
		const TCHAR* const PebbleMesh = TEXT("/Engine/BasicShapes/Plane.Plane");
		const TCHAR* const StoneMesh = TEXT("/Engine/EngineMeshes/Sphere.Sphere");

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

	/** The nearest of Others to Item on the same actor (a partition cell sees only its own), in meters. */
	double Nearest(const FPlaced& Item, const TArray<FPlaced>& Others)
	{
		double Best = TNumericLimits<double>::Max();
		for (const FPlaced& Other : Others)
		{
			if (&Other == &Item || Other.Owner != Item.Owner) continue;
			Best = FMath::Min(Best, FVector::Dist2D(Item.Transform.GetLocation(), Other.Transform.GetLocation()) / 100.0);
		}
		return Best;
	}

	/** The share of the meadow's 2 m cells, clear of the road and the edges, that have grass. */
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
				if (Y >= 59 && Y < 67) continue;
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
	// Far from the bench level and from the scatter test's ground.
	const FVector Corner(1500000.0, 900000.0, 30000.0);
	const ULandscapeSubsystem* LandscapeSubsystem = World->GetSubsystem<ULandscapeSubsystem>();
	const bool bPartitioned = LandscapeSubsystem && LandscapeSubsystem->IsGridBased() && World->GetWorldPartition();
	if (!TestNotNull(TEXT("the painted ground is made"), MakeEcosystemGround(*World, Corner, GroundLabel, Folder))) return false;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	const TSharedRef<FEcosystemState> State = MakeShared<FEcosystemState>();
	auto Cleanup = [World, VolumeLabel, GroundLabel, Folder, State]()
	{
		if (APCGVolume* Volume = FindVolume(VolumeLabel)) World->EditorDestroyActor(Volume, /*bShouldModifyLevel=*/true);
		// The partition actors the generation made, once nothing generates into them.
		for (AActor* Actor : Pinned(State->Outputs))
		{
			const APCGPartitionActor* Cell = Cast<APCGPartitionActor>(Actor);
			if (Cell && !Cell->HasGraphInstances()) World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
		}
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
	FHyperAIPCGBiome Woods;
	Woods.Name = TEXT("Woods");
	Woods.Layer = TEXT("Forest");
	Woods.SlopeMax = 30.f;
	Woods.Species = {Trees, Bushes, Rocks};
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
	TestEqual(TEXT("a recipe for each species"), Summary.Recipes.Num(), 6);
	TestTrue(TEXT("the grass grows in stands"), Recipes.Contains(TEXT("Stands 20 m, 40%")));
	TestTrue(TEXT("the trees stop drawing at 300 m"), Recipes.Contains(TEXT("Cylinder x1 cull 240-300 m")));
	TestTrue(TEXT("the rocks keep out of the trees' room"), Recipes.Contains(TEXT("Differences: Room of Trees")));
	TestTrue(TEXT("the description names the biomes"), Summary.Settings.Num() == 1 && Summary.Settings[0].Contains(TEXT("Woods on Forest")));
	FHyperAIPCGScatterOp Spec;
	TestTrue(TEXT("the op that rebuilds it reads as one: ") + Summary.Ecosystem.Left(300), FJsonObjectConverter::JsonObjectStringToUStruct(Summary.Ecosystem, &Spec));
	TestEqual(TEXT("as ecosystem.set"), Spec.Kind, FString(TEXT("ecosystem.set")));
	TestTrue(TEXT("with every biome, species and setting"), Spec.Biomes.Num() == 4 && Spec.Biomes[0].Species.Num() == 3 && Spec.Biomes[0].Species[1].Cluster == 5
		&& Spec.Biomes[0].Species[2].bAlignToGround && Spec.AvoidLayers == TArray<FString>({TEXT("Road")}));
	TestTrue(TEXT("and it checks as a plan"), Run({Spec}, /*bDryRun=*/true).bOk);
	TestTrue(TEXT("the reading says it is an ecosystem"), Summary.Diagnostic.Contains(TEXT("an ecosystem this tool built")));
	// Its nodes are a scatter graph's kinds, many times over: graph.set must not reshape it.
	FHyperAIPCGScatterOp Reshape = Op(TEXT("graph.set"));
	Reshape.Graph = Graph;
	Reshape.Density = 0.5f;
	TestFalse(TEXT("graph.set will not reshape an ecosystem"), Run({Reshape}, /*bDryRun=*/true).bOk);
	Look.Detail = TEXT("full");
	const FHyperAIPCGScatterInspectReport Full = C::Inspect(Look);
	TestTrue(FString::Printf(TEXT("full detail lists every node and edge (%d, %d)"), Full.Nodes.Num(), Full.Edges.Num()), Full.Nodes.Num() >= 55 && Full.Edges.Num() >= 60 && !Full.bTruncated);
	Look.Detail = TEXT("nodes");
	Look.Filter = TEXT("Bushes");
	const FHyperAIPCGScatterInspectReport Bush = C::Inspect(Look);
	TestTrue(TEXT("a filter narrows the reading to the bushes"), Bush.Recipes.Num() == 1 && Bush.Nodes.Num() >= 3 && Bush.Nodes.Num() < 10);
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
		AddInfo(FString::Printf(TEXT("Trees %d, bushes %d, rocks %d, grass %d, pebbles %d, stones %d."), Trees.Num(), Bushes.Num(), Rocks.Num(), Grass.Num(), Pebbles.Num(), Stones.Num()));
		State->Trees = Trees.Num();
		TestTrue(TEXT("trees grow"), Trees.Num() > 100);
		TestTrue(TEXT("bushes grow"), Bushes.Num() > 100);
		TestTrue(TEXT("rocks grow"), Rocks.Num() > 200);
		TestTrue(TEXT("grass grows"), Grass.Num() > 3000);
		TestTrue(TEXT("pebbles lie about"), Pebbles.Num() > 1000);
		TestTrue(TEXT("stones lie on the heights"), Stones.Num() > 100);

		// Each biome on its paint layer; the pebbles' biome has none and lies on both.
		const auto West = [](const FVector2D& At) { return At.X < Eco::Middle - 0.5; };
		const auto East = [](const FVector2D& At) { return At.X > Eco::Middle + 0.5; };
		TestEqual(TEXT("no tree off the Forest layer"), CountWhere(Trees, Corner, West), 0);
		TestEqual(TEXT("no bush off the Forest layer"), CountWhere(Bushes, Corner, West), 0);
		TestEqual(TEXT("no rock off the Forest layer"), CountWhere(Rocks, Corner, West), 0);
		TestEqual(TEXT("no grass off the Meadow layer"), CountWhere(Grass, Corner, East), 0);
		TestTrue(TEXT("pebbles on both layers"), CountWhere(Pebbles, Corner, West) > 300 && CountWhere(Pebbles, Corner, East) > 300);
		// Nothing on the road.
		const auto OnRoad = [](const FVector2D& At) { return At.Y > Eco::RoadFrom && At.Y < Eco::RoadTo - 1.0; };
		int32 Road = 0;
		for (const TArray<FPlaced>* List : {&Trees, &Bushes, &Rocks, &Grass, &Pebbles, &Stones}) Road += CountWhere(*List, Corner, OnRoad);
		TestEqual(TEXT("nothing grows on the road"), Road, 0);
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
		AddInfo(FString::Printf(TEXT("Measured: grass covers %.0f%% of the meadow; %.0f%% of bushes in clumps; trees lean at most %.1f degrees on the slope, rocks %.1f there and %.1f on the flat; nearest bush %.2f m and rock %.2f m from a tree."),
			Coverage * 100.0, ClumpedShare * 100.0, TreeLean, SlopeLean, FlatLean, BushToTree, RockToTree));

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
		FHyperAIPCGScatterInspectRequest Look;
		Look.Graph = Graph;
		const FHyperAIPCGScatterInspectReport Read = C::Inspect(Look);
		TestTrue(TEXT("inspect reads the rebuilt op"), Read.Recipes.Num() == 5 && !Read.Ecosystem.Contains(TEXT("Rocks")) && Read.Ecosystem.Contains(TEXT("Grass")));
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
