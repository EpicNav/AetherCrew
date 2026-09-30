// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioGASToolset.h"

/**
 * What hyper_gas_apply_plan runs: Gameplay Effect and Gameplay Ability Blueprints made and edited on their class defaults as
 * one undo step, then compiled and saved, the way the Details panel edits them. The typed planner
 * (FHyperAIStudioGASContracts::BuildPlan) stays as the strict, exact-revision check it was.
 */
namespace HyperAIStudio::GAS::Authoring
{
	FHyperAIGASApplyPlanReport Run(const FHyperAIGASApplyPlanRequest& Request);

	/** Loads the requested assets that exist, so inspect and validate read what is on disk as well as what is open. */
	void LoadRequested(const TArray<FString>& AssetPaths);
}
