// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioPCGScatterToolset.h"
#include "HyperAIStudioSettings.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeSubsystem.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "ObjectTools.h"
#include "PCGVolume.h"
#include "Tests/AutomationCommon.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
