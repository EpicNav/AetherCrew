// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "HyperAIStudioBotShared.h"

class AActor;
class UActorComponent;

/**
 * Reads the Gameplay Ability System of an actor without linking it, so the bot runs in games that do not use it: the
 * ability system component is found through the engine's own lookup (the AbilitySystemInterface, then a component
 * search) called by reflection, and its attribute sets, granted abilities and active effects are read from their
 * reflected properties. Tags go through IGameplayTagAssetInterface, which the ability system component implements.
 */
namespace HyperAIStudio::Bot::Gameplay
{
	/** Target's ability system component: the actor's own, or for a pawn or controller its player state's. Null when it has none. */
	UActorComponent* FindAbilitySystem(AActor& Target);

	/** Whether Class is UAbilitySystemComponent or derives from it, by name, so GAS need not be loaded. */
	bool IsAbilitySystemClass(const UClass* Class);

	/**
	 * Attribute Name on AbilitySystem's attribute sets: Health, or HealthSet.Health for one set's; Health.Base reads the
	 * base value instead of the current one. OutAttributes lists the attributes there are when none matches.
	 */
	Private::FReading ReadAttribute(UActorComponent& AbilitySystem, const FString& Name);

	/** Every attribute as Set.Name = value, for a report; up to Max. */
	TArray<FString> ListAttributes(UActorComponent& AbilitySystem, int32 Max = 24);

	/** Target's gameplay tags: its ability system's when it has one, and the actor's own when it implements IGameplayTagAssetInterface. */
	bool ReadTags(AActor& Target, FGameplayTagContainer& OutTags);

	struct FAbility
	{
		FString Name;
		int32 Level = 0;
		/** How many times it is running now. */
		int32 ActiveCount = 0;
	};
	TArray<FAbility> ReadAbilities(UActorComponent& AbilitySystem);

	struct FEffect
	{
		FString Name;
		int32 Stacks = 0;
		bool bInhibited = false;
	};
	TArray<FEffect> ReadEffects(UActorComponent& AbilitySystem);

	/** A class or asset name as an agent writes it matches one as reflection names it: GA_Dash matches GA_Dash_C and Default__GA_Dash_C, case aside. */
	bool NameMatches(const FString& Reflected, const FString& Wanted);
}
