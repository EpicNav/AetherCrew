// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioWorldPartitionToolset.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"

namespace HyperAIStudio::WorldPartitionTests
{
	using C = FHyperAIStudioWorldPartitionContracts;

	FHyperAIWorldPartitionOp Op(const TCHAR* Kind)
	{
		FHyperAIWorldPartitionOp Out;
		Out.Kind = Kind;
		return Out;
	}

	FHyperAIWorldPartitionSetting Setting(const TCHAR* Name, const FString& Value)
	{
		FHyperAIWorldPartitionSetting Out;
		Out.Name = Name;
		Out.Value = Value;
		return Out;
	}

	FHyperAIWorldPartitionPlanReport Run(TArray<FHyperAIWorldPartitionOp> Ops, const bool bDryRun = false)
	{
		FHyperAIWorldPartitionPlanRequest Plan;
		Plan.Operations = MoveTemp(Ops);
		Plan.bDryRun = bDryRun;
		return C::ApplyPlan(Plan);
	}

	UDataLayerInstance* Layer(const FString& Name)
	{
		UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
		for (UDataLayerInstance* Instance : Subsystem ? Subsystem->GetAllDataLayers() : TArray<UDataLayerInstance*>())
		{
			if (Instance && Instance->GetDataLayerShortName() == Name) return Instance;
		}
		return nullptr;
	}

	/** The number after "LoadingRange " in a grid line the inspect tool wrote. */
	FString LoadingRangeOf(const FString& GridLine)
	{
		FString After;
		if (!GridLine.Split(TEXT("LoadingRange "), nullptr, &After)) return FString();
		FString Number;
		After.Split(TEXT(" "), &Number, nullptr);
		return Number;
	}

	AStaticMeshActor* Box(UWorld& World, const FString& Label, const FVector& Where)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		AStaticMeshActor* Actor = Cube ? World.SpawnActor<AStaticMeshActor>(Where, FRotator::ZeroRotator, FActorSpawnParameters()) : nullptr;
		if (!Actor) return nullptr;
		Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Actor->SetActorLabel(Label);
		return Actor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioWorldPartitionEndToEndTest,
	"HyperAIStudio.NativeTools.Worldbuilding.WorldPartitionEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioWorldPartitionEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::WorldPartitionTests;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
	if (!Partition || !UDataLayerEditorSubsystem::Get())
	{
		AddInfo(TEXT("Skipped: the open level does not use World Partition."));
		return true;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/WorldPartition_") + Suffix;
	const FString Runtime = TEXT("HAIRuntime_") + Suffix;
	const FString Child = TEXT("HAIChild_") + Suffix;
	const FString EditorOnly = TEXT("HAIEditor_") + Suffix;
	const FString HLOD = Folder + TEXT("/HLOD_Test");
	const FString RegionName = TEXT("Test_") + Suffix;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	UHLODLayer* DefaultBefore = Partition->GetDefaultHLODLayer();
	const FHyperAIWorldPartitionInspectReport Before = C::Inspect(FHyperAIWorldPartitionInspectRequest());
	const FString RangeBefore = Before.Grids.IsEmpty() ? FString() : LoadingRangeOf(Before.Grids[0]);
	const FVector Far(600000.0, 600000.0, 30000.0);
	AStaticMeshActor* A = Box(*World, TEXT("HyperAI_TestWP_A_") + Suffix, Far);
	AStaticMeshActor* B = Box(*World, TEXT("HyperAI_TestWP_B_") + Suffix, Far + FVector(500.0, 0.0, 0.0));
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		for (AActor* Actor : TArray<AActor*>{A, B})
		{
			if (IsValid(Actor)) World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
		}
		if (UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get())
		{
			for (const FString& Name : {Child, Runtime, EditorOnly})
			{
				if (UDataLayerInstance* Left = Layer(Name)) Subsystem->DeleteDataLayer(Left);
			}
		}
		Partition->SetDefaultHLODLayer(DefaultBefore);
		if (!RangeBefore.IsEmpty())
		{
			FHyperAIWorldPartitionOp Restore = Op(TEXT("world_partition.set_grid"));
			Restore.Settings = {Setting(TEXT("LoadingRange"), RangeBefore)};
			Run({Restore});
		}
		FHyperAIWorldPartitionOp Unload = Op(TEXT("region.unload"));
		Unload.Label = RegionName;
		Run({Unload});
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
	if (!TestTrue(TEXT("the test actors are placed"), A && B)) return false;
	TestTrue(TEXT("inspect reads the level's World Partition: ") + Before.Diagnostic, Before.bOk && Before.bWorldPartition && !Before.Grids.IsEmpty() && !Before.RuntimeHash.IsEmpty());

	FHyperAIWorldPartitionOp MakeRuntime = Op(TEXT("data_layer.create"));
	MakeRuntime.DataLayer = Runtime;
	MakeRuntime.Asset = Folder + TEXT("/") + Runtime;
	MakeRuntime.InitialRuntimeState = TEXT("Activated");
	FHyperAIWorldPartitionOp MakeChild = Op(TEXT("data_layer.create"));
	MakeChild.Asset = Folder + TEXT("/") + Child;
	MakeChild.Parent = Runtime;
	FHyperAIWorldPartitionOp MakeEditor = Op(TEXT("data_layer.create"));
	MakeEditor.Asset = Folder + TEXT("/") + EditorOnly;
	MakeEditor.Type = TEXT("Editor");
	MakeEditor.Visible = TEXT("false");
	FHyperAIWorldPartitionOp FillRuntime = Op(TEXT("data_layer.add_actors"));
	FillRuntime.DataLayer = Runtime;
	FillRuntime.Actors = {A->GetActorLabel(), B->GetActorLabel()};
	FHyperAIWorldPartitionOp FillEditor = Op(TEXT("data_layer.add_actors"));
	FillEditor.DataLayer = EditorOnly;
	FillEditor.Actors = {B->GetActorLabel()};
	FHyperAIWorldPartitionOp MakeHLOD = Op(TEXT("hlod_layer.create"));
	MakeHLOD.Asset = HLOD;
	MakeHLOD.Settings = {Setting(TEXT("LayerType"), TEXT("Instancing")), Setting(TEXT("CellSize"), TEXT("51200")), Setting(TEXT("LoadingRange"), TEXT("102400"))};
	FHyperAIWorldPartitionOp StreamA = Op(TEXT("actors.set"));
	StreamA.Actors = {A->GetActorLabel()};
	StreamA.SpatiallyLoaded = TEXT("false");
	StreamA.HLODLayer = HLOD;

	// Refused before anything changes.
	FHyperAIWorldPartitionOp Missing = Op(TEXT("data_layer.set"));
	Missing.DataLayer = TEXT("NoSuchLayer_") + Suffix;
	Missing.Visible = TEXT("true");
	TestFalse(TEXT("setting a data layer that is not there is refused"), Run({Missing}, /*bDryRun=*/true).bOk);
	FHyperAIWorldPartitionOp Mixed = MakeChild;
	Mixed.Parent = EditorOnly;
	const FHyperAIWorldPartitionPlanReport MixedTypes = Run({MakeEditor, Mixed}, /*bDryRun=*/true);
	TestTrue(TEXT("a runtime layer under an editor layer is refused: ") + MixedTypes.Diagnostic, !MixedTypes.bOk && MixedTypes.Diagnostic.Contains(TEXT("its own type")));
	FHyperAIWorldPartitionOp Stranger = StreamA;
	Stranger.Actors = {TEXT("NoSuchActor_") + Suffix};
	TestFalse(TEXT("an actor that is not there is refused"), Run({Stranger}, /*bDryRun=*/true).bOk);
	FHyperAIWorldPartitionOp Typo = MakeHLOD;
	Typo.Settings = {Setting(TEXT("CelSize"), TEXT("1"))};
	const FHyperAIWorldPartitionPlanReport Mistyped = Run({Typo}, /*bDryRun=*/true);
	TestTrue(TEXT("a mistyped HLOD setting is refused with the settings listed: ") + Mistyped.Diagnostic, !Mistyped.bOk && Mistyped.Diagnostic.Contains(TEXT("LoadingRange")));
	TestNull(TEXT("and no data layer was made"), Layer(EditorOnly));

	// One plan: three data layers (one under another), actors put in them, an HLOD layer, and an actor always loaded.
	const FHyperAIWorldPartitionPlanReport Done = Run({MakeRuntime, MakeChild, MakeEditor, FillRuntime, FillEditor, MakeHLOD, StreamA});
	if (!TestTrue(TEXT("the plan runs: ") + Done.Diagnostic + TEXT(" | ") + FString::Join(Done.Notes, TEXT(" | ")), Done.bOk && Done.Applied == 7))
	{
		return false;
	}
	TestEqual(TEXT("the three data layer assets and the HLOD layer are saved"), Done.Saved.Num(), 4);
	UDataLayerInstance* RuntimeLayer = Layer(Runtime);
	UDataLayerInstance* ChildLayer = Layer(Child);
	UDataLayerInstance* EditorLayer = Layer(EditorOnly);
	if (!TestTrue(TEXT("all three data layers are in the level"), RuntimeLayer && ChildLayer && EditorLayer)) return false;
	TestTrue(TEXT("the runtime layer starts activated"), RuntimeLayer->IsRuntime() && RuntimeLayer->GetInitialRuntimeState() == EDataLayerRuntimeState::Activated);
	TestTrue(TEXT("the child sits under it"), ChildLayer->GetParent() == RuntimeLayer && ChildLayer->IsRuntime());
	TestTrue(TEXT("the editor layer is an editor layer, hidden"), !EditorLayer->IsRuntime() && !EditorLayer->IsVisible());
	TestTrue(TEXT("both actors are in the runtime layer"), A->GetDataLayerInstances().Contains(RuntimeLayer) && B->GetDataLayerInstances().Contains(RuntimeLayer));
	TestTrue(TEXT("and B in the editor layer too"), B->GetDataLayerInstances().Contains(EditorLayer) && !A->GetDataLayerInstances().Contains(EditorLayer));
	UHLODLayer* HLODLayer = LoadObject<UHLODLayer>(nullptr, *(HLOD + TEXT(".HLOD_Test")));
	if (TestNotNull(TEXT("the HLOD layer is an asset"), HLODLayer))
	{
		TestTrue(TEXT("with its cells and range"), HLODLayer->IsSpatiallyLoaded() && HLODLayer->GetCellSize() == 51200 && FMath::IsNearlyEqual(HLODLayer->GetLoadingRange(), 102400.0));
		TestNotNull(TEXT("and builder settings for its type"), HLODLayer->GetHLODBuilderSettings());
	}
	TestTrue(TEXT("A is always loaded and stands in through the HLOD layer"), !A->GetIsSpatiallyLoaded() && A->GetHLODLayer() == HLODLayer);
	const FHyperAIWorldPartitionInspectReport Seen = C::Inspect(FHyperAIWorldPartitionInspectRequest());
	const FHyperAIDataLayerInfo* SeenRuntime = Seen.DataLayers.FindByPredicate([&Runtime](const FHyperAIDataLayerInfo& Info) { return Info.Name == Runtime; });
	const FHyperAIDataLayerInfo* SeenChild = Seen.DataLayers.FindByPredicate([&Child](const FHyperAIDataLayerInfo& Info) { return Info.Name == Child; });
	TestTrue(TEXT("inspect reads the layers back with their actors and states"), SeenRuntime && SeenChild && SeenRuntime->Type == TEXT("Runtime")
		&& SeenRuntime->InitialRuntimeState == TEXT("Activated") && SeenRuntime->LoadedActors == 2 && SeenChild->Parent == Runtime);

	// The grid, the default HLOD layer and an editor region.
	if (!RangeBefore.IsEmpty())
	{
		const FString Wider = FString::Printf(TEXT("%lld"), FCString::Atoi64(*RangeBefore) + 100);
		FHyperAIWorldPartitionOp Grid = Op(TEXT("world_partition.set_grid"));
		Grid.Settings = {Setting(TEXT("LoadingRange"), Wider)};
		const FHyperAIWorldPartitionPlanReport GridSet = Run({Grid});
		const FHyperAIWorldPartitionInspectReport AfterGrid = C::Inspect(FHyperAIWorldPartitionInspectRequest());
		TestTrue(FString::Printf(TEXT("the first grid's loading range goes from %s to %s: %s"), *RangeBefore, *Wider, *GridSet.Diagnostic),
			GridSet.bOk && !AfterGrid.Grids.IsEmpty() && LoadingRangeOf(AfterGrid.Grids[0]) == Wider);
	}
	FHyperAIWorldPartitionOp Default = Op(TEXT("world_partition.set"));
	Default.HLODLayer = HLOD;
	TestTrue(TEXT("the default HLOD layer is set"), Run({Default}).bOk && Partition->GetDefaultHLODLayer() == HLODLayer);
	FHyperAIWorldPartitionOp Load = Op(TEXT("region.load"));
	Load.Center = Far;
	Load.Size = FVector2D(50.0, 50.0);
	Load.Label = RegionName;
	TestTrue(TEXT("a region loads in the editor"), Run({Load}).bOk);
	TestTrue(TEXT("and inspect lists it"), C::Inspect(FHyperAIWorldPartitionInspectRequest()).Regions.ContainsByPredicate([&RegionName](const FString& Line) { return Line.Contains(RegionName); }));
	FHyperAIWorldPartitionOp Unload = Op(TEXT("region.unload"));
	Unload.Label = RegionName;
	const FHyperAIWorldPartitionPlanReport Unloaded = Run({Unload});
	TestTrue(TEXT("and unloads: ") + FString::Join(Unloaded.Notes, TEXT(" ")), Unloaded.bOk && Unloaded.Notes.Contains(TEXT("1 region(s) unloaded.")));

	// A plan is one undo step.
	FHyperAIWorldPartitionOp StreamB = Op(TEXT("actors.set"));
	StreamB.Actors = {B->GetActorLabel()};
	StreamB.SpatiallyLoaded = TEXT("false");
	TestTrue(TEXT("B is made always loaded"), Run({StreamB}).bOk && !B->GetIsSpatiallyLoaded());
	GEditor->UndoTransaction();
	TestTrue(TEXT("and undo streams it again"), B->GetIsSpatiallyLoaded());

	// Deleting the layers leaves their actors in the level.
	FHyperAIWorldPartitionOp DropChild = Op(TEXT("data_layer.delete"));
	DropChild.DataLayer = Child;
	FHyperAIWorldPartitionOp DropRuntime = DropChild;
	DropRuntime.DataLayer = Runtime;
	FHyperAIWorldPartitionOp DropEditor = DropChild;
	DropEditor.DataLayer = EditorOnly;
	const FHyperAIWorldPartitionPlanReport Dropped = Run({DropChild, DropRuntime, DropEditor});
	TestTrue(TEXT("the three data layers are deleted: ") + Dropped.Diagnostic, Dropped.bOk && !Layer(Runtime) && !Layer(Child) && !Layer(EditorOnly));
	TestTrue(TEXT("and the actors stay, in no layer"), IsValid(A) && IsValid(B) && A->GetDataLayerInstances().IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
