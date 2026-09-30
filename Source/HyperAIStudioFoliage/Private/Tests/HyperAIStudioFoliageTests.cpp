// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "HyperAIStudioFoliageToolset.h"
#include "HyperAIStudioSettings.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"

namespace HyperAIStudio::FoliageTests
{
	using C = FHyperAIStudioFoliageContracts;

	FHyperAIFoliageOp Op(const TCHAR* Kind, const TArray<FString>& Types)
	{
		FHyperAIFoliageOp Out;
		Out.Kind = Kind;
		Out.Types = Types;
		return Out;
	}

	FHyperAIFoliageSetting Setting(const TCHAR* Name, const TCHAR* Value)
	{
		FHyperAIFoliageSetting Out;
		Out.Name = Name;
		Out.Value = Value;
		return Out;
	}

	FHyperAIFoliagePlanReport Run(TArray<FHyperAIFoliageOp> Ops, const bool bDryRun = false)
	{
		FHyperAIFoliagePlanRequest Plan;
		Plan.Operations = MoveTemp(Ops);
		Plan.bDryRun = bDryRun;
		return C::ApplyPlan(Plan);
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	UFoliageType* TypeAt(const FString& Path)
	{
		return LoadObject<UFoliageType>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** Every loaded instance of the type, whichever foliage actor holds it. */
	TArray<FVector> InstancesOf(const UFoliageType* Type)
	{
		TArray<FVector> Out;
		UWorld* World = EditorWorld();
		for (TActorIterator<AInstancedFoliageActor> It(World); World && Type && It; ++It)
		{
			if (const FFoliageInfo* Info = It->FindInfo(Type))
			{
				for (const FFoliageInstance& Instance : Info->Instances) Out.Add(Instance.Location);
			}
		}
		return Out;
	}

	int32 CountWithin(const TArray<FVector>& Points, const FVector& Center, const double Radius)
	{
		return Points.FilterByPredicate([&Center, Radius](const FVector& Point) { return FVector2D::Distance(FVector2D(Point), FVector2D(Center)) <= Radius; }).Num();
	}

	/** A box from the engine's cube (100 cm a side) scaled into a slab. */
	AStaticMeshActor* Slab(UWorld& World, const FString& Label, const FVector& Center, const FVector& Scale, const FRotator& Rotation = FRotator::ZeroRotator)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		AStaticMeshActor* Actor = Cube ? World.SpawnActor<AStaticMeshActor>(Center, Rotation, FActorSpawnParameters()) : nullptr;
		if (!Actor) return nullptr;
		Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Actor->SetActorScale3D(Scale);
		Actor->SetActorLabel(Label);
		return Actor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioFoliageEndToEndTest,
	"HyperAIStudio.NativeTools.Worldbuilding.FoliageEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioFoliageEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::FoliageTests;
	UWorld* World = EditorWorld();
	if (!World)
	{
		AddInfo(TEXT("Skipped: no editor world."));
		return true;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/Foliage_") + Suffix;
	const FString Cone = Folder + TEXT("/FT_Cone");
	const FString Block = Folder + TEXT("/FT_Block");
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	TSet<AInstancedFoliageActor*> FoliageActorsBefore;
	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It) FoliageActorsBefore.Add(*It);
	TArray<AActor*> Made;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		for (AActor* Actor : Made)
		{
			if (IsValid(Actor)) World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
		}
		TArray<AInstancedFoliageActor*> Added;
		for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
		{
			for (UFoliageType* Type : {TypeAt(Cone), TypeAt(Block)})
			{
				if (Type && It->FindInfo(Type)) It->RemoveFoliageType(&Type, 1);
			}
			if (!FoliageActorsBefore.Contains(*It)) Added.Add(*It);
		}
		for (AInstancedFoliageActor* Actor : Added) World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/true);
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

	// Far from anything in the bench level: a 40 m slab 300 m up, under a see-through roof (cover, but not ground the
	// cones may stand on) and beside a 45-degree ramp, steeper than the cones allow.
	const FVector Origin(400000.0, 400000.0, 30000.0);
	const double Ground = Origin.Z + 10.0;
	const FVector RoofAt = Origin + FVector(-1000.0, -1000.0, 400.0);
	const FVector RampAt = Origin + FVector(1000.0, 1000.0, 300.0);
	Made.Add(Slab(*World, TEXT("HyperAI_TestFoliage_Floor_") + Suffix, Origin, FVector(40.0, 40.0, 0.2)));
	AStaticMeshActor* Roof = Slab(*World, TEXT("HyperAI_TestFoliage_Roof_") + Suffix, RoofAt, FVector(6.0, 6.0, 0.2));
	Made.Add(Roof);
	Made.Add(Slab(*World, TEXT("HyperAI_TestFoliage_Ramp_") + Suffix, RampAt, FVector(6.0, 6.0, 0.2), FRotator(45.0, 0.0, 0.0)));
	UMaterialInterface* SeeThrough = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/Widget3DPassThrough_Translucent.Widget3DPassThrough_Translucent"));
	if (!TestTrue(TEXT("the test scene is built"), !Made.Contains(nullptr) && SeeThrough != nullptr)) return false;
	Roof->GetStaticMeshComponent()->SetMaterial(0, SeeThrough);

	FHyperAIFoliageOp MakeCone = Op(TEXT("foliage_type.create"), {Cone});
	MakeCone.Mesh = TEXT("/Engine/BasicShapes/Cone");
	MakeCone.Settings = {Setting(TEXT("Density"), TEXT("40")), Setting(TEXT("Radius"), TEXT("100")), Setting(TEXT("GroundSlopeAngle"), TEXT("(Min=0,Max=30)")),
		Setting(TEXT("AlignToNormal"), TEXT("false")), Setting(TEXT("ScaleX"), TEXT("(Min=0.5,Max=0.7)"))};
	FHyperAIFoliageOp MakeBlock = Op(TEXT("foliage_type.create"), {Block});
	MakeBlock.Mesh = TEXT("/Engine/BasicShapes/Cube");
	MakeBlock.Settings = {Setting(TEXT("Density"), TEXT("10")), Setting(TEXT("ScaleX"), TEXT("(Min=0.2,Max=0.2)"))};
	FHyperAIFoliageOp Scatter = Op(TEXT("foliage.scatter"), {Cone});
	Scatter.Center = Origin;
	Scatter.Size = FVector2D(36.0, 36.0);
	Scatter.Surfaces = {TEXT("static_mesh")};
	FHyperAIFoliageClearing Camp;
	Camp.Center = Origin;
	Camp.Radius = 4.f;
	Scatter.Clearings = {Camp};
	FHyperAIFoliageOp Place = Op(TEXT("foliage.place"), {Block});
	Place.Surfaces = {TEXT("static_mesh")};
	Place.Points = {Origin + FVector(200.0, 0.0, 0.0), Origin + FVector(-200.0, 0.0, 0.0), Origin + FVector(0.0, 1500.0, 0.0), Origin + FVector(3000.0, 0.0, 0.0)};
	FHyperAIFoliageOp Bare = Op(TEXT("foliage.scatter"), {Block});
	Bare.Center = Origin;
	Bare.Radius = 10.f;

	// Refused before anything changes: no region, a mistyped setting, a type that is not there, far too many spots.
	FHyperAIFoliageOp Nowhere = Scatter;
	Nowhere.Size = FVector2D::ZeroVector;
	const FHyperAIFoliagePlanReport NoRegion = Run({MakeCone, Nowhere}, /*bDryRun=*/true);
	TestTrue(TEXT("a scatter without a region is refused: ") + NoRegion.Diagnostic, !NoRegion.bOk && NoRegion.Diagnostic.Contains(TEXT("Give the region")));
	FHyperAIFoliageOp Typo = MakeCone;
	Typo.Settings = {Setting(TEXT("Densty"), TEXT("4"))};
	const FHyperAIFoliagePlanReport Mistyped = Run({Typo}, /*bDryRun=*/true);
	TestTrue(TEXT("a mistyped setting is refused with the settings listed: ") + Mistyped.Diagnostic, !Mistyped.bOk && Mistyped.Diagnostic.Contains(TEXT("GroundSlopeAngle")));
	TestFalse(TEXT("scattering a type that is not there is refused"), Run({Scatter}, /*bDryRun=*/true).bOk);
	FHyperAIFoliageOp Vast = Scatter;
	Vast.Size = FVector2D(5000.0, 5000.0);
	const FHyperAIFoliagePlanReport TooMany = Run({MakeCone, Vast}, /*bDryRun=*/true);
	TestTrue(TEXT("a scatter past the spot limit is refused with the numbers: ") + TooMany.Diagnostic, !TooMany.bOk && TooMany.Diagnostic.Contains(TEXT("spots")));
	TestTrue(TEXT("the whole plan checks"), Run({MakeCone, MakeBlock, Scatter, Place, Bare}, /*bDryRun=*/true).Status == TEXT("checked"));
	TestNull(TEXT("and nothing was made"), TypeAt(Cone));

	// One plan: two types made, cones scattered by their rules, three blocks placed, and a scatter with nothing under it.
	const FHyperAIFoliagePlanReport Done = Run({MakeCone, MakeBlock, Scatter, Place, Bare});
	const FString Notes = FString::Join(Done.Notes, TEXT(" | "));
	if (!TestTrue(TEXT("the plan runs: ") + Done.Diagnostic + TEXT(" | ") + Notes, Done.bOk && Done.Status == TEXT("completed") && Done.Applied == 5))
	{
		return false;
	}
	TestEqual(TEXT("both types are saved"), Done.Saved.Num(), 2);
	UFoliageType* ConeType = TypeAt(Cone);
	UFoliageType* BlockType = TypeAt(Block);
	if (!TestTrue(TEXT("both types are assets"), ConeType && BlockType)) return false;
	const TArray<FVector> Cones = InstancesOf(ConeType);
	TestTrue(FString::Printf(TEXT("cones cover the slab (%d)"), Cones.Num()), Cones.Num() >= 100);
	int32 Off = 0;
	int32 Crowded = 0;
	for (int32 Index = 0; Index < Cones.Num(); ++Index)
	{
		const FVector Local = Cones[Index] - Origin;
		if (!FMath::IsNearlyEqual(Cones[Index].Z, Ground, 2.0) || FMath::Abs(Local.X) > 1801.0 || FMath::Abs(Local.Y) > 1801.0) ++Off;
		for (int32 Other = Index + 1; Other < Cones.Num(); ++Other)
		{
			if (FVector::Dist(Cones[Index], Cones[Other]) < 99.0) ++Crowded;
		}
	}
	TestEqual(TEXT("every cone stands on the slab, inside the region"), Off, 0);
	TestEqual(TEXT("no two cones are closer than their 1 m Radius"), Crowded, 0);
	TestEqual(TEXT("none is in the 4 m clearing"), CountWithin(Cones, Origin, 399.0), 0);
	TestEqual(TEXT("none is under the see-through roof"), Cones.FilterByPredicate([&RoofAt](const FVector& Point)
		{
			return FMath::Abs(Point.X - RoofAt.X) < 300.0 && FMath::Abs(Point.Y - RoofAt.Y) < 300.0;
		}).Num(), 0);
	TestEqual(TEXT("none is on or under the 45-degree ramp"), Cones.FilterByPredicate([&RampAt](const FVector& Point)
		{
			return FMath::Abs(Point.X - RampAt.X) < 210.0 && FMath::Abs(Point.Y - RampAt.Y) < 300.0;
		}).Num(), 0);
	TestTrue(TEXT("the scatter says why spots were turned away: ") + Notes,
		Notes.Contains(TEXT("under cover")) && Notes.Contains(TEXT("GroundSlopeAngle")) && Notes.Contains(TEXT("in a clearing")));
	TestEqual(TEXT("three blocks are placed; the point off the slab has no ground"), InstancesOf(BlockType).Num(), 3);
	TestTrue(TEXT("and the notes say so"), Notes.Contains(TEXT("3 placed at 4 points")) && Notes.Contains(TEXT("1 found nothing to stand on")));
	TestTrue(TEXT("a scatter with no landscape under it says what to change"), Notes.Contains(TEXT("no landscape under it")));
	TestEqual(TEXT("the report counts what was placed"), Done.Placed, Cones.Num() + 3);

	FHyperAIFoliageInspectRequest Look;
	Look.Types = {Cone, Block};
	const FHyperAIFoliageInspectReport Seen = C::Inspect(Look);
	if (TestTrue(TEXT("inspect reads both types back: ") + Seen.Diagnostic, Seen.bOk && Seen.Types.Num() == 2))
	{
		const FHyperAIFoliageTypeInfo* Read = Seen.Types.FindByPredicate([](const FHyperAIFoliageTypeInfo& Info) { return Info.Type.EndsWith(TEXT("FT_Cone")); });
		TestTrue(TEXT("with the cones' count and rules: ") + (Read ? Read->Summary : FString()), Read && Read->Instances == Cones.Num()
			&& Read->Summary.Contains(TEXT("Density 40")) && Read->Summary.Contains(TEXT("Radius 100 cm")) && Read->Summary.Contains(TEXT("GroundSlopeAngle 0 to 30 deg")));
	}

	// A setting changed on a type in use reaches the foliage already placed.
	FHyperAIFoliageOp Cull = Op(TEXT("foliage_type.set"), {Cone});
	Cull.Settings = {Setting(TEXT("CullDistance"), TEXT("(Min=500,Max=6000)"))};
	const FHyperAIFoliagePlanReport Culled = Run({Cull});
	TestTrue(TEXT("the cull distance is set: ") + Culled.Diagnostic, Culled.bOk);
	int32 Updated = 0;
	int32 Components = 0;
	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		if (const FFoliageInfo* Info = It->FindInfo(ConeType))
		{
			if (const UHierarchicalInstancedStaticMeshComponent* Component = Info->GetComponent())
			{
				++Components;
				if (Component->InstanceEndCullDistance == 6000) ++Updated;
			}
		}
	}
	TestTrue(FString::Printf(TEXT("and the placed cones' components cull at 60 m (%d of %d)"), Updated, Components), Components > 0 && Updated == Components);

	// An erase is one undo step.
	const FVector Patch = Origin + FVector(1000.0, -1000.0, 0.0);
	const int32 InPatch = CountWithin(Cones, Patch, 600.0);
	FHyperAIFoliageOp Clear = Op(TEXT("foliage.erase"), {Cone});
	Clear.Center = Patch;
	Clear.Radius = 6.f;
	const FHyperAIFoliagePlanReport Erased = Run({Clear});
	TestTrue(FString::Printf(TEXT("the patch's %d cones are erased: %s"), InPatch, *Erased.Diagnostic), Erased.bOk && InPatch > 0 && Erased.Removed == InPatch);
	TestEqual(TEXT("none is left there"), CountWithin(InstancesOf(ConeType), Patch, 600.0), 0);
	TestEqual(TEXT("and the rest stay"), InstancesOf(ConeType).Num(), Cones.Num() - InPatch);
	GEditor->UndoTransaction();
	TestEqual(TEXT("undo brings the patch back"), InstancesOf(ConeType).Num(), Cones.Num());

	// Taking the types out removes every instance.
	const FHyperAIFoliagePlanReport Removed = Run({Op(TEXT("foliage_type.remove"), {Cone, Block})});
	TestTrue(TEXT("both types are taken out with their instances: ") + Removed.Diagnostic, Removed.bOk && Removed.Removed == Cones.Num() + 3);
	TestTrue(TEXT("and nothing of them is left"), InstancesOf(ConeType).IsEmpty() && InstancesOf(BlockType).IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
