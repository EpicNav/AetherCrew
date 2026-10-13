// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerStart.h"
#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionActor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioDestructionGate.h"
#include "HyperAIStudioDestructionToolset.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IPluginManager.h"
#include "JsonObjectConverter.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/ToolCallAsyncResult.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"

namespace HyperAIStudio::DestructionTests
{
	using C = FHyperAIStudioDestructionContracts;
	namespace Gate = HyperAIStudio::Destruction::Gate;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* const Cube = TEXT("/Engine/BasicShapes/Cube.Cube");
	/** The engine cube's own material is the world grid; the inside gets another. */
	const TCHAR* const InsideMaterial = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	FHyperAIDestructionOp Op(const TCHAR* Kind, const TCHAR* Params = TEXT(""))
	{
		FHyperAIDestructionOp Out;
		Out.Kind = Kind;
		Out.Params = Params;
		return Out;
	}

	FHyperAIDestructionOp Make(TArray<FString> Sources, const TCHAR* Place = TEXT(""), const TCHAR* Location = TEXT(""))
	{
		FHyperAIDestructionOp Out = Op(TEXT("make"));
		Out.Sources = MoveTemp(Sources);
		Out.Place = Place;
		Out.Location = Location;
		return Out;
	}

	FString TestAsset(const TCHAR* Name)
	{
		return FString::Printf(TEXT("/Game/HyperAITests/Destruction/%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	/** Runs a request with approval off and no pictures, as an agent with one-shot edits allowed would. */
	FHyperAIDestructionPlanReport Run(const FString& Asset, TArray<FHyperAIDestructionOp> Ops, const bool bDryRun, const bool bPreview = false)
	{
		FHyperAIDestructionPlanRequest Request;
		Request.Asset = Asset;
		Request.Operations = MoveTemp(Ops);
		Request.bDryRun = bDryRun;
		Request.bPreview = bPreview;
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bApproval = Settings->bRequireApprovalForAgentEdits;
		Settings->bRequireApprovalForAgentEdits = false;
		FHyperAIDestructionPlanReport Report = C::ApplyPlan(Request);
		Settings->bRequireApprovalForAgentEdits = bApproval;
		return Report;
	}

	bool Refused(const FHyperAIDestructionPlanReport& Report, const TCHAR* Says)
	{
		return !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Says);
	}

	FVector Vec(const FString& Text)
	{
		FVector Out = FVector::ZeroVector;
		Gate::ParseVector(Text, Out);
		return Out;
	}

	const FHyperAIDestructionLevel* LevelOf(const FHyperAIDestructionPlanReport& Report, const int32 Level)
	{
		return Report.Levels.IsValidIndex(Level) ? &Report.Levels[Level] : nullptr;
	}

	UGeometryCollection* LoadAsset(const FString& Path)
	{
		return FindObject<UGeometryCollection>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
	}

	AActor* FindByLabel(UWorld& World, const FString& Label)
	{
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel() == Label) return *It;
		}
		return nullptr;
	}

	/** The plan's undo steps taken back, whatever the test found. */
	void UndoAll(const int32 Steps)
	{
		for (int32 Index = 0; Index < Steps && GEditor; ++Index) GEditor->UndoTransaction();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDestructionParamsTest, "HyperAIStudio.NativeTools.Destruction.Params", HyperAIStudio::DestructionTests::Flags)

bool FHyperAIStudioDestructionParamsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DestructionTests;
	const Gate::FKindSpec* Uniform = Gate::FindKind(TEXT("fracture.uniform"));
	if (!TestNotNull(TEXT("fracture.uniform is a kind"), Uniform)) return false;
	Gate::FParams Params;
	TArray<FString> Fitted;
	FString Error;
	TestTrue(TEXT("Sites and Seed parse: ") + Error, Gate::ParseParams(*Uniform, TEXT("Sites=12; seed=7"), Params, Fitted, Error));
	TestEqual(TEXT("Sites is 12"), Params.Int(TEXT("Sites")), 12);
	TestEqual(TEXT("names match in any case"), Params.Int(TEXT("Seed")), 7);
	TestEqual(TEXT("defaults fill in: Level -1"), Params.Int(TEXT("Level")), -1);
	TestTrue(TEXT("and SplitIslands"), Params.Bool(TEXT("SplitIslands")));
	Fitted.Reset();
	TestTrue(TEXT("a near miss past the most is fitted: ") + Error, Gate::ParseParams(*Uniform, TEXT("Sites=600"), Params, Fitted, Error));
	TestEqual(TEXT("to the most, 500"), Params.Int(TEXT("Sites")), C::MaxSites);
	TestTrue(TEXT("and noted"), Fitted.Num() == 1 && Fitted[0] == TEXT("Sites 600 -> 500"));
	TestFalse(TEXT("an absurd count is refused"), Gate::ParseParams(*Uniform, TEXT("Sites=100000"), Params, Fitted, Error));
	TestTrue(TEXT("as far past the most: ") + Error, Error.Contains(TEXT("far past the most")));
	TestFalse(TEXT("Sites is required"), Gate::ParseParams(*Uniform, TEXT("Seed=3"), Params, Fitted, Error));
	TestTrue(TEXT("and the refusal says so: ") + Error, Error.Contains(TEXT("needs Sites")));
	TestFalse(TEXT("an unknown param is refused"), Gate::ParseParams(*Uniform, TEXT("Sites=4; Explode=true"), Params, Fitted, Error));
	TestTrue(TEXT("with the params it takes: ") + Error, Error.Contains(TEXT("Explode")) && Error.Contains(TEXT("Grout")));
	TestFalse(TEXT("a word is not a number"), Gate::ParseParams(*Uniform, TEXT("Sites=many"), Params, Fitted, Error));
	TestFalse(TEXT("a fraction is not an integer"), Gate::ParseParams(*Uniform, TEXT("Sites=4.5"), Params, Fitted, Error));
	const Gate::FKindSpec* Brick = Gate::FindKind(TEXT("fracture.brick"));
	TestFalse(TEXT("a choice outside its words is refused"), Brick && Gate::ParseParams(*Brick, TEXT("Bond=herringbone"), Params, Fitted, Error));
	TestTrue(TEXT("naming them: ") + Error, Error.Contains(TEXT("flemish")));

	// Sites: the same seed scatters the same sites, inside the bounds.
	const FBox Box(FVector(-50.0), FVector(50.0));
	const TArray<FVector> A = Gate::UniformSites(Box, 12, 7);
	const TArray<FVector> B = Gate::UniformSites(Box, 12, 7);
	const TArray<FVector> D = Gate::UniformSites(Box, 12, 8);
	TestEqual(TEXT("12 sites"), A.Num(), 12);
	TestTrue(TEXT("the same seed, the same sites"), A == B);
	TestFalse(TEXT("another seed, other sites"), A == D);
	TestTrue(TEXT("all inside the bounds"), !A.ContainsByPredicate([&Box](const FVector& Site) { return !Box.IsInsideOrOn(Site); }));
	const TArray<FVector> Rings = Gate::RadialSites(FVector(0.0, 0.0, 10.0), FVector::UpVector, 100.0, 4, 8, 0.0, 0.0, 1);
	TestEqual(TEXT("radial: rings x steps sites"), Rings.Num(), 32);
	TestTrue(TEXT("in the plane about the centre, the outer ring at the radius"), !Rings.ContainsByPredicate([](const FVector& Site)
		{
			return FMath::Abs(Site.Z - 10.0) > 0.01 || FVector::Dist2D(Site, FVector::ZeroVector) > 100.01;
		}) && FMath::IsNearlyEqual(FVector::Dist2D(Rings.Last(), FVector::ZeroVector), 100.0, 0.01));
	TestEqual(TEXT("clustered: clusters x sites each"), Gate::ClusteredSites(Box, 3, 5, 0.2, 1).Num(), 15);

	TArray<float> Thresholds;
	TestTrue(TEXT("thresholds parse"), Gate::ParseThresholds(TEXT("500000, 50000,5000"), Thresholds, Error) && Thresholds.Num() == 3 && Thresholds[1] == 50000.f);
	TestFalse(TEXT("a negative threshold is refused"), Gate::ParseThresholds(TEXT("100, -1"), Thresholds, Error));
	TArray<FTransform> Planes;
	TestTrue(TEXT("planes parse"), Gate::ParsePlanes(TEXT("0,0,50,0,0,1 | 0,0,0,1,0,0"), Planes, Error) && Planes.Num() == 2);
	TestTrue(TEXT("each through its point, facing its normal"), Planes.Num() == 2 && Planes[0].GetLocation().Equals(FVector(0.0, 0.0, 50.0))
		&& Planes[1].GetUnitAxis(EAxis::Z).Equals(FVector::ForwardVector, 1.e-4));
	TestFalse(TEXT("a zero normal is refused"), Gate::ParsePlanes(TEXT("0,0,0,0,0,0"), Planes, Error));
	TArray<TPair<float, FString>> Sizes;
	TestTrue(TEXT("sizes parse, smallest first"), Gate::ParseSizes(TEXT("100:box, 30:sphere"), Sizes, Error) && Sizes.Num() == 2 && Sizes[0].Key == 30.f && Sizes[0].Value == TEXT("sphere"));
	TestFalse(TEXT("an unknown shape is refused"), Gate::ParseSizes(TEXT("30:cone"), Sizes, Error));

	// Every kind describes its params for hyper_destruction_inspect.
	for (const Gate::FKindSpec& Spec : Gate::Kinds())
	{
		TestEqual(FString::Printf(TEXT("%s describes each param"), Spec.Kind), Gate::DescribeParams(Spec).Num(), Spec.Params.Num());
	}
	FHyperAIDestructionInspectRequest Kinds;
	const FHyperAIDestructionInspectReport Listed = C::Inspect(Kinds);
	TestTrue(TEXT("inspect without an asset lists the kinds: ") + Listed.Diagnostic, Listed.bOk && Listed.Kinds.Num() == Gate::Kinds().Num());
	const FHyperAIDestructionKind* Radial = Listed.Kinds.FindByPredicate([](const FHyperAIDestructionKind& Kind) { return Kind.Kind == TEXT("fracture.radial"); });
	TestTrue(TEXT("with their params, ranges and defaults"), Radial && Radial->Params.ContainsByPredicate([](const FString& Line)
		{
			return Line.StartsWith(TEXT("Rings: integer 1 to 20, default 4"));
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDestructionFractureCubeTest, "HyperAIStudio.NativeTools.Destruction.FractureCube", HyperAIStudio::DestructionTests::Flags)

bool FHyperAIStudioDestructionFractureCubeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DestructionTests;
	const FString Asset = TestAsset(TEXT("GC_Cube"));
	// The engine cube, 100 cm across, into 12 Voronoi cells from seed 7: a site inside a convex cell always meets the
	// cube, so 12 sites make exactly 12 pieces.
	const FHyperAIDestructionPlanReport Done = Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=12; Seed=7"))}, /*bDryRun=*/true);
	if (!TestTrue(TEXT("the dry run builds: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("checked"))) return false;
	TestEqual(TEXT("12 sites make 12 pieces"), Done.Leaves, 12);
	TestEqual(TEXT("under one cluster, the whole cube"), Done.Clusters, 1);
	TestEqual(TEXT("13 bones"), Done.Bones, 13);
	TestEqual(TEXT("in two levels"), Done.Levels.Num(), 2);
	const FHyperAIDestructionLevel* Pieces = LevelOf(Done, 1);
	TestTrue(TEXT("level 1 holds the 12 leaves"), Pieces && Pieces->Leaves == 12 && Pieces->Clusters == 0);
	TestTrue(TEXT("the pieces keep the cube's bounds: ") + Done.Size, Vec(Done.Size).Equals(FVector(100.0), 0.5));
	// 1 m3 at Unreal's default 2500 kg/m3.
	TestTrue(FString::Printf(TEXT("the mass is the cube's volume at 2500 kg/m3 (%.1f kg)"), Done.MassKg), FMath::IsNearlyEqual(Done.MassKg, 2500.f, 25.f));
	TestTrue(TEXT("a dry run makes nothing"), !LoadAsset(Asset) && !FPackageName::DoesPackageExist(Asset));
	const FHyperAIDestructionPlanReport Again = Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=12; Seed=7"))}, true);
	TestTrue(TEXT("the same seed cuts the same pieces"), Again.bOk && Again.Leaves == 12 && Again.Triangles == Done.Triangles
		&& LevelOf(Again, 1) && Pieces && LevelOf(Again, 1)->SmallestCm == Pieces->SmallestCm);
	const FHyperAIDestructionPlanReport Other = Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=12; Seed=8"))}, true);
	TestTrue(TEXT("another seed cuts other pieces"), Other.bOk && Other.Leaves == 12 && LevelOf(Other, 1) && Pieces && LevelOf(Other, 1)->SmallestCm != Pieces->SmallestCm);
	TestTrue(TEXT("the notes count the cut"), Done.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("1 piece(s) became 12 from 12 site(s)")); }));

	// The other cutters, on the same cube.
	const FHyperAIDestructionPlanReport Sliced = Run(Asset, {Make({Cube}), Op(TEXT("fracture.slice"), TEXT("X=1; Y=1; Z=1"))}, true);
	TestTrue(TEXT("one cut across each axis makes 8 pieces: ") + Sliced.Diagnostic, Sliced.bOk && Sliced.Leaves == 8);
	const FHyperAIDestructionPlanReport Planar = Run(Asset, {Make({Cube}), Op(TEXT("fracture.planar"), TEXT("Planes=0,0,0,0,0,1 | 0,0,0,1,0,0"))}, true);
	TestTrue(TEXT("two planes through the middle make 4 pieces: ") + Planar.Diagnostic, Planar.bOk && Planar.Leaves == 4);
	const FHyperAIDestructionPlanReport Bricks = Run(Asset, {Make({Cube}), Op(TEXT("fracture.brick"), TEXT("Length=50; Height=50; Depth=100; Bond=stack"))}, true);
	TestTrue(FString::Printf(TEXT("stacked bricks half its size cut it into 4 to 12 (%d): %s"), Bricks.Leaves, *Bricks.Diagnostic), Bricks.bOk && Bricks.Leaves >= 4 && Bricks.Leaves <= 12);
	const FHyperAIDestructionPlanReport Radial = Run(Asset, {Make({Cube}), Op(TEXT("fracture.radial"), TEXT("Rings=2; AngularSteps=6; Normal=0,0,1"))}, true);
	TestTrue(TEXT("a radial cut makes a piece per cell, at most 12: ") + Radial.Diagnostic, Radial.bOk && Radial.Leaves > 6 && Radial.Leaves <= 12);
	const FHyperAIDestructionPlanReport Clumps = Run(Asset, {Make({Cube}), Op(TEXT("fracture.clustered"), TEXT("Clusters=2; SitesPerCluster=5; Seed=3"))}, true);
	TestTrue(TEXT("clustered sites cut it into up to 10: ") + Clumps.Diagnostic, Clumps.bOk && Clumps.Leaves > 1 && Clumps.Leaves <= 10);
	const FHyperAIDestructionPlanReport Noisy = Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=6; Seed=7; Noise=3; NoiseSpacing=8"))}, true);
	TestTrue(FString::Printf(TEXT("noise on the cuts still makes about 6 pieces (%d): %s"), Noisy.Leaves, *Noisy.Diagnostic), Noisy.bOk && Noisy.Leaves >= 6 && Noisy.Leaves <= 8);
	TestTrue(TEXT("with more triangles on the cut faces than flat cuts"), Noisy.Triangles > Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=6; Seed=7"))}, true).Triangles);

	// The exploded preview, where this editor can draw.
	const FHyperAIDestructionPlanReport Drawn = Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=12; Seed=7"))}, true, /*bPreview=*/true);
	if (FApp::CanEverRender())
	{
		TestTrue(TEXT("the dry run draws an exploded PNG: ") + Drawn.PreviewPath, !Drawn.PreviewPath.IsEmpty() && IFileManager::Get().FileSize(*Drawn.PreviewPath) > 1000);
	}
	else
	{
		TestTrue(TEXT("without rendering the notes say why there is no picture"), Drawn.PreviewPath.IsEmpty()
			&& Drawn.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("No exploded preview")); }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDestructionClusterLevelsTest, "HyperAIStudio.NativeTools.Destruction.ClusterLevels", HyperAIStudio::DestructionTests::Flags)

bool FHyperAIStudioDestructionClusterLevelsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DestructionTests;
	const FString Asset = TestAsset(TEXT("GC_Levels"));
	const FHyperAIDestructionPlanReport Done = Run(Asset, {
		Make({Cube}),
		Op(TEXT("fracture.uniform"), TEXT("Sites=40; Seed=11")),
		Op(TEXT("cluster"), TEXT("Clusters=5; Connected=false")),
		Op(TEXT("damage"), TEXT("Thresholds=20000, 800"))}, true);
	if (!TestTrue(TEXT("the plan builds: ") + Done.Diagnostic, Done.bOk)) return false;
	TestEqual(TEXT("three levels: the cube, its chunks, their pieces"), Done.Levels.Num(), 3);
	const FHyperAIDestructionLevel* Chunks = LevelOf(Done, 1);
	const FHyperAIDestructionLevel* Pieces = LevelOf(Done, 2);
	TestTrue(FString::Printf(TEXT("level 1 holds about 5 chunks (%d)"), Chunks ? Chunks->Clusters : -1), Chunks && Chunks->Clusters >= 3 && Chunks->Clusters <= 5);
	TestTrue(TEXT("level 2 the 40 pieces"), Pieces && Pieces->Leaves == 40);
	TestEqual(TEXT("40 leaves in all"), Done.Leaves, 40);
	TestTrue(TEXT("the whole cube breaks at the first threshold"), LevelOf(Done, 0) && LevelOf(Done, 0)->DamageThreshold == 20000.f);
	TestTrue(TEXT("its chunks at the second"), Chunks && Chunks->DamageThreshold == 800.f);
	TestTrue(TEXT("a level of leaves holds no threshold"), Pieces && Pieces->DamageThreshold == 0.f);

	// A level deeper: the chunks themselves grouped is refused when there is nothing under them to group.
	const FHyperAIDestructionPlanReport Nothing = Run(Asset, {Make({Cube}), Op(TEXT("cluster"), TEXT("Clusters=3"))}, true);
	TestTrue(TEXT("clustering an unfractured cube is refused: ") + Nothing.Diagnostic, !Nothing.bOk && Nothing.Diagnostic.Contains(TEXT("no cluster at level 0")));

	// Small pieces merged into their neighbours.
	const FHyperAIDestructionPlanReport Raw = Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=60; Seed=5"))}, true);
	const float RawSmallest = LevelOf(Raw, 1) ? LevelOf(Raw, 1)->SmallestCm : 0.f;
	const FHyperAIDestructionPlanReport Tidy = Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=60; Seed=5")), Op(TEXT("cleanup"), TEXT("MinSize=20"))}, true);
	TestTrue(FString::Printf(TEXT("cleanup merges the pieces under 20 cm: %d left, the smallest %.1f cm"), Tidy.Leaves, LevelOf(Tidy, 1) ? LevelOf(Tidy, 1)->SmallestCm : -1.f),
		Tidy.bOk && Raw.Leaves == 60 && Tidy.Leaves < 60 && LevelOf(Tidy, 1) && LevelOf(Tidy, 1)->SmallestCm > RawSmallest);
	const FHyperAIDestructionPlanReport Removed = Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=60; Seed=5")), Op(TEXT("cleanup"), TEXT("MinSize=20; Mode=remove"))}, true);
	TestTrue(TEXT("or removes them, losing mass"), Removed.bOk && Removed.Leaves < 60 && Removed.MassKg < Tidy.MassKg);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDestructionRefusalsTest, "HyperAIStudio.NativeTools.Destruction.Refusals", HyperAIStudio::DestructionTests::Flags)

bool FHyperAIStudioDestructionRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DestructionTests;
	const FString Asset = TestAsset(TEXT("GC_Refused"));
	const auto Fracture = [](const TCHAR* Params) { return Op(TEXT("fracture.uniform"), Params); };
	TestTrue(TEXT("Engine content is refused"), Refused(Run(TEXT("/Engine/BasicShapes/GC_Cube"), {Make({Cube}), Fracture(TEXT("Sites=4"))}, false), TEXT("not project content")));
	TestTrue(TEXT("so is plugin content"), Refused(Run(TEXT("/HyperAISb3f9f07c5a33V1/GC_Cube"), {Make({Cube}), Fracture(TEXT("Sites=4"))}, false), TEXT("not project content")));
	TestTrue(TEXT("a missing mesh is refused"), Refused(Run(Asset, {Make({TEXT("/Game/Nowhere/SM_Missing")}), Fracture(TEXT("Sites=4"))}, false), TEXT("is not a static mesh")));
	TestTrue(TEXT("an actor that is not there is refused"), Refused(Run(Asset, {Make({TEXT("No Such Wall")}), Fracture(TEXT("Sites=4"))}, false), TEXT("No actor in the open level")));
	TestTrue(TEXT("an absurd site count is refused"), Refused(Run(Asset, {Make({Cube}), Fracture(TEXT("Sites=100000"))}, false), TEXT("far past the most")));
	TestTrue(TEXT("absurd slices are refused"), Refused(Run(Asset, {Make({Cube}), Op(TEXT("fracture.slice"), TEXT("X=31; Y=31; Z=31"))}, false), TEXT("the most is 2000")));
	TestTrue(TEXT("absurd clusters of sites are refused"), Refused(Run(Asset, {Make({Cube}), Op(TEXT("fracture.clustered"), TEXT("Clusters=64; SitesPerCluster=100"))}, false),
		TEXT("far past the most")));
	TestTrue(TEXT("an unknown kind is refused, with the kinds"), Refused(Run(Asset, {Make({Cube}), Op(TEXT("fracture.explode"))}, false), TEXT("fracture.radial")));
	TestTrue(TEXT("make comes first"), Refused(Run(Asset, {Fracture(TEXT("Sites=4")), Make({Cube})}, false), TEXT("make comes first")));
	TestTrue(TEXT("changing a collection that is not there is refused"), Refused(Run(Asset, {Fracture(TEXT("Sites=4"))}, false), TEXT("start the plan with make")));
	TestTrue(TEXT("replacing needs level actors"), Refused(Run(Asset, {Make({Cube}, TEXT("replace")), Fracture(TEXT("Sites=4"))}, false), TEXT("needs level actors")));
	TestTrue(TEXT("placing at a point needs the point"), Refused(Run(Asset, {Make({Cube}, TEXT("at")), Fracture(TEXT("Sites=4"))}, false), TEXT("needs Location")));
	FHyperAIDestructionOp Inside = Op(TEXT("material.internal"));
	Inside.Material = TEXT("/Game/Nowhere/M_Missing");
	TestTrue(TEXT("a missing material is refused"), Refused(Run(Asset, {Make({Cube}), Inside}, false), TEXT("is not a material")));
	TestTrue(TEXT("refused plans make nothing"), !LoadAsset(Asset) && !FPackageName::DoesPackageExist(Asset));

	// A near miss is fitted and the reply says so.
	const FHyperAIDestructionPlanReport Fitted = Run(Asset, {Make({Cube}), Fracture(TEXT("Sites=520; Seed=2"))}, true);
	TestTrue(TEXT("520 sites is fitted to 500: ") + Fitted.Diagnostic, Fitted.bOk
		&& Fitted.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("Sites 520 -> 500")); }));
	TestEqual(TEXT("and cuts 500 pieces"), Fitted.Leaves, 500);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDestructionApplyUndoTest, "HyperAIStudio.NativeTools.Destruction.ApplyAndUndo", HyperAIStudio::DestructionTests::Flags)

bool FHyperAIStudioDestructionApplyUndoTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DestructionTests;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor with a level open, its undo buffer, and no play session."));
		return false;
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, Cube);
	if (!TestNotNull(TEXT("the engine cube loads"), Mesh)) return false;
	// A wall of a cube in the level, to be replaced.
	const FString Label = FString::Printf(TEXT("DestructionWall_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6));
	const FVector At(12000.0, -9000.0, 300.0);
	AStaticMeshActor* Wall = nullptr;
	{
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		Wall = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(FRotator::ZeroRotator, At, FVector(2.0, 0.5, 2.0)));
		Wall->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Wall->SetActorLabel(Label);
	}
	ON_SCOPE_EXIT
	{
		if (IsValid(Wall)) World->EditorDestroyActor(Wall, false);
	};
	const FString Asset = TestAsset(TEXT("GC_Wall"));
	const int32 UndoBefore = GEditor->Trans->GetQueueLength();
	FHyperAIDestructionOp Inside = Op(TEXT("material.internal"));
	Inside.Material = InsideMaterial;
	const FHyperAIDestructionPlanReport Done = Run(Asset, {
		Make({Label}),
		Op(TEXT("fracture.uniform"), TEXT("Sites=16; Seed=4")),
		Op(TEXT("damage"), TEXT("Thresholds=1200, 300; Collision=true")),
		Op(TEXT("collision"), TEXT("Implicit=box; Sizes=20:sphere")),
		Op(TEXT("mass"), TEXT("Density=2400")),
		Op(TEXT("state"), TEXT("State=kinematic")),
		Op(TEXT("anchor"), TEXT("Ground=true; Height=10")),
		Inside,
		Op(TEXT("rendering"), TEXT("Nanite=false"))}, false);
	const int32 Steps = GEditor->Trans->GetQueueLength() - UndoBefore;
	ON_SCOPE_EXIT
	{
		// Whatever failed below: the plan taken back.
		if (LoadAsset(Asset) && IsValid(LoadAsset(Asset))) UndoAll(GEditor->Trans->GetQueueLength() - UndoBefore - GEditor->Trans->GetUndoCount());
	};
	if (!TestTrue(TEXT("the plan runs: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("completed"))) return false;
	TestEqual(TEXT("in one undo step"), Steps, 1);
	TestTrue(TEXT("named for the plan"), GEditor->Trans->GetUndoContext().Title.ToString().Contains(TEXT("Destruction Plan")));
	UGeometryCollection* Made = LoadAsset(Asset);
	if (!TestNotNull(TEXT("the geometry collection is made"), Made)) return false;
	TestEqual(TEXT("16 pieces"), Done.Leaves, 16);
	TestTrue(TEXT("the thresholds are stored"), Made->DamageThreshold == TArray<float>({1200.f, 300.f})
		&& Made->DamageModel == EDamageModelTypeEnum::Chaos_Damage_Model_UserDefined_Damage_Threshold);
	TestTrue(TEXT("its density"), Made->bMassAsDensity && Made->Mass == 2400.f);
	const FGeometryCollectionSizeSpecificData& Default = Made->GetDefaultSizeSpecificData();
	TestTrue(TEXT("box collision"), !Default.CollisionShapes.IsEmpty() && Default.CollisionShapes[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Box);
	TestTrue(TEXT("and spheres for pieces up to 20 cm"), Made->SizeSpecificData.ContainsByPredicate([](const FGeometryCollectionSizeSpecificData& Data)
		{
			return Data.MaxSize == 20.f && !Data.CollisionShapes.IsEmpty() && Data.CollisionShapes[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Sphere;
		}));
	TestFalse(TEXT("Nanite off"), Made->EnableNanite);
	TestTrue(FString::Printf(TEXT("the pieces on the ground are anchored (%d)"), Done.Anchored), Done.Anchored > 0 && Done.Anchored < 16);
	TestEqual(TEXT("it starts kinematic"), Done.InitialState, FString(TEXT("kinematic")));
	// The inside faces carry the chosen material.
	FHyperAIDestructionInspectRequest Look;
	Look.Asset = Asset;
	Look.MaxPieces = 4;
	const FHyperAIDestructionInspectReport Read = C::Inspect(Look);
	const FHyperAIDestructionMaterialSlot* Slot = Read.Materials.FindByPredicate([](const FHyperAIDestructionMaterialSlot& Each) { return Each.Material.Contains(TEXT("BasicShapeMaterial")); });
	TestTrue(TEXT("inspect reads it back: ") + Read.Diagnostic, Read.bOk && Read.Leaves == 16 && Read.Levels.Num() == 2 && Read.Pieces.Num() == 4);
	FString Slots;
	for (const FHyperAIDestructionMaterialSlot& Each : Read.Materials) Slots += FString::Printf(TEXT(" [%d %s outer %d inner %d]"), Each.Slot, *Each.Material, Each.OuterFaces, Each.InnerFaces);
	TestTrue(TEXT("with the inside faces on the inside material, and only those:") + Slots, Slot && Slot->InnerFaces > 0 && Slot->OuterFaces == 0);
	TestTrue(TEXT("and the thresholds"), Read.DamageThresholds.Num() == 2 && Read.DamageThresholds[0] == 1200.f);
	TestTrue(TEXT("and box collision"), Read.Collision.ContainsByPredicate([](const FHyperAIDestructionCollision& Each) { return Each.Implicit == TEXT("box"); }));
	// The wall is replaced where it stood.
	AActor* Placed = Done.Actors.Num() == 1 ? FindByLabel(*World, Done.Actors[0]) : nullptr;
	TestTrue(TEXT("a geometry collection actor takes the wall's place"), Placed && Placed->IsA<AGeometryCollectionActor>() && Placed->GetActorLocation().Equals(At, 0.1));
	TestTrue(TEXT("using the asset"), Placed && Cast<AGeometryCollectionActor>(Placed)->GetGeometryCollectionComponent()->GetRestCollection() == Made);
	TestNull(TEXT("and the wall is gone"), FindByLabel(*World, Label));
	TestTrue(TEXT("inspect lists the actor"), Read.Actors.Num() == 1);
	TSharedPtr<FJsonObject> Case;
	TestTrue(TEXT("the reply hands back a verification case: ") + Done.VerifyCase, FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Done.VerifyCase), Case) && Case.IsValid()
		&& Done.VerifyCase.Contains(TEXT("\"Damage\"")) && Done.VerifyCase.Contains(TEXT("\"AssertBroken\"")) && Done.VerifyCase.Contains(Done.Actors.IsEmpty() ? TEXT("?") : *Done.Actors[0]));

	// One undo takes it all back: the actor, the wall, and the asset.
	if (!TestTrue(TEXT("undo"), GEditor->UndoTransaction())) return false;
	TestFalse(TEXT("undo removes the geometry collection asset"), IsValid(Made));
	TestNull(TEXT("and its actor"), Done.Actors.IsEmpty() ? nullptr : FindByLabel(*World, Done.Actors[0]));
	TestNotNull(TEXT("and brings the wall back"), FindByLabel(*World, Label));
	TestFalse(TEXT("leaving nothing to save"), Made->GetPackage()->IsDirty());
	if (!TestTrue(TEXT("redo"), GEditor->RedoTransaction())) return false;
	TestTrue(TEXT("redo brings the asset back whole"), IsValid(Made) && Made->NumElements(FGeometryCollection::TransformGroup) == 17 && Made->DamageThreshold.Num() == 2);
	TestNull(TEXT("and replaces the wall again"), FindByLabel(*World, Label));
	GEditor->UndoTransaction();
	// Undone, the path is free to make again, as a person tweaking the plan would.
	const FHyperAIDestructionPlanReport Again = Run(Asset, {Make({Cube}), Op(TEXT("fracture.uniform"), TEXT("Sites=4; Seed=1"))}, false);
	TestTrue(TEXT("the same Asset can be made again after undo: ") + Again.Diagnostic, Again.bOk && Again.Leaves == 4);
	UGeometryCollection* Remade = LoadAsset(Asset);
	TestTrue(TEXT("as a new, whole asset"), Remade && IsValid(Remade) && Remade != Made);
	if (Again.bOk) GEditor->UndoTransaction();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDestructionBreaksInVerifyTest, "HyperAIStudio.NativeTools.Destruction.BreaksInVerify", HyperAIStudio::DestructionTests::Flags)

bool FHyperAIStudioDestructionBreaksInVerifyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DestructionTests;
	// The case runs in copies of the open level. A partitioned level's copies stream only saved actors, and this one is new
	// and unsaved, so unattended runs play it in a new level without World Partition, then load the open level back.
	auto Check = [this]() -> bool
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World || GEditor->PlayWorld)
		{
			AddError(TEXT("Needs the editor with a level open and no play session."));
			return false;
		}
		const APlayerStart* Start = nullptr;
		for (TActorIterator<APlayerStart> It(World); It && !Start; ++It) Start = *It;
		if (!Start)
		{
			AddError(TEXT("The level needs a player start: verification plays it."));
			return false;
		}
		// Placed in the air ahead of the player, kinematic until the damage breaks it.
		const FVector At = Start->GetActorLocation() + FRotator(0.0, Start->GetActorRotation().Yaw, 0.0).Vector() * 800.0 + FVector(0.0, 0.0, 300.0);
		const FString Asset = TestAsset(TEXT("GC_Verify"));
		const int32 UndoBefore = GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
		const FHyperAIDestructionPlanReport Done = Run(Asset, {Make({Cube}, TEXT("at"), *Gate::VectorText(At)), Op(TEXT("fracture.uniform"), TEXT("Sites=12; Seed=9")),
			Op(TEXT("damage"), TEXT("Thresholds=500")), Op(TEXT("state"), TEXT("State=kinematic"))}, false);
		ON_SCOPE_EXIT
		{
			if (GEditor->Trans) UndoAll(GEditor->Trans->GetQueueLength() - UndoBefore - GEditor->Trans->GetUndoCount());
		};
		if (!TestTrue(TEXT("the collection is placed: ") + Done.Diagnostic, Done.bOk && Done.Actors.Num() == 1 && !Done.VerifyCase.IsEmpty())) return false;
		// A second one on the ground beside it (a player start stands about a metre up), held by its anchored base as a wall is, for a test ball to land on.
		const FVector Beside = FVector(At.X, At.Y, Start->GetActorLocation().Z) + FRotator(0.0, Start->GetActorRotation().Yaw + 90.0, 0.0).Vector() * 600.0
			- FVector(0.0, 0.0, 50.0);
		const FHyperAIDestructionPlanReport Wall = Run(TestAsset(TEXT("GC_Wall")), {Make({Cube}, TEXT("at"), *Gate::VectorText(Beside)),
			Op(TEXT("fracture.uniform"), TEXT("Sites=10; Seed=3")), Op(TEXT("damage"), TEXT("Thresholds=300")), Op(TEXT("anchor"), TEXT("Ground=true; Height=15"))}, false);
		if (!TestTrue(TEXT("and the wall: ") + Wall.Diagnostic, Wall.bOk && Wall.Actors.Num() == 1)) return false;

		// The case the plan handed back, through hyper_verify_run as an agent calls it: by name, with the JSON it sent.
		UClass* Toolset = FindObject<UClass>(nullptr, TEXT("/Script/HyperAIStudio.HyperAIStudioPIEPlaytestToolset"));
		UFunction* Function = Toolset ? Toolset->FindFunctionByName(TEXT("hyper_verify_run")) : nullptr;
		FStructProperty* RequestProperty = Function ? CastField<FStructProperty>(Function->FindPropertyByName(TEXT("Request"))) : nullptr;
		FObjectProperty* ReturnProperty = Function ? CastField<FObjectProperty>(Function->GetReturnProperty()) : nullptr;
		if (!TestTrue(TEXT("hyper_verify_run is there"), RequestProperty && ReturnProperty)) return false;
		FStructOnScope Params(Function);
		TSharedPtr<FJsonObject> Json;
		if (!TestTrue(TEXT("the case is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Done.VerifyCase), Json) && Json.IsValid())) return false;
		// And a case of the test's own: the wall stands whole, then a dropped test ball breaks pieces off it.
		const FString WallCase = FString::Printf(TEXT("{\"name\":\"wall-is-hit\",\"timeoutSeconds\":20,\"steps\":[")
			TEXT("{\"action\":\"WaitUntil\",\"until\":\"AssertExists\",\"target\":\"%s\",\"timeoutSeconds\":10},")
			TEXT("{\"action\":\"Wait\",\"seconds\":0.3},")
			TEXT("{\"action\":\"AssertBroken\",\"target\":\"%s\",\"compare\":\"==\",\"value\":\"0\"},")
			TEXT("{\"action\":\"Fire\",\"target\":\"%s\",\"range\":400},")
			TEXT("{\"action\":\"WaitUntil\",\"until\":\"AssertBroken\",\"target\":\"%s\",\"compare\":\">=\",\"value\":\"1\",\"timeoutSeconds\":4}]}"),
			*Wall.Actors[0], *Wall.Actors[0], *Wall.Actors[0], *Wall.Actors[0]);
		TSharedPtr<FJsonObject> Extra;
		TArray<TSharedPtr<FJsonValue>> Cases = Json->GetArrayField(TEXT("cases"));
		if (!TestTrue(TEXT("the wall case is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(WallCase), Extra) && Extra.IsValid())) return false;
		Cases.Add(MakeShared<FJsonValueObject>(Extra));
		Json->SetArrayField(TEXT("cases"), Cases);
		if (!TestTrue(TEXT("and fits the request"), FJsonObjectConverter::JsonObjectToUStruct(Json.ToSharedRef(), RequestProperty->Struct,
			RequestProperty->ContainerPtrToValuePtr<void>(Params.GetStructMemory()))))
		{
			return false;
		}
		Toolset->GetDefaultObject()->ProcessEvent(Function, Params.GetStructMemory());
		const TStrongObjectPtr<UToolCallAsyncResult> Result(Cast<UToolCallAsyncResult>(ReturnProperty->GetObjectPropertyValue_InContainer(Params.GetStructMemory())));
		if (!TestTrue(TEXT("the batch starts"), Result.IsValid())) return false;
		const double Until = FPlatformTime::Seconds() + 120.0;
		while (!Result->bIsComplete && FPlatformTime::Seconds() < Until)
		{
			++GFrameCounter;
			GEngine->CleanupGameViewport();
			FTSTicker::GetCoreTicker().Tick(1.f / 30.f);
		}
		if (!TestTrue(TEXT("and finishes"), Result->bIsComplete)) return false;
		const TSharedPtr<FJsonValue> Value = Result->GetValueAsJson();
		FString Text;
		if (Value.IsValid() && Value->Type == EJson::Object)
		{
			FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		}
		AddInfo(TEXT("Verification: ") + Text.Left(8000));
		const TSharedPtr<FJsonObject> Report = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		double Passed = 0.0;
		if (Report)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Report->Values)
			{
				if (Field.Key.Equals(TEXT("passed"), ESearchCase::IgnoreCase) && Field.Value.IsValid()) Passed = Field.Value->AsNumber();
			}
		}
		TestEqual(TEXT("both cases pass: damage breaks the one, a dropped ball the other"), Passed, 2.0);
		TestTrue(TEXT("and the report says how many pieces broke off and what fell"), Text.Contains(TEXT("broken off")) && Text.Contains(TEXT("fell more than 50 cm")));
		TestTrue(TEXT("and that a test ball was dropped"), Text.Contains(TEXT("dropped a 40 cm, 200 kg test ball")));
		return true;
	};
	UWorld* Open = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!Open || !Open->IsPartitionedWorld()) return Check();
	const FString OpenName = Open->GetOutermost()->GetName();
	if (!FApp::IsUnattended() || !FPackageName::DoesPackageExist(OpenName))
	{
		AddInfo(TEXT("Skipped: the open level uses World Partition; unattended runs check this in a new level without it."));
		return true;
	}
	GEditor->NewMap(/*bIsPartitionedWorld=*/false);
	if (UWorld* Level = GEditor->GetEditorWorldContext().World())
	{
		// Ground and a player start, as the game needs to put the player down.
		if (AStaticMeshActor* Ground = Level->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(FRotator::ZeroRotator, FVector(0.0, 0.0, -50.0), FVector(80.0, 80.0, 1.0))))
		{
			Ground->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Cube));
		}
		Level->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), FTransform(FVector(0.0, 0.0, 100.0)));
	}
	const bool bChecked = Check();
	FEditorFileUtils::LoadMap(FPackageName::LongPackageNameToFilename(OpenName, FPackageName::GetMapPackageExtension()), /*LoadAsTemplate=*/false, /*bShowProgress=*/false);
	const UWorld* Back = GEditor->GetEditorWorldContext().World();
	return TestTrue(TEXT("the open level is loaded back: ") + OpenName, Back && Back->GetOutermost()->GetName() == OpenName) && bChecked;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDestructionSkillTest, "HyperAIStudio.NativeTools.Destruction.Skill", HyperAIStudio::DestructionTests::Flags)

bool FHyperAIStudioDestructionSkillTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DestructionTests;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	FString Skill;
	if (!TestTrue(TEXT("the destruction skill ships with the plugin"), Plugin.IsValid()
		&& FFileHelper::LoadFileToString(Skill, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Skills/unreal-destruction/SKILL.md")))))
	{
		return false;
	}
	TestTrue(TEXT("it is a skill"), Skill.StartsWith(TEXT("---")) && Skill.Contains(TEXT("name: unreal-destruction")));
	for (const FString& Name : C::GetToolNames())
	{
		TestTrue(FString::Printf(TEXT("it names %s"), *Name), Skill.Contains(Name));
	}
	for (const Gate::FKindSpec& Spec : Gate::Kinds())
	{
		TestTrue(FString::Printf(TEXT("it explains %s"), Spec.Kind), Skill.Contains(FString::Printf(TEXT("`%s`"), Spec.Kind)));
	}
	for (const TCHAR* Step : {TEXT("Damage"), TEXT("Fire"), TEXT("AssertBroken"), TEXT("PhysicsAssetToolset"), TEXT("DataflowAgentToolset")})
	{
		TestTrue(FString::Printf(TEXT("and %s"), Step), Skill.Contains(Step));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
