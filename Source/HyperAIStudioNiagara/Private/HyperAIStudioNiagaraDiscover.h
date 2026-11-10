// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioNiagaraToolset.h"

/**
 * hyper_niagara_discover: what effects are built from, read without changing anything. Templates, modules and dynamic
 * inputs come from the asset registry (Niagara's own library filter for scripts); script schemas and the dynamic
 * inputs of a type come through the external-edit gate; renderer and data interface properties are read from their
 * classes by reflection; recipes compile through HyperAIStudioNiagaraRecipes. Game thread only.
 */
namespace HyperAIStudio::Niagara::Discover
{
	FHyperAINiagaraDiscoverReport Run(const FHyperAINiagaraDiscoverRequest& Request);

	/** All of Query's words appear in Text, ignoring case; an empty query matches everything. */
	bool Matches(const FString& Query, const FString& Text);
}
