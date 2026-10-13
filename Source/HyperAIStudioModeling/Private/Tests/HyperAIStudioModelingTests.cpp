// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioModelingGate.h"
#include "HyperAIStudioModelingToolset.h"
#include "HyperAIStudioOperationWait.h"
#include "HyperAIStudioResultPreview.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSourceControl.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Interfaces/IPluginManager.h"
#include "JsonObjectConverter.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
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

	/** Runs a request with approval off, as an agent with one-shot edits allowed would. */
	FHyperAIModelPlanReport RunWith(const FHyperAIModelPlanRequest& Request)
	{
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bApproval = Settings->bRequireApprovalForAgentEdits;
		Settings->bRequireApprovalForAgentEdits = false;
		FHyperAIModelPlanReport Report = C::ApplyPlan(Request);
		Settings->bRequireApprovalForAgentEdits = bApproval;
		return Report;
	}

	FHyperAIModelPlanReport Run(TArray<FHyperAIModelOp> Ops, const bool bDryRun)
	{
		FHyperAIModelPlanRequest Request;
		Request.Operations = MoveTemp(Ops);
		Request.bDryRun = bDryRun;
		return RunWith(Request);
	}

	/** A mesh's vertex positions by vertex id, as the plan builds it in memory. */
	bool Vertices(const TArray<FHyperAIModelOp>& Ops, const TCHAR* Name, TArray<FVector>& OutPositions, FString& OutError)
	{
		namespace Gate = HyperAIStudio::Modeling::Gate;
		OutPositions.Reset();
		Gate::FPlan Plan;
		Gate::FBuilt Built;
		if (!Gate::Check(Ops, Plan, OutError) || !Gate::Build(Plan, Built, OutError)) return false;
		const UDynamicMesh* Mesh = Built.Find(Name);
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("no mesh %s"), Name);
			return false;
		}
		Mesh->ProcessMesh([&OutPositions](const FDynamicMesh3& Read)
		{
			for (const int32 Vid : Read.VertexIndicesItr()) OutPositions.Add(Read.GetVertex(Vid));
		});
		return true;
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
			// Writes draw a preview of each asset: those go with it.
			IFileManager::Get().Delete(*FHyperAIStudioResultPreview::GetAssetPreviewPath(Asset.GetObjectPathString()), false, false, true);
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
	TestEqual(TEXT("eight tools"), C::GetToolNames(), TArray<FString>({TEXT("hyper_model_inspect"), TEXT("hyper_model_apply_plan"), TEXT("hyper_model_validate"),
		TEXT("hyper_model_handoff"), TEXT("hyper_model_uv_check"), TEXT("hyper_model_uv_apply"), TEXT("hyper_model_bake"), TEXT("hyper_model_prepare")}));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingLongPathsTest, "HyperAIStudio.NativeTools.Modeling.LongAndMalformedPaths", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingLongPathsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	// Each of these once reached an FName past 1023 characters, or a load of a package with a double slash, and stopped
	// the editor; now each is refused before any lookup.
	const FString Blob = TEXT("/Game/") + FString::ChrN(1100, TEXT('a'));
	FHyperAIModelInspectRequest Folder;
	Folder.Folder = Blob;
	const FHyperAIModelInspectReport Listed = C::Inspect(Folder);
	TestTrue(TEXT("inspect refuses a Folder past an FName's length: ") + Listed.Diagnostic.Left(200), !Listed.bOk && Listed.Diagnostic.Contains(TEXT("characters long")));
	FHyperAIModelInspectRequest Asset;
	Asset.Asset = Blob;
	Asset.bPreview = true;
	TestTrue(TEXT("and an Asset to preview"), !C::Inspect(Asset).bOk);
	Asset.Asset = TEXT("/Game//SM_Nope");
	TestTrue(TEXT("and an Asset with a double slash"), C::Inspect(Asset).Diagnostic.Contains(TEXT("double slash")));
	FHyperAIModelValidateRequest Validate;
	Validate.Assets = {Blob, TEXT("/Game//SM_Nope.SM_Nope"), Blob + TEXT(".") + FString::ChrN(1100, TEXT('b'))};
	const FHyperAIModelValidateReport Checked = C::Validate(Validate);
	TestEqual(TEXT("validate reports each as no static mesh"), Checked.Issues.FilterByPredicate([](const FHyperAIModelIssue& Issue) { return Issue.Code == TEXT("not_a_static_mesh"); }).Num(), 3);
	TestFalse(TEXT("and fails"), Checked.bOk);
	const FHyperAIModelOp Box = Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"));
	TestTrue(TEXT("a plan refuses a write_asset path past an FName's length, naming the field"), Refused(Run({Box, Write(TEXT("Block"), Blob)}, true), TEXT("Operations[1].Asset"),
		TEXT("characters long")));
	TestTrue(TEXT("and one with a double slash"), Refused(Run({Box, Write(TEXT("Block"), TEXT("/Game//SM_Block"))}, true), TEXT("Op 1"), TEXT("double slash")));
	TestTrue(TEXT("and a material path past an FName's length"), Refused(Run({Box, Write(TEXT("Block"), TestFolder() + TEXT("/SM_Block"), *(TEXT("Materials=0:") + Blob))}, true),
		TEXT("Op 1"), TEXT("characters long")));
	TestTrue(TEXT("and a material path with a double slash"), Refused(Run({Box, Write(TEXT("Block"), TestFolder() + TEXT("/SM_Block"), TEXT("Materials=0:/Game//M_Nope"))}, true),
		TEXT("Op 1"), TEXT("double slash")));
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
	// Extruding every face on its own makes seven triangles of each. Check cannot tell what a Select picks, so it guesses
	// two: 64 copies of the result pass Check, but are refused against the meshes as built before they are made.
	FHyperAIModelOp Copies = Op(TEXT("array.linear"), TEXT("Many"), TEXT("Count=64"));
	Copies.Source = TEXT("Block");
	Copies.Location = TEXT("200,0,0");
	FHyperAIModelOp Each = Op(TEXT("extrude"), TEXT("Block"), TEXT("Distance=2; Area=face"));
	Each.Select = TEXT("box:-60,-60,-10,60,60,110");
	const FHyperAIModelOp Box10 = Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=10"));
	const FHyperAIModelPlanReport Grown = Run({Box10, Each, Copies}, true);
	TestTrue(TEXT("copies past the cap are refused before they are made: ") + Grown.Diagnostic, Refused(Grown, TEXT("Op 2"), TEXT("from the meshes as built")));
	// With no Select every face is picked, and Check counts the seven.
	const FHyperAIModelPlanReport Counted = Run({Box10, Op(TEXT("extrude"), TEXT("Block"), TEXT("Distance=2; Area=face")), Copies}, true);
	TestTrue(TEXT("without a Select, Check predicts seven triangles a face: ") + Counted.Diagnostic, Refused(Counted, TEXT("Op 2"), TEXT("would make about 650496 triangles")));
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
	// Verify is the backstop: a write that left them behind fails, rather than passing as the plan's mesh.
	FHyperAIModelAssetResult Expected;
	Expected.Path = Path;
	Expected.Triangles = SourceTriangles(*Mesh);
	FString Error;
	const bool bVerified = HyperAIStudio::Modeling::Gate::Verify({Mesh}, {Expected}, Error);
	TestTrue(TEXT("verify refuses a mesh that still holds them: ") + Error, !bVerified && Error.Contains(TEXT("holds 3 source LOD(s) and a hi-res source")));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingFingerprintTest, "HyperAIStudio.NativeTools.Modeling.Fingerprint", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingFingerprintTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	namespace Gate = HyperAIStudio::Modeling::Gate;
	const FString Folder = TestFolder();
	const FString PathA = Folder + TEXT("/SM_A");
	const FHyperAIModelPlanReport Made = Run({Sized(TEXT("add.box"), TEXT("Box"), TEXT("100")), Write(TEXT("Box"), PathA), Write(TEXT("Box"), Folder + TEXT("/SM_B"))}, false);
	UStaticMesh* A = Made.bOk ? LoadMesh(PathA) : nullptr;
	UStaticMesh* B = Made.bOk ? LoadMesh(Folder + TEXT("/SM_B")) : nullptr;
	if (!TestTrue(TEXT("two identical boxes are written: ") + Made.Diagnostic, A && B))
	{
		DeleteFolder(Folder);
		return false;
	}
	const Gate::FGeometryPrint PrintA = Gate::Fingerprint(*A);
	const Gate::FGeometryPrint PrintB = Gate::Fingerprint(*B);
	TestTrue(TEXT("a box has a fingerprint: ") + PrintA.Hash + PrintA.Why, PrintA.Hash.StartsWith(TEXT("geo-")) && PrintA.Hash.Len() == 20 && PrintA.Triangles == 12);
	TestEqual(TEXT("the same geometry at another path has the same one"), PrintB.Hash, PrintA.Hash);
	// Committed again unchanged, as an editor round trip does: content, not the bulk data's id.
	A->GetMeshDescription(0);
	A->CommitMeshDescription(0);
	TestEqual(TEXT("committing the same source again keeps it"), Gate::Fingerprint(*A).Hash, PrintA.Hash);
	FHyperAIModelInspectRequest Look;
	Look.Asset = PathA;
	const FHyperAIModelInspectReport Read = C::Inspect(Look);
	TestTrue(TEXT("inspect reports it: ") + Read.Fingerprint, Read.bOk && Read.Fingerprint == PrintA.Hash
		&& Read.Details.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Fingerprint geo-")); }));
	const FHyperAIModelPlanReport Replaced = Run({Sized(TEXT("add.sphere"), TEXT("Ball"), TEXT("100"), TEXT("Steps=24")), Write(TEXT("Ball"), PathA, TEXT("Replace=true"))}, false);
	const Gate::FGeometryPrint After = Gate::Fingerprint(*A);
	TestTrue(TEXT("a Replace write changes it: ") + Replaced.Diagnostic, Replaced.bOk && !After.Hash.IsEmpty() && After.Hash != PrintA.Hash && After.Triangles != 12);
	TestEqual(TEXT("and inspect reads the new one"), C::Inspect(Look).Fingerprint, After.Hash);

	// Hand edits that leave positions, the first UV set and the slots alone still change it, each committed on B as an
	// Accept commits it. The layers they change exist first, so only the values differ.
	{
		FMeshDescription& Source = *B->GetMeshDescription(0);
		if (!Source.PolygonAttributes().HasAttribute(TEXT("PolyTriGroups")))
		{
			Source.PolygonAttributes().RegisterAttribute<int32>(TEXT("PolyTriGroups"), 1, 0, EMeshAttributeFlags::AutoGenerated);
		}
		Source.TriangleAttributes().RegisterAttribute<int32>(TEXT("HyperAITestGroups"), 1, 0, EMeshAttributeFlags::AutoGenerated);
		FStaticMeshAttributes(Source).GetVertexInstanceUVs().SetNumChannels(2);
		B->CommitMeshDescription(0);
	}
	auto Edit = [this, B](const TCHAR* What, TFunctionRef<void(FMeshDescription&)> Change)
	{
		const FString Before = Gate::Fingerprint(*B).Hash;
		Change(*B->GetMeshDescription(0));
		B->CommitMeshDescription(0);
		const FString Now = Gate::Fingerprint(*B).Hash;
		TestTrue(FString::Printf(TEXT("%s changes it: %s to %s"), What, *Before, *Now), !Before.IsEmpty() && !Now.IsEmpty() && Now != Before);
	};
	Edit(TEXT("new polygroups alone, as Generate PolyGroups writes them"), [](FMeshDescription& Source)
	{
		TPolygonAttributesRef<int32> Groups = Source.PolygonAttributes().GetAttributesRef<int32>(TEXT("PolyTriGroups"));
		int32 Group = 100;
		for (const FPolygonID Polygon : Source.Polygons().GetElementIDs()) Groups.Set(Polygon, Group++);
	});
	Edit(TEXT("a polygroup layer alone"), [](FMeshDescription& Source)
	{
		TTriangleAttributesRef<int32> Layer = Source.TriangleAttributes().GetAttributesRef<int32>(TEXT("HyperAITestGroups"));
		for (const FTriangleID Triangle : Source.Triangles().GetElementIDs()) Layer.Set(Triangle, 7);
	});
	Edit(TEXT("the second UV set alone, as AutoUV into channel 1 writes it"), [](FMeshDescription& Source)
	{
		TVertexInstanceAttributesRef<FVector2f> UVs = FStaticMeshAttributes(Source).GetVertexInstanceUVs();
		for (const FVertexInstanceID Corner : Source.VertexInstances().GetElementIDs()) UVs.Set(Corner, 1, FVector2f(0.5f, 0.25f));
	});
	Edit(TEXT("normals alone"), [](FMeshDescription& Source)
	{
		TVertexInstanceAttributesRef<FVector3f> Normals = FStaticMeshAttributes(Source).GetVertexInstanceNormals();
		for (const FVertexInstanceID Corner : Source.VertexInstances().GetElementIDs()) Normals.Set(Corner, -Normals.Get(Corner));
	});
	Edit(TEXT("hard edges alone"), [](FMeshDescription& Source)
	{
		TEdgeAttributesRef<bool> Hard = FStaticMeshAttributes(Source).GetEdgeHardnesses();
		for (const FEdgeID Edge : Source.Edges().GetElementIDs()) Hard.Set(Edge, !Hard.Get(Edge));
	});
	Edit(TEXT("vertex colours alone"), [](FMeshDescription& Source)
	{
		TVertexInstanceAttributesRef<FVector4f> Colors = FStaticMeshAttributes(Source).GetVertexInstanceColors();
		for (const FVertexInstanceID Corner : Source.VertexInstances().GetElementIDs()) Colors.Set(Corner, FVector4f(1.f, 0.f, 0.f, 1.f));
	});

	// A hi-res source counts, as Modeling Mode edits it, and is let go again: a fingerprint leaves nothing unpacked.
	const FString WithoutHiRes = Gate::Fingerprint(*B).Hash;
	B->CreateHiResMeshDescription(*B->GetMeshDescription(0));
	B->CommitHiResMeshDescription();
	B->ClearHiResMeshDescription();
	const Gate::FGeometryPrint WithHiRes = Gate::Fingerprint(*B);
	TestTrue(TEXT("a hi-res source is hashed and counted: ") + WithHiRes.Hash + WithHiRes.Why, !WithHiRes.Hash.IsEmpty() && WithHiRes.Hash != WithoutHiRes
		&& WithHiRes.HiResTriangles == 12 && WithHiRes.Triangles == 12);
	TestNull(TEXT("and is not left unpacked"), B->GetHiResSourceModel().GetCachedMeshDescription());
	// Its saved count is read before anything is unpacked: one past the cap is counted, never unpacked or hashed.
	B->GetHiResSourceModel().CacheMeshDescriptionTrianglesCount = C::MaxFingerprintTriangles + 1;
	const Gate::FGeometryPrint Huge = Gate::Fingerprint(*B);
	TestTrue(TEXT("a hi-res source past the cap has no fingerprint: ") + Huge.Why, Huge.Hash.IsEmpty() && Huge.Why.Contains(TEXT("over"))
		&& Huge.HiResTriangles == C::MaxFingerprintTriangles + 1);
	TestNull(TEXT("and is never unpacked"), B->GetHiResSourceModel().GetCachedMeshDescription());
	FHyperAIModelInspectRequest LookB;
	LookB.Asset = Folder + TEXT("/SM_B");
	const FHyperAIModelInspectReport ReadB = C::Inspect(LookB);
	TestTrue(TEXT("inspect says why, with both counts: ") + FString::Join(ReadB.Details, TEXT(" | ")), ReadB.Fingerprint.IsEmpty()
		&& ReadB.Details.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("No fingerprint: over")) && Line.Contains(TEXT("hi-res source")); }));
	TestNull(TEXT("inspect does not unpack it either"), B->GetHiResSourceModel().GetCachedMeshDescription());
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingSubdivideTest, "HyperAIStudio.NativeTools.Modeling.Subdivide", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingSubdivideTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	namespace Gate = HyperAIStudio::Modeling::Gate;
	// A box of 54 quad groups (Steps is vertices along an edge: 4 make 3 quads): each quad's 4 corners make 4 quads at
	// level 1, and 4 times as many at level 2.
	const FHyperAIModelOp Quads = Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=4; Groups=quad"));
	const TArray<FHyperAIModelOp> Cushion = {Quads, Op(TEXT("subdivide"), TEXT("Block"), TEXT("Level=2"))};
	const FHyperAIModelPlanReport Smooth = Run(Cushion, true);
	const FHyperAIModelMeshStats* Block = Find(Smooth, TEXT("Block"));
	if (!TestTrue(TEXT("a box of quad groups subdivides: ") + Smooth.Diagnostic, Smooth.bOk && Block != nullptr)) return false;
	TestEqual(TEXT("into the triangles predicted: 2 x 54 quads x 4 corners x 4"), Block->Triangles, 1728);
	TestTrue(TEXT("closed, in one piece, with its UVs"), Block->bClosed && Block->Components == 1 && Block->UVSets >= 1);
	TestTrue(TEXT("its flat face centers stay where they were: ") + Block->Size, Parse(Block->Size).Equals(FVector(100.0), 0.5));
	TArray<FVector> Points;
	FString Error;
	if (TestTrue(TEXT("its vertices read: ") + Error, Vertices(Cushion, TEXT("Block"), Points, Error)))
	{
		double Farthest = 0.0;
		for (const FVector& Point : Points) Farthest = FMath::Max(Farthest, FMath::Abs(Point.X) + FMath::Abs(Point.Y) + FMath::Abs(Point.Z - 50.0));
		TestTrue(FString::Printf(TEXT("and its corners are rounded off (%.1f from the center along a diagonal, where a sharp corner is 150)"), Farthest), Farthest < 140.0);
	}
	// A box of six face groups is a cube cage: it smooths into a pebble.
	const FHyperAIModelPlanReport Pebble = Run({Sized(TEXT("add.box"), TEXT("Block"), TEXT("100")), Op(TEXT("subdivide"), TEXT("Block"), TEXT("Level=2"))}, true);
	const FHyperAIModelMeshStats* Round = Find(Pebble, TEXT("Block"));
	TestTrue(TEXT("a box of face groups smooths into a pebble: ") + (Round ? Round->Size : Pebble.Diagnostic), Round && Round->Triangles == 192 && Round->bClosed
		&& Parse(Round->Size).GetMax() < 95.0);
	// Groups of one triangle are polygons too: each makes three quads.
	const FHyperAIModelOp Ball = Sized(TEXT("add.sphere"), TEXT("Ball"), TEXT("100"), TEXT("Steps=8; Groups=quad"));
	Gate::FPlan Plan;
	Gate::FBuilt Built;
	if (TestTrue(TEXT("a sphere builds: ") + Error, Gate::Check({Ball}, Plan, Error) && Gate::Build(Plan, Built, Error)))
	{
		UDynamicMesh* Sphere = Built.Find(TEXT("Ball"));
		const Gate::FCage Cage = Gate::ReadCage(*Sphere, TEXT("Ball"));
		TestTrue(TEXT("its quad groups and its poles' triangles are a cage: ") + Cage.Problem, Cage.Problem.IsEmpty() && Cage.Polygons > 0);
		const FHyperAIModelPlanReport Once = Run({Ball, Op(TEXT("subdivide"), TEXT("Ball"), TEXT("Level=1"))}, true);
		const FHyperAIModelMeshStats* Smoothed = Find(Once, TEXT("Ball"));
		TestTrue(FString::Printf(TEXT("and it subdivides into 2 x its %lld corners: %s"), Cage.Corners, *Once.Diagnostic), Smoothed && Smoothed->Triangles == 2 * Cage.Corners);
		int32 Triangles = 0;
		Sphere->EditMesh([&Triangles](FDynamicMesh3& Edit)
		{
			for (const int32 Tid : Edit.TriangleIndicesItr())
			{
				Edit.SetTriangleGroup(Tid, Edit.AllocateTriangleGroup());
				++Triangles;
			}
		});
		const Gate::FCage Each = Gate::ReadCage(*Sphere, TEXT("Ball"));
		TestTrue(FString::Printf(TEXT("a group per triangle is a cage of triangles, 3 corners each (%lld for %d)"), Each.Corners, Triangles), Each.Problem.IsEmpty()
			&& Each.Polygons == Triangles && Each.Corners == 3 * Triangles);
	}
	// Budgets: counted from the cage before OpenSubdiv runs, and Level stays within 3. 63 x 63 quads a side make 23814.
	const FHyperAIModelOp Dense = Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=64; Groups=quad"));
	const FHyperAIModelPlanReport Heavy = Run({Dense, Op(TEXT("subdivide"), TEXT("Block"), TEXT("Level=3"))}, true);
	TestTrue(TEXT("too many levels for the cage are refused before they run: ") + Heavy.Diagnostic, Refused(Heavy, TEXT("Op 1"), TEXT("its cage of 23814 polygons would make 3048192 triangles"))
		&& Heavy.Diagnostic.Contains(TEXT("lower its Level")) && Heavy.Meshes.IsEmpty());
	// Two such boxes are too many even at Level 1, where lowering it is no way out.
	FHyperAIModelOp Second = Dense;
	Second.Location = TEXT("200,0,0");
	const FHyperAIModelPlanReport Once = Run({Dense, Second, Op(TEXT("subdivide"), TEXT("Block"))}, true);
	TestTrue(TEXT("a cage too big at Level 1 asks for a coarser cage: ") + Once.Diagnostic, Refused(Once, TEXT("Op 2"), TEXT("would make 381024 triangles at Level 1"))
		&& Once.Diagnostic.Contains(TEXT("coarser cage")) && !Once.Diagnostic.Contains(TEXT("lower its Level")));
	// A dense box of six face groups is a cube cage all the same: Check does not guess from its triangles, which would
	// refuse 4^3 times them, and the result fits an asset without Nanite.
	const FHyperAIModelPlanReport Coarse = Run({Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=20")), Op(TEXT("subdivide"), TEXT("Block"), TEXT("Level=3")),
		Write(TEXT("Block"), TestFolder() + TEXT("/SM_Pebble"))}, true);
	const FHyperAIModelMeshStats* Pebbled = Find(Coarse, TEXT("Block"));
	TestTrue(TEXT("a coarse cage on many triangles subdivides at Level 3: ") + Coarse.Diagnostic, Coarse.bOk && Pebbled && Pebbled->Triangles == 768 && Coarse.Assets.Num() == 1);
	TestTrue(TEXT("a Level past 3 is refused"), Refused(Run({Quads, Op(TEXT("subdivide"), TEXT("Block"), TEXT("Level=4"))}, true), TEXT("Op 1"), TEXT("Level is a whole number from 1 to 3")));
	// Groups that are no cage are refused, naming what fixes them.
	const FHyperAIModelPlanReport Single = Run({Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Groups=single")), Op(TEXT("subdivide"), TEXT("Block"))}, true);
	TestTrue(TEXT("one group over a closed box has no border: ") + Single.Diagnostic, Refused(Single, TEXT("Op 1"), TEXT("no border")) && Single.Diagnostic.Contains(TEXT("Groups=quad")));
	const FHyperAIModelPlanReport Band = Run({Sized(TEXT("add.cylinder"), TEXT("Post"), TEXT("40,40,100")), Op(TEXT("subdivide"), TEXT("Post"))}, true);
	TestTrue(TEXT("a cylinder's side of one face group is a band: ") + Band.Diagnostic, Refused(Band, TEXT("Op 1"), TEXT("Groups=quad")));
	const TArray<FHyperAIModelOp> Voxels = {Sized(TEXT("add.box"), TEXT("Block"), TEXT("100")), Op(TEXT("solidify"), TEXT("Block"), TEXT("Resolution=32")),
		Op(TEXT("subdivide"), TEXT("Block"))};
	const FHyperAIModelPlanReport Bare = Run(Voxels, true);
	TestTrue(TEXT("a voxel surface has no cage of its own: ") + Bare.Diagnostic, Refused(Bare, TEXT("Op 2"), TEXT("Cage=quads")));
	TArray<FHyperAIModelOp> Paired = Voxels;
	Paired[2].Params = TEXT("Cage=quads");
	const FHyperAIModelPlanReport Solid = Run({Voxels[0], Voxels[1]}, true);
	const FHyperAIModelPlanReport Caged = Run(Paired, true);
	const FHyperAIModelMeshStats* Before = Find(Solid, TEXT("Block"));
	const FHyperAIModelMeshStats* After = Find(Caged, TEXT("Block"));
	TestTrue(TEXT("Cage=quads pairs its triangles into one, and it subdivides: ") + Caged.Diagnostic, Before && After && After->bClosed && After->Triangles > Before->Triangles);
	const FHyperAIModelInspectReport Inspected = C::Inspect(FHyperAIModelInspectRequest());
	TestTrue(TEXT("inspect lists subdivide with its Level and Cage"), Inspected.Kinds.ContainsByPredicate([](const FString& Line)
		{
			return Line.StartsWith(TEXT("subdivide:")) && Line.Contains(TEXT("Level 1-3 =1")) && Line.Contains(TEXT("Cage groups|quads"));
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingOffsetFacesTest, "HyperAIStudio.NativeTools.Modeling.OffsetFaces", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingOffsetFacesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FHyperAIModelOp Box = Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"));
	auto Offset = [](const TCHAR* Select, const TCHAR* Params)
	{
		FHyperAIModelOp Out = Op(TEXT("offset_faces"), TEXT("Block"), Params);
		Out.Select = Select;
		return Out;
	};
	const FHyperAIModelPlanReport Raised = Run({Box, Offset(TEXT("top"), TEXT("Distance=25"))}, true);
	const FHyperAIModelMeshStats* Up = Find(Raised, TEXT("Block"));
	if (!TestTrue(TEXT("the top of a box moves up: ") + Raised.Diagnostic, Raised.bOk && Up != nullptr)) return false;
	TestTrue(TEXT("by exactly Distance: ") + Up->Max, Parse(Up->Max).Equals(FVector(50.0, 50.0, 125.0), 0.05) && Parse(Up->Min).Equals(FVector(-50.0, -50.0, 0.0), 0.05));
	TestTrue(FString::Printf(TEXT("with side walls, two triangles to each of its four edges (%d)"), Up->Triangles), Up->Triangles == 20 && Up->bClosed && Up->Components == 1);
	// A negative Distance sinks the faces into the mesh: walls from the rim, which stays, down to them.
	const TArray<FHyperAIModelOp> Pocket = {Box, Offset(TEXT("top"), TEXT("Distance=-30"))};
	const FHyperAIModelPlanReport Sunk = Run(Pocket, true);
	const FHyperAIModelMeshStats* Down = Find(Sunk, TEXT("Block"));
	TArray<FVector> Points;
	FString Error;
	TestTrue(TEXT("a negative Distance sinks the top 30 cm below its rim: ") + (Down ? Down->Max : Sunk.Diagnostic), Down && FMath::IsNearlyEqual(Parse(Down->Max).Z, 100.0, 0.05)
		&& Down->Triangles == 20 && Down->bClosed && Vertices(Pocket, TEXT("Block"), Points, Error)
		&& Points.FilterByPredicate([](const FVector& Point) { return FMath::IsNearlyEqual(Point.Z, 70.0, 0.01); }).Num() == 4);
	// No Select: the whole closed box swells with its faces kept parallel, and keeps its six groups.
	const FHyperAIModelOp Swell = Offset(TEXT(""), TEXT("Distance=10"));
	const FHyperAIModelPlanReport Swollen = Run({Box, Swell}, true);
	const FHyperAIModelMeshStats* Big = Find(Swollen, TEXT("Block"));
	TestTrue(TEXT("with no Select a closed box swells 10 cm on every side: ") + (Big ? Big->Size + TEXT(" from ") + Big->Min : Swollen.Diagnostic), Big
		&& Parse(Big->Size).Equals(FVector(120.0), 0.05) && Parse(Big->Min).Equals(FVector(-60.0, -60.0, -10.0), 0.05) && Big->Triangles == 12 && Big->bClosed);
	const FHyperAIModelPlanReport Kept = Run({Box, Swell, Op(TEXT("subdivide"), TEXT("Block"))}, true);
	const FHyperAIModelMeshStats* Grouped = Find(Kept, TEXT("Block"));
	TestTrue(TEXT("its faces keep their own groups, so it still subdivides as a cube: ") + Kept.Diagnostic, Grouped && Grouped->Triangles == 48);
	// A band around a round post moves out along the faces' own normals, where extrude would push it one way.
	FHyperAIModelOp Hoop = Op(TEXT("offset_faces"), TEXT("Post"), TEXT("Distance=2"));
	Hoop.Select = TEXT("box:-40,-40,9,40,40,21");
	const FHyperAIModelPlanReport Banded = Run({Sized(TEXT("add.cylinder"), TEXT("Post"), TEXT("60,60,90"), TEXT("Steps=24; HeightSteps=8")), Hoop}, true);
	const FHyperAIModelMeshStats* Post = Find(Banded, TEXT("Post"));
	const FVector Size = Post ? Parse(Post->Size) : FVector::ZeroVector;
	TestTrue(TEXT("a band around a cylinder stands out on every side: ") + (Post ? Post->Size : Banded.Diagnostic), Post && Size.X > 63.9 && Size.X < 64.2 && Size.Y > 63.9
		&& Size.Y < 64.2 && FMath::IsNearlyEqual(Size.Z, 90.0, 0.05) && Post->bClosed);
	TestTrue(TEXT("a Distance of 0 is refused"), Refused(Run({Box, Offset(TEXT("top"), TEXT("Distance=0"))}, true), TEXT("Op 1"), TEXT("Distance other than 0")));
	// Piece by piece: in one mesh a closed post swells and an open disc beside it thickens into a slab.
	const FHyperAIModelOp Lone = Sized(TEXT("add.cylinder"), TEXT("Post"), TEXT("40,40,100"), TEXT("Steps=16"));
	FHyperAIModelOp Disc = Sized(TEXT("add.disc"), TEXT("Post"), TEXT("60,60,0"), TEXT("Steps=16"));
	Disc.Location = TEXT("200,0,0");
	const FHyperAIModelOp Thicken = Op(TEXT("offset_faces"), TEXT("Post"), TEXT("Distance=2"));
	const FHyperAIModelPlanReport Alone = Run({Lone, Thicken}, true);
	const FHyperAIModelPlanReport Mixed = Run({Lone, Disc, Thicken}, true);
	const FHyperAIModelPlanReport Sheet = Run({Disc, Thicken}, true);
	const FHyperAIModelMeshStats* Swelled = Find(Alone, TEXT("Post"));
	const FHyperAIModelMeshStats* Both = Find(Mixed, TEXT("Post"));
	const FHyperAIModelMeshStats* Slab = Find(Sheet, TEXT("Post"));
	if (TestTrue(TEXT("a post, a disc, and both in one mesh offset: ") + Mixed.Diagnostic, Swelled && Both && Slab))
	{
		TestTrue(TEXT("a post alone swells, one closed piece: ") + Swelled->Size, Swelled->Components == 1 && Swelled->bClosed && Parse(Swelled->Size).Equals(FVector(44.0, 44.0, 104.0), 1.0));
		TestTrue(TEXT("a disc alone thickens into a closed slab: ") + Slab->Size, Slab->Components == 1 && Slab->bClosed && FMath::IsNearlyEqual(Parse(Slab->Size).Z, 2.0, 0.05));
		TestTrue(FString::Printf(TEXT("beside an open disc the post still swells, not into a hollow shell: %d pieces, %d triangles (%d + %d apart)"), Both->Components, Both->Triangles,
			Swelled->Triangles, Slab->Triangles), Both->Components == 2 && Both->bClosed && Both->Triangles == Swelled->Triangles + Slab->Triangles
			&& FMath::IsNearlyEqual(Parse(Both->Min).Z, Parse(Swelled->Min).Z, 0.01));
	}
	// Every face on its own: three quads of wall each, seven triangles where there was one. An open box (its top cut off)
	// is one sheet, where Geometry Script would flood the whole sheet once per face to see whether that face is all of it.
	FHyperAIModelOp Cut = Op(TEXT("plane_cut"), TEXT("Block"), TEXT("Fill=false"));
	Cut.Location = TEXT("0,0,99");
	const FHyperAIModelOp Faces = Op(TEXT("offset_faces"), TEXT("Block"), TEXT("Distance=2; Area=face"));
	const FHyperAIModelPlanReport Bare = Run({Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=40")), Cut}, true);
	const FHyperAIModelPlanReport Tiled = Run({Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=40")), Cut, Faces}, true);
	const FHyperAIModelMeshStats* Before = Find(Bare, TEXT("Block"));
	const FHyperAIModelMeshStats* After = Find(Tiled, TEXT("Block"));
	if (TestTrue(TEXT("an open box offsets face by face: ") + Tiled.Diagnostic, Before && After && Before->OpenEdges > 0 && Before->Triangles > 12000))
	{
		TestEqual(TEXT("into seven triangles a face"), After->Triangles, 7 * Before->Triangles);
		TestTrue(FString::Printf(TEXT("without flooding the sheet per face (%d ms for %d faces)"), Tiled.BuildMs, Before->Triangles), Tiled.BuildMs < 4000);
	}
	// Check counts seven a face when every face is picked; with a Select, the build counts the walls before making them.
	const FHyperAIModelOp Denser = Sized(TEXT("add.box"), TEXT("Block"), TEXT("100"), TEXT("Steps=64"));
	const FHyperAIModelPlanReport Predicted = Run({Denser, Cut, Faces}, true);
	TestTrue(TEXT("every face of a dense sheet on its own is refused before anything runs: ") + Predicted.Diagnostic, Refused(Predicted, TEXT("Op 2"), TEXT("would make about"))
		&& Predicted.Diagnostic.Contains(TEXT("Area=all")) && Predicted.Meshes.IsEmpty());
	FHyperAIModelOp Picked = Faces;
	Picked.Select = TEXT("box:-60,-60,-10,60,60,110");
	const FHyperAIModelPlanReport Counted = Run({Denser, Cut, Picked}, true);
	TestTrue(TEXT("and so are the faces a Select picks, counted before the walls are made: ") + Counted.Diagnostic, Refused(Counted, TEXT("Op 2"), TEXT("side walls along"))
		&& Counted.Meshes.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingWaveTest, "HyperAIStudio.NativeTools.Modeling.Wave", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingWaveTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	// 17 vertices along each edge, 12.5 cm apart: a 50 cm wave's crests and troughs fall on them.
	const FHyperAIModelOp Panel = Sized(TEXT("add.box"), TEXT("Panel"), TEXT("200,200,10"), TEXT("Steps=17"));
	const FHyperAIModelOp Wave = Op(TEXT("wave"), TEXT("Panel"), TEXT("Magnitude=8; Wavelength=50"));
	const FHyperAIModelPlanReport First = Run({Panel, Wave}, true);
	const FHyperAIModelPlanReport Again = Run({Panel, Wave}, true);
	const FHyperAIModelMeshStats* A = Find(First, TEXT("Panel"));
	const FHyperAIModelMeshStats* B = Find(Again, TEXT("Panel"));
	if (!TestTrue(TEXT("a wave runs over a panel: ") + First.Diagnostic, A && B)) return false;
	TestTrue(TEXT("the same every time"), First.PlanHash == Again.PlanHash && A->Min == B->Min && A->Max == B->Max && A->Size == B->Size);
	TestTrue(TEXT("its waves rise and fall Magnitude cm, the panel no longer or wider: ") + A->Min + TEXT(" to ") + A->Max, FMath::IsNearlyEqual(Parse(A->Size).X, 200.0, 0.05)
		&& FMath::IsNearlyEqual(Parse(A->Size).Y, 200.0, 0.05) && FMath::IsNearlyEqual(Parse(A->Max).Z, 18.0, 0.05) && FMath::IsNearlyEqual(Parse(A->Min).Z, -8.0, 0.05));
	TArray<FVector> Flat;
	TArray<FVector> Waved;
	FString Error;
	if (!TestTrue(TEXT("the vertices read: ") + Error, Vertices({Panel}, TEXT("Panel"), Flat, Error) && Vertices({Panel, Wave}, TEXT("Panel"), Waved, Error)
		&& Flat.Num() == Waved.Num() && Flat.Num() > 0))
	{
		return false;
	}
	double Most = 0.0;
	bool bAlongZ = true;
	for (int32 Index = 0; Index < Flat.Num(); ++Index)
	{
		const FVector Moved = Waved[Index] - Flat[Index];
		bAlongZ &= FMath::Abs(Moved.X) < 0.001 && FMath::Abs(Moved.Y) < 0.001 && FMath::Abs(Moved.Z) <= 8.001;
		Most = FMath::Max(Most, FMath::Abs(Moved.Z));
	}
	TestTrue(FString::Printf(TEXT("every vertex moves along Direction alone, within Magnitude (at most %.2f)"), Most), bAlongZ && Most > 6.0);
	// The top's middle vertex, by its id on the flat panel: the waves move vertices, never renumber them.
	const int32 Middle = Flat.IndexOfByPredicate([](const FVector& Point) { return Point.Equals(FVector(0.0, 0.0, 10.0), 0.01); });
	if (!TestTrue(TEXT("the panel has a vertex in the middle of its top"), Middle != INDEX_NONE)) return false;
	auto Height = [Middle](const TArray<FVector>& Points)
	{
		return Points.IsValidIndex(Middle) ? Points[Middle].Z : TNumericLimits<double>::Lowest();
	};
	// A ripple's rings spread from Location, which stays where it is.
	FHyperAIModelOp Ripple = Op(TEXT("wave"), TEXT("Panel"), TEXT("Mode=ripple; Magnitude=8; Wavelength=50"));
	Ripple.Location = TEXT("0,0,10");
	TArray<FVector> Rippled;
	if (TestTrue(TEXT("a ripple runs: ") + Error, Vertices({Panel, Ripple}, TEXT("Panel"), Rippled, Error) && Rippled.Num() == Flat.Num()))
	{
		bool bWithin = true;
		for (int32 Index = 0; Index < Flat.Num(); ++Index) bWithin &= FMath::Abs((Rippled[Index] - Flat[Index]).Z) <= 8.001 && FVector2D(Rippled[Index] - Flat[Index]).IsNearlyZero(0.001);
		TestTrue(TEXT("its center does not move"), FMath::IsNearlyEqual(Height(Rippled), 10.0, 0.01));
		TestTrue(TEXT("and every vertex moves along Z within Magnitude"), bWithin);
	}
	// A quarter turn of Phase lifts what Phase 0 leaves at its start.
	const FHyperAIModelOp Shifted = Op(TEXT("wave"), TEXT("Panel"), TEXT("Magnitude=8; Wavelength=50; Phase=90"));
	TArray<FVector> Turned;
	if (TestTrue(TEXT("a shifted wave runs: ") + Error, Vertices({Panel, Shifted}, TEXT("Panel"), Turned, Error)))
	{
		TestTrue(TEXT("Phase 0 leaves the panel's middle where it is"), FMath::IsNearlyEqual(Height(Waved), 10.0, 0.01));
		TestTrue(TEXT("Phase 90 lifts it Magnitude"), FMath::IsNearlyEqual(Height(Turned), 18.0, 0.01));
		TestNotEqual(TEXT("and hashes differently"), Run({Panel, Shifted}, true).PlanHash, First.PlanHash);
	}
	TestTrue(TEXT("a 3D wave is refused, naming the modes there are"), Refused(Run({Panel, Op(TEXT("wave"), TEXT("Panel"), TEXT("Mode=3d"))}, true), TEXT("Op 1"),
		TEXT("Mode is line, ripple")));
	TestTrue(TEXT("a line wave moving vertices the way it travels is refused"), Refused(Run({Panel, Op(TEXT("wave"), TEXT("Panel"), TEXT("Along=0,0,1"))}, true), TEXT("Op 1"),
		TEXT("parallel")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingPreviewsTest, "HyperAIStudio.NativeTools.Modeling.Previews", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingPreviewsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	namespace Gate = HyperAIStudio::Modeling::Gate;
	const FString Folder = TestFolder();
	TArray<FString> Made;
	ON_SCOPE_EXIT
	{
		Gate::SetPreviewsOffForTests(false);
		for (const FString& File : Made) IFileManager::Get().Delete(*File, false, false, true);
		DeleteFolder(Folder);
	};
	// A PNG of more than 1 KB with the mesh drawn in it: its middle is not the background its corner shows.
	auto IsPng = [](const FString& File)
	{
		TArray<uint8> Bytes;
		FImage Image;
		FImage Pixels;
		if (!FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.Num() <= 1024 || Bytes[1] != 'P' || Bytes[2] != 'N' || Bytes[3] != 'G'
			|| !FImageUtils::LoadImage(*File, Image) || Image.SizeX < 8 || Image.SizeY < 8)
		{
			return false;
		}
		Image.CopyTo(Pixels, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<const FColor> Colors = Pixels.AsBGRA8();
		const FColor Corner = Colors[0];
		const FColor Middle = Colors[int64(Pixels.SizeY / 2) * Pixels.SizeX + Pixels.SizeX / 2];
		return FMath::Abs(Corner.R - Middle.R) + FMath::Abs(Corner.G - Middle.G) + FMath::Abs(Corner.B - Middle.B) > 40;
	};
	const bool bCanRender = FApp::CanEverRender();
	const FHyperAIModelPlanReport Written = Run({Sized(TEXT("add.box"), TEXT("Crate"), TEXT("80")), Write(TEXT("Crate"), Folder + TEXT("/SM_Crate"))}, false);
	if (!TestTrue(TEXT("a crate is written: ") + Written.Diagnostic, Written.bOk && Written.Status == TEXT("completed") && Written.Assets.Num() == 1)) return false;
	const FHyperAIModelAssetResult& Crate = Written.Assets[0];
	Made.Add(Crate.PreviewPath);
	if (bCanRender)
	{
		TestEqual(TEXT("its preview is the asset's own PNG"), Crate.PreviewPath, FHyperAIStudioResultPreview::GetAssetPreviewPath(Crate.Path));
		TestTrue(TEXT("a PNG of more than 1 KB, the crate drawn in it: ") + Crate.PreviewPath, IsPng(Crate.PreviewPath));
		TestTrue(TEXT("and the report says so first: ") + Written.Diagnostic, Written.Diagnostic.StartsWith(TEXT("1 asset(s) written and saved. Previews: 1 of 1 drawn")));
	}
	else
	{
		AddInfo(TEXT("No renderer in this run: the write notes that it drew nothing."));
		TestTrue(TEXT("no preview, and a note says why"), Crate.PreviewPath.IsEmpty() && Written.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("without rendering")); }));
	}
	// An editor that cannot draw writes all the same, and says so; the crate's old picture goes, so its path never shows
	// the crate as it was.
	Gate::SetPreviewsOffForTests(true);
	const FHyperAIModelPlanReport Blind = Run({Sized(TEXT("add.box"), TEXT("Crate"), TEXT("60")), Write(TEXT("Crate"), Folder + TEXT("/SM_Crate"), TEXT("Replace=true"))}, false);
	Gate::SetPreviewsOffForTests(false);
	TestTrue(TEXT("without rendering the write still completes: ") + Blind.Diagnostic, Blind.bOk && Blind.Status == TEXT("completed") && Blind.Saved.Num() == 1);
	TestTrue(TEXT("with no preview, and a note saying why: ") + FString::Join(Blind.Notes, TEXT(" | ")), Blind.Assets.Num() == 1 && Blind.Assets[0].PreviewPath.IsEmpty()
		&& Blind.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("No previews: this editor runs without rendering")); })
		&& Blind.Diagnostic.Contains(TEXT("Previews: none")));
	TestFalse(TEXT("and no earlier PNG is left at its preview path"), IFileManager::Get().FileExists(*Gate::PreviewPathOf(Folder + TEXT("/SM_Crate"))));
	// A dry run draws only when asked, from a temporary mesh that leaves nothing behind.
	const FString DryPath = Folder + TEXT("/SM_Dry");
	FHyperAIModelPlanRequest Dry;
	Dry.Operations = {Sized(TEXT("add.sphere"), TEXT("Ball"), TEXT("60"), TEXT("Steps=16; Color=#3a5f9e")), Write(TEXT("Ball"), DryPath)};
	Dry.bDryRun = true;
	const FHyperAIModelPlanReport Unasked = RunWith(Dry);
	TestTrue(TEXT("a dry run draws nothing unless asked: ") + Unasked.Diagnostic, Unasked.bOk && Unasked.Assets.Num() == 1 && Unasked.Assets[0].PreviewPath.IsEmpty()
		&& !Unasked.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("preview")); }));
	Dry.bPreviewDryRun = true;
	const int32 UndoBefore = GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
	const FHyperAIModelPlanReport Asked = RunWith(Dry);
	if (Asked.Assets.Num() == 1) Made.Add(Asked.Assets[0].PreviewPath);
	TestTrue(TEXT("asked, the dry run still checks: ") + Asked.Diagnostic, Asked.bOk && Asked.Status == TEXT("checked") && Asked.Assets.Num() == 1);
	if (bCanRender && Asked.Assets.Num() == 1)
	{
		TestTrue(TEXT("and draws the asset it would write: ") + Asked.Assets[0].PreviewPath + TEXT(" ") + FString::Join(Asked.Notes, TEXT(" | ")), IsPng(Asked.Assets[0].PreviewPath));
		TestNotEqual(TEXT("apart from that asset's own preview"), Asked.Assets[0].PreviewPath, FHyperAIStudioResultPreview::GetAssetPreviewPath(DryPath + TEXT(".SM_Dry")));
	}
	TestFalse(TEXT("writing no asset"), FPackageName::DoesPackageExist(DryPath) || FindPackage(nullptr, *DryPath) != nullptr);
	TestEqual(TEXT("and nothing in the undo history"), GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0, UndoBefore);
	// Inspect draws an existing asset on request.
	FHyperAIModelInspectRequest Look;
	Look.Folder = Folder;
	Look.Asset = Folder + TEXT("/SM_Crate");
	Look.bPreview = true;
	const FHyperAIModelInspectReport Seen = C::Inspect(Look);
	Made.Add(Seen.PreviewPath);
	TestTrue(TEXT("inspect reads the crate: ") + Seen.Diagnostic, Seen.bOk);
	if (bCanRender) TestTrue(TEXT("and draws it: ") + Seen.PreviewPath, IsPng(Seen.PreviewPath));
	Look.Asset.Reset();
	const FHyperAIModelInspectReport Nothing = C::Inspect(Look);
	TestTrue(TEXT("bPreview without an Asset is refused: ") + Nothing.Diagnostic, !Nothing.bOk && Nothing.Diagnostic.Contains(TEXT("give Asset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingApprovalPreviewsTest, "HyperAIStudio.NativeTools.Modeling.ApprovalNamesPreviews", HyperAIStudio::ModelingTests::Flags)

bool FHyperAIStudioModelingApprovalPreviewsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingTests;
	const FString Folder = TestFolder();
	const FString OperationId = TEXT("model-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bWasRequired = Settings->bRequireApprovalForAgentEdits;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bWasRequired;
		if (FHyperAIStudioApprovalGate::HasPending(OperationId)) FHyperAIStudioApprovalGate::Reject(OperationId, TEXT("Test finished."));
		DeleteFolder(Folder);
	};
	Settings->bRequireApprovalForAgentEdits = true;
	// Three assets with long names: their lines alone run past the 512 characters hyper_operation_wait shows.
	const TArray<FString> Names = {TEXT("SM_Approved_Crate_With_A_Long_Name"), TEXT("SM_Approved_Ball_With_A_Long_Name"), TEXT("SM_Approved_Post_With_A_Long_Name")};
	FHyperAIModelPlanRequest Request;
	Request.OperationId = OperationId;
	Request.Operations = {Sized(TEXT("add.box"), TEXT("Crate"), TEXT("80")), Sized(TEXT("add.sphere"), TEXT("Ball"), TEXT("60")), Sized(TEXT("add.cylinder"), TEXT("Post"), TEXT("20,20,120")),
		Write(TEXT("Crate"), Folder / Names[0]), Write(TEXT("Ball"), Folder / Names[1]), Write(TEXT("Post"), Folder / Names[2])};
	const FHyperAIModelPlanReport Waiting = C::ApplyPlan(Request);
	if (!TestEqual(TEXT("with approval on, the write waits: ") + Waiting.Diagnostic, Waiting.Status, FString(TEXT("awaiting_user_approval")))) return false;
	bool bNamed = Waiting.Assets.Num() == Names.Num();
	for (int32 Index = 0; bNamed && Index < Names.Num(); ++Index)
	{
		const FString ObjectPath = Folder / Names[Index] + TEXT(".") + Names[Index];
		bNamed = Waiting.Assets[Index].PreviewPath == FHyperAIStudioResultPreview::GetAssetPreviewPath(ObjectPath) && !IFileManager::Get().FileExists(*Waiting.Assets[Index].PreviewPath);
	}
	TestTrue(TEXT("naming each asset's preview PNG, the file the write will draw, before anything runs"), bNamed);
	TestTrue(TEXT("and saying where to look: ") + Waiting.Diagnostic, Waiting.Diagnostic.Contains(TEXT("Assets[].PreviewPath")));
	FString Error;
	if (!TestTrue(TEXT("approved, it writes: ") + Error, FHyperAIStudioApprovalGate::Approve(OperationId, Error))) return false;
	// What hyper_operation_wait answers the agent: the pack's activity entry, cut to 512 characters.
	const TSharedRef<TOptional<FHyperAIStudioEditOutcome>> Outcome = MakeShared<TOptional<FHyperAIStudioEditOutcome>>();
	HyperAIStudio::OperationWait::WhenDone(OperationId, 1000, [Outcome](const FHyperAIStudioEditOutcome& Done) { *Outcome = Done; });
	if (!TestTrue(TEXT("the wait answers at once, the edit being done"), Outcome->IsSet())) return false;
	const FHyperAIStudioEditOutcome& Done = Outcome->GetValue();
	TestTrue(TEXT("as completed: ") + Done.Status + TEXT(" ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("completed"));
	const bool bCanRender = FApp::CanEverRender();
	const FString Previews = bCanRender ? TEXT("Previews: 3 of 3 drawn") : TEXT("Previews: none");
	TestTrue(TEXT("its diagnostic leads with the previews: ") + Done.Diagnostic, Done.Diagnostic.StartsWith(TEXT("3 asset(s) written and saved. ") + Previews));
	if (bCanRender)
	{
		const bool bDrawn = !Waiting.Assets.ContainsByPredicate([](const FHyperAIModelAssetResult& Asset) { return !IFileManager::Get().FileExists(*Asset.PreviewPath); });
		TestTrue(TEXT("at the paths it named"), bDrawn);
	}
	for (const FHyperAIModelAssetResult& Asset : Waiting.Assets) IFileManager::Get().Delete(*Asset.PreviewPath, false, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
