// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions
#include "Logging/LogCategory.h"

#include "GameFeaturesToolset59.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGameFeaturesToolset, Log, All);

class UAsyncFunctionResultBool;

/// Simplified Game Feature Plugin state for AI tools.
/// Maps the engine's ~34 internal states to user-facing
/// states that an LLM would act on.
UENUM(BlueprintType)
enum class EPluginToolsetGFPState59 : uint8
{
	Uninitialized,
	Installed,
	Registered,
	Loaded,
	Active,
	Unknown
};

/// Mirror of the engine's EBuiltInAutoState (which is not BlueprintType-tagged),
/// suitable for use as a UFUNCTION parameter / return value.
UENUM(BlueprintType)
enum class EPluginToolsetGFPAutoState : uint8
{
	Invalid,
	Installed,
	Registered,
	Loaded,
	Active
};

/// AI-facing projection of FGameFeaturePluginDetails covering the fields the
/// metadata edit tools support (BuiltInAutoState and AdditionalMetadata).
/// PluginDependencies and bHotfixable are intentionally omitted.
USTRUCT(BlueprintType)
struct FGameFeaturePluginDetailsToolsetInfo
{
	GENERATED_BODY()

	/// Plugin's BuiltInInitialFeatureState from the .uplugin (mirror enum).
	UPROPERTY(BlueprintReadOnly, Category = "GameFeatures")
	EPluginToolsetGFPAutoState BuiltInAutoState = EPluginToolsetGFPAutoState::Invalid;

	/// Configured AdditionalPluginMetadataKeys mapped to their JSON-literal
	/// values (e.g. "true", "\"v1.0\"", "{\"Major\":1}"). Keys come from
	/// UGameFeaturesSubsystemSettings::AdditionalPluginMetadataKeys; values
	/// the .uplugin does not specify round-trip as the empty string, matching
	/// the engine parser's default.
	UPROPERTY(BlueprintReadOnly, Category = "GameFeatures")
	TMap<FString, FString> AdditionalMetadata;
};

/// Provides tools for inspecting Game Feature Plugins, driving them through their lifecycle of
/// load, activate, deactivate and unload, and editing their descriptor metadata.
UCLASS(BlueprintType, Hidden)
class UGameFeaturesToolset59 : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:

	/**
	 * Lists enabled Game Feature Plugins that the Game Features system has registered, sorted by
	 * name. The project's policy can keep an enabled plugin from registering;
	 * ListDiscoveredGameFeaturePlugins still includes those.
	 * Use the Plugins toolset to do general plugin enable/disable tasks.
	 * @return Sorted names of registered, enabled Game Feature Plugins.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameFeatures")
	static TArray<FString> ListEnabledGameFeaturePlugins();

	/**
	 * Lists all discovered Game Feature Plugins sorted by name. This includes disabled plugins and
	 * plugins the Game Features system has not registered, which the other tools cannot resolve.
	 * Use the Plugins toolset to do general plugin enable/disable tasks.
	 * @return Sorted names of all discovered Game Feature Plugins.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameFeatures")
	static TArray<FString> ListDiscoveredGameFeaturePlugins();

	/**
	 * Return whether or not a plugin is a Game Feature Plugin. Will error if no plugin of this
	 * name can be found by the Plugin Manager.
	 * @param PluginName Name of the plugin
	 * @return True if the plugin is a Game Feature Plugin
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameFeatures")
	static bool IsGameFeaturePlugin(const FString& PluginName);

	/**
	 * Checks whether a Game Feature Plugin is active. Raises an error if the subsystem is unavailable
	 * or the plugin is not found.
	 * Use GetGameFeatureState if you need the current state when the plugin is not active.
	 * @param PluginName Name of the Game Feature Plugin.
	 * @return True if the Game Feature Plugin is active.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameFeatures")
	static bool IsGameFeatureActive(const FString& PluginName);

	/**
	 * Gets the current state of a Game Feature Plugin.
	 * @param PluginName Name of the Game Feature Plugin.
	 * @return Simplified state enum. Raises an error if the subsystem is unavailable or the
	 *         plugin is not found.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameFeatures")
	static EPluginToolsetGFPState59 GetGameFeatureState(const FString& PluginName);

	/**
	 * Requests activation of a Game Feature Plugin, loading it first if it is not already loaded.
	 * @param PluginName Name of the GFP.
	 * @return True once the plugin is active, or an error describing why it could not be.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameFeatures")
	static UAsyncFunctionResultBool* RequestActivateGameFeature(const FString& PluginName);

	/**
	 * Requests load of a Game Feature Plugin without activating it. Use RequestActivateGameFeature
	 * instead to make the plugin's features live.
	 * @param PluginName Name of the GFP.
	 * @return True once the plugin is loaded, or an error describing why it could not be.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameFeatures")
	static UAsyncFunctionResultBool* RequestLoadGameFeature(const FString& PluginName);

	/**
	 * Requests deactivation of a Game Feature Plugin, which leaves it loaded.
	 * @param PluginName Name of the GFP.
	 * @return True once the plugin is deactivated, or an error describing why it could not be.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameFeatures")
	static UAsyncFunctionResultBool* RequestDeactivateGameFeature(const FString& PluginName);

	/**
	 * Requests unload of a Game Feature Plugin, taking it out of the Loaded state.
	 * @param PluginName Name of the GFP.
	 * @param bKeepRegistered Leave the plugin registered so its assets stay discoverable and it
	 *        can be reloaded without rescanning. Pass false to also unregister it, which is what
	 *        makes the editor stop reporting a plugin whose files are gone.
	 * @return True once the plugin is unloaded, or an error describing why it could not be.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameFeatures")
	static UAsyncFunctionResultBool* RequestUnloadGameFeature(const FString& PluginName, bool bKeepRegistered = false);

	/**
	 * Reads the editable metadata on a Game Feature Plugin's .uplugin descriptor.
	 * Returns the BuiltInInitialFeatureState and the configured additional metadata keys
	 * (UGameFeaturesSubsystemSettings::AdditionalPluginMetadataKeys) with their values
	 * rendered as JSON literals.
	 * @param PluginName Name of the GFP.
	 * @return Current details (BuiltInAutoState + AdditionalMetadata).
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameFeatures")
	static FGameFeaturePluginDetailsToolsetInfo GetGameFeaturePluginDetails(const FString& PluginName);

	/**
	 * Lists the additional metadata keys the GameFeatures subsystem is configured to read
	 * from each .uplugin (UGameFeaturesSubsystemSettings::AdditionalPluginMetadataKeys),
	 * sorted. These are the only keys SetGameFeaturePluginMetadataValue and
	 * RemoveGameFeaturePluginMetadataValue will accept.
	 * @return Sorted list of configured additional metadata keys.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "GameFeatures")
	static TArray<FString> GetConfiguredAdditionalMetadataKeys();

	/**
	 * Sets the BuiltInInitialFeatureState on the GFP's .uplugin descriptor and writes the
	 * descriptor back to disk. EPluginToolsetGFPAutoState::Invalid is not accepted.
	 * @param PluginName Name of the GFP.
	 * @param NewState New BuiltInInitialFeatureState (must not be Invalid).
	 * @return Re-parsed plugin details reflecting the new state.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameFeatures")
	static FGameFeaturePluginDetailsToolsetInfo SetGameFeaturePluginBuiltInAutoState(const FString& PluginName, EPluginToolsetGFPAutoState NewState);

	/**
	 * Writes a value for one of the configured additional metadata keys to the GFP's
	 * .uplugin. JsonValueLiteral must be a valid JSON literal (e.g. true, 42, "hello",
	 * {"Major":1}); booleans and numbers are stored as native JSON types rather than
	 * strings.
	 * @param PluginName Name of the GFP.
	 * @param Key Metadata key (must appear in UGameFeaturesSubsystemSettings::AdditionalPluginMetadataKeys).
	 * @param JsonValueLiteral JSON literal text for the new value.
	 * @return Re-parsed plugin details reflecting the write.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameFeatures")
	static FGameFeaturePluginDetailsToolsetInfo SetGameFeaturePluginMetadataValue(const FString& PluginName, const FString& Key, const FString& JsonValueLiteral);

	/**
	 * Removes one of the configured additional metadata keys from the GFP's .uplugin.
	 * After removal the key round-trips through AdditionalMetadata as an empty string --
	 * that is the engine parser's default for missing keys, surfaced honestly.
	 * @param PluginName Name of the GFP.
	 * @param Key Metadata key (must appear in UGameFeaturesSubsystemSettings::AdditionalPluginMetadataKeys).
	 * @return Re-parsed plugin details reflecting the removal.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "GameFeatures")
	static FGameFeaturePluginDetailsToolsetInfo RemoveGameFeaturePluginMetadataValue(const FString& PluginName, const FString& Key);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface
};
