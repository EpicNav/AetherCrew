// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "HyperAIStudioLandscapeToolset.h"
#include "HyperAIStudioSettings.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeSplineActor.h"
#include "LandscapeSubsystem.h"
#include "LandscapeUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
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
	TestFalse(TEXT("making the same material twice is refused"), Run({Create, Make, Make}, /*bDryRun=*/true).bOk);

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

#endif // WITH_DEV_AUTOMATION_TESTS
