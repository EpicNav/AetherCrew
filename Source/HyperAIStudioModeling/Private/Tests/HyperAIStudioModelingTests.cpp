// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioModelingGate.h"
#include "HyperAIStudioModelingToolset.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSourceControl.h"
#include "Interfaces/IPluginManager.h"
#include "JsonObjectConverter.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshCompiler.h"
#include "UDynamicMesh.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"

namespace HyperAIStudio::ModelingTests
{
	using C = FHyperAIStudioModelingContracts;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FHyperAIModelOp Op(const TCHAR* Kind, const TCHAR* Mesh, const TCHAR* Params = TEXT(""))
	{
		FHyperAIModelOp Out;
		Out.Kind = Kind;
		Out.Mesh = Mesh;
		Out.Params = Params;
		return Out;
	}

	FHyperAIModelOp Sized(const TCHAR* Kind, const TCHAR* Mesh, const TCHAR* Size, const TCHAR* Params = TEXT(""))
	{
		FHyperAIModelOp Out = Op(Kind, Mesh, Params);
		Out.Size = Size;
		return Out;
	}

	FHyperAIModelOp Write(const TCHAR* Mesh, const FString& Asset, const TCHAR* Params = TEXT(""))
	{
		FHyperAIModelOp Out = Op(TEXT("write_asset"), Mesh, Params);
		Out.Asset = Asset;
		return Out;
	}

	/** Runs a plan with approval off, as an agent with one-shot edits allowed would. */
	FHyperAIModelPlanReport Run(TArray<FHyperAIModelOp> Ops, const bool bDryRun)
	{
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bApproval = Settings->bRequireApprovalForAgentEdits;
		Settings->bRequireApprovalForAgentEdits = false;
		FHyperAIModelPlanRequest Request;
		Request.Operations = MoveTemp(Ops);
		Request.bDryRun = bDryRun;
		FHyperAIModelPlanReport Report = C::ApplyPlan(Request);
		Settings->bRequireApprovalForAgentEdits = bApproval;
		return Report;
	}

	const FHyperAIModelMeshStats* Find(const FHyperAIModelPlanReport& Report, const FString& Name)
	{
		return Report.Meshes.FindByPredicate([&Name](const FHyperAIModelMeshStats& Stats) { return Stats.Name == Name; });
	}

	FVector Parse(const FString& Text)
	{
		TArray<double> Values;
		HyperAIStudio::Modeling::Gate::ParseNumbers(Text, Values);
		return Values.Num() == 3 ? FVector(Values[0], Values[1], Values[2]) : FVector::ZeroVector;
	}

	FString TestFolder()
	{
		return TEXT("/Game/__HyperAIStudioTests/Modeling_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	}

	void DeleteFolder(const FString& Folder)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Folder), Assets, /*bRecursive=*/true);
		TArray<UObject*> Objects;
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* Object = Asset.GetAsset()) Objects.Add(Object);
		}
		if (!Objects.IsEmpty()) ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
	}

	UStaticMesh* LoadMesh(const FString& Path)
	{
		return LoadObject<UStaticMesh>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	int32 SourceTriangles(UStaticMesh& Mesh)
	{
		const FMeshDescription* Description = Mesh.GetMeshDescription(0);
		return Description ? Description->Triangles().Num() : 0;
	}

	/** True when the refusal names the op and says what was wrong. */
	bool Refused(const FHyperAIModelPlanReport& Report, const TCHAR* OpIndex, const TCHAR* Words)
	{
		return !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(OpIndex) && Report.Diagnostic.Contains(Words);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingContractsTest, "HyperAIStudio.NativeTools.Modeling.Contracts", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingContractsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	TestEqual(TEXT("three tools"), C::GetToolNames(), TArray<FString>({TEXT("hyper_model_inspect"), TEXT("hyper_model_apply_plan"), TEXT("hyper_model_validate")}));
	FHyperAIStudioExtensionCohortAdmission Admission;
	TestTrue(TEXT("the mesh_modeling pack's cohort is in the generated catalog, exactly: ")
		+ FHyperAIStudioExtensionRuntime::DescribeExactGeneratedCohortMismatch(C::PackId, C::AtomicCohortId, C::GetToolNames()),
		FHyperAIStudioExtensionRuntime::QueryExactGeneratedCohort(C::PackId, C::AtomicCohortId, C::GetToolNames(), Admission) && Admission.bExactCohortMatch);
	for (const FString& Name : C::GetToolNames())
	{
		const UFunction* Function = UHyperAIStudioModelingToolset::StaticClass()->FindFunctionByName(FName(*Name));
		if (!TestNotNull(*FString::Printf(TEXT("%s is a function of the toolset"), *Name), Function)) continue;
		const FString Description = Function->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description under 400 characters (%d)"), *Name, Description.Len()), !Description.IsEmpty() && Description.Len() < 400);
	}
	const FHyperAIModelInspectReport Inspected = C::Inspect(FHyperAIModelInspectRequest());
	TestTrue(TEXT("inspect answers: ") + Inspected.Diagnostic, Inspected.bOk);
	TestTrue(TEXT("and lists every op kind with its params"), Inspected.Kinds.Num() >= 40 && Inspected.Kinds.ContainsByPredicate([](const FString& Line)
		{
			return Line.StartsWith(TEXT("boolean:")) && Line.Contains(TEXT("Op union|subtract|intersect required"));
		}));
	TestEqual(TEXT("and the twelve recipes"), Inspected.Recipes.Num(), 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingParseAndRefuseTest, "HyperAIStudio.NativeTools.Modeling.ParseAndRefuse", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingParseAndRefuseTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FHyperAIModelOp Box = Sized(TEXT("add.box"), TEXT("Block"), TEXT("100,100,100"));
	TestTrue(TEXT("an unknown kind is refused, naming the kinds"), Refused(Run({Box, Op(TEXT("add.teapot"), TEXT("Block"))}, true), TEXT("Op 1"), TEXT("is not an op kind")));
	TestTrue(TEXT("an unknown param is refused, naming the ones it takes"), Refused(Run({Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Radius=4"))}, true), TEXT("Op 0"),
		TEXT("takes no param Radius; it takes Steps")));
	FHyperAIModelOp Moved = Box;
	Moved.Location = TEXT("1,2");
	TestTrue(TEXT("a malformed vector is refused"), Refused(Run({Moved}, true), TEXT("Op 0"), TEXT("Location '1,2' is not x,y,z")));
	FHyperAIModelOp Lathe = Op(TEXT("add.revolve"), TEXT("Vase"));
	Lathe.Points = TEXT("10,0");
	TestTrue(TEXT("a profile of one point is refused"), Refused(Run({Lathe}, true), TEXT("Op 0"), TEXT("2 or more points")));
	TestTrue(TEXT("a mesh no op made is refused, naming the ones there are"), Refused(Run({Box, Op(TEXT("noise"), TEXT("Rock"))}, true), TEXT("Op 1"),
		TEXT("no earlier op made mesh Rock; meshes so far: Block")));
	FHyperAIModelOp Picked = Op(TEXT("extrude"), TEXT("Block"), TEXT("Distance=10"));
	Picked.Select = TEXT("normal:0,0,1");
	TestTrue(TEXT("a selector of the wrong shape is refused"), Refused(Run({Box, Picked}, true), TEXT("Op 1"), TEXT("Select normal takes nx,ny,nz,maxdeg")));
	TestTrue(TEXT("a field the kind does not take is refused"), Refused(Run({Box, [] { FHyperAIModelOp Out = Op(TEXT("bevel"), TEXT("Block")); Out.Path = TEXT("0,0,0; 1,1,1"); return Out; }()}, true),
		TEXT("Op 1"), TEXT("bevel takes no Path")));
	TestTrue(TEXT("a required param is asked for"), Refused(Run({Box, Op(TEXT("boolean"), TEXT("Block"))}, true), TEXT("Op 1"), TEXT("boolean needs Source")));
	TestTrue(TEXT("an asset outside /Game is refused"), Refused(Run({Box, Write(TEXT("Block"), TEXT("/Engine/SM_Block"))}, true), TEXT("Op 1"), TEXT("not a path under /Game")));
	// Writing there would make a static mesh beside the material of the same name, which the engine cannot survive.
	TestTrue(TEXT("the vertex-colour material's path is refused, even with Replace"), Refused(Run({Box, Write(TEXT("Block"), C::VertexColorMaterial, TEXT("Replace=true"))}, true),
		TEXT("Op 1"), TEXT("is the placeholders' vertex-colour material")));
	TestTrue(TEXT("a disc with no width is refused"), Refused(Run({Sized(TEXT("add.disc"), TEXT("Plate"), TEXT("0,100,0"))}, true), TEXT("Op 0"), TEXT("Size is above 0")));
	TestTrue(TEXT("a material that is not one is refused"), Refused(Run({Box, Write(TEXT("Block"), TestFolder() + TEXT("/SM_Block"), TEXT("Materials=0:/Engine/BasicShapes/Cube"))}, true),
		TEXT("Op 1"), TEXT("is not a material")));
	TestTrue(TEXT("a value out of range is refused"), Refused(Run({Sized(TEXT("add.sphere"), TEXT("Ball"), TEXT("100"), TEXT("Steps=512"))}, true), TEXT("Op 0"), TEXT("Steps is a whole number from 3 to 128")));
	TestTrue(TEXT("a recipe that does not exist is refused, naming the recipes"), Refused(Run({Op(TEXT("recipe"), TEXT("Thing"), TEXT("Shape=teapot"))}, true), TEXT("Op 0"),
		TEXT("recipes are rock")));
	TestTrue(TEXT("a plan that writes nothing is refused when not a dry run"), !Run({Box}, false).bOk);
	// Parsers, on their own.
	TArray<FVector2D> Points;
	FString Error;
	TestTrue(TEXT("circle: makes a polygon"), HyperAIStudio::Modeling::Gate::ParseProfile(TEXT("circle:20,8"), Points, Error) && Points.Num() == 8 && FMath::IsNearlyEqual(Points[0].X, 20.0));
	TArray<TPair<FString, FString>> Pairs;
	TestFalse(TEXT("a param given twice is refused"), HyperAIStudio::Modeling::Gate::ParseParams(TEXT("Steps=2; steps=3"), Pairs, Error));
	FLinearColor Color;
	TestTrue(TEXT("colours read as #rrggbb"), HyperAIStudio::Modeling::Gate::ParseColor(TEXT("#ff8000"), Color) && FMath::IsNearlyEqual(Color.G, 128.f / 255.f, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingBudgetsTest, "HyperAIStudio.NativeTools.Modeling.Budgets", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingBudgetsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FHyperAIModelOp Dense = Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=64"));
	const FHyperAIModelPlanReport Tessellated = Run({Dense, Op(TEXT("tessellate"), TEXT("Block"), TEXT("Level=3"))}, true);
	TestTrue(TEXT("a plan predicted over the triangles a mesh may hold is refused before it runs: ") + Tessellated.Diagnostic,
		Refused(Tessellated, TEXT("Op 1"), TEXT("would make about 811200 triangles")) && Tessellated.Meshes.IsEmpty());
	TestTrue(TEXT("tessellate past level 3 is refused"), Refused(Run({Dense, Op(TEXT("tessellate"), TEXT("Block"), TEXT("Level=5"))}, true), TEXT("Op 1"), TEXT("Level is a whole number from 1 to 3")));
	FHyperAIModelOp Ring = Op(TEXT("array.radial"), TEXT("Ring"), TEXT("Count=500"));
	Ring.Source = TEXT("Block");
	TestTrue(TEXT("an array past 64 copies is refused"), Refused(Run({Dense, Ring}, true), TEXT("Op 1"), TEXT("Count is a whole number from 1 to 64")));
	TestTrue(TEXT("a voxel grid past 256 cells is refused"), Refused(Run({Dense, Op(TEXT("solidify"), TEXT("Block"), TEXT("Resolution=1024"))}, true), TEXT("Op 1"),
		TEXT("Resolution is a whole number from 8 to 256")));
	TArray<FHyperAIModelOp> Many;
	for (int32 Index = 0; Index < C::MaxOperations + 1; ++Index) Many.Add(Sized(TEXT("add.box"), TEXT("Block"), TEXT("10")));
	TestFalse(TEXT("more ops than a plan may have are refused"), Run(Many, true).bOk);
	TArray<FHyperAIModelOp> Rocks;
	for (int32 Index = 0; Index < 20; ++Index) Rocks.Add(Op(TEXT("recipe"), *FString::Printf(TEXT("Rock%d"), Index), TEXT("Shape=rock")));
	const FHyperAIModelPlanReport Expanded = Run(Rocks, true);
	TestTrue(TEXT("a plan past the ops limit after its recipes expand is refused: ") + Expanded.Diagnostic, !Expanded.bOk && Expanded.Diagnostic.Contains(TEXT("after recipes")));
	const FHyperAIModelPlanReport Heavy = Run({Dense, Op(TEXT("tessellate"), TEXT("Block"), TEXT("Level=1")), Write(TEXT("Block"), TestFolder() + TEXT("/SM_Heavy"))}, true);
	TestTrue(TEXT("an asset over 100000 triangles without Nanite is refused: ") + Heavy.Diagnostic, Refused(Heavy, TEXT("Op 2"), TEXT("without Nanite")));
	// Extruding every face on its own makes about seven triangles of each, where Check guesses two: 64 copies of the
	// result pass Check, but are refused against the meshes as built before they are made.
	FHyperAIModelOp Copies = Op(TEXT("array.linear"), TEXT("Many"), TEXT("Count=64"));
	Copies.Source = TEXT("Block");
	Copies.Location = TEXT("200,0,0");
	const FHyperAIModelPlanReport Grown = Run({Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=10")),
		Op(TEXT("extrude"), TEXT("Block"), TEXT("Distance=2; Area=face")), Copies}, true);
	TestTrue(TEXT("copies past the cap are refused before they are made: ") + Grown.Diagnostic, Refused(Grown, TEXT("Op 2"), TEXT("from the meshes as built")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingDryRunTest, "HyperAIStudio.NativeTools.Modeling.DryRunBuildsNothingPersistent", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingDryRunTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FString Asset = TestFolder() + TEXT("/SM_Holed");
	const int32 UndoBefore = GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
	FHyperAIModelOp Drill = Sized(TEXT("add.cylinder"), TEXT("Drill"), TEXT("40,40,200"), TEXT("Steps=24"));
	Drill.Location = TEXT("0,0,-50");
	FHyperAIModelOp Cut = Op(TEXT("boolean"), TEXT("Block"), TEXT("Op=subtract"));
	Cut.Source = TEXT("Drill");
	const FHyperAIModelPlanReport Report = Run({Sized(TEXT("add.box"), TEXT("Block"), TEXT("100,100,100")), Drill, Cut, Op(TEXT("drop"), TEXT("Drill")),
		Write(TEXT("Block"), Asset, TEXT("Collision=hulls:4"))}, true);
	if (!TestTrue(TEXT("a box with a hole drilled through it builds in memory: ") + Report.Diagnostic, Report.bOk && Report.Status == TEXT("checked"))) return false;
	const FHyperAIModelMeshStats* Block = Find(Report, TEXT("Block"));
	if (!TestNotNull(TEXT("the block is reported"), Block)) return false;
	TestTrue(FString::Printf(TEXT("with triangles (%d)"), Block->Triangles), Block->Triangles > 12);
	TestTrue(TEXT("closed"), Block->bClosed && Block->OpenEdges == 0);
	TestEqual(TEXT("in one piece"), Block->Components, 1);
	TestTrue(TEXT("the box's size: ") + Block->Size, Parse(Block->Size).Equals(FVector(100.0, 100.0, 100.0), 0.5));
	TestTrue(TEXT("sitting on its base"), FMath::IsNearlyZero(Parse(Block->Min).Z, 0.5));
	TestNull(TEXT("the dropped drill is gone"), Find(Report, TEXT("Drill")));
	TestTrue(TEXT("the asset it would write is reported, with its collision"), Report.Assets.Num() == 1 && Report.Assets[0].Triangles == Block->Triangles && Report.Assets[0].CollisionShapes >= 1);
	TestFalse(TEXT("nothing is written"), FPackageName::DoesPackageExist(Asset) || FindPackage(nullptr, *Asset) != nullptr);
	TestEqual(TEXT("and nothing goes in the undo history"), GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0, UndoBefore);
	TestTrue(TEXT("a plan with no recipe lists no expanded ops"), Report.ExpandedOps.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingDeterministicTest, "HyperAIStudio.NativeTools.Modeling.Deterministic", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingDeterministicTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	auto Plan = [](const TCHAR* Seed)
	{
		return Run({Sized(TEXT("add.sphere"), TEXT("Lump"), TEXT("120,100,80"), TEXT("Style=box; Steps=10")),
			Op(TEXT("noise"), TEXT("Lump"), *FString::Printf(TEXT("Magnitude=8; Frequency=0.02; Seed=%s"), Seed)),
			Op(TEXT("recipe"), TEXT("Crystals"), *FString::Printf(TEXT("Shape=crystal_cluster; Seed=%s"), Seed))}, true);
	};
	const FHyperAIModelPlanReport First = Plan(TEXT("4"));
	const FHyperAIModelPlanReport Again = Plan(TEXT("4"));
	const FHyperAIModelPlanReport Other = Plan(TEXT("5"));
	if (!TestTrue(TEXT("the plan builds: ") + First.Diagnostic, First.bOk && Again.bOk && Other.bOk)) return false;
	TestEqual(TEXT("the same ops and seeds hash the same"), Again.PlanHash, First.PlanHash);
	TestNotEqual(TEXT("another seed hashes differently"), Other.PlanHash, First.PlanHash);
	const FHyperAIModelMeshStats* A = Find(First, TEXT("Crystals"));
	const FHyperAIModelMeshStats* B = Find(Again, TEXT("Crystals"));
	TestTrue(TEXT("and builds the same mesh"), A && B && A->Triangles == B->Triangles && A->Size == B->Size && Find(First, TEXT("Lump"))->Max == Find(Again, TEXT("Lump"))->Max);
	TestTrue(TEXT("a recipe lists the ops it expanded into"), First.ExpandedOps.Num() > 5 && First.ExpandedOps.ContainsByPredicate(
		[](const FString& Line) { return Line.Contains(TEXT("scatter_on")) && Line.Contains(TEXT("<- op 2 recipe")); }));
	// A recipe's seed reaches its noise and scatter ops multiplied: the largest seed still gives seeds those ops take.
	const FHyperAIModelPlanReport Large = Run({Op(TEXT("recipe"), TEXT("Crystals"), TEXT("Shape=crystal_cluster; Seed=1000000000"))}, true);
	TestTrue(TEXT("the largest seed builds: ") + Large.Diagnostic, Large.bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSelectionOpsTest, "HyperAIStudio.NativeTools.Modeling.SelectionOps", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingSelectionOpsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FHyperAIModelOp Box = Sized(TEXT("add.box"), TEXT("Block"), TEXT("100,100,100"));
	FHyperAIModelOp Raise = Op(TEXT("extrude"), TEXT("Block"), TEXT("Distance=50"));
	Raise.Select = TEXT("normal:0,0,1,5");
	const FHyperAIModelPlanReport Extruded = Run({Box, Raise}, true);
	const FHyperAIModelMeshStats* Tall = Find(Extruded, TEXT("Block"));
	TestTrue(TEXT("extruding the top 50 cm raises the box to 150: ") + Extruded.Diagnostic, Tall && FMath::IsNearlyEqual(Parse(Tall->Max).Z, 150.0, 0.5) && Tall->bClosed);
	FHyperAIModelOp Shrink = Op(TEXT("inset"), TEXT("Block"), TEXT("Distance=20"));
	Shrink.Select = TEXT("top");
	const FHyperAIModelPlanReport Inset = Run({Box, Shrink}, true);
	const FHyperAIModelMeshStats* Bordered = Find(Inset, TEXT("Block"));
	TestTrue(TEXT("insetting the top adds its border's triangles and keeps it closed"), Bordered && Bordered->Triangles > 12 && Bordered->bClosed);
	const FHyperAIModelPlanReport Bevelled = Run({Box, Op(TEXT("bevel"), TEXT("Block"), TEXT("Distance=5; Subdivisions=1"))}, true);
	const FHyperAIModelMeshStats* Rounded = Find(Bevelled, TEXT("Block"));
	TestTrue(TEXT("bevelling adds triangles and keeps the box closed: ") + Bevelled.Diagnostic, Rounded && Rounded->Triangles > 12 && Rounded->bClosed
		&& Parse(Rounded->Size).Equals(FVector(100.0), 0.5));
	FHyperAIModelOp Lid = Op(TEXT("material"), TEXT("Block"), TEXT("Id=1"));
	Lid.Select = TEXT("top");
	const FHyperAIModelPlanReport Painted = Run({Box, Lid, Op(TEXT("color"), TEXT("Block"), TEXT("Color=#ff0000")) }, true);
	const FHyperAIModelMeshStats* TwoSlots = Find(Painted, TEXT("Block"));
	TestTrue(TEXT("a material id on the top gives two slots"), TwoSlots && TwoSlots->MaterialIds == 2);
	FHyperAIModelOp Cut = Op(TEXT("plane_cut"), TEXT("Block"));
	Cut.Location = TEXT("0,0,40");
	const FHyperAIModelPlanReport Halved = Run({Box, Cut}, true);
	const FHyperAIModelMeshStats* Low = Find(Halved, TEXT("Block"));
	TestTrue(TEXT("a plane cut keeps what is below it, closed"), Low && FMath::IsNearlyEqual(Parse(Low->Max).Z, 40.0, 0.5) && Low->bClosed);
	FHyperAIModelOp Half = Sized(TEXT("add.box"), TEXT("Wing"), TEXT("100,50,20"));
	Half.Location = TEXT("-50,0,0");
	FHyperAIModelOp Mirror = Op(TEXT("mirror"), TEXT("Wing"));
	const FHyperAIModelPlanReport Mirrored = Run({Half, Mirror}, true);
	const FHyperAIModelMeshStats* Wings = Find(Mirrored, TEXT("Wing"));
	TestTrue(TEXT("mirroring across X doubles the wing: ") + (Wings ? Wings->Size : Mirrored.Diagnostic), Wings && Parse(Wings->Size).Equals(FVector(200.0, 50.0, 20.0), 0.5));
	// Profiles and paths, whichever way their points run.
	FHyperAIModelOp Lathe = Op(TEXT("add.revolve"), TEXT("Drum"), TEXT("Steps=24"));
	Lathe.Points = TEXT("0,50; 30,50; 30,0; 0,0");
	FHyperAIModelOp Pipe = Op(TEXT("add.sweep"), TEXT("Pipe"), TEXT("EndScale=0.5"));
	Pipe.Points = TEXT("circle:10,12");
	Pipe.Path = TEXT("0,0,0; 0,0,100; 50,0,150; 100,0,150");
	FHyperAIModelOp Outline = Op(TEXT("add.extrude_polygon"), TEXT("Slab"), TEXT("Height=20"));
	Outline.Points = TEXT("0,0; 0,100; 60,100; 100,0");
	const FHyperAIModelPlanReport Profiles = Run({Lathe, Pipe, Outline}, true);
	for (const TCHAR* Name : {TEXT("Drum"), TEXT("Pipe"), TEXT("Slab")})
	{
		const FHyperAIModelMeshStats* Shape = Find(Profiles, Name);
		TestTrue(FString::Printf(TEXT("%s is closed, even with its profile on the axis or running clockwise: %s"), Name, *Profiles.Diagnostic), Shape && Shape->bClosed);
	}
	const FHyperAIModelMeshStats* Drum = Find(Profiles, TEXT("Drum"));
	TestTrue(TEXT("the drum is 60 cm across and 50 tall"), Drum && Parse(Drum->Size).Equals(FVector(60.0, 60.0, 50.0), 1.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingRecipesTest, "HyperAIStudio.NativeTools.Modeling.Recipes", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingRecipesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FString Folder = TestFolder();
	TArray<FString> Written;
	TArray<FHyperAIModelOp> Batch;
	auto Flush = [&]()
	{
		if (Batch.IsEmpty()) return;
		const FHyperAIModelPlanReport Report = Run(Batch, false);
		TestTrue(TEXT("the recipes are written: ") + Report.Diagnostic, Report.bOk && Report.Status == TEXT("completed"));
		Written.Append(Report.Saved);
		Batch.Reset();
	};
	for (const HyperAIStudio::Modeling::Recipes::FInfo& Recipe : HyperAIStudio::Modeling::Recipes::List())
	{
		const FString Mesh = FString(Recipe.Name).Replace(TEXT("_"), TEXT(""));
		const FHyperAIModelPlanReport Built = Run({Op(TEXT("recipe"), *Mesh, *FString::Printf(TEXT("Shape=%s; Seed=3"), Recipe.Name))}, true);
		const FHyperAIModelMeshStats* Stats = Find(Built, Mesh);
		if (!TestTrue(FString::Printf(TEXT("%s builds: %s"), Recipe.Name, *Built.Diagnostic), Built.bOk && Stats != nullptr)) continue;
		AddInfo(FString::Printf(TEXT("%s: %d triangles, %s cm, %d piece(s), %d open edge(s), %d slot(s), %d ops, %d ms"), Recipe.Name, Stats->Triangles, *Stats->Size, Stats->Components,
			Stats->OpenEdges, Stats->MaterialIds, Built.ExpandedOps.Num(), Built.BuildMs));
		TestEqual(FString::Printf(TEXT("%s leaves only its mesh: its parts are dropped"), Recipe.Name), Built.Meshes.Num(), 1);
		TestTrue(FString::Printf(TEXT("%s is not empty and within budget"), Recipe.Name), Stats->Triangles > 50 && Stats->Triangles <= C::MaxTrianglesPerAsset);
		TestTrue(FString::Printf(TEXT("%s is watertight (%d open edges)"), Recipe.Name, Stats->OpenEdges), Stats->bClosed);
		TestTrue(FString::Printf(TEXT("%s has UVs"), Recipe.Name), Stats->UVSets >= 1);
		const FVector Size = Parse(Stats->Size);
		TestTrue(FString::Printf(TEXT("%s is about its size (%s for %g)"), Recipe.Name, *Stats->Size, Recipe.DefaultSize),
			Size.GetMax() > Recipe.DefaultSize * 0.5 && Size.GetMax() < Recipe.DefaultSize * 1.6);
		TestTrue(FString::Printf(TEXT("%s sits on its base"), Recipe.Name), FMath::IsNearlyZero(Parse(Stats->Min).Z, 0.5));
		// Rubble and blades are pieces of their own; every other recipe fuses into one.
		const bool bPieces = FString(Recipe.Name) == TEXT("ruined_wall") || FString(Recipe.Name) == TEXT("grass_tuft");
		TestTrue(FString::Printf(TEXT("%s is %s (%d)"), Recipe.Name, bPieces ? TEXT("in pieces") : TEXT("one piece"), Stats->Components), bPieces ? Stats->Components > 1 : Stats->Components == 1);
		if (FString(Recipe.Name) == TEXT("arch"))
		{
			TestTrue(TEXT("the arch's ring stands up over its piers: ") + Stats->Size, Size.Z > Recipe.DefaultSize * 0.8);
		}
		FHyperAIModelOp Recipe2 = Op(TEXT("recipe"), *Mesh, *FString::Printf(TEXT("Shape=%s; Seed=3"), Recipe.Name));
		Batch.Add(Recipe2);
		Batch.Add(Write(*Mesh, Folder + TEXT("/SM_") + Mesh, FString(Recipe.Name) == TEXT("arch") ? TEXT("Collision=hulls:4") : TEXT("")));
		if (Batch.Num() >= 12) Flush();
	}
	Flush();
	TestEqual(TEXT("every recipe is written"), Written.Num(), HyperAIStudio::Modeling::Recipes::List().Num());
	FHyperAIModelValidateRequest Check;
	for (const FString& Path : Written) Check.Assets.Add(Path);
	Check.Policy = TEXT("placeholder");
	const FHyperAIModelValidateReport Valid = C::Validate(Check);
	for (const FHyperAIModelIssue& Issue : Valid.Issues) AddInfo(FString::Printf(TEXT("%s %s %s: %s"), *Issue.Severity, *Issue.Code, *Issue.Asset, *Issue.Message));
	TestTrue(TEXT("and every one passes validation as a placeholder: ") + Valid.Diagnostic, Valid.bOk);
	TestFalse(TEXT("with no open edges"), Valid.Issues.ContainsByPredicate([](const FHyperAIModelIssue& Issue) { return Issue.Code == TEXT("open_edges"); }));
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingApplyTest, "HyperAIStudio.NativeTools.Modeling.ApplyCreatesAsset", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingApplyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FString Folder = TestFolder();
	const FString Arch = Folder + TEXT("/SM_TestArch");
	const FString Plain = Folder + TEXT("/SM_TestPlain");
	const TArray<FHyperAIModelOp> Ops = {Op(TEXT("recipe"), TEXT("Arch"), TEXT("Shape=arch; Size=300")),
		Write(TEXT("Arch"), Arch, TEXT("Nanite=false; Collision=convex; Materials=0:/Engine/BasicShapes/BasicShapeMaterial")),
		Sized(TEXT("add.cylinder"), TEXT("Post"), TEXT("30,30,200"), TEXT("Color=#806040")), Write(TEXT("Post"), Plain)};
	const FHyperAIModelPlanReport Dry = Run(Ops, true);
	const FHyperAIModelPlanReport Done = Run(Ops, false);
	if (!TestTrue(TEXT("the plan writes two assets: ") + Done.Diagnostic, Dry.bOk && Done.bOk && Done.Status == TEXT("completed") && Done.Saved.Num() == 2))
	{
		DeleteFolder(Folder);
		return false;
	}
	UStaticMesh* Mesh = LoadMesh(Arch);
	if (!TestNotNull(TEXT("the arch is a static mesh"), Mesh))
	{
		DeleteFolder(Folder);
		return false;
	}
	TArray<UStaticMesh*> Compile = {Mesh};
	FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
	const int32 Planned = Dry.Assets.IsEmpty() ? 0 : Dry.Assets[0].Triangles;
	const int32 Rendered = Mesh->GetNumTriangles(0);
	TestTrue(FString::Printf(TEXT("LOD0 has the dry run's triangles, within 1%% (%d of %d)"), Rendered, Planned), Planned > 0 && FMath::Abs(Rendered - Planned) <= FMath::Max(1, Planned / 100));
	TestTrue(TEXT("its body has a convex hull"), Mesh->GetBodySetup() && Mesh->GetBodySetup()->AggGeom.ConvexElems.Num() >= 1);
	TestTrue(TEXT("one slot, with the material named"), Mesh->GetStaticMaterials().Num() == 1 && Mesh->GetStaticMaterials()[0].MaterialInterface
		&& Mesh->GetStaticMaterials()[0].MaterialInterface->GetName() == TEXT("BasicShapeMaterial"));
	TestFalse(TEXT("Nanite off"), Mesh->IsNaniteEnabled());
	TestTrue(TEXT("transactional, so later edits undo"), Mesh->HasAnyFlags(RF_Transactional));
	const FString* Hash = Mesh->GetPackage()->GetMetaData().FindValue(Mesh, C::PlanHashKey);
	TestTrue(TEXT("it keeps the plan's hash"), Hash && *Hash == Done.PlanHash);
	TestTrue(TEXT("and is saved"), FPackageName::DoesPackageExist(Arch));
	UStaticMesh* Post = LoadMesh(Plain);
	TestTrue(TEXT("a mesh written without materials gets the vertex-colour material"), Post && Post->GetStaticMaterials().Num() == 1 && Post->GetStaticMaterials()[0].MaterialInterface
		&& Post->GetStaticMaterials()[0].MaterialInterface->GetPathName().StartsWith(C::VertexColorMaterial));
	TestTrue(TEXT("which exists as an asset"), FPackageName::DoesPackageExist(C::VertexColorMaterial));
	FHyperAIModelInspectRequest Look;
	Look.Folder = Folder;
	Look.Asset = Arch;
	const FHyperAIModelInspectReport Read = C::Inspect(Look);
	TestTrue(TEXT("inspect lists both and reads the arch in full"), Read.bOk && Read.Assets.Num() == 2 && Read.Details.ContainsByPredicate(
		[&Done](const FString& Line) { return Line.Contains(Done.PlanHash); }));
	TestTrue(TEXT("writing the same path again without Replace is refused"), Refused(Run(Ops, true), TEXT("Op "), TEXT("exists; give Replace=true")));
	// Approval on: the plan waits, and nothing is written until it is approved.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = true;
	FHyperAIModelPlanRequest Waiting;
	Waiting.Operations = {Sized(TEXT("add.box"), TEXT("Crate"), TEXT("80")), Write(TEXT("Crate"), Folder + TEXT("/SM_Crate"))};
	const FHyperAIModelPlanReport Held = C::ApplyPlan(Waiting);
	Settings->bRequireApprovalForAgentEdits = bApproval;
	TestTrue(TEXT("with approval on, the plan waits: ") + Held.Diagnostic, Held.bOk && Held.Status == TEXT("awaiting_user_approval"));
	TestFalse(TEXT("and nothing is written yet"), FPackageName::DoesPackageExist(Folder + TEXT("/SM_Crate")));
	FHyperAIStudioApprovalGate::Reject(Held.OperationId, TEXT("test"));
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingReplaceUndoTest, "HyperAIStudio.NativeTools.Modeling.ReplaceIsUndoable", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingReplaceUndoTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	if (!GEditor || !GEditor->Trans)
	{
		AddInfo(TEXT("Skipped: no editor transaction buffer."));
		return true;
	}
	const FString Folder = TestFolder();
	const FString Path = Folder + TEXT("/SM_Swap");
	const FHyperAIModelPlanReport First = Run({Sized(TEXT("add.box"), TEXT("Swap"), TEXT("100")), Write(TEXT("Swap"), Path)}, false);
	UStaticMesh* Mesh = LoadMesh(Path);
	if (!TestTrue(TEXT("a box is written: ") + First.Diagnostic, First.bOk && Mesh != nullptr))
	{
		DeleteFolder(Folder);
		return false;
	}
	const int32 Before = SourceTriangles(*Mesh);
	const FHyperAIModelPlanReport Second = Run({Sized(TEXT("add.sphere"), TEXT("Swap"), TEXT("100"), TEXT("Steps=24")), Write(TEXT("Swap"), Path, TEXT("Replace=true; Collision=box"))}, false);
	const int32 After = SourceTriangles(*Mesh);
	TestTrue(FString::Printf(TEXT("a sphere replaces it (%d triangles, then %d): %s"), Before, After, *Second.Diagnostic), Second.bOk && Second.Assets.Num() == 1
		&& Second.Assets[0].bReplaced && After != Before && After == Second.Assets[0].Triangles);
	TestTrue(TEXT("the undo step is named"), GEditor->Trans->GetUndoContext().Title.ToString().Contains(TEXT("Model Plan")));
	GEditor->UndoTransaction();
	TArray<UStaticMesh*> Compile = {Mesh};
	FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
	TestEqual(TEXT("undo brings the box back"), SourceTriangles(*Mesh), Before);
	TestEqual(TEXT("and its render data"), Mesh->GetNumTriangles(0), Before);
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingReplaceRedirectorTest, "HyperAIStudio.NativeTools.Modeling.ReplaceRefusesARedirector", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingReplaceRedirectorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	namespace Gate = HyperAIStudio::Modeling::Gate;
	const FString Folder = TestFolder();
	const FString Target = Folder + TEXT("/SM_Target");
	const FString Moved = Folder + TEXT("/SM_Moved");
	const FHyperAIModelPlanReport Made = Run({Sized(TEXT("add.box"), TEXT("Crate"), TEXT("100")), Write(TEXT("Crate"), Target)}, false);
	UStaticMesh* Mesh = Made.bOk ? LoadMesh(Target) : nullptr;
	if (!TestTrue(TEXT("a box is written: ") + Made.Diagnostic, Mesh != nullptr))
	{
		DeleteFolder(Folder);
		return false;
	}
	// Checked and built while nothing is at the path; a move then leaves a redirector there before the write runs.
	const TArray<FHyperAIModelOp> Ops = {Sized(TEXT("add.sphere"), TEXT("Ball"), TEXT("100"), TEXT("Steps=24")), Write(TEXT("Ball"), Moved, TEXT("Replace=true"))};
	Gate::FPlan Plan;
	Gate::FBuilt Built;
	FString Error;
	if (!TestTrue(TEXT("the plan checks and builds: ") + Error, Gate::Check(Ops, Plan, Error) && Gate::Build(Plan, Built, Error)))
	{
		DeleteFolder(Folder);
		return false;
	}
	UObjectRedirector* Redirector = NewObject<UObjectRedirector>(CreatePackage(*Moved), *FPackageName::GetShortName(Moved), RF_Public | RF_Standalone);
	Redirector->DestinationObject = Mesh;
	FAssetRegistryModule::AssetCreated(Redirector);
	const FString Named = Moved + TEXT(" is a redirector to ") + Target;
	const FHyperAIModelPlanReport Unsaved = Run(Ops, true);
	TestTrue(TEXT("Replace onto a redirector not saved yet is refused, naming its target: ") + Unsaved.Diagnostic, Refused(Unsaved, TEXT("Op 1"), *Named));
	if (!TestTrue(TEXT("the redirector saves: ") + Error, FHyperAIStudioSourceControl::SaveAsset(*Redirector, Error)))
	{
		DeleteFolder(Folder);
		return false;
	}
	// Saved, a load of the path follows it to the box.
	const FHyperAIModelPlanReport Saved = Run(Ops, true);
	TestTrue(TEXT("Replace onto a saved redirector is refused, naming its target: ") + Saved.Diagnostic, Refused(Saved, TEXT("Op 1"), *Named));
	const FHyperAIModelPlanReport Applied = Run(Ops, false);
	TestTrue(TEXT("and so is the plan itself: ") + Applied.Diagnostic, Refused(Applied, TEXT("Op 1"), *Named) && Applied.Saved.IsEmpty());
	TArray<FHyperAIModelAssetResult> Assets;
	TArray<UStaticMesh*> Written;
	TArray<UStaticMesh*> Created;
	TArray<FString> Notes;
	const bool bWritten = Gate::Write(Plan, Built, Assets, Written, Created, Notes, Error);
	TestTrue(TEXT("a plan checked before the move is refused at its write: ") + Error, !bWritten && Error.StartsWith(TEXT("Op 1")) && Error.Contains(Named));
	Gate::Discard(Created);
	TestEqual(TEXT("and the box the redirector points to is untouched"), SourceTriangles(*Mesh), 12);
	// The redirector first, so the box is deleted with nothing referencing it.
	ObjectTools::ForceDeleteObjects({Redirector}, /*ShowConfirmation=*/false);
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingReplaceLodsTest, "HyperAIStudio.NativeTools.Modeling.ReplaceKeepsOnlyThePlansMesh", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingReplaceLodsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FString Folder = TestFolder();
	const FString Path = Folder + TEXT("/SM_Imported");
	const FHyperAIModelPlanReport First = Run({Sized(TEXT("add.box"), TEXT("Prop"), TEXT("100")), Write(TEXT("Prop"), Path)}, false);
	UStaticMesh* Mesh = First.bOk ? LoadMesh(Path) : nullptr;
	if (!TestTrue(TEXT("a box is written: ") + First.Diagnostic, Mesh != nullptr))
	{
		DeleteFolder(Folder);
		return false;
	}
	// As an import can leave it: two LODs of its own and a hi-res source for Nanite, all of them the old model.
	const FMeshDescription Old = *Mesh->GetMeshDescription(0);
	Mesh->SetNumSourceModels(3);
	for (int32 Lod = 1; Lod < 3; ++Lod)
	{
		Mesh->CreateMeshDescription(Lod, Old);
		Mesh->CommitMeshDescription(Lod);
	}
	Mesh->CreateHiResMeshDescription(Old);
	Mesh->CommitHiResMeshDescription();
	Mesh->GetHiResSourceModel().SourceImportFilename = TEXT("SM_Imported_HiRes.fbx");
	Mesh->PostEditChange();
	TArray<UStaticMesh*> Compile = {Mesh};
	FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
	if (!TestTrue(TEXT("the box has three LODs and a hi-res source"), Mesh->GetNumSourceModels() == 3 && Mesh->GetNumLODs() == 3 && Mesh->IsHiResMeshDescriptionValid()))
	{
		DeleteFolder(Folder);
		return false;
	}
	const FHyperAIModelPlanReport Second = Run({Sized(TEXT("add.sphere"), TEXT("Prop"), TEXT("100"), TEXT("Steps=24")), Write(TEXT("Prop"), Path, TEXT("Replace=true"))}, false);
	FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
	if (!TestTrue(TEXT("a sphere replaces it: ") + Second.Diagnostic, Second.bOk && Second.Assets.Num() == 1 && Second.Assets[0].bReplaced))
	{
		DeleteFolder(Folder);
		return false;
	}
	TestEqual(TEXT("leaving exactly one source model"), Mesh->GetNumSourceModels(), 1);
	TestEqual(TEXT("and one LOD to render"), Mesh->GetNumLODs(), 1);
	TestFalse(TEXT("and no hi-res description"), Mesh->IsHiResMeshDescriptionValid());
	TestTrue(TEXT("nor the hi-res file it came from"), Mesh->GetHiResSourceModel().SourceImportFilename.IsEmpty());
	TestEqual(TEXT("the one source is the sphere"), SourceTriangles(*Mesh), Second.Assets[0].Triangles);
	TestTrue(TEXT("and the report says what went: ") + FString::Join(Second.Notes, TEXT(" | ")), Second.Notes.ContainsByPredicate([](const FString& Note)
		{
			return Note.Contains(TEXT("its 2 other LOD(s) and its hi-res source removed"));
		}));
	if (GEditor && GEditor->Trans)
	{
		GEditor->UndoTransaction();
		FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
		const FMeshDescription* Lod2 = Mesh->GetNumSourceModels() == 3 ? Mesh->GetMeshDescription(2) : nullptr;
		TestTrue(TEXT("undo brings back the box, its LODs and its hi-res source"), SourceTriangles(*Mesh) == 12 && Lod2 && Lod2->Triangles().Num() == 12
			&& Mesh->IsHiResMeshDescriptionValid());
	}
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingValidateTest, "HyperAIStudio.NativeTools.Modeling.Validate", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingValidateTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FString Folder = TestFolder();
	FHyperAIModelOp Raised = Sized(TEXT("add.box"), TEXT("Raised"), TEXT("100"));
	Raised.Location = TEXT("0,0,100");
	const FHyperAIModelPlanReport Made = Run({Sized(TEXT("add.disc"), TEXT("Plate"), TEXT("200,200,0")), Write(TEXT("Plate"), Folder + TEXT("/SM_Plate"), TEXT("Collision=none")),
		Sized(TEXT("add.box"), TEXT("Speck"), TEXT("0.5")), Write(TEXT("Speck"), Folder + TEXT("/SM_Speck")), Raised, Write(TEXT("Raised"), Folder + TEXT("/SM_Raised")),
		Sized(TEXT("add.box"), TEXT("Dense"), TEXT("100"), TEXT("Steps=44")), Op(TEXT("tessellate"), TEXT("Dense")), Write(TEXT("Dense"), Folder + TEXT("/SM_Dense"), TEXT("Collision=box"))}, false);
	if (!TestTrue(TEXT("the fixtures are written: ") + Made.Diagnostic, Made.bOk))
	{
		DeleteFolder(Folder);
		return false;
	}
	auto Has = [](const FHyperAIModelValidateReport& Report, const FString& Asset, const TCHAR* Code, const TCHAR* Severity)
	{
		return Report.Issues.ContainsByPredicate([&](const FHyperAIModelIssue& Issue) { return Issue.Asset.StartsWith(Asset) && Issue.Code == Code && Issue.Severity == Severity; });
	};
	FHyperAIModelValidateRequest Request;
	Request.Assets = {Folder + TEXT("/SM_Plate"), Folder + TEXT("/SM_Speck"), Folder + TEXT("/SM_Raised"), Folder + TEXT("/SM_Dense")};
	const FHyperAIModelValidateReport Placeholder = C::Validate(Request);
	for (const FHyperAIModelIssue& Issue : Placeholder.Issues) AddInfo(FString::Printf(TEXT("%s %s %s"), *Issue.Severity, *Issue.Code, *Issue.Asset));
	TestTrue(TEXT("a flat plate has open edges"), Has(Placeholder, Folder + TEXT("/SM_Plate"), TEXT("open_edges"), TEXT("warning")));
	TestTrue(TEXT("and no collision, a warning for a placeholder"), Has(Placeholder, Folder + TEXT("/SM_Plate"), TEXT("no_collision"), TEXT("warning")));
	TestTrue(TEXT("a speck half a centimeter across is too small"), Has(Placeholder, Folder + TEXT("/SM_Speck"), TEXT("too_small"), TEXT("error")));
	TestTrue(TEXT("a box floating a meter over its pivot is flagged"), Has(Placeholder, Folder + TEXT("/SM_Raised"), TEXT("pivot_not_at_base"), TEXT("warning")));
	TestTrue(TEXT("a placeholder of 88752 triangles without Nanite is a warning"), Has(Placeholder, Folder + TEXT("/SM_Dense"), TEXT("nanite_off"), TEXT("warning")));
	TestFalse(TEXT("the errors fail validation"), Placeholder.bOk);
	Request.Policy = TEXT("production");
	const FHyperAIModelValidateReport Production = C::Validate(Request);
	TestTrue(TEXT("for production, no collision is an error"), Has(Production, Folder + TEXT("/SM_Plate"), TEXT("no_collision"), TEXT("error")));
	TestTrue(TEXT("and so are 88752 triangles without Nanite"), Has(Production, Folder + TEXT("/SM_Dense"), TEXT("too_many_triangles"), TEXT("error")));
	if (UStaticMesh* Raised2 = LoadMesh(Folder + TEXT("/SM_Raised")))
	{
		Raised2->GetStaticMaterials()[0].MaterialInterface = nullptr;
		FHyperAIModelValidateRequest Slot;
		Slot.Assets = {Folder + TEXT("/SM_Raised")};
		TestTrue(TEXT("an empty material slot is an error"), Has(C::Validate(Slot), Folder + TEXT("/SM_Raised"), TEXT("empty_material_slot"), TEXT("error")));
	}
	Request.Policy = TEXT("strict");
	TestFalse(TEXT("an unknown policy is refused"), C::Validate(Request).bOk);
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingHeavyTopologyTest, "HyperAIStudio.NativeTools.Modeling.HeavyMeshTopologyNotMeasured", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingHeavyTopologyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FString Folder = TestFolder();
	const FString Path = Folder + TEXT("/SM_Heavy");
	const FHyperAIModelPlanReport Made = Run({Sized(TEXT("add.box"), TEXT("Heavy"), TEXT("100")), Write(TEXT("Heavy"), Path, TEXT("Collision=box"))}, false);
	UStaticMesh* Mesh = Made.bOk ? LoadMesh(Path) : nullptr;
	if (!TestTrue(TEXT("a box is written: ") + Made.Diagnostic, Mesh != nullptr))
	{
		DeleteFolder(Folder);
		return false;
	}
	FHyperAIModelValidateRequest Request;
	Request.Assets = {Path};
	const FHyperAIModelValidateReport Light = C::Validate(Request);
	TestTrue(TEXT("under the cap, its open edges and pieces are counted: ") + (Light.Assets.IsEmpty() ? Light.Diagnostic : Light.Assets[0]),
		Light.Assets.Num() == 1 && Light.Assets[0].Contains(TEXT("0 open edge(s), 1 piece(s)")));
	// Past what a plan may write, as an imported production mesh is: a box of 150 steps a side, 270000 triangles.
	const TStrongObjectPtr<UDynamicMesh> Dense(NewObject<UDynamicMesh>(GetTransientPackage()));
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(Dense.Get(), FGeometryScriptPrimitiveOptions(), FTransform::Identity, 100, 100, 100, 149, 149, 149);
	FGeometryScriptCopyMeshToAssetOptions Options;
	Options.bEmitTransaction = false;
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(Dense.Get(), Mesh, Options, FGeometryScriptMeshWriteLOD(), Outcome, /*bUseSectionMaterials=*/false);
	if (!TestTrue(FString::Printf(TEXT("the asset takes %d triangles, over the cap of %d"), SourceTriangles(*Mesh), C::MaxTrianglesPerMesh),
		Outcome == EGeometryScriptOutcomePins::Success && SourceTriangles(*Mesh) > C::MaxTrianglesPerMesh))
	{
		DeleteFolder(Folder);
		return false;
	}
	const FString NotMeasured = FString::Printf(TEXT("open edges and pieces not measured (over %d triangles)"), C::MaxTrianglesPerMesh);
	const FHyperAIModelValidateReport Heavy = C::Validate(Request);
	TestTrue(TEXT("over it, validate says its topology was not measured: ") + (Heavy.Assets.IsEmpty() ? Heavy.Diagnostic : Heavy.Assets[0]),
		Heavy.Assets.Num() == 1 && Heavy.Assets[0].Contains(NotMeasured) && !Heavy.Assets[0].Contains(TEXT("open edge(s)")));
	TestFalse(TEXT("rather than counting open edges"), Heavy.Issues.ContainsByPredicate([](const FHyperAIModelIssue& Issue) { return Issue.Code == TEXT("open_edges"); }));
	FHyperAIModelInspectRequest Look;
	Look.Folder = Folder;
	Look.Asset = Path;
	const FHyperAIModelInspectReport Read = C::Inspect(Look);
	TestTrue(TEXT("and so does inspect: ") + (Read.Details.IsEmpty() ? Read.Diagnostic : Read.Details[0]), Read.bOk && !Read.Details.IsEmpty() && Read.Details[0].Contains(NotMeasured));
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingFailureTest, "HyperAIStudio.NativeTools.Modeling.FailureWritesNothing", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingFailureTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FString Folder = TestFolder();
	FHyperAIModelOp Far = Sized(TEXT("add.box"), TEXT("Far"), TEXT("50"));
	Far.Location = TEXT("1000,0,0");
	FHyperAIModelOp Meet = Op(TEXT("boolean"), TEXT("Near"), TEXT("Op=intersect"));
	Meet.Source = TEXT("Far");
	// Geometry Script logs the empty result as an error as well as returning it.
	AddExpectedError(TEXT("Boolean operation failed due to an empty result"), EAutomationExpectedErrorFlags::Contains, 0);
	const FHyperAIModelPlanReport Report = Run({Sized(TEXT("add.box"), TEXT("Near"), TEXT("50")), Write(TEXT("Near"), Folder + TEXT("/SM_First")), Far, Meet,
		Write(TEXT("Near"), Folder + TEXT("/SM_Second"), TEXT("Replace=true"))}, false);
	TestTrue(TEXT("an intersection of boxes a meter apart fails, naming the op: ") + Report.Diagnostic, !Report.bOk && Report.Diagnostic.Contains(TEXT("Op 3 (boolean")));
	TestFalse(TEXT("and the first write_asset, before it, wrote nothing"), FPackageName::DoesPackageExist(Folder + TEXT("/SM_First")) || FindPackage(nullptr, *(Folder + TEXT("/SM_First"))));
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingFailedWriteTest, "HyperAIStudio.NativeTools.Modeling.FailedWriteRollsBack", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingFailedWriteTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	namespace Gate = HyperAIStudio::Modeling::Gate;
	const FString Folder = TestFolder();
	const FString First = Folder + TEXT("/SM_First");
	// Both writes checked, but only the first one's mesh built: the second fails after the first made its asset.
	const TArray<FHyperAIModelOp> Ops = {Sized(TEXT("add.box"), TEXT("A"), TEXT("50")), Write(TEXT("A"), First), Sized(TEXT("add.box"), TEXT("B"), TEXT("50")),
		Write(TEXT("B"), Folder + TEXT("/SM_Second"))};
	Gate::FPlan Plan;
	Gate::FPlan FirstOnly;
	FString Error;
	if (!TestTrue(TEXT("the plans check: ") + Error, Gate::Check(Ops, Plan, Error) && Gate::Check({Ops[0], Ops[1]}, FirstOnly, Error))) return false;
	Gate::FBuilt Built;
	if (!TestTrue(TEXT("the first write's mesh builds: ") + Error, Gate::Build(FirstOnly, Built, Error))) return false;
	TArray<FHyperAIModelAssetResult> Assets;
	TArray<UStaticMesh*> Written;
	TArray<UStaticMesh*> Created;
	TArray<FString> Notes;
	const bool bWritten = Gate::Write(Plan, Built, Assets, Written, Created, Notes, Error);
	TestTrue(TEXT("the second write fails, naming its op: ") + Error, !bWritten && Error.Contains(TEXT("Op 3")));
	TestEqual(TEXT("after the first made its asset"), Created.Num(), 1);
	Gate::Discard(Created);
	const FString ObjectPath = First + TEXT(".") + FPackageName::GetShortName(First);
	TestTrue(TEXT("which is deleted again, never saved"), Created.IsEmpty() && !IsValid(FindObject<UStaticMesh>(nullptr, *ObjectPath)) && !FPackageName::DoesPackageExist(First));
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingWriteAtItsOpTest, "HyperAIStudio.NativeTools.Modeling.WriteTakesTheMeshAtItsOp", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingWriteAtItsOpTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FString Folder = TestFolder();
	const FString Crate = Folder + TEXT("/SM_Crate");
	// The crate is written as a plain box: the sphere added after the write, and the drop, change only the working mesh.
	FHyperAIModelOp Lid = Sized(TEXT("add.sphere"), TEXT("Crate"), TEXT("60"), TEXT("Steps=24"));
	Lid.Location = TEXT("0,0,100");
	const TArray<FHyperAIModelOp> Ops = {Sized(TEXT("add.box"), TEXT("Crate"), TEXT("100")), Write(TEXT("Crate"), Crate), Lid, Op(TEXT("drop"), TEXT("Crate"))};
	const FHyperAIModelPlanReport Dry = Run(Ops, true);
	TestTrue(TEXT("the dry run reports the write with the box's 12 triangles: ") + Dry.Diagnostic, Dry.bOk && Dry.Assets.Num() == 1 && Dry.Assets[0].Triangles == 12);
	TestNull(TEXT("and leaves no working mesh"), Find(Dry, TEXT("Crate")));
	const FHyperAIModelPlanReport Done = Run(Ops, false);
	UStaticMesh* Mesh = Done.bOk ? LoadMesh(Crate) : nullptr;
	TestTrue(TEXT("the box is what is written: ") + Done.Diagnostic, Mesh && SourceTriangles(*Mesh) == 12);
	// Written under the cap without Nanite, then tessellated past it: the asset is the mesh as written.
	const FHyperAIModelPlanReport Grown = Run({Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=64")), Write(TEXT("Block"), Folder + TEXT("/SM_Block")),
		Op(TEXT("tessellate"), TEXT("Block"))}, true);
	const FHyperAIModelMeshStats* Block = Find(Grown, TEXT("Block"));
	TestTrue(TEXT("a mesh tessellated after its write is written as it was: ") + Grown.Diagnostic, Grown.bOk && Block && Grown.Assets.Num() == 1
		&& Grown.Assets[0].Triangles * 4 == Block->Triangles && Grown.Assets[0].Triangles <= C::MaxTrianglesPerAsset && Block->Triangles > C::MaxTrianglesPerAsset);
	DeleteFolder(Folder);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSkillExamplesTest, "HyperAIStudio.NativeTools.Modeling.SkillExamples", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingSkillExamplesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	FString Skill;
	if (!TestTrue(TEXT("the modeling skill ships with the plugin"), Plugin.IsValid()
		&& FFileHelper::LoadFileToString(Skill, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Skills/unreal-modeling/SKILL.md")))))
	{
		return false;
	}
	// Every whole plan the skill shows checks and builds as written.
	int32 Plans = 0;
	int32 From = 0;
	while (true)
	{
		const int32 Start = Skill.Find(TEXT("```json"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (Start == INDEX_NONE) break;
		const int32 End = Skill.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start + 7);
		if (End == INDEX_NONE) break;
		From = End + 3;
		const FString Json = Skill.Mid(Start + 7, End - Start - 7);
		if (!Json.Contains(TEXT("\"Operations\""))) continue;
		FHyperAIModelPlanRequest Request;
		if (!TestTrue(TEXT("an example reads as a plan: ") + Json.Left(80), FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Request))) continue;
		Request.bDryRun = true;
		const FHyperAIModelPlanReport Report = C::ApplyPlan(Request);
		TestTrue(TEXT("the skill's example builds: ") + Report.Diagnostic, Report.bOk && !Report.Meshes.IsEmpty());
		for (const FHyperAIModelMeshStats& Mesh : Report.Meshes)
		{
			TestTrue(FString::Printf(TEXT("the example's %s is watertight (%d open edges)"), *Mesh.Name, Mesh.OpenEdges), Mesh.bClosed);
		}
		++Plans;
	}
	TestTrue(TEXT("the skill shows plans"), Plans >= 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
