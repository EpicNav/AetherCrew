// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioLandscapeToolset.h"
#include "HyperAIStudioSettings.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeSplineActor.h"
#include "LandscapeSubsystem.h"
#include "LandscapeUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"

namespace HyperAIStudio::LandscapeTests
{
	using C = FHyperAIStudioLandscapeContracts;

	FHyperAILandscapeOp Op(const TCHAR* Kind, const FString& Landscape)
	{
		FHyperAILandscapeOp Out;
		Out.Kind = Kind;
		Out.Landscape = Landscape;
		return Out;
	}

	FHyperAILandscapePlanReport Run(TArray<FHyperAILandscapeOp> Ops, const bool bDryRun = false)
	{
		FHyperAILandscapePlanRequest Plan;
		Plan.Operations = MoveTemp(Ops);
		Plan.bDryRun = bDryRun;
		return C::ApplyPlan(Plan);
	}

	ALandscape* Find(const FString& Label)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		for (TActorIterator<ALandscape> It(World); World && It; ++It)
		{
			if (It->GetActorLabel() == Label) return *It;
		}
		return nullptr;
	}

	/** The ground's height in meters at a world XY, from whichever of the landscape's proxies holds it. */
	TOptional<float> GroundAt(ALandscape& Landscape, const FVector& Where)
	{
		TOptional<float> Height;
		if (ULandscapeInfo* Info = Landscape.GetLandscapeInfo())
		{
			Info->ForEachLandscapeProxy([&Height, &Where](ALandscapeProxy* Proxy)
			{
				Height = Proxy->GetHeightAtLocation(Where, EHeightfieldSource::Editor);
				return !Height.IsSet();
			});
		}
		if (Height.IsSet()) Height = Height.GetValue() / 100.f;
		return Height;
	}

	FHyperAILandscapePaintRule Rule(const TCHAR* Layer)
	{
		FHyperAILandscapePaintRule Out;
		Out.Layer = Layer;
		Out.Breakup = 0.f;
		return Out;
	}

	/** A test landscape, its streaming proxies, and the assets made in Folder. */
	void Remove(const FString& Label, const FString& Folder)
	{
		if (ALandscape* Left = Find(Label))
		{
			if (ULandscapeInfo* Info = Left->GetLandscapeInfo())
			{
				TArray<ALandscapeProxy*> Proxies;
				Info->ForEachLandscapeProxy([&Proxies](ALandscapeProxy* Proxy) { Proxies.Add(Proxy); return true; });
				for (ALandscapeProxy* Proxy : Proxies) Proxy->GetWorld()->EditorDestroyActor(Proxy, /*bShouldModifyLevel=*/true);
			}
		}
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

	/** A made material's layer blend: the one expression that names landscape layers. */
	const UMaterialExpressionLandscapeLayerBlend* BlendOf(UMaterial* Material)
	{
		if (!Material) return nullptr;
		for (const UMaterialExpression* Expression : Material->GetExpressionCollection().Expressions)
		{
			if (const UMaterialExpressionLandscapeLayerBlend* Blend = Cast<UMaterialExpressionLandscapeLayerBlend>(Expression)) return Blend;
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLandscapeEndToEndTest,
	"HyperAIStudio.NativeTools.Worldbuilding.LandscapeEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLandscapeEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LandscapeTests;
	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		AddInfo(TEXT("Skipped: no editor world."));
		return true;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Label = TEXT("HyperAI_TestLandscape_") + Suffix;
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/Landscape_") + Suffix;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	const FString RoadLabel = TEXT("HyperAI_TestRoad_") + Suffix;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		if (UWorld* World = GEditor->GetEditorWorldContext().World())
		{
			for (TActorIterator<ALandscapeSplineActor> It(World); It; ++It)
			{
				if (It->GetActorLabel() == RoadLabel) World->EditorDestroyActor(*It, /*bShouldModifyLevel=*/true);
			}
		}
		if (ALandscape* Left = Find(Label))
		{
			if (ULandscapeInfo* Info = Left->GetLandscapeInfo())
			{
				TArray<ALandscapeProxy*> Proxies;
				Info->ForEachLandscapeProxy([&Proxies](ALandscapeProxy* Proxy) { Proxies.Add(Proxy); return true; });
				for (ALandscapeProxy* Proxy : Proxies) Proxy->GetWorld()->EditorDestroyActor(Proxy, /*bShouldModifyLevel=*/true);
			}
		}
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

	FHyperAILandscapeOp Create = Op(TEXT("landscape.create"), Label);
	Create.Size = 252.f;
	Create.Resolution = 100.f;
	Create.HeightRange = 256.f;
	Create.Noise.Amplitude = 40.f;
	Create.Noise.Wavelength = 150.f;
	Create.PaintLayers = {TEXT("Grass"), TEXT("Rock"), TEXT("Snow")};
	Create.LayerInfoFolder = Folder;
	FHyperAILandscapeOp Flatten = Op(TEXT("landscape.sculpt"), Label);
	Flatten.Mode = TEXT("flatten");
	Flatten.Center = FVector(0.0, 0.0, 0.0);
	Flatten.Radius = 20.f;
	Flatten.Falloff = 10.f;
	Flatten.Height = 12.f;
	FHyperAILandscapeOp Paint = Op(TEXT("landscape.paint"), Label);
	FHyperAILandscapePaintRule Rock = Rule(TEXT("Rock"));
	Rock.SlopeMin = 25.f;
	FHyperAILandscapePaintRule Snow = Rule(TEXT("Snow"));
	Snow.HeightMin = 14.f;
	Paint.Rules = {Rule(TEXT("Grass")), Rock, Snow};

	// Refused before anything changes: a landscape that is not there, a size past the limit, an unknown paint layer.
	TestFalse(TEXT("sculpting a landscape that is not there is refused"), Run({Flatten}, /*bDryRun=*/true).bOk);
	FHyperAILandscapeOp Huge = Create;
	Huge.Size = 20000.f;
	Huge.Resolution = 50.f;
	const FHyperAILandscapePlanReport TooBig = Run({Huge}, /*bDryRun=*/true);
	TestTrue(TEXT("a landscape past the vertex limit is refused with the numbers: ") + TooBig.Diagnostic, !TooBig.bOk && TooBig.Diagnostic.Contains(TEXT("vertices a side")));
	FHyperAILandscapeOp NoSuchLayer = Paint;
	NoSuchLayer.Rules = {Rule(TEXT("Lava"))};
	TestFalse(TEXT("painting a layer the landscape lacks is refused"), Run({Create, NoSuchLayer}, /*bDryRun=*/true).bOk);
	TestNull(TEXT("and nothing was made"), Find(Label));

	// One plan: noise hills, a flattened plateau in the middle, and grass, rock and snow painted by slope and height.
	const FHyperAILandscapePlanReport Done = Run({Create, Flatten, Paint});
	if (!TestTrue(TEXT("The landscape is made, sculpted and painted: ") + Done.Diagnostic + TEXT(" | ") + FString::Join(Done.Notes, TEXT(" | ")),
		Done.bOk && Done.Status == TEXT("completed") && Done.Applied == 3 && Done.Made.Num() == 1))
	{
		return false;
	}
	ALandscape* Landscape = Find(Label);
	if (!TestNotNull(TEXT("it is in the level under its label"), Landscape)) return false;
	TestEqual(TEXT("its three layer infos are saved"), Done.Saved.Num(), 3);
	FHyperAILandscapeInspectRequest Look;
	Look.Landscapes = {Label};
	const FHyperAILandscapeInspectReport Inspected = C::Inspect(Look);
	if (TestTrue(TEXT("inspect reads it back: ") + Inspected.Diagnostic, Inspected.bOk && Inspected.Landscapes.Num() == 1))
	{
		const FHyperAILandscapeInfo& Info = Inspected.Landscapes[0];
		TestEqual(TEXT("253 vertices a side (two components of 126 quads)"), Info.VerticesX, 253);
		TestEqual(TEXT("at 100 cm"), Info.Resolution, 100.f);
		TestTrue(FString::Printf(TEXT("its ground spans the noise (%.1f to %.1f m)"), Info.HeightMin, Info.HeightMax),
			Info.HeightMax - Info.HeightMin > 5.f && Info.HeightMax - Info.HeightMin < 60.f);
		TestTrue(TEXT("with three paint layers, each measured: ") + FString::Join(Info.PaintLayers, TEXT(", ")),
			Info.PaintLayers.Num() == 3 && Info.PaintLayers[0].StartsWith(TEXT("Grass")) && Info.PaintLayers[0].EndsWith(TEXT("%")));
	}
	const TOptional<float> Plateau = GroundAt(*Landscape, FVector(0.0, 0.0, 0.0));
	TestTrue(FString::Printf(TEXT("the middle is flattened to 12 m (it reads %.2f)"), Plateau.Get(-1000.f)), Plateau.IsSet() && FMath::IsNearlyEqual(Plateau.GetValue(), 12.f, 0.25f));

	// A second plan: an edit layer to sculpt in, and a terrace step on the plateau's edge.
	FHyperAILandscapeOp Layer = Op(TEXT("landscape.add_edit_layer"), Label);
	Layer.EditLayer = TEXT("Terraces");
	FHyperAILandscapeOp Raise = Op(TEXT("landscape.sculpt"), Label);
	Raise.Mode = TEXT("raise");
	Raise.Center = FVector(0.0, 0.0, 0.0);
	Raise.Radius = 5.f;
	Raise.Falloff = 2.f;
	Raise.Amount = 3.f;
	Raise.EditLayer = TEXT("Terraces");
	const FHyperAILandscapePlanReport Edited = Run({Layer, Raise});
	TestTrue(TEXT("an edit layer is added and sculpted in: ") + Edited.Diagnostic, Edited.bOk);
	const TOptional<float> Raised = GroundAt(*Landscape, FVector(0.0, 0.0, 0.0));
	TestTrue(FString::Printf(TEXT("the raise adds 3 m over the plateau (it reads %.2f)"), Raised.Get(-1000.f)), Raised.IsSet() && FMath::IsNearlyEqual(Raised.GetValue(), 15.f, 0.3f));
	const FHyperAILandscapeInspectReport Layers = C::Inspect(Look);
	TestTrue(TEXT("and the landscape lists the edit layer"), Layers.bOk && Layers.Landscapes.Num() == 1 && Layers.Landscapes[0].EditLayers.Contains(TEXT("Terraces")));

	// A road over the hills, away from the plateau: the ground is leveled across it and painted under it. The middle
	// point's neighbors are level with each other, so there the road runs along X and its cross-section along Y; it
	// goes where the hills fall most steeply across it.
	FVector Middle(0.0, -9000.0, 0.0);
	float Steepest = -1.f;
	for (double X = -3000.0; X <= 3000.0; X += 500.0)
	{
		const TOptional<float> Low = GroundAt(*Landscape, FVector(X, -9250.0, 0.0));
		const TOptional<float> High = GroundAt(*Landscape, FVector(X, -8750.0, 0.0));
		if (Low.IsSet() && High.IsSet() && FMath::Abs(Low.GetValue() - High.GetValue()) > Steepest)
		{
			Steepest = FMath::Abs(Low.GetValue() - High.GetValue());
			Middle.X = X;
		}
	}
	TestTrue(FString::Printf(TEXT("the road crosses a real slope (%.2f m over 5 m)"), Steepest), Steepest > 0.2f);
	const TOptional<float> NaturalLeft = GroundAt(*Landscape, Middle + FVector(0.0, -250.0, 0.0));
	const TOptional<float> NaturalRight = GroundAt(*Landscape, Middle + FVector(0.0, 250.0, 0.0));
	FHyperAILandscapeOp Dirt = Op(TEXT("landscape.add_paint_layer"), Label);
	Dirt.PaintLayers = {TEXT("Dirt")};
	Dirt.LayerInfoFolder = Folder;
	FHyperAILandscapeOp Road = Op(TEXT("landscape.road"), Label);
	Road.Road = RoadLabel;
	Road.Points = {FVector(Middle.X - 9000.0, -7000.0, 0.0), Middle, FVector(Middle.X + 9000.0, -7000.0, 0.0)};
	Road.Width = 8.f;
	Road.Falloff = 6.f;
	Road.Layer = TEXT("Dirt");
	FHyperAILandscapeOp Nowhere = Road;
	Nowhere.Layer = TEXT("Lava");
	TestFalse(TEXT("a road painting a layer the landscape lacks is refused"), Run({Nowhere}, /*bDryRun=*/true).bOk);
	const FHyperAILandscapePlanReport Paved = Run({Dirt, Road});
	TestTrue(TEXT("the road is laid: ") + Paved.Diagnostic + TEXT(" | ") + FString::Join(Paved.Notes, TEXT(" | ")), Paved.bOk && Paved.Applied == 2);
	const TOptional<float> Center = GroundAt(*Landscape, Middle);
	const TOptional<float> Left = GroundAt(*Landscape, Middle + FVector(0.0, -250.0, 0.0));
	const TOptional<float> Right = GroundAt(*Landscape, Middle + FVector(0.0, 250.0, 0.0));
	const FString Across = FString::Printf(TEXT("across the road %.2f / %.2f / %.2f m, the sides were %.2f / %.2f m"),
		Left.Get(-1.f), Center.Get(-1.f), Right.Get(-1.f), NaturalLeft.Get(-1.f), NaturalRight.Get(-1.f));
	AddInfo(Across);
	TestTrue(TEXT("the ground is level across the road: ") + Across,
		Center.IsSet() && Left.IsSet() && Right.IsSet() && FMath::Abs(Left.GetValue() - Center.GetValue()) < 0.15f && FMath::Abs(Right.GetValue() - Center.GetValue()) < 0.15f);
	const FHyperAILandscapeInspectReport Roads = C::Inspect(Look);
	const FString* DirtLine = Roads.bOk && Roads.Landscapes.Num() == 1 ? Roads.Landscapes[0].PaintLayers.FindByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Dirt")); }) : nullptr;
	float DirtShare = 0.f;
	if (DirtLine)
	{
		FString Share;
		DirtLine->Split(TEXT(": "), nullptr, &Share, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		DirtShare = FCString::Atof(*Share);
	}
	TestTrue(FString::Printf(TEXT("dirt is painted under the road (%.1f%% of the landscape)"), DirtShare), DirtShare >= 1.f);
	TestTrue(TEXT("and the landscape has a splines edit layer"), Roads.bOk && Roads.Landscapes.Num() == 1 && Roads.Landscapes[0].EditLayers.Contains(TEXT("Splines")));
	const ULandscapeSubsystem* Subsystem = GEditor->GetEditorWorldContext().World()->GetSubsystem<ULandscapeSubsystem>();
	if (Subsystem && Subsystem->IsGridBased())
	{
		bool bRoadActor = false;
		for (TActorIterator<ALandscapeSplineActor> It(GEditor->GetEditorWorldContext().World()); It; ++It)
		{
			bRoadActor |= It->GetActorLabel() == RoadLabel;
		}
		TestTrue(TEXT("in World Partition the road is its own spline actor"), bRoadActor);
	}

	// Biomes as a mosaic: meadow in patches over half the ground, and a verge of dirt along the road.
	FHyperAILandscapeOp Meadow = Op(TEXT("landscape.add_paint_layer"), Label);
	Meadow.PaintLayers = {TEXT("Meadow")};
	Meadow.LayerInfoFolder = Folder;
	FHyperAILandscapeOp Mosaic = Op(TEXT("landscape.paint"), Label);
	FHyperAILandscapePaintRule Patches = Rule(TEXT("Meadow"));
	Patches.Patch = 40.f;
	Patches.Coverage = 0.5f;
	FHyperAILandscapePaintRule Verge = Rule(TEXT("Dirt"));
	Verge.DistanceMax = 6.f;
	Verge.DistanceFade = 2.f;
	Mosaic.Rules = {Rule(TEXT("Grass")), Patches, Verge};
	FHyperAILandscapeOp Tiny = Mosaic;
	Tiny.Rules[1].Patch = 2.f;
	TestFalse(TEXT("patches under 5 m are refused"), Run({Meadow, Tiny}, /*bDryRun=*/true).bOk);
	FHyperAILandscapeOp Far = Mosaic;
	Far.Rules[2].DistanceMax = 5000.f;
	TestFalse(TEXT("a distance past 2 km is refused"), Run({Meadow, Far}, /*bDryRun=*/true).bOk);
	const FHyperAILandscapePlanReport Mosaicked = Run({Meadow, Mosaic});
	TestTrue(TEXT("the mosaic is painted: ") + Mosaicked.Diagnostic, Mosaicked.bOk && Mosaicked.Applied == 2);
	const FHyperAILandscapeInspectReport Shares = C::Inspect(Look);
	auto ShareOf = [&Shares](const TCHAR* Layer)
	{
		const FString* Line = Shares.bOk && Shares.Landscapes.Num() == 1 ? Shares.Landscapes[0].PaintLayers.FindByPredicate([Layer](const FString& Each) { return Each.StartsWith(Layer); }) : nullptr;
		FString Share;
		if (Line) Line->Split(TEXT(": "), nullptr, &Share, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		return FCString::Atof(*Share);
	};
	const float MeadowShare = ShareOf(TEXT("Meadow"));
	const float VergeShare = ShareOf(TEXT("Dirt"));
	AddInfo(FString::Printf(TEXT("Mosaic: meadow %.1f%%, dirt %.1f%%, grass %.1f%%."), MeadowShare, VergeShare, ShareOf(TEXT("Grass"))));
	TestTrue(FString::Printf(TEXT("meadow patches take about half the ground (%.1f%%)"), MeadowShare), MeadowShare > 38.f && MeadowShare < 56.f);
	TestTrue(FString::Printf(TEXT("dirt lines the road and no more (%.1f%%)"), VergeShare), VergeShare > 1.5f && VergeShare < 10.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLandscapeMaterialTest,
	"HyperAIStudio.NativeTools.Worldbuilding.LandscapeMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLandscapeMaterialTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LandscapeTests;
	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		AddInfo(TEXT("Skipped: no editor world."));
		return true;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Label = TEXT("HyperAI_TestLandscapeColors_") + Suffix;
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/LandscapeMaterial_") + Suffix;
	const FString Colors = Folder + TEXT("/M_Colors");
	const FString Own = Folder + TEXT("/M_Own");
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		Remove(Label, Folder);
	};

	FHyperAILandscapeOp Create = Op(TEXT("landscape.create"), Label);
	Create.Location = FVector(-600000.0, 600000.0, 0.0);
	Create.Size = 252.f;
	Create.PaintLayers = {TEXT("Grass"), TEXT("Rock"), TEXT("Snow")};
	Create.LayerInfoFolder = Folder;
	// A material made with no landscape named, then set on the landscape by a later op of the same plan.
	FHyperAILandscapeOp Make = Op(TEXT("landscape.create_material"), FString());
	Make.Material = Colors;
	Make.PaintLayers = {TEXT("Grass"), TEXT("Rock"), TEXT("Snow")};
	Make.LayerColors = {FLinearColor(0.8f, 0.1f, 0.1f)};
	FHyperAILandscapeOp Set = Op(TEXT("landscape.set_material"), Label);
	Set.Material = Colors;

	// Refused before anything changes.
	FHyperAILandscapeOp Outside = Make;
	Outside.Material = TEXT("/Engine/M_Colors_") + Suffix;
	TestFalse(TEXT("a material outside /Game is refused"), Run({Outside}, /*bDryRun=*/true).bOk);
	FHyperAILandscapeOp Nothing = Make;
	Nothing.PaintLayers.Reset();
	Nothing.LayerColors.Reset();
	TestFalse(TEXT("a material with no layers and no landscape to take them from is refused"), Run({Nothing}, /*bDryRun=*/true).bOk);
	FHyperAILandscapeOp Glowing = Make;
	Glowing.LayerColors = {FLinearColor(2.f, 0.f, 0.f)};
	TestFalse(TEXT("a color past 1 is refused"), Run({Glowing}, /*bDryRun=*/true).bOk);
	FHyperAILandscapeOp Twice = Make;
	Twice.PaintLayers = {TEXT("Grass"), TEXT("grass")};
	TestFalse(TEXT("a layer named twice is refused"), Run({Twice}, /*bDryRun=*/true).bOk);
	TestFalse(TEXT("setting a material no op makes is refused"), Run({Create, Set}, /*bDryRun=*/true).bOk);
	TestTrue(TEXT("setting the material an earlier op makes checks"), Run({Create, Make, Set}, /*bDryRun=*/true).bOk);
	FHyperAILandscapeOp SetOther = Set;
	SetOther.Material = Colors + TEXT(".M_Other");
	TestFalse(TEXT("setting another object in the made material's package is refused"), Run({Create, Make, SetOther}, /*bDryRun=*/true).bOk);
	TestFalse(TEXT("making the same material twice is refused"), Run({Create, Make, Make}, /*bDryRun=*/true).bOk);
	{
		// An asset only in memory, never saved, takes its path as surely as one on disk.
		const FString Taken = Folder + TEXT("/M_Taken");
		UMaterial* InMemory = NewObject<UMaterial>(CreatePackage(*Taken), TEXT("M_Taken"), RF_Public | RF_Standalone | RF_Transient);
		FHyperAILandscapeOp Over = Make;
		Over.Material = Taken;
		TestFalse(TEXT("a path an unsaved asset holds is refused"), Run({Over}, /*bDryRun=*/true).bOk);
		InMemory->ClearFlags(RF_Public | RF_Standalone);
		InMemory->MarkAsGarbage();
	}

	const FHyperAILandscapePlanReport Done = Run({Create, Make, Set});
	if (!TestTrue(TEXT("the landscape is made, and the material made and set on it: ") + Done.Diagnostic + TEXT(" | ") + FString::Join(Done.Notes, TEXT(" | ")),
		Done.bOk && Done.Applied == 3))
	{
		return false;
	}
	AddInfo(FString::Join(Done.Notes, TEXT(" | ")));
	TestTrue(TEXT("the material is saved with the layer infos"), Done.Saved.ContainsByPredicate([&Colors](const FString& Path) { return Path.StartsWith(Colors + TEXT(".")); }));
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *(Colors + TEXT(".M_Colors")));
	const UMaterialExpressionLandscapeLayerBlend* Blend = BlendOf(Material);
	if (!TestNotNull(TEXT("it has a landscape layer blend"), Blend)) return false;
	TestTrue(TEXT("whose layers are the landscape's paint layers, in order"), Blend->Layers.Num() == 3 && Blend->Layers[0].LayerName == FName(TEXT("Grass"))
		&& Blend->Layers[1].LayerName == FName(TEXT("Rock")) && Blend->Layers[2].LayerName == FName(TEXT("Snow")));
	bool bConstant = true;
	bool bDistinct = true;
	for (int32 Index = 0; Index < Blend->Layers.Num(); ++Index)
	{
		const FLayerBlendInput& Layer = Blend->Layers[Index];
		bConstant &= Layer.LayerInput.Expression == nullptr && Layer.BlendType == LB_WeightBlend;
		for (int32 Other = Index + 1; Other < Blend->Layers.Num(); ++Other) bDistinct &= FVector::Dist(Layer.ConstLayerInput, Blend->Layers[Other].ConstLayerInput) > 0.05;
	}
	TestTrue(TEXT("each a weight blended flat color, no texture"), bConstant);
	TestTrue(TEXT("every layer in a color of its own"), bDistinct);
	TestTrue(TEXT("the grass in the red asked for"), Blend->Layers[0].ConstLayerInput.Equals(FVector(0.8, 0.1, 0.1), 0.001));
	TestTrue(TEXT("the snow white from its name"), Blend->Layers[2].ConstLayerInput.X > 0.8 && Blend->Layers[2].ConstLayerInput.Z > 0.8);
	TestTrue(TEXT("the blend is the material's base color"), Material->GetEditorOnlyData()->BaseColor.Expression == Blend);
	bool bBound = true;
	for (const UMaterialExpression* Expression : Material->GetExpressionCollection().Expressions)
	{
		bBound &= Expression && Expression->Material == Material && Expression->HasAnyFlags(RF_Transactional);
	}
	TestTrue(TEXT("its nodes are bound to it, as the material editor binds them, and undoable"), bBound && Material->GetExpressionCollection().Expressions.Num() == 2);
	const TArray<FName> Named = UE::Landscape::RetrieveTargetLayerNamesFromMaterial(Material);
	TestTrue(TEXT("landscapes read its three layers from it"), Named.Contains(FName(TEXT("Grass"))) && Named.Contains(FName(TEXT("Rock"))) && Named.Contains(FName(TEXT("Snow"))));
	ALandscape* Landscape = Find(Label);
	if (!TestNotNull(TEXT("the landscape is in the level"), Landscape)) return false;
	TestTrue(TEXT("the landscape takes the material"), Landscape->LandscapeMaterial == Material);
	bool bProxies = true;
	int32 Proxies = 0;
	if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
	{
		Info->ForEachLandscapeProxy([&bProxies, &Proxies, Material](ALandscapeProxy* Proxy)
		{
			bProxies &= Proxy->GetLandscapeMaterial() == Material;
			++Proxies;
			return true;
		});
	}
	TestTrue(FString::Printf(TEXT("and so does each of its %d proxies"), Proxies), bProxies && Proxies > 0);
	FHyperAILandscapeInspectRequest Look;
	Look.Landscapes = {Label};
	const FHyperAILandscapeInspectReport Read = C::Inspect(Look);
	TestTrue(TEXT("inspect names the material"), Read.bOk && Read.Landscapes.Num() == 1 && Read.Landscapes[0].Material == Material->GetPathName());

	// A second, named on the landscape: its layers are the landscape's own, and it is set on the landscape at once.
	FHyperAILandscapeOp Mine = Op(TEXT("landscape.create_material"), Label);
	Mine.Material = Own;
	// Asked for approval first: nothing is made while it waits, and it names the material and the landscape it is set on.
	Settings->bRequireApprovalForAgentEdits = true;
	const FHyperAILandscapePlanReport Waiting = Run({Mine});
	Settings->bRequireApprovalForAgentEdits = false;
	const TArray<FHyperAIStudioPendingApproval> Pending = FHyperAIStudioApprovalGate::GetPending();
	const FHyperAIStudioPendingApproval* Asked = Pending.FindByPredicate([&Waiting](const FHyperAIStudioPendingApproval& Each) { return Each.OperationId == Waiting.OperationId; });
	TestTrue(TEXT("with approval on, the plan waits, naming the material and the landscape"), Waiting.Status == TEXT("awaiting_user_approval") && Asked
		&& Asked->Summary.Touches.Contains(Own) && Asked->Summary.Touches.Contains(Label));
	TestTrue(TEXT("and nothing is made while it waits"), !FPackageName::DoesPackageExist(Own) && !FindObject<UMaterial>(nullptr, *(Own + TEXT(".M_Own"))));
	FHyperAIStudioApprovalGate::Reject(Waiting.OperationId, TEXT("test"));
	const FHyperAILandscapePlanReport Given = Run({Mine});
	TestTrue(TEXT("a material made from the landscape's own layers is set on it: ") + Given.Diagnostic, Given.bOk && Given.Made.Num() == 1);
	UMaterial* Second = LoadObject<UMaterial>(nullptr, *(Own + TEXT(".M_Own")));
	const UMaterialExpressionLandscapeLayerBlend* Taken = BlendOf(Second);
	TArray<FString> TakenNames;
	for (int32 Index = 0; Taken && Index < Taken->Layers.Num(); ++Index) TakenNames.Add(Taken->Layers[Index].LayerName.ToString());
	TestTrue(TEXT("its blend names the landscape's three layers: ") + FString::Join(TakenNames, TEXT(", ")) + TEXT(" | ") + FString::Join(Given.Notes, TEXT(" | ")),
		TakenNames == TArray<FString>({TEXT("Grass"), TEXT("Rock"), TEXT("Snow")}));
	TestTrue(TEXT("and the landscape wears it"), Second && Landscape->LandscapeMaterial == Second);
	return true;
}

namespace HyperAIStudio::LandscapeTests
{
	/** A paint layer's weight (0 to 255) at a world point: in one edit layer, or with no guid in the merged result. */
	int32 WeightAt(ALandscape& Landscape, const FGuid& EditLayer, const TCHAR* Layer, const FVector& Where)
	{
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		ULandscapeLayerInfoObject* LayerInfo = Info ? Info->GetLayerInfoByName(FName(Layer)) : nullptr;
		if (!LayerInfo) return -1;
		const FVector Local = Landscape.GetActorTransform().InverseTransformPosition(Where);
		const int32 X = FMath::RoundToInt(Local.X);
		const int32 Y = FMath::RoundToInt(Local.Y);
		FLandscapeEditDataInterface Edit(Info, EditLayer);
		uint8 Weight = 0;
		Edit.GetWeightDataFast(LayerInfo, X, Y, X, Y, &Weight, 0);
		return Weight;
	}

	FGuid EditLayerNamed(const ALandscape& Landscape, const TCHAR* Name)
	{
		for (const ULandscapeEditLayerBase* Layer : Landscape.GetEditLayersConst())
		{
			if (Layer && Layer->GetName().ToString() == Name) return Layer->GetGuid();
		}
		return FGuid();
	}

	bool NotesSay(const FHyperAILandscapePlanReport& Report, const FString& Text)
	{
		return Report.Notes.ContainsByPredicate([&Text](const FString& Note) { return Note.Contains(Text); });
	}

	/**
	 * Roads against paint: the road's layer paints away from it, its Splines edit layer covers the paint beneath it (the
	 * tool's layer infos use Advanced Weight Blending), an edit layer added after it paints over it, the Splines layer
	 * itself is refused, and an older landscape's layer infos without Advanced Weight Blending add up, as the plan notes.
	 */
	bool CheckRoadLayers(FAutomationTestBase& Test, const FString& Level)
	{
		Test.AddInfo(TEXT("In a level ") + Level + TEXT(":"));
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
		const FString Label = TEXT("HyperAI_TestLandscapeRoads_") + Suffix;
		const FString Older = TEXT("HyperAI_TestLandscapeOlder_") + Suffix;
		const FString Folder = TEXT("/Game/__HyperAIStudioTests/LandscapeRoads_") + Suffix;
		const FString OlderFolder = Folder + TEXT("_Older");
		const FString RoadLabel = TEXT("HyperAI_TestRoadLayer_") + Suffix;
		const FString OlderRoadLabel = TEXT("HyperAI_TestRoadOlder_") + Suffix;
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bApproval = Settings->bRequireApprovalForAgentEdits;
		Settings->bRequireApprovalForAgentEdits = false;
		ON_SCOPE_EXIT
		{
			Settings->bRequireApprovalForAgentEdits = bApproval;
			if (UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				for (TActorIterator<ALandscapeSplineActor> It(World); It; ++It)
				{
					if (It->GetActorLabel() == RoadLabel || It->GetActorLabel() == OlderRoadLabel) World->EditorDestroyActor(*It, /*bShouldModifyLevel=*/true);
				}
			}
			Remove(Label, Folder);
			Remove(Older, OlderFolder);
		};

		// 504 m a side at 1 m: four components of 126 m each way. Verge everywhere, then a road painted Path along the first
		// row of components, then an edit layer added after it, so above the road's Splines edit layer.
		const FVector Origin(600000.0, 600000.0, 0.0);
		const TArray<FString> Layers = {TEXT("Grass"), TEXT("Verge"), TEXT("Path"), TEXT("Snow")};
		FHyperAILandscapeOp Create = Op(TEXT("landscape.create"), Label);
		Create.Location = Origin;
		Create.Size = 504.f;
		Create.PaintLayers = Layers;
		Create.LayerInfoFolder = Folder;
		FHyperAILandscapeOp Verge = Op(TEXT("landscape.paint"), Label);
		Verge.Rules = {Rule(TEXT("Verge"))};
		FHyperAILandscapeOp Road = Op(TEXT("landscape.road"), Label);
		Road.Road = RoadLabel;
		Road.Points = {Origin + FVector(-22000.0, -20000.0, 0.0), Origin + FVector(22000.0, -20000.0, 0.0)};
		Road.Width = 6.f;
		Road.Falloff = 4.f;
		Road.Layer = TEXT("Path");
		FHyperAILandscapeOp Above = Op(TEXT("landscape.add_edit_layer"), Label);
		Above.EditLayer = TEXT("Above");
		auto Circle = [](const FString& Landscape, const FVector& Center, const TCHAR* Layer, const TCHAR* EditLayer)
		{
			FHyperAILandscapeOp Paint = Op(TEXT("landscape.paint"), Landscape);
			Paint.Center = Center;
			Paint.Radius = 6.f;
			Paint.Falloff = 2.f;
			Paint.Rules = {Rule(Layer)};
			Paint.EditLayer = EditLayer;
			return Paint;
		};
		const FVector NearRoad = Origin + FVector(-15000.0, -14000.0, 0.0);
		const FHyperAILandscapeOp OnSplines = Circle(Label, NearRoad, TEXT("Snow"), TEXT("Splines"));
		const FHyperAILandscapePlanReport Planned = Run({Create, Verge, Road, Above, OnSplines}, /*bDryRun=*/true);
		Test.TestTrue(TEXT("painting on the Splines edit layer a road in the same plan adds is refused: ") + Planned.Diagnostic,
			!Planned.bOk && Planned.Diagnostic.Contains(TEXT("the roads' edit layer")));
		const FHyperAILandscapePlanReport Made = Run({Create, Verge, Road, Above});
		if (!Test.TestTrue(TEXT("the landscape is made, painted Verge, and the road laid: ") + Made.Diagnostic, Made.bOk && Made.Applied == 4)) return false;
		ALandscape* Landscape = Find(Label);
		if (!Test.TestNotNull(TEXT("it is in the level"), Landscape)) return false;
		bool bAdvanced = true;
		for (const FString& Name : Layers)
		{
			const ULandscapeLayerInfoObject* LayerInfo = Landscape->GetLandscapeInfo()->GetLayerInfoByName(FName(*Name));
			bAdvanced &= LayerInfo && LayerInfo->GetBlendMethod() == ELandscapeTargetLayerBlendMethod::PremultipliedAlphaBlending;
		}
		Test.TestTrue(TEXT("the layer infos the tool makes use Advanced Weight Blending"), bAdvanced);
		Test.TestFalse(TEXT("so the plan has no note about layers adding up: ") + FString::Join(Made.Notes, TEXT(" | ")), NotesSay(Made, TEXT("Advanced Weight Blending")));
		TArray<FString> Stack;
		for (const ULandscapeEditLayerBase* Layer : Landscape->GetEditLayersConst()) Stack.Add(Layer ? Layer->GetName().ToString() : FString());
		Test.TestEqual(TEXT("its edit layers, bottom to top"), FString::Join(Stack, TEXT(", ")), FString(TEXT("Layer, Splines, Above")));
		const FGuid Base = EditLayerNamed(*Landscape, TEXT("Layer"));

		// Under the road its Path covers the Verge painted beneath it; the base edit layer keeps its Verge.
		const FVector OnRoad = Origin + FVector(0.0, -20000.0, 0.0);
		const int32 RoadPath = WeightAt(*Landscape, FGuid(), TEXT("Path"), OnRoad);
		const int32 RoadVerge = WeightAt(*Landscape, FGuid(), TEXT("Verge"), OnRoad);
		const int32 BaseVerge = WeightAt(*Landscape, Base, TEXT("Verge"), OnRoad);
		const FString UnderRoad = FString::Printf(TEXT("under the road Path covers the Verge beneath (Path %d, Verge %d; the base layer holds Verge %d)"), RoadPath, RoadVerge, BaseVerge);
		Test.AddInfo(UnderRoad);
		Test.TestTrue(UnderRoad, RoadPath >= 250 && RoadVerge <= 5 && BaseVerge >= 250);

		// The road's own layer paints away from the road, in a component the road crosses and in one it does not. Snow
		// painted on the base layer over the road leaves the road showing; painted on the edit layer above Splines, it covers it.
		const FVector Untouched = Origin + FVector(15000.0, 15000.0, 0.0);
		const FVector SnowBelow = Origin + FVector(-10000.0, -20000.0, 0.0);
		const FVector SnowAbove = Origin + FVector(10000.0, -20000.0, 0.0);
		const FHyperAILandscapePlanReport Painted = Run({Circle(Label, NearRoad, TEXT("Path"), TEXT("")), Circle(Label, Untouched, TEXT("Path"), TEXT("")),
			Circle(Label, SnowBelow, TEXT("Snow"), TEXT("")), Circle(Label, SnowAbove, TEXT("Snow"), TEXT("Above"))});
		if (!Test.TestTrue(TEXT("the circles are painted: ") + Painted.Diagnostic, Painted.bOk && Painted.Applied == 4)) return false;
		struct FProbe { const TCHAR* What; FVector Where; const TCHAR* Shows; const TCHAR* Hides; };
		const TArray<FProbe> Probes = {
			{TEXT("Path painted 60 m from the road, in a component it crosses, takes"), NearRoad, TEXT("Path"), TEXT("Verge")},
			{TEXT("Path painted in a component the road does not reach takes"), Untouched, TEXT("Path"), TEXT("Verge")},
			{TEXT("Snow painted on the base layer over the road leaves the road"), SnowBelow, TEXT("Path"), TEXT("Snow")},
			{TEXT("Snow painted on an edit layer above Splines covers the road"), SnowAbove, TEXT("Snow"), TEXT("Path")}};
		for (const FProbe& Probe : Probes)
		{
			const int32 Shown = WeightAt(*Landscape, FGuid(), Probe.Shows, Probe.Where);
			const int32 Hidden = WeightAt(*Landscape, FGuid(), Probe.Hides, Probe.Where);
			const FString Line = FString::Printf(TEXT("%s (%s %d, %s %d)"), Probe.What, Probe.Shows, Shown, Probe.Hides, Hidden);
			Test.AddInfo(Line);
			Test.TestTrue(Line, Shown >= 250 && Hidden <= 5);
		}

		// The Splines edit layer is the engine's to rebuild from the roads: no painting or sculpting there.
		FHyperAILandscapeOp Raise = Op(TEXT("landscape.sculpt"), Label);
		Raise.Mode = TEXT("raise");
		Raise.Center = NearRoad;
		Raise.Radius = 5.f;
		Raise.Amount = 1.f;
		Raise.EditLayer = TEXT("splines");
		for (const FHyperAILandscapeOp& Refused : {OnSplines, Raise})
		{
			const FHyperAILandscapePlanReport Report = Run({Refused}, /*bDryRun=*/true);
			Test.AddInfo(Report.Diagnostic);
			Test.TestTrue(FString::Printf(TEXT("%s on the Splines edit layer is refused: %s"), *Refused.Kind, *Report.Diagnostic), !Report.bOk && Report.Diagnostic.Contains(TEXT("add one after the road")));
		}

		// An older landscape's layer infos, reused from its folder, without Advanced Weight Blending: its edit layers add up,
		// so under the road Verge and Path both show at full weight. The check says so before anything changes.
		for (const FString& Name : Layers)
		{
			if (ULandscapeLayerInfoObject* LayerInfo = UE::Landscape::CreateTargetLayerInfo(FName(*Name), OlderFolder))
			{
				LayerInfo->SetBlendMethod(ELandscapeTargetLayerBlendMethod::None, /*bInModify=*/false);
			}
		}
		const FVector OlderOrigin(-600000.0, -600000.0, 0.0);
		FHyperAILandscapeOp CreateOlder = Create;
		CreateOlder.Landscape = Older;
		CreateOlder.Location = OlderOrigin;
		CreateOlder.LayerInfoFolder = OlderFolder;
		FHyperAILandscapeOp VergeOlder = Verge;
		VergeOlder.Landscape = Older;
		FHyperAILandscapeOp RoadOlder = Road;
		RoadOlder.Landscape = Older;
		RoadOlder.Road = OlderRoadLabel;
		RoadOlder.Points = {OlderOrigin + FVector(-22000.0, -20000.0, 0.0), OlderOrigin + FVector(22000.0, -20000.0, 0.0)};
		const FHyperAILandscapePlanReport OlderChecked = Run({CreateOlder, VergeOlder, RoadOlder}, /*bDryRun=*/true);
		Test.TestTrue(TEXT("the dry run notes that its layers add up: ") + FString::Join(OlderChecked.Notes, TEXT(" | ")),
			OlderChecked.bOk && NotesSay(OlderChecked, Older + TEXT(": the layer infos of Grass, Verge, Path, Snow have no Advanced Weight Blending")));
		const FHyperAILandscapePlanReport OlderMade = Run({CreateOlder, VergeOlder, RoadOlder});
		Test.TestTrue(TEXT("and so does the plan when it runs: ") + OlderMade.Diagnostic, OlderMade.bOk && NotesSay(OlderMade, TEXT("Advanced Weight Blending")));
		ALandscape* OlderLandscape = Find(Older);
		if (!Test.TestNotNull(TEXT("the older landscape is in the level"), OlderLandscape)) return false;
		Test.AddInfo(FString::Join(OlderMade.Notes, TEXT(" | ")));
		const FVector OlderRoad = OlderOrigin + FVector(0.0, -20000.0, 0.0);
		const int32 OlderRoadPath = WeightAt(*OlderLandscape, FGuid(), TEXT("Path"), OlderRoad);
		const int32 OlderRoadVerge = WeightAt(*OlderLandscape, FGuid(), TEXT("Verge"), OlderRoad);
		const FString Added = FString::Printf(TEXT("under its road Path is added to the Verge beneath, not covering it (Path %d, Verge %d)"), OlderRoadPath, OlderRoadVerge);
		Test.AddInfo(Added);
		Test.TestTrue(Added, OlderRoadPath >= 250 && OlderRoadVerge >= 250);
		const FVector OlderAway = OlderOrigin + FVector(-15000.0, -14000.0, 0.0);
		const FHyperAILandscapePlanReport OlderPainted = Run({Circle(Older, OlderAway, TEXT("Path"), TEXT(""))});
		const int32 AwayPath = WeightAt(*OlderLandscape, FGuid(), TEXT("Path"), OlderAway);
		const int32 AwayVerge = WeightAt(*OlderLandscape, FGuid(), TEXT("Verge"), OlderAway);
		const FString Away = FString::Printf(TEXT("away from its road Path paints there too (Path %d, Verge %d): %s"), AwayPath, AwayVerge, *OlderPainted.Diagnostic);
		Test.AddInfo(Away);
		Test.TestTrue(Away, OlderPainted.bOk && AwayPath >= 250 && AwayVerge <= 5);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLandscapeRoadLayerTest,
	"HyperAIStudio.NativeTools.Worldbuilding.LandscapeRoadLayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLandscapeRoadLayerTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LandscapeTests;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		AddInfo(TEXT("Skipped: no editor world."));
		return true;
	}
	const ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>();
	const bool bPartitioned = Subsystem && Subsystem->IsGridBased();
	const FString Open = World->GetOutermost()->GetName();
	// Unattended runs check the other kind of level too, in a new untitled map, then load the open level back from disk.
	// Loading it back drops its unsaved changes, so an editor someone works in checks only the open level.
	if (FApp::IsUnattended() && FPackageName::DoesPackageExist(Open))
	{
		GEditor->NewMap(/*bIsPartitionedWorld=*/!bPartitioned);
		const bool bOther = CheckRoadLayers(*this, bPartitioned ? TEXT("without World Partition") : TEXT("with World Partition"));
		FEditorFileUtils::LoadMap(FPackageName::LongPackageNameToFilename(Open, FPackageName::GetMapPackageExtension()), /*LoadAsTemplate=*/false, /*bShowProgress=*/false);
		if (!TestTrue(TEXT("the open level is loaded back: ") + Open, GEditor->GetEditorWorldContext().World() && GEditor->GetEditorWorldContext().World()->GetOutermost()->GetName() == Open) || !bOther)
		{
			return false;
		}
	}
	else
	{
		AddInfo(TEXT("Checked the open level only; unattended runs of a saved level check the other kind too."));
	}
	return CheckRoadLayers(*this, bPartitioned ? TEXT("with World Partition") : TEXT("without World Partition"));
}

#endif // WITH_DEV_AUTOMATION_TESTS
