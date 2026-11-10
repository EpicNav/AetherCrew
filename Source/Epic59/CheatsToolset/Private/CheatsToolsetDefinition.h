// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "CheatsToolsetDefinition.generated.h"

class UCheatManager;

/// Describes a single cheat command available on the local player's cheat manager.
USTRUCT(BlueprintType)
struct FCheatInfo
{
	GENERATED_BODY()

	/// The cheat command name, e.g. "God".
	UPROPERTY(BlueprintReadWrite, Category = "Cheats")
	FString CheatName;

	/// The cheat's parameters as a console-style hint, each shown as "<type name>" (e.g. "<float F>");
	/// empty when the cheat takes no arguments.
	UPROPERTY(BlueprintReadWrite, Category = "Cheats")
	FString Args;

	/// The cheat's tooltip/help text, when authored (empty otherwise).
	UPROPERTY(BlueprintReadWrite, Category = "Cheats")
	FString Description;

	/// The class that declares the cheat (a cheat manager or one of its cheat extensions).
	UPROPERTY(BlueprintReadWrite, Category = "Cheats")
	TObjectPtr<UClass> SourceClass;
};

/**
 * Provides tools for listing and running cheat commands on the local player, the same commands you
 * would type into the console.
 */
UCLASS(BlueprintType, Hidden)
class UCheatsToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Lists the cheat commands available on the local player.
	 * @param NameFilter Case-insensitive substring matched against cheat names. Empty lists everything.
	 * @return The matching cheats, or an empty array when none match the filter.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "Cheats")
	static TArray<FCheatInfo> ListCheats(const FString& NameFilter = TEXT(""));

	/**
	 * Runs a cheat on the local player, exactly as if it were typed into the console.
	 * @param CheatName The cheat command name (case-insensitive), as returned by ListCheats.
	 * @param Args      The arguments as a single space-separated string matching the cheat's Args hint
	 *                  from ListCheats. Empty for a cheat that takes no arguments.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "Cheats")
	static void CallCheat(const FString& CheatName, const FString& Args);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface

private:
	/**
	 * The core of ListCheats: enumerates the manager's exec cheats plus those of its registered
	 * extensions, filters by case-insensitive name substring, and returns a de-duplicated, name-sorted
	 * result. Authority-only cheats are omitted when bLocalHasAuthority is false.
	 */
	static TArray<FCheatInfo> CollectCheats(
		UCheatManager* CheatManager, const FString& NameFilter, bool bLocalHasAuthority);

	/**
	 * Resolves a cheat name to its exec UFunction and owning object (the manager, else a registered
	 * extension instance), manager first. Returns nullptr if no active match; if the name belongs to an
	 * extension class not registered here, OutInactiveExtensionClass names it.
	 */
	static UFunction* ResolveCheat(
		UCheatManager* CheatManager, FName CheatName, UObject*& OutOwner, FString& OutInactiveExtensionClass);

	/**
	 * ResolveCheat wrapper used by CallCheat that raises a descriptive script error (returning nullptr)
	 * when the cheat is unknown or its declaring extension is inactive.
	 */
	static UFunction* ResolveCheatOrRaise(UCheatManager* CheatManager, const FString& CheatName, UObject*& OutOwner);

	friend class FCheatsToolsetSpec;
};
