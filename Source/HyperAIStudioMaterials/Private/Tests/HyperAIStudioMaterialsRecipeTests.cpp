// Games by Hyper 2026.

#include "HyperAIStudioMaterialsRecipes.h"
#include "HyperAIStudioMaterialsToolset.h"

#include "Algo/Count.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioSettings.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/SoftObjectPath.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::Materials::RecipeTests
{
	using namespace HyperAIStudio::Materials::Recipes;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* const GoodCode = TEXT("return Tint * Amount;");

	/** Every wire and output names nodes the graph has. */
	bool IsClosed(const FGraph& Graph, FString& OutWhy)
	{
		TSet<FString> Ids;
		for (const FNode& Node : Graph.Nodes)
		{
			bool bDuplicate = false;
			Ids.Add(Node.Id, &bDuplicate);
			if (bDuplicate)
			{
				OutWhy = TEXT("duplicate node ") + Node.Id;
				return false;
			}
		}
		for (const FWire& Wire : Graph.Wires)
		{
			if (!Ids.Contains(Wire.From) || !Ids.Contains(Wire.To))
			{
				OutWhy = FString::Printf(TEXT("wire %s -> %s"), *Wire.From, *Wire.To);
				return false;
			}
		}
		for (const FOutput& Output : Graph.Outputs)
		{
			if (!Ids.Contains(Output.From))
			{
				OutWhy = TEXT("output from ") + Output.From;
				return false;
			}
		}
		return true;
	}

	FHyperAIMaterialRecipeRequest Request(const TCHAR* Recipe, const FString& Path, TArray<FHyperAIMaterialNodeProperty> Settings = {}, const FString& Code = FString())
	{
		FHyperAIMaterialRecipeRequest Out;
		Out.Recipe = Recipe;
		Out.Path = Path;
		Out.Settings = MoveTemp(Settings);
		Out.Code = Code;
		return Out;
	}

	/** A folder of its own per run, everything in it deleted after, with the approval setting put back. */
	struct FScratch
	{
		FString Folder = TEXT("/Game/__HyperAIStudioTests/Recipes_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
		bool bApproval = true;

		FScratch()
		{
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			bApproval = Settings->bRequireApprovalForAgentEdits;
			Settings->bRequireApprovalForAgentEdits = false;
		}

		~FScratch()
		{
			GetMutableDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits = bApproval;
			TArray<FAssetData> Assets;
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPath(FName(*Folder), Assets, /*bRecursive=*/true);
			// Callers before the functions they call, or the function stays "in use".
			TArray<UObject*> Materials;
			TArray<UObject*> Functions;
			for (const FAssetData& Asset : Assets)
			{
				if (UObject* Object = Asset.GetAsset())
				{
					(Object->IsA<UMaterialFunction>() ? Functions : Materials).Add(Object);
				}
			}
			for (const TArray<UObject*>* Objects : {&Materials, &Functions})
			{
				if (!Objects->IsEmpty()) ObjectTools::ForceDeleteObjects(*Objects, /*ShowConfirmation=*/false);
			}
		}

		FString Path(const TCHAR* Name) const { return Folder / Name; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIMaterialRecipeGraphsTest, "HyperAIStudio.NativeTools.Materials.Recipes.Graphs",
	HyperAIStudio::Materials::RecipeTests::Flags)

bool FHyperAIMaterialRecipeGraphsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Materials::RecipeTests;
	const TArray<FHyperAIMaterialRecipeInfo> All = List();
	TestEqual(TEXT("seven recipes"), All.Num(), 7);
	for (const FHyperAIMaterialRecipeInfo& Info : All)
	{
		FGraph Graph;
		FString Error;
		const TMap<FString, FString> Settings = Info.Id == TEXT("custom_hlsl")
			? TMap<FString, FString>{{TEXT("inputs"), TEXT("Tint=1,0.5,0.2; Amount=0.5")}} : TMap<FString, FString>();
		const bool bBuilt = BuildGraph(Info.Id, Settings, GoodCode, Graph, Error);
		TestTrue(FString::Printf(TEXT("%s builds with its defaults: %s"), *Info.Id, *Error), bBuilt);
		FString Why;
		TestTrue(FString::Printf(TEXT("%s's graph is closed: %s"), *Info.Id, *Why), bBuilt && IsClosed(Graph, Why));
		TestTrue(FString::Printf(TEXT("%s lists its settings"), *Info.Id), !Info.Settings.IsEmpty() && !Info.Summary.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s exposes parameters"), *Info.Id), !Graph.Parameters.IsEmpty());
	}

	FGraph Graph;
	FString Error;
	TestTrue(TEXT("layered_pbr builds"), BuildGraph(TEXT("layered_pbr"), {{TEXT("tiling"), TEXT("4")}}, FString(), Graph, Error));
	for (const TCHAR* Parameter : {TEXT("BaseColor (texture)"), TEXT("Normal (texture)"), TEXT("ORM (texture)"), TEXT("DetailNormal (texture)"),
		TEXT("Tiling (scalar)"), TEXT("DetailTiling (scalar)"), TEXT("DetailNormalStrength (scalar)"), TEXT("Tint (vector)")})
	{
		TestTrue(FString::Printf(TEXT("layered_pbr exposes %s"), Parameter), Graph.Parameters.Contains(Parameter));
	}
	TestTrue(TEXT("the tiling setting is the parameter's default"), Graph.Nodes.ContainsByPredicate([](const FNode& Node)
	{
		return Node.Kind == TEXT("scalar_parameter") && Node.Properties.Contains(TPair<FString, FString>(TEXT("name"), TEXT("Tiling")))
			&& Node.Properties.Contains(TPair<FString, FString>(TEXT("default"), TEXT("4.0")));
	}));
	for (const TCHAR* Property : {TEXT("base_color"), TEXT("normal"), TEXT("ao"), TEXT("roughness"), TEXT("metallic")})
	{
		TestTrue(FString::Printf(TEXT("layered_pbr drives %s"), Property), Graph.Outputs.ContainsByPredicate([Property](const FOutput& Output) { return Output.Property == Property; }));
	}

	TestTrue(TEXT("water builds"), BuildGraph(TEXT("water"), {}, FString(), Graph, Error));
	TestEqual(TEXT("as Single Layer Water"), Graph.ShadingModel, FString(TEXT("single_layer_water")));
	TestTrue(TEXT("with the water output fed depth absorption"), Graph.Wires.ContainsByPredicate([](const FWire& Wire)
	{
		return Wire.From == TEXT("absorption") && Wire.To == TEXT("water_output") && Wire.ToInput == TEXT("AbsorptionCoefficients");
	}));
	TestTrue(TEXT("two panners on the normals"), Graph.Nodes.FilterByPredicate([](const FNode& Node) { return Node.Kind == TEXT("panner"); }).Num() == 2);
	TestTrue(TEXT("and refraction"), Graph.Outputs.ContainsByPredicate([](const FOutput& Output) { return Output.Property == TEXT("refraction"); }));

	TestTrue(TEXT("decal builds"), BuildGraph(TEXT("decal"), {{TEXT("opacity"), TEXT("0.7")}}, FString(), Graph, Error));
	TestTrue(TEXT("as a translucent deferred decal"), Graph.Domain == TEXT("deferred_decal") && Graph.BlendMode == TEXT("translucent"));
	TestTrue(TEXT("with opacity and a normal"), Graph.Outputs.ContainsByPredicate([](const FOutput& Output) { return Output.Property == TEXT("opacity"); })
		&& Graph.Outputs.ContainsByPredicate([](const FOutput& Output) { return Output.Property == TEXT("normal"); }));

	TestTrue(TEXT("landscape_layers builds"), BuildGraph(TEXT("landscape_layers"), {{TEXT("layers"), TEXT("Grass, Dirt, Snow")}}, FString(), Graph, Error));
	TestTrue(TEXT("one blend of the three layers"), Graph.Nodes.ContainsByPredicate([](const FNode& Node)
	{
		return Node.Kind == TEXT("landscape_layer_blend") && Node.Properties.Contains(TPair<FString, FString>(TEXT("layers"), TEXT("Grass,Dirt,Snow")));
	}));
	TestTrue(TEXT("each layer feeds its colour and height by name"), Graph.Wires.ContainsByPredicate([](const FWire& Wire) { return Wire.ToInput == TEXT("Layer Dirt"); })
		&& Graph.Wires.ContainsByPredicate([](const FWire& Wire) { return Wire.ToInput == TEXT("Height Dirt"); }));
	TestTrue(TEXT("each layer gets a tint parameter"), Graph.Parameters.Contains(TEXT("SnowTint (vector)")));
	TestFalse(TEXT("a repeated layer is refused"), BuildGraph(TEXT("landscape_layers"), {{TEXT("layers"), TEXT("Grass,grass")}}, FString(), Graph, Error));
	TestFalse(TEXT("so are nine layers"), BuildGraph(TEXT("landscape_layers"), {{TEXT("layers"), TEXT("A,B,C,D,E,F,G,H,I")}}, FString(), Graph, Error));

	TestFalse(TEXT("an unknown setting is refused"), BuildGraph(TEXT("decal"), {{TEXT("glossiness"), TEXT("1")}}, FString(), Graph, Error));
	TestTrue(TEXT("naming the settings there are"), Error.Contains(TEXT("normal_strength")));
	TestFalse(TEXT("a number that is not one is refused"), BuildGraph(TEXT("water"), {{TEXT("ior"), TEXT("wet")}}, FString(), Graph, Error));
	TestFalse(TEXT("custom_hlsl without code is refused"), BuildGraph(TEXT("custom_hlsl"), {}, FString(), Graph, Error));
	TestFalse(TEXT("an input that is not Name=default is refused"), BuildGraph(TEXT("custom_hlsl"), {{TEXT("inputs"), TEXT("Tint")}}, GoodCode, Graph, Error));

	TestTrue(TEXT("custom_hlsl builds"), BuildGraph(TEXT("custom_hlsl"), {{TEXT("inputs"), TEXT("Tint=1,0.5,0.2; Amount=0.5")}, {TEXT("output"), TEXT("emissive")}}, GoodCode, Graph, Error));
	TestTrue(TEXT("feeding a scalar and a vector parameter into the code"), Graph.Parameters.Contains(TEXT("Amount (scalar)")) && Graph.Parameters.Contains(TEXT("Tint (vector)")));
	TestTrue(TEXT("into emissive"), Graph.Outputs.Num() == 1 && Graph.Outputs[0].Property == TEXT("emissive"));

	FGraph Function;
	TestTrue(TEXT("material_function builds: ") + Error, BuildGraph(TEXT("material_function"), {{TEXT("inputs"), TEXT("Color:vector3, Strength:scalar")}},
		TEXT("return Color * Strength;"), Function, Error));
	TestTrue(TEXT("as a function with two inputs and an output"), Function.bFunction && Function.FunctionInputs.Num() == 2 && Function.FunctionOutput == TEXT("Result"));
	FGraph Caller;
	TestTrue(TEXT("its caller builds"), BuildCallerGraph(Function, TEXT("/Game/F/MF_Glow.MF_Glow"), {}, Caller, Error));
	TestTrue(TEXT("calling it with a parameter per input"), Caller.Parameters.Contains(TEXT("Color (vector)")) && Caller.Parameters.Contains(TEXT("Strength (scalar)")));
	TestTrue(TEXT("its output into emissive"), Caller.Outputs.Num() == 1 && Caller.Outputs[0].Property == TEXT("emissive") && Caller.Outputs[0].FromOutput == TEXT("Result"));
	TestFalse(TEXT("an input type that is not one is refused"), BuildGraph(TEXT("material_function"), {{TEXT("inputs"), TEXT("Color:colour")}}, GoodCode, Function, Error));
	TestTrue(TEXT("Describe lists nodes, wires and outputs"), Describe(Caller).ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("-> emissive")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIMaterialRecipeDryRunTest, "HyperAIStudio.NativeTools.Materials.Recipes.DryRunAndRefusals",
	HyperAIStudio::Materials::RecipeTests::Flags)

bool FHyperAIMaterialRecipeDryRunTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Materials::RecipeTests;
	using C = FHyperAIStudioMaterialsContracts;
	const FScratch Scratch;
	const FHyperAIMaterialRecipeReport Listed = UHyperAIStudioMaterialsToolset::hyper_material_recipe(FHyperAIMaterialRecipeRequest());
	TestTrue(TEXT("an empty recipe lists them"), Listed.bOk && Listed.Status == TEXT("listed") && Listed.Recipes.Num() == 7);
	TestEqual(TEXT("an unknown recipe is refused"), C::Recipe(Request(TEXT("velvet"), Scratch.Path(TEXT("M_A")))).Status, FString(TEXT("invalid_request")));
	TestEqual(TEXT("a path outside /Game is refused"), C::Recipe(Request(TEXT("decal"), TEXT("/Engine/M_A"))).Status, FString(TEXT("invalid_request")));
	TestEqual(TEXT("caller_path goes only with material_function"), [&]()
	{
		FHyperAIMaterialRecipeRequest Wrong = Request(TEXT("decal"), Scratch.Path(TEXT("M_A")));
		Wrong.CallerPath = Scratch.Path(TEXT("M_B"));
		return C::Recipe(Wrong).Status;
	}(), FString(TEXT("invalid_request")));

	const FHyperAIMaterialRecipeReport Checked = C::Recipe(Request(TEXT("layered_pbr"), Scratch.Path(TEXT("M_Rock"))));
	TestTrue(TEXT("a dry run checks: ") + Checked.Diagnostic, Checked.bOk && Checked.Status == TEXT("checked"));
	TestTrue(TEXT("lists the graph and parameters"), Checked.Graph.Num() > 20 && Checked.Parameters.Contains(TEXT("ORM (texture)")));
	TestFalse(TEXT("and makes nothing"), FPackageName::DoesPackageExist(Scratch.Path(TEXT("M_Rock"))) || FindObject<UObject>(nullptr, *Checked.Path));
	TestEqual(TEXT("a texture that does not load is refused"), C::Recipe(Request(TEXT("decal"), Scratch.Path(TEXT("M_A")),
		{{TEXT("normal_texture"), TEXT("/Game/Nope/T_Nope.T_Nope")}})).Status, FString(TEXT("invalid_request")));

	const FHyperAIMaterialRecipeReport Broken = C::Recipe(Request(TEXT("custom_hlsl"), Scratch.Path(TEXT("M_Code")),
		{{TEXT("inputs"), TEXT("Tint=1,0.5,0.2")}}, TEXT("return Tint * ;")));
	TestEqual(TEXT("broken HLSL is caught in the dry run by compiling it"), Broken.Status, FString(TEXT("compile_errors")));
	TestTrue(TEXT("with the compiler's errors"), !Broken.Stats.CompileErrors.IsEmpty());
	const FHyperAIMaterialRecipeReport Compiles = C::Recipe(Request(TEXT("custom_hlsl"), Scratch.Path(TEXT("M_Code")),
		{{TEXT("inputs"), TEXT("Tint=1,0.5,0.2; Amount=0.5")}}, GoodCode));
	TestTrue(TEXT("good HLSL dry-runs clean: ") + Compiles.Diagnostic, Compiles.bOk && Compiles.Status == TEXT("checked") && Compiles.Stats.CompileErrors.IsEmpty());
	TestTrue(TEXT("with its instruction count"), Compiles.Stats.bAvailable && Compiles.Stats.NumPixelShaderInstructions > 0);
	FHyperAIMaterialRecipeRequest FunctionCheck = Request(TEXT("material_function"), Scratch.Path(TEXT("MF_Bad")), {{TEXT("inputs"), TEXT("Color:vector3")}},
		TEXT("return Colour * 2;"));
	TestEqual(TEXT("a function's code is compiled through a caller in the dry run"), C::Recipe(FunctionCheck).Status, FString(TEXT("compile_errors")));

	const FHyperAIMaterialRecipeReport Slab = C::Recipe(Request(TEXT("substrate_slab"), Scratch.Path(TEXT("M_Slab"))));
	if (Slab.bSubstrateEnabled)
	{
		TestTrue(TEXT("with Substrate on, the slab dry-runs: ") + Slab.Diagnostic, Slab.bOk && Slab.Status == TEXT("checked"));
	}
	else
	{
		TestEqual(TEXT("with Substrate off, the slab says so"), Slab.Status, FString(TEXT("substrate_disabled")));
		TestTrue(TEXT("and how to turn it on"), Slab.Diagnostic.Contains(TEXT("r.Substrate")));
	}

	// With approval on, a real run waits for the user; turned down, nothing is made.
	GetMutableDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits = true;
	FHyperAIMaterialRecipeRequest Held = Request(TEXT("decal"), Scratch.Path(TEXT("M_Held")));
	Held.bDryRun = false;
	const FHyperAIMaterialRecipeReport Waiting = C::Recipe(Held);
	TestTrue(TEXT("a real run waits for approval: ") + Waiting.Diagnostic, Waiting.bOk
		&& (Waiting.Status == TEXT("awaiting_user_approval") || Waiting.Status == TEXT("queued")) && FHyperAIStudioApprovalGate::HasPending(Waiting.OperationId));
	FHyperAIStudioApprovalGate::Reject(Waiting.OperationId, TEXT("Not now."));
	TestFalse(TEXT("rejected, nothing is made"), FindObject<UObject>(nullptr, *Waiting.Path) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIMaterialRecipeApplyTest, "HyperAIStudio.NativeTools.Materials.Recipes.MakeEveryRecipe",
	HyperAIStudio::Materials::RecipeTests::Flags)

bool FHyperAIMaterialRecipeApplyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Materials::RecipeTests;
	using C = FHyperAIStudioMaterialsContracts;
	const FScratch Scratch;
	auto Make = [&](FHyperAIMaterialRecipeRequest In)
	{
		In.bDryRun = false;
		const FHyperAIMaterialRecipeReport Report = C::Recipe(In);
		TestTrue(FString::Printf(TEXT("%s is made, compiled and saved: %s %s"), *In.Recipe, *Report.Status, *Report.Diagnostic),
			Report.bOk && Report.Status == TEXT("completed") && Report.Stats.CompileErrors.IsEmpty() && Report.Saved.Contains(Report.Path));
		TestTrue(FString::Printf(TEXT("%s has a preview on disk"), *In.Recipe), !Report.PreviewImagePath.IsEmpty() && IFileManager::Get().FileExists(*Report.PreviewImagePath));
		return Report;
	};
	auto Load = [](const FString& Path) { return Cast<UMaterial>(FSoftObjectPath(Path).TryLoad()); };

	const FHyperAIMaterialRecipeReport Pbr = Make(Request(TEXT("layered_pbr"), Scratch.Path(TEXT("M_Rock")), {{TEXT("tiling"), TEXT("2")}}));
	if (UMaterial* Material = Load(Pbr.Path))
	{
		TestTrue(TEXT("the PBR material samples four textures"), Pbr.Stats.NumPixelTextureSamples >= 4);
		TestTrue(TEXT("and drives roughness"), Material->GetExpressionInputForProperty(MP_Roughness)->IsConnected());
	}
	const FHyperAIMaterialRecipeReport Water = Make(Request(TEXT("water"), Scratch.Path(TEXT("M_Lake"))));
	if (UMaterial* Material = Load(Water.Path))
	{
		TestTrue(TEXT("the water uses the Single Layer Water shading model"), Material->GetShadingModels().HasShadingModel(MSM_SingleLayerWater));
	}
	const FHyperAIMaterialRecipeReport Decal = Make(Request(TEXT("decal"), Scratch.Path(TEXT("M_Puddle"))));
	if (UMaterial* Material = Load(Decal.Path))
	{
		TestTrue(TEXT("the decal is a deferred decal"), Material->MaterialDomain == MD_DeferredDecal);
	}
	const FHyperAIMaterialRecipeReport Ground = Make(Request(TEXT("landscape_layers"), Scratch.Path(TEXT("M_Ground"))));
	if (UMaterial* Material = Load(Ground.Path))
	{
		const TObjectPtr<UMaterialExpression>* Blend = Material->GetExpressions().FindByPredicate([](const TObjectPtr<UMaterialExpression>& Expression)
		{
			return Expression && Expression->IsA<UMaterialExpressionLandscapeLayerBlend>();
		});
		TestTrue(TEXT("the landscape material blends Grass, Rock and Snow by height"), Blend
			&& Cast<UMaterialExpressionLandscapeLayerBlend>(Blend->Get())->Layers.Num() == 3
			&& Cast<UMaterialExpressionLandscapeLayerBlend>(Blend->Get())->Layers[1].LayerName == FName(TEXT("Rock"))
			&& Cast<UMaterialExpressionLandscapeLayerBlend>(Blend->Get())->Layers[1].BlendType == LB_HeightBlend);
	}
	FHyperAIMaterialRecipeRequest FunctionRequest = Request(TEXT("material_function"), Scratch.Path(TEXT("MF_Glow")),
		{{TEXT("inputs"), TEXT("Color:vector3, Strength:scalar")}}, TEXT("return Color * Strength;"));
	FunctionRequest.CallerPath = Scratch.Path(TEXT("M_Glow"));
	const FHyperAIMaterialRecipeReport Glow = Make(FunctionRequest);
	const UMaterialFunction* Function = Cast<UMaterialFunction>(FSoftObjectPath(Glow.Path).TryLoad());
	TestTrue(TEXT("the function has its two inputs"), Function && Algo::CountIf(Function->GetExpressions(), [](const TObjectPtr<UMaterialExpression>& Expression)
	{
		return Expression && Expression->IsA<UMaterialExpressionFunctionInput>();
	}) == 2);
	UMaterial* Caller = Load(Glow.CallerPath);
	TestTrue(TEXT("and the caller calls it"), Caller && Caller->GetExpressions().ContainsByPredicate([Function](const TObjectPtr<UMaterialExpression>& Expression)
	{
		const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression.Get());
		return Call && Call->MaterialFunction == Function;
	}));
	TestTrue(TEXT("both are saved"), Glow.Saved.Num() == 2);

	const FHyperAIMaterialRecipeReport Code = Make(Request(TEXT("custom_hlsl"), Scratch.Path(TEXT("M_Code")),
		{{TEXT("inputs"), TEXT("Tint=1,0.5,0.2; Amount=0.5")}, {TEXT("output"), TEXT("emissive")}}, GoodCode));
	TestEqual(TEXT("a recipe already made is refused the second time"), C::Recipe(Request(TEXT("decal"), Scratch.Path(TEXT("M_Code")))).Status,
		FString(TEXT("asset_exists")));

	// Each recipe is one undo step, named as such.
	if (GEditor && GEditor->Trans)
	{
		TestEqual(TEXT("the last edit in the undo history is the recipe"), GEditor->Trans->GetUndoContext().Title.ToString(), FString(TEXT("HyperAI: Material Recipe")));
		TestTrue(TEXT("and undo takes it back in one step"), GEditor->UndoTransaction());
		TestTrue(TEXT("and redo puts it back"), GEditor->RedoTransaction());
	}
	return true;
}

#endif
