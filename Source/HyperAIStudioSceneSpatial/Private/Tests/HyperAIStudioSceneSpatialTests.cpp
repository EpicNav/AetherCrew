// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioSceneSpatialToolset.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TriggerBox.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Helpers/PCGHelpers.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioSceneCost.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSpatialPlan.h"
#include "HyperAIStudioSpatialQueries.h"
#include "InstancedFoliageActor.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::SceneSpatial::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	UStaticMesh* Cube()
	{
		return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	}

	/** The engine cube's drawn sections at LOD0. */
	int32 CubeSections()
	{
		int32 Sections = 0;
		for (const FStaticMeshSection& Section : Cube()->GetRenderData()->LODResources[0].Sections)
		{
			Sections += Section.NumTriangles > 0 ? 1 : 0;
		}
		return Sections;
	}

	/** A 100 cm engine cube (scaled), labelled, undoable like an actor placed by hand. */
	AStaticMeshActor* AddCube(UWorld& World, const TCHAR* Label, const FVector& Location, const FVector& Scale = FVector::OneVector,
		const FRotator& Rotation = FRotator::ZeroRotator, UStaticMesh* Mesh = nullptr)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transactional;
		AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(Rotation, Location, Scale), Params);
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		Component->UnregisterComponent();
		Component->SetStaticMesh(Mesh ? Mesh : Cube());
		Component->RegisterComponent();
		Actor->SetActorLabel(Label);
		return Actor;
	}

	/** A floor 100 cm thick with its top at Z = 0, Size cm across. */
	AStaticMeshActor* AddFloor(UWorld& World, const double Size = 4000.0)
	{
		return AddCube(World, TEXT("Floor"), FVector(0.0, 0.0, -50.0), FVector(Size / 100.0, Size / 100.0, 1.0));
	}

	/** Count cubes (or Mesh) instanced on a new actor of OwnerClass, 120 cm apart along X at Y = 1000. */
	UInstancedStaticMeshComponent* AddInstanced(UWorld& World, const TCHAR* Label, const int32 Count, UClass* OwnerClass, const FName Tag = NAME_None,
		UStaticMesh* Mesh = nullptr)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transactional;
		AActor* Owner = World.SpawnActor<AActor>(OwnerClass, FTransform::Identity, Params);
		UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("Instances"));
		Component->SetStaticMesh(Mesh ? Mesh : Cube());
		if (!Tag.IsNone()) Component->ComponentTags.Add(Tag);
		if (USceneComponent* Root = Owner->GetRootComponent()) Component->SetupAttachment(Root);
		else Owner->SetRootComponent(Component);
		Owner->AddInstanceComponent(Component);
		Component->RegisterComponent();
		TArray<FTransform> Transforms;
		Transforms.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Transforms.Add(FTransform(FVector(Index * 120.0, 1000.0, 0.0)));
		}
		Component->AddInstances(Transforms, /*bShouldReturnIndices=*/false);
		Owner->SetActorLabel(Label);
		return Component;
	}

	/** A flat landscape of one component, 63 quads a side, labelled. */
	ALandscape* AddLandscape(UWorld& World, const TCHAR* Label)
	{
		constexpr int32 Size = 64;
		TArray<uint16> Heights;
		Heights.Init(32768, Size * Size);
		ALandscape* Landscape = World.SpawnActor<ALandscape>(FVector::ZeroVector, FRotator::ZeroRotator);
		if (!Landscape) return nullptr;
		Landscape->SetActorRelativeScale3D(FVector(100.0, 100.0, 100.0));
		TMap<FGuid, TArray<uint16>> HeightData;
		HeightData.Add(FGuid(), MoveTemp(Heights));
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> Layers;
		Layers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
		Landscape->Import(FGuid::NewGuid(), 0, 0, Size - 1, Size - 1, 1, 63, HeightData, TEXT(""), Layers, ELandscapeImportAlphamapType::Additive,
			TArrayView<const FLandscapeLayer>());
		Landscape->SetActorLabel(Label);
		return Landscape;
	}

	FHyperAISceneCostReport Measure(UWorld& World, const FHyperAISceneCostRequest& Request = FHyperAISceneCostRequest())
	{
		FHyperAISceneCostReport Report;
		Cost::Measure(World, Request, Report);
		return Report;
	}

	FHyperAISpatialResult Ask(UWorld& World, const FHyperAISpatialQuery& Query)
	{
		FHyperAISpatialResult Result;
		Spatial::Run(World, Query, FPlatformTime::Seconds() + 5.0, Result);
		return Result;
	}

	FHyperAISpatialQuery Query(const TCHAR* Kind)
	{
		FHyperAISpatialQuery Out;
		Out.Kind = Kind;
		return Out;
	}

	FHyperAISpatialOp Op(const TCHAR* Kind, const TArray<FString>& Actors = {})
	{
		FHyperAISpatialOp Out;
		Out.Kind = Kind;
		Out.Actors = Actors;
		return Out;
	}

	FHyperAISpatialPlanRequest PlanOf(const TArray<FHyperAISpatialOp>& Ops)
	{
		FHyperAISpatialPlanRequest Request;
		Request.Operations = Ops;
		return Request;
	}

	AActor* Labelled(UWorld& World, const TCHAR* Label)
	{
		FString Error;
		return Spatial::FindActor(World, Label, Error);
	}

	/** Whether a box of half size Half (plus nothing) at Center is free of what blocks WorldDynamic, Ignored aside. */
	bool IsClear(UWorld& World, const FVector& Center, const FVector& Half, const AActor* Ignored)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAISpatialTest), false, Ignored);
		return !World.OverlapBlockingTestByChannel(Center, FQuat::Identity, ECC_WorldDynamic, FCollisionShape::MakeBox(Half), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneSpatialCohortTest,
	"HyperAIStudio.SceneSpatial.Contracts.ExactThreeToolCohort",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneSpatialCohortTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Names = FHyperAIStudioSceneSpatialContracts::GetToolNames();
	TestEqual(TEXT("three tools"), Names.Num(), 3);
	TestTrue(TEXT("with the names prompts call"), Names.Contains(TEXT("hyper_scene_cost_report")) && Names.Contains(TEXT("hyper_spatial_query"))
		&& Names.Contains(TEXT("hyper_spatial_apply_plan")));
	int32 Callables = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioSceneSpatialToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->HasMetaData(TEXT("AICallable"))) continue;
		++Callables;
		TestTrue(*FString::Printf(TEXT("%s is a manifest tool"), *It->GetName()), Names.Contains(It->GetName()));
		const FString ToolTip = It->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description within 400 characters (%d)"), *It->GetName(), ToolTip.Len()), !ToolTip.IsEmpty() && ToolTip.Len() <= 400);
	}
	TestEqual(TEXT("AICallable functions match the manifest"), Callables, Names.Num());
	TestEqual(TEXT("in the level design pack"), FString(FHyperAIStudioSceneSpatialContracts::PackId), FString(TEXT("level_design")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneCostCountsTest,
	"HyperAIStudio.SceneSpatial.Cost.CountsStaticAndInstanced",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneCostCountsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISceneCostCountsWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	UStaticMesh* Mesh = Cube();
	if (!TestNotNull(TEXT("the engine cube loads"), Mesh)) return false;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AddCube(*World, *FString::Printf(TEXT("Cube%d"), Index), FVector(Index * 300.0, 0.0, 0.0));
	}
	AddInstanced(*World, TEXT("Rocks"), 100, AActor::StaticClass());

	const FHyperAISceneCostReport Report = Measure(*World);
	TestTrue(*FString::Printf(TEXT("measured (%s)"), *Report.Diagnostic), Report.bOk);
	if (Report.NanitePrimitives > 0)
	{
		AddInfo(TEXT("The engine cube draws with Nanite here; the non-Nanite counts are not comparable."));
		return true;
	}
	const int64 Triangles = Mesh->GetNumTriangles(0);
	TestEqual(TEXT("three static mesh components"), Report.StaticMeshComponents, 3);
	TestEqual(TEXT("one instanced component"), Report.InstancedComponents, 1);
	TestEqual(TEXT("103 instances"), Report.Instances, 103);
	TestEqual(TEXT("LOD0 triangles times instances"), Report.TrianglesNonNanite, 103 * Triangles);
	TestEqual(TEXT("one draw per section per component, instances batched"), Report.EstimatedDrawCalls, 4 * CubeSections());
	TestEqual(TEXT("one mesh"), Report.UniqueMeshes, 1);
	TestEqual(TEXT("the default brush and world settings draw nothing: ") + FString::Join(Report.OtherClasses, TEXT(", ")), Report.OtherPrimitives, 0);
	const FHyperAISceneCostEntry* Heaviest = Report.HeaviestMeshes.IsEmpty() ? nullptr : &Report.HeaviestMeshes[0];
	TestTrue(TEXT("the cube is the heaviest mesh, with every instance counted"), Heaviest && Heaviest->Name == Mesh->GetPathName() && Heaviest->Instances == 103
		&& Heaviest->TotalTriangles == 103 * Triangles);
	TestTrue(TEXT("the instanced actor is the heaviest actor"), !Report.HeaviestActors.IsEmpty() && Report.HeaviestActors[0].Name == TEXT("Rocks")
		&& Report.HeaviestActors[0].Triangles == 100 * Triangles);
	TestTrue(TEXT("a private world is not World Partition"), !Report.bWorldPartition);
	TestFalse(TEXT("nothing was cut"), Report.bTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneCostNaniteTest,
	"HyperAIStudio.SceneSpatial.Cost.NaniteSplit",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneCostNaniteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	// A copy of the engine cube built with Nanite.
	UStaticMesh* NaniteCube = DuplicateObject<UStaticMesh>(Cube(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("HyperAISceneCostNaniteCube")));
	if (!TestNotNull(TEXT("the cube copies"), NaniteCube)) return false;
	FMeshNaniteSettings Settings = NaniteCube->GetNaniteSettings();
	Settings.bEnabled = true;
	NaniteCube->SetNaniteSettings(Settings);
	NaniteCube->Build(/*bInSilent=*/true);
	const TArray<UStaticMesh*> Built = {NaniteCube};
	FStaticMeshCompilingManager::Get().FinishCompilation(Built);
	if (!NaniteCube->HasValidNaniteData())
	{
		AddInfo(TEXT("No Nanite data was built here; skipped."));
		return true;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISceneCostNaniteWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddCube(*World, TEXT("Nanite"), FVector::ZeroVector, FVector::OneVector, FRotator::ZeroRotator, NaniteCube);
	AStaticMeshActor* Fallback = AddCube(*World, TEXT("Fallback"), FVector(500.0, 0.0, 0.0), FVector::OneVector, FRotator::ZeroRotator, NaniteCube);
	Fallback->GetStaticMeshComponent()->SetForceDisableNanite(true);

	const FHyperAISceneCostReport Report = Measure(*World);
	if (Report.NanitePrimitives == 0)
	{
		AddInfo(TEXT("This platform does not draw Nanite; skipped."));
		return true;
	}
	TestEqual(TEXT("one Nanite primitive"), Report.NanitePrimitives, 1);
	TestEqual(TEXT("its source triangles"), Report.TrianglesNaniteSource, static_cast<int64>(NaniteCube->GetNumNaniteTriangles()));
	TestEqual(TEXT("the forced fallback counts its LOD0 as non-Nanite, and only it"), Report.TrianglesNonNanite, static_cast<int64>(NaniteCube->GetNumTriangles(0)));
	TestEqual(TEXT("Nanite adds no draw calls"), Report.EstimatedDrawCalls, NaniteCube->GetRenderData()->LODResources[0].Sections.Num());
	TestTrue(TEXT("its material is a Nanite shading bin"), Report.NaniteMaterialBins >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneCostCompilingTest,
	"HyperAIStudio.SceneSpatial.Cost.SkipsCompilingMeshes",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneCostCompilingTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	// A copy of the engine cube, built once so a cube can show it, then built again so it is compiling while measured.
	UStaticMesh* Building = DuplicateObject<UStaticMesh>(Cube(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("HyperAISceneCostBuildingCube")));
	if (!TestNotNull(TEXT("the cube copies"), Building)) return false;
	const TArray<UStaticMesh*> Built = {Building};
	Building->Build(/*bInSilent=*/true);
	FStaticMeshCompilingManager::Get().FinishCompilation(Built);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISceneCostCompilingWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddCube(*World, TEXT("Ready"), FVector::ZeroVector);
	AddCube(*World, TEXT("Building"), FVector(500.0, 0.0, 0.0), FVector::OneVector, FRotator::ZeroRotator, Building);
	Building->Build(/*bInSilent=*/true);
	// The build ends before the world it shows in goes.
	ON_SCOPE_EXIT { FStaticMeshCompilingManager::Get().FinishCompilation(Built); };
	if (!Building->IsCompiling())
	{
		AddInfo(TEXT("Static meshes build synchronously here; skipped."));
		return true;
	}

	const FHyperAISceneCostReport Report = Measure(*World);
	TestTrue(TEXT("the report did not wait for the build"), Building->IsCompiling());
	TestTrue(*FString::Printf(TEXT("measured (%s)"), *Report.Diagnostic), Report.bOk);
	TestEqual(TEXT("the ready cube is counted, the compiling one left out"), Report.StaticMeshComponents, 1);
	TestEqual(TEXT("so is its mesh"), Report.UniqueMeshes, 1);
	TestTrue(*FString::Printf(TEXT("one finding says how many were left out: %s"), *FString::Join(Report.Findings, TEXT(" | "))),
		Report.Findings.FilterByPredicate([](const FString& Finding) { return Finding.StartsWith(TEXT("meshes_compiling: 1 mesh component(s)")); }).Num() == 1);
	TestFalse(TEXT("not as a mesh without render data"), Report.Findings.ContainsByPredicate([](const FString& Finding) { return Finding.StartsWith(TEXT("no_render_data:")); }));
	TestTrue(*FString::Printf(TEXT("and the report says it is partial (%s)"), *Report.TruncatedReason), Report.bTruncated && Report.TruncatedReason == TEXT("meshes_compiling"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneCostShadowsTest,
	"HyperAIStudio.SceneSpatial.Cost.ShadowsLightsTranslucency",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneCostShadowsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISceneCostShadowsWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddCube(*World, TEXT("Lit"), FVector::ZeroVector);
	AStaticMeshActor* Far = AddCube(*World, TEXT("Far"), FVector(3000.0, 0.0, 0.0));
	// A movable point light reaching the first cube only, and a static sun that casts no dynamic shadows.
	APointLight* Point = World->SpawnActor<APointLight>(FVector(0.0, 0.0, 200.0), FRotator::ZeroRotator);
	UPointLightComponent* PointLight = Point ? Cast<UPointLightComponent>(Point->GetLightComponent()) : nullptr;
	if (!TestNotNull(TEXT("a point light"), PointLight)) return false;
	PointLight->SetMobility(EComponentMobility::Movable);
	PointLight->SetAttenuationRadius(500.f);
	PointLight->SetCastShadows(true);
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 1000.0), FRotator(-45.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("a sun"), Sun)) return false;
	Sun->GetLightComponent()->SetMobility(EComponentMobility::Static);
	// In the level's package, as a project's material is: the editor's transient materials are not counted.
	UMaterial* Glass = NewObject<UMaterial>(World->GetOutermost(), MakeUniqueObjectName(World->GetOutermost(), UMaterial::StaticClass(), TEXT("HyperAISceneCostGlass")));
	Glass->BlendMode = BLEND_Translucent;
	Far->GetStaticMeshComponent()->SetMaterial(0, Glass);

	const FHyperAISceneCostReport Report = Measure(*World);
	TestEqual(TEXT("one point light"), Report.Lights.Point, 1);
	TestEqual(TEXT("one directional light"), Report.Lights.Directional, 1);
	TestEqual(TEXT("one movable"), Report.Lights.Movable, 1);
	TestEqual(TEXT("one static"), Report.Lights.Static, 1);
	TestEqual(TEXT("only the movable light casts dynamic shadows"), Report.Lights.ShadowCastingDynamic, 1);
	TestEqual(TEXT("both cubes cast shadows"), Report.ShadowCastingPrimitives, 2);
	TestEqual(TEXT("only the cube the light reaches draws its shadow"), Report.EstimatedShadowDrawCalls, CubeSections());
	TestEqual(TEXT("one translucent material"), Report.TranslucentMaterials, 1);
	TestEqual(TEXT("on one primitive"), Report.TranslucentPrimitives, 1);
	TestTrue(TEXT("and it is listed"), Report.TranslucentList.Num() == 1 && Report.TranslucentList[0].Contains(Glass->GetName()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneCostFoliageTest,
	"HyperAIStudio.SceneSpatial.Cost.FoliageAndPCGTagging",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneCostFoliageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	// The module matches PCG's tag as text so it needs no PCG; this holds the text to PCG's own.
	TestEqual(TEXT("the generated-component tag is PCG's"), Spatial::PCGGeneratedTag(), PCGHelpers::DefaultPCGTag);
	if (FModuleManager::Get().IsModuleLoaded(TEXT("PCG")))
	{
		TestNotNull(TEXT("PCG's partition actor class is found by path"), Spatial::PCGPartitionActorClass());
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISceneCostFoliageWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	UInstancedStaticMeshComponent* Foliage = AddInstanced(*World, TEXT("Foliage"), 10, AInstancedFoliageActor::StaticClass());
	UInstancedStaticMeshComponent* Generated = AddInstanced(*World, TEXT("Generated"), 7, AActor::StaticClass(), Spatial::PCGGeneratedTag());
	AStaticMeshActor* Plain = AddCube(*World, TEXT("Plain"), FVector::ZeroVector);

	const FHyperAISceneCostReport Report = Measure(*World);
	TestEqual(TEXT("foliage instances"), Report.FoliageInstances, 10);
	TestEqual(TEXT("PCG instances"), Report.PCGInstances, 7);
	TestEqual(TEXT("one PCG component"), Report.PCGComponents, 1);
	TestEqual(TEXT("every instance"), Report.Instances, 18);
	TestTrue(TEXT("foliage is passed through when asked"), Spatial::IsFoliageOrPCG(*Foliage));
	TestTrue(TEXT("so is a tagged PCG component"), Spatial::IsFoliageOrPCG(*Generated));
	TestFalse(TEXT("a placed mesh is not"), Spatial::IsFoliageOrPCG(*Plain->GetStaticMeshComponent()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneCostLandscapeMaterialsTest,
	"HyperAIStudio.SceneSpatial.Cost.LandscapeToolMaterials",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneCostLandscapeMaterialsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISceneCostLandscapeWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	ALandscape* Landscape = AddLandscape(*World, TEXT("Ground"));
	if (!TestNotNull(TEXT("a landscape"), Landscape) || !TestFalse(TEXT("with a component"), Landscape->LandscapeComponents.IsEmpty()))
	{
		return false;
	}
	// What the report counted before: in the editor every landscape component lists the landscape tools' brush, selection
	// and layer materials among its used materials, and some of them are translucent.
	TArray<UMaterialInterface*> Used;
	Landscape->LandscapeComponents[0]->GetUsedMaterials(Used);
	const int32 ToolTranslucent = Used.FilterByPredicate([](const UMaterialInterface* Material) { return Material && IsTranslucentBlendMode(*Material); }).Num();
	TestTrue(*FString::Printf(TEXT("the editor lists translucent tool materials on a landscape component (%d of %d)"), ToolTranslucent, Used.Num()), ToolTranslucent > 0);

	// A window whose glass is a dynamic instance, made in the transient package as game code makes one: it still counts.
	UMaterial* Glass = NewObject<UMaterial>(World->GetOutermost(), MakeUniqueObjectName(World->GetOutermost(), UMaterial::StaticClass(), TEXT("HyperAISceneCostWindowGlass")));
	Glass->BlendMode = BLEND_Translucent;
	UMaterialInstanceDynamic* Pane = UMaterialInstanceDynamic::Create(Glass, nullptr);
	AStaticMeshActor* Window = AddCube(*World, TEXT("Window"), FVector(0.0, 0.0, 2000.0));
	Window->GetStaticMeshComponent()->SetMaterial(0, Pane);

	const FHyperAISceneCostReport Report = Measure(*World);
	TestTrue(*FString::Printf(TEXT("measured (%s)"), *Report.Diagnostic), Report.bOk);
	TestEqual(TEXT("the landscape's components are counted"), Report.LandscapeComponents, Landscape->LandscapeComponents.Num());
	TestEqual(*FString::Printf(TEXT("only the window's material is translucent: %s"), *FString::Join(Report.TranslucentList, TEXT(", "))), Report.TranslucentMaterials, 1);
	TestEqual(TEXT("on the window alone"), Report.TranslucentPrimitives, 1);
	TestTrue(TEXT("and it is the one listed"), Report.TranslucentList.Num() == 1 && Report.TranslucentList[0].Contains(Pane->GetName()));
	const FHyperAISceneCostEntry* Ground = Report.HeaviestActors.FindByPredicate([](const FHyperAISceneCostEntry& Entry) { return Entry.Name == TEXT("Ground"); });
	TestTrue(*FString::Printf(TEXT("the landscape counts its own materials, not the tools' (%d of %d)"), Ground ? Ground->Materials : -1, Used.Num()),
		Ground && Ground->Materials >= 1 && Ground->Materials < Used.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneCostHeavyInstancedTest,
	"HyperAIStudio.SceneSpatial.Cost.HeavyInstancedMeshes",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneCostHeavyInstancedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	// Ground cover: four meshes light one by one (the engine sphere and three copies of it), each instanced just past the
	// bar, as a scatter puts grass and ferns down.
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!TestNotNull(TEXT("the engine sphere loads"), Sphere)) return false;
	TArray<UStaticMesh*> Meshes = {Sphere};
	TArray<UStaticMesh*> Copies;
	for (int32 Index = 1; Index < 4; ++Index)
	{
		UStaticMesh* Copy = DuplicateObject<UStaticMesh>(Sphere, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), *FString::Printf(TEXT("HyperAISceneCostTuft%d"), Index)));
		if (!TestNotNull(TEXT("the sphere copies"), Copy)) return false;
		Copy->Build(/*bInSilent=*/true);
		Copies.Add(Copy);
		Meshes.Add(Copy);
	}
	FStaticMeshCompilingManager::Get().FinishCompilation(Copies);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISceneCostInstancedWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	// Each just past the bar, the sphere the heaviest and each copy lighter than the one before.
	TArray<int32> Counts;
	int64 Expected = 0;
	for (int32 Index = 0; Index < Meshes.Num(); ++Index)
	{
		const int64 Triangles = Meshes[Index]->GetNumTriangles(0);
		if (!TestTrue(TEXT("the mesh has triangles"), Triangles > 0)) return false;
		Counts.Add(static_cast<int32>(Cost::HeavyInstancedTriangles / Triangles) + 1 + (Meshes.Num() - Index) * 10);
		Expected += Triangles * Counts.Last();
	}
	TArray<UInstancedStaticMeshComponent*> Components;
	// The sphere's instances are split over two components, one culled at 50 m: still one mesh, every instance counted.
	Components.Add(AddInstanced(*World, TEXT("Tufts0"), Counts[0] / 2, AActor::StaticClass(), NAME_None, Sphere));
	Components.Add(AddInstanced(*World, TEXT("Tufts0Far"), Counts[0] - Counts[0] / 2, AActor::StaticClass(), NAME_None, Sphere));
	Components.Last()->SetCullDistances(0, 5000);
	for (int32 Index = 1; Index < Meshes.Num(); ++Index)
	{
		Components.Add(AddInstanced(*World, *FString::Printf(TEXT("Tufts%d"), Index), Counts[Index], AActor::StaticClass(), NAME_None, Meshes[Index]));
	}
	// And pebbles well under it.
	Components.Add(AddInstanced(*World, TEXT("Pebbles"), 100, AActor::StaticClass()));
	Expected += Cube()->GetNumTriangles(0) * 100;
	for (UInstancedStaticMeshComponent* Component : Components)
	{
		Component->SetForceDisableNanite(true);
	}

	const FHyperAISceneCostReport Report = Measure(*World);
	TestTrue(*FString::Printf(TEXT("measured (%s)"), *Report.Diagnostic), Report.bOk);
	TestEqual(TEXT("every instance counts, culled or not"), Report.TrianglesNonNanite, Expected);
	TestTrue(TEXT("and the report says cull distances are not applied"), Report.Diagnostic.Contains(TEXT("before cull distances")));
	const TArray<FString> Heavy = Report.Findings.FilterByPredicate([](const FString& Finding) { return Finding.StartsWith(TEXT("non_nanite_instances: ")); });
	TestEqual(*FString::Printf(TEXT("three meshes named and the fourth summed: %s"), *FString::Join(Report.Findings, TEXT(" | "))), Heavy.Num(), 4);
	if (Heavy.Num() == 4)
	{
		TestTrue(TEXT("the heaviest first, by its path"), Heavy[0].StartsWith(FString::Printf(TEXT("non_nanite_instances: %s draws "), *Sphere->GetPathName())));
		TestTrue(TEXT("with the instances of both its components"), Heavy[0].Contains(FString::Printf(TEXT("%lld at LOD0 x %d instances in 2 component(s)"),
			static_cast<int64>(Sphere->GetNumTriangles(0)), Counts[0])));
		TestTrue(TEXT("saying which has no cull distance"), Heavy[0].Contains(TEXT("1 without a cull distance")) && Heavy[0].Contains(TEXT("set cull distances")));
		TestTrue(TEXT("and what would cut it"), Heavy[0].Contains(TEXT("Nanite")) && Heavy[0].Contains(TEXT("thin the instances")) && Heavy[0].Contains(TEXT("simplify it")));
		TestTrue(TEXT("and that the total is before cull distances"), Heavy[0].Contains(TEXT("before cull distances and LODs")));
		TestTrue(TEXT("a mesh in one component without a cull distance says so"), Heavy[1].Contains(TEXT("which has no cull distance")));
		TestTrue(TEXT("the next two by their paths"), Heavy[1].Contains(Meshes[1]->GetPathName()) && Heavy[2].Contains(Meshes[2]->GetPathName()));
		TestTrue(TEXT("and the rest summed in one"), Heavy[3].StartsWith(TEXT("non_nanite_instances: 1 more mesh(es) draw ")) && Heavy[3].Contains(Meshes[3]->GetPathName()));
	}
	TestFalse(TEXT("pebbles under the bar are not named"), Heavy.ContainsByPredicate([](const FString& Finding) { return Finding.Contains(Cube()->GetPathName()); }));
	TestTrue(TEXT("within the findings cap"), Report.Findings.Num() <= Cost::MaxFindings);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneCostScopeTest,
	"HyperAIStudio.SceneSpatial.Cost.BoundsAndScope",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneCostScopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISceneCostScopeWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AStaticMeshActor* First = AddCube(*World, TEXT("Cube0"), FVector::ZeroVector);
	AddCube(*World, TEXT("Cube1"), FVector(1000.0, 0.0, 0.0));
	AddCube(*World, TEXT("Cube2"), FVector(2000.0, 0.0, 0.0));

	FHyperAISceneCostRequest Box;
	Box.Scope = TEXT("box");
	Box.BoxMin = FVector(-200.0);
	Box.BoxMax = FVector(200.0);
	const FHyperAISceneCostReport InBox = Measure(*World, Box);
	TestEqual(TEXT("a box around the first cube counts it alone"), InBox.StaticMeshComponents, 1);
	TestEqual(TEXT("and one actor"), InBox.Actors, 1);

	FHyperAISceneCostRequest Named;
	Named.Scope = TEXT("actors");
	Named.Actors = {TEXT("cube1")};
	TestEqual(TEXT("named actors count alone, labels in any case"), Measure(*World, Named).StaticMeshComponents, 1);
	Named.Actors = {TEXT("Nobody")};
	const FHyperAISceneCostReport Missing = Measure(*World, Named);
	TestTrue(TEXT("an unknown name is refused by name"), Missing.Status == TEXT("invalid_request") && Missing.Diagnostic.Contains(TEXT("Nobody")));
	FHyperAISceneCostRequest Bad;
	Bad.Scope = TEXT("view");
	TestEqual(TEXT("there is no view scope"), Measure(*World, Bad).Status, FString(TEXT("invalid_request")));

	FHyperAISceneCostRequest One;
	One.MaxActors = 1;
	const FHyperAISceneCostReport Stopped = Measure(*World, One);
	TestTrue(TEXT("MaxActors stops the pass and says so"), Stopped.bTruncated && Stopped.TruncatedReason.Contains(TEXT("max_actors")) && Stopped.ActorsScanned == 1);

	First->SetIsTemporarilyHiddenInEditor(true);
	TestEqual(TEXT("an actor hidden in the editor is left out"), Measure(*World).StaticMeshComponents, 2);
	FHyperAISceneCostRequest Hidden;
	Hidden.bIncludeHidden = true;
	TestEqual(TEXT("unless asked for"), Measure(*World, Hidden).StaticMeshComponents, 3);
	First->SetIsTemporarilyHiddenInEditor(false);

	for (int32 Index = 0; Index < 40; ++Index)
	{
		AddCube(*World, *FString::Printf(TEXT("PaddingCubeWithALongLabel%02d"), Index), FVector(Index * 150.0, 3000.0, 0.0));
	}
	FHyperAISceneCostRequest Small;
	Small.TopN = 50;
	Small.MaxOutputBytes = 4096;
	const FHyperAISceneCostReport Cut = Measure(*World, Small);
	TestTrue(TEXT("lists are cut to fit"), Cut.bTruncated && Cut.TruncatedReason.Contains(TEXT("max_output_bytes")) && Cut.HeaviestActors.Num() < 43);
	TestTrue(*FString::Printf(TEXT("and the answer fits (%lld bytes)"), Cost::EstimateBytes(Cut)), Cost::EstimateBytes(Cut) <= 4096);
	TestEqual(TEXT("the counts still cover everything"), Cut.StaticMeshComponents, 43);

	// Many per-mesh findings leave room for the ones about the whole report, which come last.
	UWorld* Bare = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISceneCostFindingsWorld"));
	ON_SCOPE_EXIT { Bare->DestroyWorld(false); };
	for (int32 Index = 0; Index < 40; ++Index)
	{
		AStaticMeshActor* Meshless = Bare->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(FVector(Index * 200.0, 0.0, 0.0)));
		if (Meshless) Meshless->SetActorLabel(FString::Printf(TEXT("Meshless%02d"), Index));
	}
	FHyperAISceneCostRequest Capped;
	Capped.MaxActors = 32;
	const FHyperAISceneCostReport Busy = Measure(*Bare, Capped);
	const int32 NoRenderData = Busy.Findings.FilterByPredicate([](const FString& Finding) { return Finding.StartsWith(TEXT("no_render_data:")); }).Num();
	TestTrue(*FString::Printf(TEXT("meshless components are found (%d), within the cap (%d findings)"), NoRenderData, Busy.Findings.Num()),
		NoRenderData > 10 && Busy.Findings.Num() <= Cost::MaxFindings);
	TestTrue(TEXT("and the stop is still said"), Busy.Findings.ContainsByPredicate([](const FString& Finding) { return Finding.StartsWith(TEXT("max_actors:")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialTraceTest,
	"HyperAIStudio.SceneSpatial.Query.TraceSweepOverlap",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialTraceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialTraceWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddFloor(*World);
	// A wall from X = 750 to 850.
	AddCube(*World, TEXT("Wall"), FVector(800.0, 0.0, 250.0), FVector(1.0, 10.0, 5.0));
	ATriggerBox* Trigger = World->SpawnActor<ATriggerBox>(FVector(400.0, 0.0, 100.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("a trigger box"), Trigger)) return false;
	Trigger->SetActorLabel(TEXT("Trigger"));
	if (UBoxComponent* Shape = Cast<UBoxComponent>(Trigger->GetCollisionComponent())) Shape->SetBoxExtent(FVector(50.0));

	FHyperAISpatialQuery Down = Query(TEXT("trace"));
	Down.Start = FVector(0.0, 0.0, 500.0);
	Down.End = FVector(0.0, 0.0, -500.0);
	const FHyperAISpatialResult Traced = Ask(*World, Down);
	TestTrue(*FString::Printf(TEXT("a trace down hits the floor (%s)"), *Traced.Diagnostic), Traced.bOk && Traced.bHit && Traced.Hits.Num() == 1);
	if (Traced.Hits.Num() == 1)
	{
		TestEqual(TEXT("labelled Floor"), Traced.Hits[0].Actor, FString(TEXT("Floor")));
		TestTrue(TEXT("at its top"), FMath::IsNearlyEqual(Traced.Hits[0].Location.Z, 0.0, 0.5));
		TestTrue(TEXT("facing up"), Traced.Hits[0].Normal.Equals(FVector::UpVector, 0.01) && Traced.Hits[0].SlopeDegrees < 0.5f);
	}

	FHyperAISpatialQuery Sweep = Query(TEXT("sweep"));
	Sweep.Shape = TEXT("sphere");
	Sweep.Extent = FVector(20.0);
	Sweep.Start = FVector(0.0, 0.0, 100.0);
	Sweep.End = FVector(2000.0, 0.0, 100.0);
	const FHyperAISpatialResult Swept = Ask(*World, Sweep);
	TestTrue(*FString::Printf(TEXT("a sphere swept at the wall stops at it (%s)"), *Swept.Diagnostic), Swept.bHit && !Swept.Hits.IsEmpty() && Swept.Hits[0].Actor == TEXT("Wall"));
	TestTrue(*FString::Printf(TEXT("its center a radius short of the wall (%.1f)"), Swept.Location.X), FMath::IsNearlyEqual(Swept.Location.X, 730.0, 2.0));

	FHyperAISpatialQuery AtWall = Query(TEXT("overlap"));
	AtWall.Point = FVector(800.0, 0.0, 250.0);
	AtWall.Extent = FVector(20.0);
	const FHyperAISpatialResult Blocked = Ask(*World, AtWall);
	TestTrue(TEXT("the wall blocks an overlap"), Blocked.bOk && Blocked.bHit && Blocked.Hits.ContainsByPredicate([](const FHyperAISpatialHit& Hit) { return Hit.Actor == TEXT("Wall") && Hit.bBlocking; }));
	FHyperAISpatialQuery AtTrigger = Query(TEXT("overlap"));
	AtTrigger.Point = FVector(400.0, 0.0, 100.0);
	AtTrigger.Extent = FVector(10.0);
	const FHyperAISpatialResult Touched = Ask(*World, AtTrigger);
	TestTrue(*FString::Printf(TEXT("a trigger is listed, but does not block (%s)"), *Touched.Diagnostic), Touched.bOk && !Touched.bHit
		&& Touched.Hits.ContainsByPredicate([](const FHyperAISpatialHit& Hit) { return Hit.Actor == TEXT("Trigger") && !Hit.bBlocking; }));

	FHyperAISpatialQuery Unknown = Down;
	Unknown.Channel = TEXT("NotAChannel");
	const FHyperAISpatialResult Refused = Ask(*World, Unknown);
	TestTrue(TEXT("an unknown channel is refused with the valid ones"), !Refused.bOk && Refused.Diagnostic.Contains(TEXT("Visibility")) && Refused.Diagnostic.Contains(TEXT("WorldDynamic")));
	FHyperAISpatialQuery ByName = Down;
	ByName.Channel = TEXT("world static");
	TestTrue(TEXT("channels are matched by name, spaces and case aside"), Ask(*World, ByName).bHit);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialEditorCollisionTest,
	"HyperAIStudio.SceneSpatial.Query.EditorTraceCollision",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialEditorCollisionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialEditorCollisionWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	// As the level editor's world does: meshes without collision still answer traces, so they can be clicked. The
	// collision is turned off before the mesh registers, as for a mesh placed that way.
	World->bEnableTraceCollision = true;
	FActorSpawnParameters Params;
	AStaticMeshActor* Ghost = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform::Identity, Params);
	if (!TestNotNull(TEXT("a mesh actor"), Ghost)) return false;
	UStaticMeshComponent* Mesh = Ghost->GetStaticMeshComponent();
	Mesh->UnregisterComponent();
	Mesh->SetStaticMesh(Cube());
	// A mesh actor takes its mesh's collision when it registers unless told otherwise, as the editor tells it on a change.
	Mesh->bUseDefaultCollision = false;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->RegisterComponent();
	Ghost->SetActorLabel(TEXT("Ghost"));

	const FVector Start(0.0, 0.0, 500.0);
	const FVector End(0.0, 0.0, -500.0);
	const FCollisionObjectQueryParams WorldStatic(ECC_WorldStatic);
	FHitResult Raw;
	const bool bRawSimple = World->LineTraceSingleByObjectType(Raw, Start, End, WorldStatic) && Raw.GetActor() == Ghost;
	const bool bRawComplex = World->LineTraceSingleByObjectType(Raw, Start, End, WorldStatic, FCollisionQueryParams(SCENE_QUERY_STAT(HyperAISpatialTest), true))
		&& Raw.GetActor() == Ghost;
	// BodyInstance gives such meshes query-only bodies with every channel ignored. Whether an object-type trace still
	// meets them depends on the engine's query filter; the tool drops such hits either way, which is what is held below.
	AddInfo(FString::Printf(TEXT("Engine: an object-type trace %s a mesh without collision in a trace-collision world (simple %d, complex %d)."),
		bRawSimple || bRawComplex ? TEXT("meets") : TEXT("passes through"), bRawSimple, bRawComplex));

	for (const bool bComplex : {false, true})
	{
		FHyperAISpatialQuery Channel = Query(TEXT("trace"));
		Channel.Start = Start;
		Channel.End = End;
		Channel.bTraceComplex = bComplex;
		const FHyperAISpatialResult ByChannel = Ask(*World, Channel);
		TestFalse(*FString::Printf(TEXT("a channel trace passes through it (complex %d: %s)"), bComplex, *ByChannel.Diagnostic), ByChannel.bHit);
		FHyperAISpatialQuery Objects = Channel;
		Objects.ObjectTypes = {TEXT("WorldStatic")};
		const FHyperAISpatialResult ByObject = Ask(*World, Objects);
		TestTrue(*FString::Printf(TEXT("and the object-type hit on it is dropped (complex %d: %s)"), bComplex, *ByObject.Diagnostic), ByObject.bOk && !ByObject.bHit && ByObject.Hits.IsEmpty());
	}
	FHyperAISpatialQuery Channel = Query(TEXT("trace"));
	Channel.Start = Start;
	Channel.End = End;
	FHyperAISpatialQuery TraceType = Channel;
	TraceType.ObjectTypes = {TEXT("Visibility")};
	TestFalse(TEXT("a trace channel is not an object type"), Ask(*World, TraceType).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialClearSpotTest,
	"HyperAIStudio.SceneSpatial.Query.ClearSpotSpiral",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialClearSpotTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialClearSpotWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddFloor(*World);
	// A 2 m block standing on the floor at the origin.
	AddCube(*World, TEXT("Block"), FVector(0.0, 0.0, 100.0), FVector(2.0));

	FHyperAISpatialQuery Spot = Query(TEXT("clear_spot"));
	Spot.Point = FVector::ZeroVector;
	Spot.Extent = FVector(50.0);
	const FHyperAISpatialResult Found = Ask(*World, Spot);
	TestTrue(*FString::Printf(TEXT("a spot is found (%s)"), *Found.Diagnostic), Found.bOk && Found.bHit);
	// Rings are 110 cm apart for a metre box with 10 cm clearance: the first that clears the block is the second.
	const double Step = 110.0;
	TestTrue(*FString::Printf(TEXT("in the nearest ring that fits (%.0f cm away)"), Found.Distance), Found.Distance > Step && Found.Distance < 2.0 * Step + 50.0);
	TestTrue(TEXT("on the floor"), FMath::IsNearlyEqual(Found.Location.Z, 0.0, 0.5));
	FHyperAISpatialQuery Under = Query(TEXT("ground"));
	Under.Point = FVector(0.0, 0.0, 50.0);
	const FHyperAISpatialResult Ground = Ask(*World, Under);
	TestTrue(*FString::Printf(TEXT("ground under a point inside the block is the floor under it (%s)"), *Ground.Diagnostic), Ground.bHit
		&& FMath::IsNearlyEqual(Ground.GroundZ, 0.f, 0.5f) && Ground.Diagnostic.Contains(TEXT("inside")));
	TestTrue(TEXT("and clear of the block"), IsClear(*World, Found.Location + FVector(0.0, 0.0, 52.0), FVector(50.0), nullptr));
	TestTrue(TEXT("after trying more than one spot"), Found.Candidates > 1);
	TestTrue(*FString::Printf(TEXT("and turning blocked ones down (%s)"), *Found.Rejected), Found.Rejected.StartsWith(TEXT("blocked:")) && !Found.Rejected.StartsWith(TEXT("blocked:0")));
	TestTrue(TEXT("the same question finds the same spot"), Ask(*World, Spot).Location.Equals(Found.Location, 0.01));

	FHyperAISpatialQuery Tight = Spot;
	Tight.MaxRadius = 50.f;
	const FHyperAISpatialResult None = Ask(*World, Tight);
	TestTrue(*FString::Printf(TEXT("too small a radius finds nothing and says why (%s)"), *None.Diagnostic), !None.bOk && None.Rejected.StartsWith(TEXT("blocked:1")));
	FHyperAISpatialQuery Bounds = Spot;
	Bounds.Mode = TEXT("bounds");
	const FHyperAISpatialResult ByBounds = Ask(*World, Bounds);
	TestTrue(*FString::Printf(TEXT("bounds mode finds a spot outside the block's bounds (%s)"), *ByBounds.Diagnostic), ByBounds.bOk && ByBounds.Distance > 100.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialMeasureTest,
	"HyperAIStudio.SceneSpatial.Query.MeasureAndNearest",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialMeasureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialMeasureWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddCube(*World, TEXT("A"), FVector::ZeroVector);
	AddCube(*World, TEXT("B"), FVector(1000.0, 0.0, 0.0));
	FHyperAISpatialQuery Between = Query(TEXT("measure"));
	Between.ActorA = TEXT("A");
	Between.ActorB = TEXT("B");
	const FHyperAISpatialResult Measured = Ask(*World, Between);
	TestTrue(*FString::Printf(TEXT("two cubes a metre wide, 10 m apart (%s)"), *Measured.Diagnostic), Measured.bOk
		&& FMath::IsNearlyEqual(Measured.Distance, 1000.f, 0.1f) && FMath::IsNearlyEqual(Measured.Horizontal, 1000.f, 0.1f)
		&& FMath::IsNearlyEqual(Measured.Vertical, 0.f, 0.1f) && FMath::IsNearlyEqual(Measured.Gap, 900.f, 0.5f));
	FHyperAISpatialQuery Points = Query(TEXT("measure"));
	Points.End = FVector(300.0, 400.0, 0.0);
	TestTrue(TEXT("points measure too"), FMath::IsNearlyEqual(Ask(*World, Points).Distance, 500.f, 0.01f));

	AddCube(*World, TEXT("Untagged"), FVector(100.0, 200.0, 0.0));
	const TCHAR* Tagged[] = {TEXT("Third"), TEXT("First"), TEXT("Second")};
	const double Where[] = {900.0, 200.0, 500.0};
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AddCube(*World, Tagged[Index], FVector(Where[Index], -300.0, 0.0))->Tags.Add(TEXT("Pick"));
	}
	FHyperAISpatialQuery Near = Query(TEXT("nearest"));
	Near.TagFilter = TEXT("Pick");
	Near.MaxResults = 2;
	const FHyperAISpatialResult Nearest = Ask(*World, Near);
	TestTrue(TEXT("only tagged actors, nearest first, at most MaxResults"), Nearest.bOk && Nearest.Nearest.Num() == 2
		&& Nearest.Nearest[0].Label == TEXT("First") && Nearest.Nearest[1].Label == TEXT("Second") && Nearest.Nearest[0].Distance <= Nearest.Nearest[1].Distance);
	FHyperAISpatialQuery ByClass = Query(TEXT("nearest"));
	ByClass.ClassFilter = TEXT("StaticMeshActor");
	ByClass.LabelFilter = TEXT("Untag");
	const FHyperAISpatialResult Found = Ask(*World, ByClass);
	TestTrue(TEXT("by class and label text"), Found.bOk && Found.Nearest.Num() == 1 && Found.Nearest[0].Label == TEXT("Untagged"));
	FHyperAISpatialQuery NoClass = Query(TEXT("nearest"));
	NoClass.ClassFilter = TEXT("NotAnActorClassAtAll");
	TestFalse(TEXT("an unknown class is refused"), Ask(*World, NoClass).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialDropTest,
	"HyperAIStudio.SceneSpatial.Plan.DryRunEqualsApply",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialDropTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialDropWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	// A 20 m slab tilted 20 degrees, and a crate turned 30 degrees floating 8 m above it.
	AddCube(*World, TEXT("Ramp"), FVector::ZeroVector, FVector(20.0, 20.0, 0.2), FRotator(20.0, 0.0, 0.0));
	AStaticMeshActor* Crate = AddCube(*World, TEXT("Crate"), FVector(0.0, 0.0, 800.0), FVector::OneVector, FRotator(0.0, 30.0, 0.0));
	const FTransform Before = Crate->GetActorTransform();
	const FVector RampUp = FRotator(20.0, 0.0, 0.0).RotateVector(FVector::UpVector);

	FHyperAISpatialOp Drop = Op(TEXT("drop_to_ground"), {TEXT("Crate")});
	Drop.bAlignToNormal = true;
	Drop.MaxAlignDegrees = 30.f;
	FHyperAISpatialPlanRequest Request = PlanOf({Drop});
	Request.bDryRun = true;
	TArray<Plan::FMove> Moves;
	FHyperAISpatialPlanReport Checked;
	if (!TestTrue(*FString::Printf(TEXT("the drop checks (%s)"), *Checked.Diagnostic), Plan::Check(*World, Request, Moves, Checked) && Moves.Num() == 1))
	{
		return false;
	}
	const FTransform Planned = Moves[0].To;
	const double Tilt = FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(Planned.GetRotation().GetUpVector(), RampUp)));
	TestTrue(*FString::Printf(TEXT("turned onto the ramp (%.2f degrees off)"), Tilt), Tilt < 0.5);
	TestTrue(TEXT("keeping its yaw about the new up"), FMath::Abs(Planned.Rotator().Pitch) > 15.0 || FMath::Abs(Planned.Rotator().Roll) > 15.0);
	TestTrue(*FString::Printf(TEXT("and the dry run says so: %s"), Checked.Moves.IsEmpty() ? TEXT("") : *Checked.Moves[0]), Checked.Moves.Num() == 1 && Checked.Moves[0].StartsWith(TEXT("Crate: ")));
	TestTrue(TEXT("a dry run moves nothing"), Crate->GetActorTransform().Equals(Before, 0.01));

	Request.bDryRun = false;
	const FHyperAISpatialPlanReport Applied = Plan::Execute(*World, Request, Moves, TEXT("spatial-test-drop"));
	TestTrue(*FString::Printf(TEXT("the drop applies (%s)"), *Applied.Diagnostic), Applied.bOk && Applied.Status == TEXT("completed") && Applied.Applied == 1);
	const FTransform After = Crate->GetActorTransform();
	TestTrue(TEXT("where the dry run said, within 1 cm"), After.GetLocation().Equals(Planned.GetLocation(), 1.0));
	TestTrue(TEXT("turned as it said, within half a degree"), FMath::RadiansToDegrees(After.GetRotation().AngularDistance(Planned.GetRotation())) <= 0.5);
	// The crate's bottom face lies on the ramp: a trace along the ramp's normal from its middle meets the ramp at once.
	const FVector Bottom = After.TransformPosition(FVector(0.0, 0.0, -50.0));
	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAISpatialTest), false, Crate);
	const bool bOnRamp = World->LineTraceSingleByChannel(Hit, Bottom + RampUp * 50.0, Bottom - RampUp * 50.0, ECC_Visibility, Params);
	TestTrue(*FString::Printf(TEXT("its bottom rests on the ramp (%.2f cm off)"), bOnRamp ? Hit.Distance - 50.0 : -1.0), bOnRamp && FMath::Abs(Hit.Distance - 50.0) < 2.0);

	// Every editor undo checks graph pins, which a package still loading in the background can leave unresolved.
	FlushAsyncLoading();
	if (TestTrue(TEXT("undo is available"), GEditor && GEditor->UndoTransaction()))
	{
		TestTrue(TEXT("one undo puts the crate back"), Crate->GetActorTransform().Equals(Before, 0.01));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialRefusalsTest,
	"HyperAIStudio.SceneSpatial.Plan.Refusals",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialRefusalsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialRefusalsWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddFloor(*World);
	AddCube(*World, TEXT("Locked"), FVector(500.0, 0.0, 300.0))->SetLockLocation(true);
	AStaticMeshActor* TwinA = AddCube(*World, TEXT("Twin"), FVector(-500.0, 0.0, 300.0));
	AStaticMeshActor* TwinB = AddCube(*World, TEXT("Twin"), FVector(-500.0, 500.0, 300.0));
	AddCube(*World, TEXT("Void"), FVector(100000.0, 100000.0, 300.0));
	AStaticMeshActor* Parent = AddCube(*World, TEXT("Table"), FVector(0.0, 1000.0, 300.0));
	AStaticMeshActor* Child = AddCube(*World, TEXT("Lamp"), FVector(0.0, 1000.0, 450.0));
	Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
	AStaticMeshActor* Lower = AddCube(*World, TEXT("Lower"), FVector(0.0, -1000.0, 300.0));
	AStaticMeshActor* Upper = AddCube(*World, TEXT("Upper"), FVector(0.0, -1000.0, 500.0));

	auto Refusal = [&](const FHyperAISpatialOp& Operation)
	{
		TArray<Plan::FMove> Moves;
		FHyperAISpatialPlanReport Report;
		Report.bOk = Plan::Check(*World, PlanOf({Operation}), Moves, Report);
		return Report;
	};
	const FHyperAISpatialPlanReport Locked = Refusal(Op(TEXT("drop_to_ground"), {TEXT("Locked")}));
	TestTrue(*FString::Printf(TEXT("a locked actor is refused by name (%s)"), *Locked.Diagnostic), !Locked.bOk && Locked.Diagnostic.Contains(TEXT("Locked")) && Locked.Diagnostic.Contains(TEXT("Lock Actor Movement")));
	const FHyperAISpatialPlanReport Twins = Refusal(Op(TEXT("drop_to_ground"), {TEXT("Twin")}));
	TestTrue(*FString::Printf(TEXT("an ambiguous label names both actors (%s)"), *Twins.Diagnostic), !Twins.bOk
		&& Twins.Diagnostic.Contains(TwinA->GetPathName()) && Twins.Diagnostic.Contains(TwinB->GetPathName()));
	const FHyperAISpatialPlanReport ByPath = Refusal(Op(TEXT("drop_to_ground"), {TwinA->GetPathName()}));
	TestTrue(*FString::Printf(TEXT("and its path picks one (%s)"), *ByPath.Diagnostic), ByPath.bOk && ByPath.Moves.Num() == 1);
	const FHyperAISpatialPlanReport Void = Refusal(Op(TEXT("drop_to_ground"), {TEXT("Void")}));
	TestTrue(*FString::Printf(TEXT("an actor over nothing has no ground (%s)"), *Void.Diagnostic), Void.Status == TEXT("no_ground") && Void.Diagnostic.Contains(TEXT("Void")));
	const FHyperAISpatialPlanReport Attached = Refusal(Op(TEXT("drop_to_ground"), {TEXT("Lamp")}));
	TestTrue(*FString::Printf(TEXT("an attached actor points at its parent (%s)"), *Attached.Diagnostic), !Attached.bOk && Attached.Diagnostic.Contains(TEXT("Table")));
	const FHyperAISpatialPlanReport Unknown = Refusal(Op(TEXT("drop_to_ground"), {TEXT("Nobody")}));
	TestTrue(TEXT("an unknown actor is refused by name"), Unknown.Status == TEXT("invalid_request") && Unknown.Diagnostic.Contains(TEXT("Nobody")));
	TestEqual(TEXT("an unknown kind is refused"), Refusal(Op(TEXT("stack"), {TEXT("Lower")})).Status, FString(TEXT("invalid_request")));
	FHyperAISpatialOp Crowd = Op(TEXT("drop_to_ground"));
	Crowd.Actors.Init(TEXT("Lower"), FHyperAIStudioSceneSpatialContracts::MaxPlanActors + 1);
	const FHyperAISpatialPlanReport TooMany = Refusal(Crowd);
	TestTrue(*FString::Printf(TEXT("more names than a plan moves are refused before any is looked up (%s)"), *TooMany.Diagnostic),
		TooMany.Status == TEXT("invalid_request") && TooMany.Diagnostic.Contains(TEXT("at most 256")));
	FHyperAISpatialOp Ignoring = Op(TEXT("drop_to_ground"), {TEXT("Lower")});
	Ignoring.IgnoreActors.Init(TEXT("Upper"), FHyperAIStudioSceneSpatialContracts::MaxScopeActors + 1);
	TestTrue(TEXT("so are more ignored actors than the limit"), Refusal(Ignoring).Diagnostic.Contains(TEXT("IgnoreActors")));

	// Stacked crates: each passes through the other, so both land on the floor rather than one on the other.
	TArray<Plan::FMove> Moves;
	FHyperAISpatialPlanReport Stacked;
	const FHyperAISpatialPlanRequest Both = PlanOf({Op(TEXT("drop_to_ground"), {TEXT("Lower"), TEXT("Upper")})});
	if (TestTrue(*FString::Printf(TEXT("the stack checks (%s)"), *Stacked.Diagnostic), Plan::Check(*World, Both, Moves, Stacked)))
	{
		TestTrue(TEXT("and applies"), Plan::Execute(*World, Both, Moves, TEXT("spatial-test-stack")).bOk);
		TestTrue(*FString::Printf(TEXT("the lower crate stands on the floor (%.1f)"), Lower->GetActorLocation().Z), FMath::IsNearlyEqual(Lower->GetActorLocation().Z, 50.0, 0.5));
		TestTrue(*FString::Printf(TEXT("and so does the upper (%.1f)"), Upper->GetActorLocation().Z), FMath::IsNearlyEqual(Upper->GetActorLocation().Z, 50.0, 0.5));
	}

	// Play In Editor running: the front door refuses before it looks at the plan.
	if (GEditor && !GEditor->PlayWorld)
	{
		GEditor->PlayWorld = World;
		const FHyperAISpatialPlanReport Playing = FHyperAIStudioSceneSpatialContracts::ApplyPlan(PlanOf({Op(TEXT("drop_to_ground"), {TEXT("Lower")})}));
		GEditor->PlayWorld = nullptr;
		TestEqual(TEXT("a running PIE session refuses the plan"), Playing.Status, FString(TEXT("play_in_editor_running")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialClearMoveTest,
	"HyperAIStudio.SceneSpatial.Plan.MoveToClearSpot",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialClearMoveTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialClearMoveWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddFloor(*World);
	AddCube(*World, TEXT("Wall"), FVector(0.0, 0.0, 150.0), FVector(1.0, 10.0, 3.0));
	AStaticMeshActor* Crate = AddCube(*World, TEXT("Crate"), FVector(30.0, 0.0, 50.0));
	AStaticMeshActor* Barrel = AddCube(*World, TEXT("Barrel"), FVector(-30.0, 100.0, 50.0));
	const FVector CrateBefore = Crate->GetActorLocation();
	TestFalse(TEXT("the crate starts inside the wall"), IsClear(*World, CrateBefore, FVector(45.0), Crate));

	const FHyperAISpatialPlanRequest Request = PlanOf({Op(TEXT("move_to_clear_spot"), {TEXT("Crate"), TEXT("Barrel")})});
	TArray<Plan::FMove> Moves;
	FHyperAISpatialPlanReport Checked;
	if (!TestTrue(*FString::Printf(TEXT("the move checks (%s)"), *Checked.Diagnostic), Plan::Check(*World, Request, Moves, Checked)))
	{
		return false;
	}
	const FHyperAISpatialPlanReport Applied = Plan::Execute(*World, Request, Moves, TEXT("spatial-test-clear"));
	TestTrue(*FString::Printf(TEXT("and applies (%s)"), *Applied.Diagnostic), Applied.bOk && Applied.Applied == 2);
	for (AStaticMeshActor* Moved : {Crate, Barrel})
	{
		const FVector Location = Moved->GetActorLocation();
		TestTrue(*FString::Printf(TEXT("%s is clear of the wall at %s"), *Moved->GetActorLabel(), *Location.ToString()), IsClear(*World, Location, FVector(45.0), Moved));
		TestTrue(*FString::Printf(TEXT("%s stands on the floor (%.1f)"), *Moved->GetActorLabel(), Location.Z), FMath::IsNearlyEqual(Location.Z, 50.0, 3.0));
	}
	TestTrue(TEXT("the two did not take the same spot"), FVector::Dist(Crate->GetActorLocation(), Barrel->GetActorLocation()) >= 100.0);
	FlushAsyncLoading();
	if (TestTrue(TEXT("undo is available"), GEditor && GEditor->UndoTransaction()))
	{
		TestTrue(TEXT("one undo puts both back"), Crate->GetActorLocation().Equals(CrateBefore, 0.01) && Barrel->GetActorLocation().Equals(FVector(-30.0, 100.0, 50.0), 0.01));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialPlaceTest,
	"HyperAIStudio.SceneSpatial.Plan.PlaceMesh",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialPlaceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialPlaceWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddFloor(*World);
	FHyperAISpatialOp Place = Op(TEXT("place"));
	Place.Mesh = TEXT("/Engine/BasicShapes/Cube");
	Place.Label = TEXT("Placed");
	Place.Folder = TEXT("HyperAI/Test");
	Place.Location = FVector(300.0, 0.0, 900.0);
	Place.Rotation = FRotator(0.0, 45.0, 0.0);
	Place.Scale = FVector(0.5);
	Place.bDropToGround = true;
	const FHyperAISpatialPlanRequest Request = PlanOf({Place});
	TArray<Plan::FMove> Moves;
	FHyperAISpatialPlanReport Checked;
	if (!TestTrue(*FString::Printf(TEXT("the placement checks (%s)"), *Checked.Diagnostic), Plan::Check(*World, Request, Moves, Checked) && Moves.Num() == 1))
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("the dry run names the mesh: %s"), *Checked.Moves[0]), Checked.Moves[0].Contains(TEXT("(new, /Engine/BasicShapes/Cube.Cube)")));
	TestTrue(*FString::Printf(TEXT("dropped onto the floor (%.1f)"), Moves[0].To.GetLocation().Z), FMath::IsNearlyEqual(Moves[0].To.GetLocation().Z, 25.0, 0.5));
	TestNull(TEXT("nothing is placed by a check"), Labelled(*World, TEXT("Placed")));

	const FHyperAISpatialPlanReport Applied = Plan::Execute(*World, Request, Moves, TEXT("spatial-test-place"));
	TestTrue(*FString::Printf(TEXT("the placement applies (%s)"), *Applied.Diagnostic), Applied.bOk);
	AStaticMeshActor* Placed = Cast<AStaticMeshActor>(Labelled(*World, TEXT("Placed")));
	if (TestNotNull(TEXT("the new actor is labelled"), Placed))
	{
		TestTrue(TEXT("where it was checked"), Placed->GetActorLocation().Equals(Moves[0].To.GetLocation(), 0.5));
		TestEqual(TEXT("in its folder"), Placed->GetFolderPath(), FName(TEXT("HyperAI/Test")));
		TestTrue(TEXT("showing the mesh, scaled"), Placed->GetStaticMeshComponent()->GetStaticMesh() == Cube() && Placed->GetActorScale3D().Equals(FVector(0.5)));
	}
	TArray<Plan::FMove> Again;
	FHyperAISpatialPlanReport Taken;
	TestFalse(TEXT("a label already taken is refused"), Plan::Check(*World, Request, Again, Taken));
	TestTrue(TEXT("and says so"), Taken.Diagnostic.Contains(TEXT("already labelled")));
	FHyperAISpatialOp NoMesh = Place;
	NoMesh.Label = TEXT("Other");
	NoMesh.Mesh = TEXT("/Game/Does/Not/Exist");
	FHyperAISpatialPlanReport Missing;
	TestFalse(TEXT("a missing mesh is refused"), Plan::Check(*World, PlanOf({NoMesh}), Again, Missing));
	TestTrue(TEXT("by its path"), Missing.Diagnostic.Contains(TEXT("/Game/Does/Not/Exist")));

	FlushAsyncLoading();
	if (TestTrue(TEXT("undo is available"), GEditor && GEditor->UndoTransaction()))
	{
		TestNull(TEXT("one undo takes the placement back"), Labelled(*World, TEXT("Placed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSceneSpatialFrontDoorTest,
	"HyperAIStudio.SceneSpatial.Contracts.FrontDoors",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSceneSpatialFrontDoorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	using C = FHyperAIStudioSceneSpatialContracts;
	FHyperAISceneCostRequest Pie;
	Pie.World = TEXT("pie");
	if (!GEditor || !GEditor->PlayWorld)
	{
		TestEqual(TEXT("no PIE world to ask about"), C::CostReport(Pie).Status, FString(TEXT("no_pie_world")));
	}
	FHyperAISceneCostRequest Moon;
	Moon.World = TEXT("moon");
	TestEqual(TEXT("an unknown world is refused"), C::CostReport(Moon).Status, FString(TEXT("invalid_request")));
	FHyperAISpatialQueryRequest Empty;
	TestEqual(TEXT("no questions is refused"), C::Query(Empty).Status, FString(TEXT("invalid_request")));
	FHyperAISpatialQueryRequest Many;
	Many.Queries.Init(Query(TEXT("ground")), C::MaxQueries + 1);
	TestEqual(TEXT("more than 32 questions are refused"), C::Query(Many).Status, FString(TEXT("invalid_request")));

	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		AddInfo(TEXT("No editor level is open; the rest needs one."));
		return true;
	}
	FHyperAISceneCostRequest Wide;
	Wide.TopN = 500;
	const FHyperAISceneCostReport Fitted = C::CostReport(Wide);
	TestTrue(*FString::Printf(TEXT("the editor level is measured, TopN fitted (%s)"), *Fitted.Diagnostic), Fitted.bOk && Fitted.World == TEXT("editor")
		&& Fitted.Diagnostic.Contains(TEXT("top_n 500 -> 50")));
	FHyperAISpatialQueryRequest Ground;
	Ground.Queries.Add(Query(TEXT("ground")));
	Ground.Queries.Add(Query(TEXT("teleport")));
	const FHyperAISpatialQueryReport Asked = C::Query(Ground);
	TestTrue(TEXT("every question gets an answer, a bad one its own refusal"), Asked.bOk && Asked.Results.Num() == 2 && Asked.Results[0].bOk && !Asked.Results[1].bOk);
	FHyperAISpatialPlanRequest Bad = PlanOf({Op(TEXT("drop_to_ground"), {TEXT("HyperAI_NoSuchActor_7f3")})});
	Bad.bDryRun = true;
	const FHyperAISpatialPlanReport Refused = C::ApplyPlan(Bad);
	TestTrue(TEXT("a plan naming no actor is refused, with an operation id"), Refused.Status == TEXT("invalid_request") && Refused.OperationId.StartsWith(TEXT("spatial-")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialFoliageGroundTest,
	"HyperAIStudio.SceneSpatial.Plan.DropThroughFoliage",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialFoliageGroundTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialFoliageGroundWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddFloor(*World);
	// A foliage canopy over one spot and a PCG-generated one over another: a cube instance 3 m up, its top at 350 cm.
	AddInstanced(*World, TEXT("Canopy"), 0, AInstancedFoliageActor::StaticClass())->AddInstance(FTransform(FVector(0.0, 1000.0, 300.0)));
	AddInstanced(*World, TEXT("Generated"), 0, AActor::StaticClass(), Spatial::PCGGeneratedTag())->AddInstance(FTransform(FVector(0.0, -1000.0, 300.0)));
	for (const double Y : {1000.0, -1000.0})
	{
		FHyperAISpatialQuery Down = Query(TEXT("trace"));
		Down.Start = FVector(0.0, Y, 1000.0);
		Down.End = FVector(0.0, Y, -500.0);
		const FHyperAISpatialResult Blocked = Ask(*World, Down);
		TestTrue(*FString::Printf(TEXT("the instances at Y %.0f stop a plain trace (%s)"), Y, *Blocked.Diagnostic), Blocked.bHit && FMath::IsNearlyEqual(Blocked.Location.Z, 350.0, 1.0));
		Down.bIgnoreFoliage = true;
		const FHyperAISpatialResult Through = Ask(*World, Down);
		TestTrue(*FString::Printf(TEXT("and bIgnoreFoliage passes through them to the floor (%s)"), *Through.Diagnostic), Through.bHit && FMath::IsNearlyEqual(Through.Location.Z, 0.0, 1.0));
	}

	AddCube(*World, TEXT("UnderTree"), FVector(0.0, 1000.0, 800.0));
	AddCube(*World, TEXT("UnderPCG"), FVector(0.0, -1000.0, 800.0));
	TArray<Plan::FMove> Moves;
	FHyperAISpatialPlanReport Checked;
	if (!TestTrue(TEXT("the drops check"), Plan::Check(*World, PlanOf({Op(TEXT("drop_to_ground"), {TEXT("UnderTree"), TEXT("UnderPCG")})}), Moves, Checked) && Moves.Num() == 2))
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("a crate under foliage lands on the floor (%.1f)"), Moves[0].To.GetLocation().Z), FMath::IsNearlyEqual(Moves[0].To.GetLocation().Z, 50.0, 0.5));
	TestTrue(*FString::Printf(TEXT("and one under PCG instances too (%.1f)"), Moves[1].To.GetLocation().Z), FMath::IsNearlyEqual(Moves[1].To.GetLocation().Z, 50.0, 0.5));
	FHyperAISpatialOp Onto = Op(TEXT("drop_to_ground"), {TEXT("UnderTree")});
	Onto.bIncludeFoliage = true;
	TestTrue(TEXT("bIncludeFoliage makes the canopy ground"), Plan::Check(*World, PlanOf({Onto}), Moves, Checked) && Moves.Num() == 1
		&& FMath::IsNearlyEqual(Moves[0].To.GetLocation().Z, 400.0, 0.5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialTriggerTest,
	"HyperAIStudio.SceneSpatial.Plan.TriggerShapesDoNotLift",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialTriggerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialTriggerWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddFloor(*World);
	// A pickup: a cube with a 2 m overlap sphere around it, as a pickup's trigger is.
	AStaticMeshActor* Pickup = AddCube(*World, TEXT("Pickup"), FVector(0.0, 0.0, 500.0));
	USphereComponent* Trigger = NewObject<USphereComponent>(Pickup, TEXT("PickupTrigger"));
	Trigger->InitSphereRadius(200.f);
	Trigger->SetCollisionProfileName(UCollisionProfile::OverlapAll_ProfileName);
	Trigger->SetupAttachment(Pickup->GetRootComponent());
	Pickup->AddInstanceComponent(Trigger);
	Trigger->RegisterComponent();
	TestTrue(TEXT("the trigger reaches below the cube"), Pickup->GetComponentsBoundingBox(/*bNonColliding=*/true).Min.Z < 400.0);
	TestTrue(TEXT("but what the pickup looks like is the cube"), FMath::IsNearlyEqual(Spatial::WorldBounds(*Pickup).Min.Z, 450.0, 0.5));
	// A guard whose body is a blocking capsule, as a character's is: the capsule is what it stands on.
	AStaticMeshActor* Guard = AddCube(*World, TEXT("Guard"), FVector(500.0, 0.0, 500.0));
	UCapsuleComponent* Body = NewObject<UCapsuleComponent>(Guard, TEXT("GuardBody"));
	Body->InitCapsuleSize(40.f, 150.f);
	Body->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
	Body->SetupAttachment(Guard->GetRootComponent());
	Guard->AddInstanceComponent(Body);
	Body->RegisterComponent();

	TArray<Plan::FMove> Moves;
	FHyperAISpatialPlanReport Checked;
	if (!TestTrue(TEXT("the drops check"), Plan::Check(*World, PlanOf({Op(TEXT("drop_to_ground"), {TEXT("Pickup"), TEXT("Guard")})}), Moves, Checked) && Moves.Num() == 2))
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("the cube rests on the floor, not the sphere (%.1f)"), Moves[0].To.GetLocation().Z), FMath::IsNearlyEqual(Moves[0].To.GetLocation().Z, 50.0, 0.5));
	TestTrue(*FString::Printf(TEXT("the guard stands on its capsule (%.1f)"), Moves[1].To.GetLocation().Z), FMath::IsNearlyEqual(Moves[1].To.GetLocation().Z, 150.0, 0.5));
	ATriggerBox* Box = World->SpawnActor<ATriggerBox>(FVector(1000.0, 0.0, 300.0), FRotator::ZeroRotator);
	TestTrue(TEXT("an actor that is only a trigger keeps its trigger's bounds"), Box && Spatial::WorldBounds(*Box).IsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialClaimTest,
	"HyperAIStudio.SceneSpatial.Plan.ClearSpotsKeepOffPlacements",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialClaimTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialClaimWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	AddFloor(*World);
	AddCube(*World, TEXT("Wall"), FVector(0.0, 0.0, 150.0), FVector(1.0, 10.0, 3.0));
	AStaticMeshActor* Barrel = AddCube(*World, TEXT("Barrel"), FVector(30.0, 0.0, 50.0));
	const FHyperAISpatialOp Move = Op(TEXT("move_to_clear_spot"), {TEXT("Barrel")});
	TArray<Plan::FMove> Moves;
	FHyperAISpatialPlanReport Checked;
	if (!TestTrue(TEXT("the barrel alone finds a spot"), Plan::Check(*World, PlanOf({Move}), Moves, Checked) && Moves.Num() == 1))
	{
		return false;
	}
	const FVector Alone = Moves[0].To.GetLocation();

	// The same plan places a crate there, listed after the move: the barrel keeps off where the crate lands.
	FHyperAISpatialOp Place = Op(TEXT("place"));
	Place.Mesh = TEXT("/Engine/BasicShapes/Cube");
	Place.Label = TEXT("NewCrate");
	Place.Location = FVector(Alone.X, Alone.Y, 600.0);
	Place.bDropToGround = true;
	const FHyperAISpatialPlanRequest Both = PlanOf({Move, Place});
	if (!TestTrue(*FString::Printf(TEXT("the plan checks (%s)"), *Checked.Diagnostic), Plan::Check(*World, Both, Moves, Checked) && Moves.Num() == 2))
	{
		return false;
	}
	TestTrue(TEXT("moves are reported in the plan's order"), Moves[0].Name == TEXT("Barrel") && Moves[1].Name == TEXT("NewCrate"));
	const FVector BarrelAt = Moves[0].To.GetLocation();
	const FVector CrateAt = Moves[1].To.GetLocation();
	TestTrue(*FString::Printf(TEXT("the crate lands where the barrel would have gone (%s)"), *CrateAt.ToString()), CrateAt.Equals(FVector(Alone.X, Alone.Y, 50.0), 1.0));
	TestTrue(*FString::Printf(TEXT("so the barrel goes elsewhere (%s)"), *BarrelAt.ToString()), FMath::Abs(BarrelAt.X - CrateAt.X) >= 100.0 || FMath::Abs(BarrelAt.Y - CrateAt.Y) >= 100.0);
	const FHyperAISpatialPlanReport Applied = Plan::Execute(*World, Both, Moves, TEXT("spatial-test-claim"));
	TestTrue(*FString::Printf(TEXT("and the plan applies (%s)"), *Applied.Diagnostic), Applied.bOk && Applied.Applied == 2);
	TestTrue(TEXT("leaving the barrel clear of the wall and the crate"), IsClear(*World, Barrel->GetActorLocation(), FVector(45.0), Barrel));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialApprovalTest,
	"HyperAIStudio.SceneSpatial.Plan.WaitsForApproval",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialApprovalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	using C = FHyperAIStudioSceneSpatialContracts;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || GEditor->PlayWorld)
	{
		AddInfo(TEXT("No editor level is open, or Play In Editor runs; skipped."));
		return true;
	}
	// Agent edits wait for the user, as they do by default.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bWasRequired = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = true;
	TArray<FString> Asked;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bWasRequired;
		for (const FString& OperationId : Asked)
		{
			if (FHyperAIStudioApprovalGate::HasPending(OperationId)) FHyperAIStudioApprovalGate::Reject(OperationId, TEXT("Test finished."));
		}
	};
	auto CountLabelled = [World](const FString& Label)
	{
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			Count += IsValid(*It) && It->GetActorLabel() == Label ? 1 : 0;
		}
		return Count;
	};
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	FHyperAISpatialOp Place = Op(TEXT("place"));
	Place.Mesh = TEXT("/Engine/BasicShapes/Cube");
	Place.Label = TEXT("HyperAI_SpatialApproval_") + Suffix;
	Place.Location = FVector(0.0, 0.0, -100000.0);
	FHyperAISpatialPlanRequest Request = PlanOf({Place});
	Request.OperationId = TEXT("spatial-test-approve-") + Suffix;
	const FHyperAISpatialPlanReport Waiting = C::ApplyPlan(Request);
	Asked.Add(Waiting.OperationId);
	TestEqual(TEXT("with approval required, the plan waits"), Waiting.Status, FString(TEXT("awaiting_user_approval")));
	TestTrue(TEXT("saying what it will do"), Waiting.Moves.Num() == 1 && Waiting.Moves[0].StartsWith(Place.Label));
	TestEqual(TEXT("and nothing is placed before approval"), CountLabelled(Place.Label), 0);
	FString Error;
	const bool bApproved = FHyperAIStudioApprovalGate::Approve(Waiting.OperationId, Error);
	TestTrue(FString::Printf(TEXT("approved (%s)"), *Error), bApproved);
	TestEqual(TEXT("approval places it"), CountLabelled(Place.Label), 1);
	if (AActor* Placed = Labelled(*World, *Place.Label)) World->EditorDestroyActor(Placed, /*bShouldModifyLevel=*/false);

	// The label is taken while the plan waits: approved, the plan checks again and changes nothing.
	Place.Label += TEXT("_Taken");
	Request = PlanOf({Place});
	Request.OperationId = TEXT("spatial-test-stale-") + Suffix;
	const FHyperAISpatialPlanReport Stale = C::ApplyPlan(Request);
	Asked.Add(Stale.OperationId);
	TestEqual(TEXT("a second plan waits too"), Stale.Status, FString(TEXT("awaiting_user_approval")));
	AStaticMeshActor* Squatter = AddCube(*World, *Place.Label, FVector(0.0, 0.0, -100000.0));
	const bool bApprovedStale = FHyperAIStudioApprovalGate::Approve(Stale.OperationId, Error);
	TestTrue(FString::Printf(TEXT("approved (%s)"), *Error), bApprovedStale);
	TestEqual(TEXT("and places nothing over the label taken meanwhile"), CountLabelled(Place.Label), 1);
	World->EditorDestroyActor(Squatter, /*bShouldModifyLevel=*/false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialRecheckTest,
	"HyperAIStudio.SceneSpatial.Plan.RecheckOnlyAfterApproval",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialRecheckTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAISpatialRecheckWorld"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };
	ON_SCOPE_EXIT { Plan::SetClearSpotBudgetForTests({}); };
	AddFloor(*World);
	AddCube(*World, TEXT("Wall"), FVector(0.0, 0.0, 150.0), FVector(1.0, 10.0, 3.0));
	AStaticMeshActor* Crate = AddCube(*World, TEXT("Crate"), FVector(30.0, 0.0, 50.0));
	const FVector Before = Crate->GetActorLocation();
	const FHyperAISpatialPlanRequest Request = PlanOf({Op(TEXT("move_to_clear_spot"), {TEXT("Crate")})});
	TArray<Plan::FMove> Moves;
	FHyperAISpatialPlanReport Checked;
	if (!TestTrue(*FString::Printf(TEXT("the move checks (%s)"), *Checked.Diagnostic), Plan::Check(*World, Request, Moves, Checked) && Moves.Num() == 1))
	{
		return false;
	}

	// Approved later, the plan is computed again; a search out of time then is not taken for a level that changed.
	Plan::SetClearSpotBudgetForTests(-1.0);
	int32 Checks = Plan::GetCheckCountForTests();
	const FHyperAISpatialPlanReport Late = Plan::Execute(*World, Request, Moves, TEXT("spatial-test-late"));
	TestEqual(TEXT("an approved plan is computed again, once"), Plan::GetCheckCountForTests() - Checks, 1);
	TestTrue(*FString::Printf(TEXT("running out of search time is said as such (%s)"), *Late.Diagnostic), !Late.bOk && Late.Status == TEXT("operation_failed")
		&& Late.Diagnostic.Contains(TEXT("ran out of time")) && Late.Diagnostic.Contains(TEXT("approval again")) && !Late.Diagnostic.Contains(TEXT("level changed")));
	TestTrue(TEXT("and nothing moved"), Crate->GetActorLocation().Equals(Before, 0.01));
	Plan::SetClearSpotBudgetForTests({});

	// Checked in the same call, the plan is applied as checked: the search does not run a second time.
	Checks = Plan::GetCheckCountForTests();
	const FHyperAISpatialPlanReport Applied = Plan::Execute(*World, Request, Moves, TEXT("spatial-test-direct"), /*bRecheck=*/false);
	TestTrue(*FString::Printf(TEXT("applied as checked (%s)"), *Applied.Diagnostic), Applied.bOk && Applied.Applied == 1);
	TestEqual(TEXT("without computing the plan again"), Plan::GetCheckCountForTests() - Checks, 0);
	TestTrue(*FString::Printf(TEXT("the crate went where the check said (%s)"), *Crate->GetActorLocation().ToString()), Crate->GetActorLocation().Equals(Moves[0].To.GetLocation(), 0.01));
	TestTrue(TEXT("clear of the wall"), IsClear(*World, Crate->GetActorLocation(), FVector(45.0), Crate));
	FlushAsyncLoading();
	if (TestTrue(TEXT("undo is available"), GEditor && GEditor->UndoTransaction()))
	{
		TestTrue(TEXT("one undo puts the crate back"), Crate->GetActorLocation().Equals(Before, 0.01));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSpatialDirectApplyTest,
	"HyperAIStudio.SceneSpatial.Plan.DirectApplySearchesOnce",
	HyperAIStudio::SceneSpatial::Tests::Flags)

bool FHyperAIStudioSpatialDirectApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::SceneSpatial;
	using namespace HyperAIStudio::SceneSpatial::Tests;
	using C = FHyperAIStudioSceneSpatialContracts;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || GEditor->PlayWorld)
	{
		AddInfo(TEXT("No editor level is open, or Play In Editor runs; skipped."));
		return true;
	}
	// Agent edits apply at once, as they do with approval turned off.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bWasRequired = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	TArray<AActor*> Made;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bWasRequired;
		for (AActor* Actor : Made)
		{
			if (IsValid(Actor)) World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
		}
	};
	// Far below the level, so nothing of it is in the way: a floor, a wall, and a crate stuck in the wall.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FVector Base(0.0, 0.0, -100000.0);
	Made.Add(AddCube(*World, *(TEXT("HyperAI_SpatialDirectFloor_") + Suffix), Base + FVector(0.0, 0.0, -50.0), FVector(40.0, 40.0, 1.0)));
	Made.Add(AddCube(*World, *(TEXT("HyperAI_SpatialDirectWall_") + Suffix), Base + FVector(0.0, 0.0, 150.0), FVector(1.0, 10.0, 3.0)));
	AStaticMeshActor* Crate = AddCube(*World, *(TEXT("HyperAI_SpatialDirectCrate_") + Suffix), Base + FVector(30.0, 0.0, 50.0));
	Made.Add(Crate);

	const int32 Checks = Plan::GetCheckCountForTests();
	const FHyperAISpatialPlanReport Applied = C::ApplyPlan(PlanOf({Op(TEXT("move_to_clear_spot"), {Crate->GetActorLabel()})}));
	TestTrue(*FString::Printf(TEXT("the plan applies at once (%s)"), *Applied.Diagnostic), Applied.bOk && Applied.Status == TEXT("completed") && Applied.Applied == 1);
	TestEqual(TEXT("computed once, so the clear-spot search ran once"), Plan::GetCheckCountForTests() - Checks, 1);
	TestTrue(*FString::Printf(TEXT("the crate is clear of the wall at %s"), *Crate->GetActorLocation().ToString()), IsClear(*World, Crate->GetActorLocation(), FVector(45.0), Crate));
	return true;
}

#endif
