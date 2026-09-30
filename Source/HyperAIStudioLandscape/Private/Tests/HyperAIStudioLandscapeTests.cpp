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
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
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
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
