// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioMeshTerrainGate.h"
#include "HyperAIStudioMeshTerrainToolset.h"
#include "MeshPartition.h"
#include "MeshPartitionModifierComponent.h"
#include "Misc/AutomationTest.h"
#include "Modifiers/MeshPartitionMeshProvider.h"

namespace HyperAIStudio::MeshTerrain::Tests
{
	FHyperAIMeshTerrainOp Op(const TCHAR* Kind, const TCHAR* Name)
	{
		FHyperAIMeshTerrainOp Value;
		Value.Kind = Kind;
		Value.Name = Name;
		return Value;
	}

	const FHyperAIMeshTerrainModifier* FindModifier(const TArray<FHyperAIMeshTerrainModifier>& Modifiers, const TCHAR* Name)
	{
		return Modifiers.FindByPredicate([Name](const FHyperAIMeshTerrainModifier& Modifier) { return Modifier.Name == Name; });
	}

	/** Removes the test terrain, its base sections and the test modifiers, whatever state a failed run left. */
	void CleanUp(UWorld& World)
	{
		TArray<AActor*> Doomed;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel().StartsWith(TEXT("HyperAITest")))
			{
				Doomed.Add(*It);
			}
			TArray<UE::MeshPartition::UModifierComponent*> Modifiers;
			It->GetComponents(Modifiers);
			for (const UE::MeshPartition::UModifierComponent* Modifier : Modifiers)
			{
				const UE::MeshPartition::AMeshPartition* Terrain = Modifier->GetAffectedMeshPartition();
				if (Modifier->IsA<UE::MeshPartition::UMeshProviderModifier>() && Terrain && Terrain->GetActorLabel().StartsWith(TEXT("HyperAITest")))
				{
					Doomed.AddUnique(*It);
				}
			}
		}
		for (AActor* Actor : Doomed)
		{
			World.EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMeshTerrainContractsTest,
	"HyperAIStudio.NativeTools.MeshTerrain.Contracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMeshTerrainContractsTest::RunTest(const FString& Parameters)
{
	namespace Gate = HyperAIStudio::MeshTerrain::Gate;
	TestTrue(TEXT("Three tools"), FHyperAIStudioMeshTerrainContracts::GetToolNames() == TArray<FString>{
		TEXT("hyper_mesh_terrain_inspect"), TEXT("hyper_mesh_terrain_apply_plan"), TEXT("hyper_mesh_terrain_validate")});
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = FHyperAIStudioMeshTerrainContracts::GetAdapterDescriptor();
	TestEqual(TEXT("Three variants"), Descriptor.Variants.Num(), 3);
	TestEqual(TEXT("Terrain edits are Edit-class"), static_cast<uint8>(Descriptor.Variants[1].Safety),
		static_cast<uint8>(EHyperAIStudioDomainSafety::Edit));

	TArray<TPair<FString, FString>> Pairs;
	FString Error;
	TestTrue(TEXT("Settings parse"), Gate::ParseProperties(TEXT("Intensity=800; NoiseFrequency=(X=0.001,Y=0.002) ;"), Pairs, Error));
	TestTrue(TEXT("into name and value pairs"), Pairs.Num() == 2 && Pairs[0].Key == TEXT("Intensity") && Pairs[0].Value == TEXT("800")
		&& Pairs[1].Value == TEXT("(X=0.001,Y=0.002)"));
	TestTrue(TEXT("A semicolon inside brackets stays in the value"), Gate::ParseProperties(TEXT("A=(1;2)"), Pairs, Error)
		&& Pairs.Num() == 1 && Pairs[0].Value == TEXT("(1;2)"));
	TestFalse(TEXT("An unclosed bracket is refused"), Gate::ParseProperties(TEXT("A=(1"), Pairs, Error));
	TestFalse(TEXT("A setting needs Name=Value"), Gate::ParseProperties(TEXT("oops"), Pairs, Error));

	TArray<FVector> Points;
	TestTrue(TEXT("Points parse"), Gate::ParsePoints(TEXT("0,0,0; 1000,-50.5,20"), Points, Error) && Points.Num() == 2
		&& Points[1].Equals(FVector(1000.0, -50.5, 20.0)));
	TestFalse(TEXT("A spline needs two points"), Gate::ParsePoints(TEXT("0,0,0"), Points, Error));
	TestFalse(TEXT("Points are numbers"), Gate::ParsePoints(TEXT("a,b,c; 1,2,3"), Points, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMeshTerrainEditTest,
	"HyperAIStudio.NativeTools.MeshTerrain.CreateShapeAndRemove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMeshTerrainEditTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MeshTerrain::Tests;
	namespace Gate = HyperAIStudio::MeshTerrain::Gate;
	UWorld* World = Gate::GetEditorWorld();
	if (!TestNotNull(TEXT("An editor world"), World))
	{
		return false;
	}
	CleanUp(*World);
	auto WaitForRebuild = [World]()
	{
		const double End = FPlatformTime::Seconds() + 30.0;
		while (Gate::IsBuilding(*World) && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(1.0f / 30.0f);
			World->Tick(LEVELTICK_All, 1.0f / 30.0f);
		}
		return !Gate::IsBuilding(*World);
	};

	TArray<FString> Notes;
	FString Error;
	FHyperAIMeshTerrainOp Huge = Op(TEXT("create_terrain"), TEXT("HyperAITestHuge"));
	Huge.Location = TEXT("0,0,0");
	Huge.Size = TEXT("100000,100000");
	Huge.Spacing = 25.f;
	TestFalse(TEXT("A terrain with too many quads is refused"), Gate::ValidateOps(*World, {Huge}, Notes, Error));
	TestTrue(TEXT("and the refusal says how to fix it"), Error.Contains(TEXT("spacing of at least")));

	FHyperAIMeshTerrainOp Create = Op(TEXT("create_terrain"), TEXT("HyperAITestTerrain"));
	Create.Location = TEXT("0,0,0");
	Create.Size = TEXT("2000,2000");
	Create.Spacing = 100.f;
	FHyperAIMeshTerrainOp Hills = Op(TEXT("add_modifier"), TEXT("HyperAITestHills"));
	Hills.Type = TEXT("noise");
	Hills.Location = TEXT("0,0,0");
	Hills.Properties = TEXT("Intensity=250");
	FHyperAIMeshTerrainOp Road = Op(TEXT("add_modifier"), TEXT("HyperAITestRoad"));
	Road.Type = TEXT("spline");
	Road.Location = TEXT("0,0,0");
	Road.Points = TEXT("-800,0,0; 800,0,0");

	FHyperAIMeshTerrainOp Unknown = Hills;
	Unknown.Properties = TEXT("NotASetting=1");
	TestFalse(TEXT("An unknown setting is refused before anything changes"), Gate::ValidateOps(*World, {Create, Unknown}, Notes, Error));
	TestTrue(TEXT("naming the setting"), Error.Contains(TEXT("NotASetting")));
	FHyperAIMeshTerrainOp BadValue = Hills;
	BadValue.Properties = TEXT("Intensity=lots");
	TestFalse(TEXT("A value of the wrong type is refused"), Gate::ValidateOps(*World, {Create, BadValue}, Notes, Error));
	FHyperAIMeshTerrainOp NoPoints = Road;
	NoPoints.Points.Reset();
	TestFalse(TEXT("A spline without points is refused"), Gate::ValidateOps(*World, {Create, NoPoints}, Notes, Error));
	TestFalse(TEXT("A modifier needs a terrain"), Gate::ValidateOps(*World, {Hills}, Notes, Error));

	Notes.Reset();
	if (!TestTrue(*FString::Printf(TEXT("The plan validates (%s)"), *Error), Gate::ValidateOps(*World, {Create, Hills, Road}, Notes, Error)))
	{
		return false;
	}
	TestTrue(TEXT("It notes the terrain's size"), Notes.Num() == 1 && Notes[0].Contains(TEXT("20x20 quads")));
	int32 Changed = 0;
	if (!TestTrue(*FString::Printf(TEXT("The plan applies (%s)"), *Error), Gate::ApplyOps(*World, {Create, Hills, Road}, Changed, Error)))
	{
		CleanUp(*World);
		return false;
	}
	TestEqual(TEXT("Three ops ran"), Changed, 3);
	TestTrue(TEXT("The terrain finishes rebuilding"), WaitForRebuild());

	TArray<FHyperAIMeshTerrain> Terrains;
	TArray<FHyperAIMeshTerrainModifier> Modifiers;
	Gate::Read(*World, Terrains, Modifiers);
	const FHyperAIMeshTerrain* Terrain = Terrains.FindByPredicate([](const FHyperAIMeshTerrain& Item) { return Item.Name == TEXT("HyperAITestTerrain"); });
	TestTrue(TEXT("The terrain exists with base sections and a definition"),
		Terrain && Terrain->SectionCount >= 1 && !Terrain->Definition.IsEmpty() && Terrain->ModifierCount == 2);
	const FHyperAIMeshTerrainModifier* HillsRecord = FindModifier(Modifiers, TEXT("HyperAITestHills"));
	const FHyperAIMeshTerrainModifier* RoadRecord = FindModifier(Modifiers, TEXT("HyperAITestRoad"));
	TestTrue(TEXT("The noise modifier shapes the terrain with its setting"), HillsRecord && HillsRecord->Type == TEXT("noise")
		&& HillsRecord->Terrain == TEXT("HyperAITestTerrain") && HillsRecord->Settings.Contains(TEXT("Intensity=250")));
	TestTrue(TEXT("The spline modifier has its points"), RoadRecord && RoadRecord->Type == TEXT("spline")
		&& RoadRecord->Points.Contains(TEXT("800,0,0")));

	const FString Before = Gate::ComputeRevision(*World);
	FHyperAIMeshTerrainOp Stronger = Op(TEXT("set_modifier"), TEXT("HyperAITestHills"));
	Stronger.Properties = TEXT("Intensity=500");
	TestTrue(*FString::Printf(TEXT("A setting changes (%s)"), *Error), Gate::ValidateOps(*World, {Stronger}, Notes, Error)
		&& Gate::ApplyOps(*World, {Stronger}, Changed, Error));
	WaitForRebuild();
	TestNotEqual(TEXT("The revision follows the change"), Gate::ComputeRevision(*World), Before);
	Terrains.Reset();
	Modifiers.Reset();
	Gate::Read(*World, Terrains, Modifiers);
	HillsRecord = FindModifier(Modifiers, TEXT("HyperAITestHills"));
	TestTrue(TEXT("The new value reads back"), HillsRecord && HillsRecord->Settings.Contains(TEXT("Intensity=500")));

	FHyperAIMeshTerrainValidateReport Report;
	Gate::Check(*World, Report);
	TestEqual(TEXT("The terrain validates without errors"), Report.ErrorCount, 0);

	TestTrue(TEXT("A modifier is removed"), Gate::ValidateOps(*World, {Op(TEXT("remove_modifier"), TEXT("HyperAITestRoad"))}, Notes, Error)
		&& Gate::ApplyOps(*World, {Op(TEXT("remove_modifier"), TEXT("HyperAITestRoad"))}, Changed, Error));
	Terrains.Reset();
	Modifiers.Reset();
	Gate::Read(*World, Terrains, Modifiers);
	TestNull(TEXT("and is gone"), FindModifier(Modifiers, TEXT("HyperAITestRoad")));
	WaitForRebuild();
	CleanUp(*World);
	return true;
}

#endif
