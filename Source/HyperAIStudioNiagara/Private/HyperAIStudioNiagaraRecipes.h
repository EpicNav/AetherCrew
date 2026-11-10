// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioNiagaraToolset.h"

/**
 * Effect recipes as data: each turns its settings into hyper_niagara_apply_plan ops (create_system, emitters from
 * Niagara's own templates, and set_input_value, renderer and property ops on them) without touching an object, so a
 * dry run shows the whole effect before it is made. Every emitter simulates on the CPU, so previews and older
 * platforms draw it the same way.
 */
namespace HyperAIStudio::Niagara::Recipes
{
	struct FRecipeSetting
	{
		FString Key;
		FString Default;
		FString Help;
	};

	struct FRecipeInfo
	{
		FString Id;
		FString Summary;
		TArray<FRecipeSetting> Settings;
	};

	const TArray<FRecipeInfo>& List();

	/**
	 * The ops for Recipe with Settings ("key=value"; unknown keys refused) making a System at TargetPath, plus the particle
	 * materials it uses (each line names a path and the hyper_material_recipe call that makes it) and notes.
	 */
	bool Compile(const FString& Recipe, const TArray<FString>& Settings, const FString& TargetPath, TArray<FHyperAINiagaraEditOp>& OutOps,
		TArray<FString>& OutMaterials, TArray<FString>& OutNotes, FString& OutError);
}
