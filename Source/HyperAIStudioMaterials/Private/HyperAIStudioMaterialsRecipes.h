// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioMaterialsToolset.h"

class UMaterial;
class UMaterialFunction;

/**
 * hyper_material_recipe: production materials described as data. A recipe and its settings become a graph (nodes with
 * typed keys, wires, outputs and material settings) without touching an object, so the dry run shows exactly what will
 * be built; Instantiate then builds that graph with the node catalog's own typed writers (HyperAIStudioMaterialsGraphGate)
 * plus the few engine nodes the catalog does not make: the Single Layer Water output, landscape layer coordinates and
 * blend, the Substrate slab, and material function inputs and outputs.
 */
namespace HyperAIStudio::Materials::Recipes
{
	struct FNode
	{
		FString Id;
		/** A node catalog kind, or slw_output, landscape_coords, landscape_layer_blend, substrate_slab, function_input, function_output. */
		FString Kind;
		TArray<TPair<FString, FString>> Properties;
	};

	struct FWire
	{
		FString From;
		/** An output's name, such as RGB or R; empty is the first output. */
		FString FromOutput;
		FString To;
		/** An input's name as the editor shows it, spaces and case aside; empty is the first input. */
		FString ToInput;
	};

	struct FOutput
	{
		/** base_color, normal, roughness, ... as hyper_material_apply_plan names them, or front_material. */
		FString Property;
		FString From;
		FString FromOutput;
	};

	struct FGraph
	{
		FString Recipe;
		bool bFunction = false;
		/** surface or deferred_decal. */
		FString Domain = TEXT("surface");
		/** opaque or translucent. */
		FString BlendMode = TEXT("opaque");
		/** default_lit, single_layer_water or substrate. */
		FString ShadingModel = TEXT("default_lit");
		TArray<FNode> Nodes;
		TArray<FWire> Wires;
		TArray<FOutput> Outputs;
		/** "Name (scalar)" and the like: what an instance can set. */
		TArray<FString> Parameters;
		TArray<FString> Notes;
		/** material_function: its inputs as (name, scalar|vector2|vector3|vector4) and its output's name. */
		TArray<TPair<FString, FString>> FunctionInputs;
		FString FunctionOutput;
	};

	/** Every recipe, or the one named. */
	TArray<FHyperAIMaterialRecipeInfo> List(const FString& Only = FString());
	bool IsRecipe(const FString& Id);
	/** Code recipes: custom_hlsl and material_function, whose dry run compiles the code off to the side. */
	bool IsCodeRecipe(const FString& Id);

	/** The recipe's graph for these settings (keys lower case), or why not. Touches no object. */
	bool BuildGraph(const FString& Recipe, const TMap<FString, FString>& Settings, const FString& Code, FGraph& Out, FString& OutError);
	/**
	 * A material calling the function at FunctionPath: a parameter per function input (default from the settings, keyed
	 * by the input's name in lower case) and the function's output into caller_output (emissive or base_color).
	 */
	bool BuildCallerGraph(const FGraph& Function, const FString& FunctionPath, const TMap<FString, FString>& Settings, FGraph& Out,
		FString& OutError);
	/** "id kind" per node, "from.output -> to.input" per wire, "from -> property" per output. */
	TArray<FString> Describe(const FGraph& Graph);
	/** Textures and functions the graph names, each of which must load. */
	bool CheckAssets(const FGraph& Graph, FString& OutError);

	/** Builds Graph into an empty material or material function (exactly one non-null), in the caller's transaction if any. */
	bool Instantiate(const FGraph& Graph, UMaterial* Material, UMaterialFunction* Function, FString& OutError);
	/** Recompiles the material and waits for its shaders: errors, instruction and sampler counts. */
	FHyperAIMaterialCompileStats CompileNow(UMaterial& Material);
	/** Substrate (r.Substrate) is on in this project. */
	bool IsSubstrateOn();
}
