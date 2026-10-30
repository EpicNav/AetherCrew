// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "HyperAIStudioTripoSettings.generated.h"

/**
 * The Tripo model creator's settings, under Editor Preferences > Plugins > HyperAIStudio Tripo. The Tripo API key itself is
 * kept in Settings > Agents > Keys (Windows Credential Manager) or the TRIPO_API_KEY environment variable, never here; prices
 * are in HyperAIStudio's MeshTaskPrices, so a price change is corrected in one place.
 */
UCLASS(Config = EditorPerProjectUserSettings, meta = (DisplayName = "HyperAIStudio Tripo"))
class UHyperAIStudioTripoSettings final : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** What a creator job's paid fixes may spend in all after the generation, in USD (Tripo API credits at $0.01). */
	UPROPERTY(Config, EditAnywhere, Category = "Model Creator", meta = (ClampMin = "0.0", ClampMax = "100.0"))
	float FixBudgetUSD = 1.5f;

	/** Fix rounds a job runs at most: each checks the model, runs the free fixes, then offers at most one paid fix. */
	UPROPERTY(Config, EditAnywhere, Category = "Model Creator", meta = (ClampMin = "1", ClampMax = "10"))
	int32 MaxFixRounds = 3;

	/**
	 * Watch for models Tripo's own DCC Bridge sends from Tripo Studio ("Send to Unreal"), and offer to check and fix each
	 * one with the free fixes in the editor. HyperAI never talks to Tripo Studio or the bridge itself.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Tripo Studio")
	bool bWatchTripoBridgeImports = true;

	/**
	 * Content folders a static or skeletal mesh arriving in counts as sent by the bridge. Tripo does not document the
	 * folder its bridge imports into; set it here once you have seen where yours lands.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Tripo Studio")
	TArray<FString> TripoBridgeFolders = {TEXT("/Game/Tripo"), TEXT("/Game/TripoBridge"), TEXT("/Game/Tripo3D"), TEXT("/Game/TripoModels")};

	/** Words in an imported mesh's name, or in the file it came from, that mark it as Tripo's (matched without case). */
	UPROPERTY(Config, EditAnywhere, Category = "Tripo Studio")
	TArray<FString> TripoNameMarkers = {TEXT("tripo")};

	/** Show a notification with Check and fix and Dismiss when one arrives (the offer also waits in Activity). */
	UPROPERTY(Config, EditAnywhere, Category = "Tripo Studio")
	bool bNotifyTripoBridgeImports = true;
};
