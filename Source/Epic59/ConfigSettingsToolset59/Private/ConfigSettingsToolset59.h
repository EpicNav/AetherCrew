// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ISettingsSection.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "ConfigSettingsToolset59.generated.h"

/// A settings section, or one property in it, that matched a search.
USTRUCT(BlueprintType)
struct FConfigSettingsSearchResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ConfigSettingsToolset")
	FString ContainerName;

	UPROPERTY(BlueprintReadOnly, Category = "ConfigSettingsToolset")
	FString CategoryName;

	UPROPERTY(BlueprintReadOnly, Category = "ConfigSettingsToolset")
	FString SectionName;

	/** The matching property, as used by GetSectionPropertyValues. Empty when the section itself matched. */
	UPROPERTY(BlueprintReadOnly, Category = "ConfigSettingsToolset")
	FString PropertyName;

	/** The label the Settings editor shows for the property or section. */
	UPROPERTY(BlueprintReadOnly, Category = "ConfigSettingsToolset")
	FString DisplayName;

	/**
	 * The console variable the property is bound to, or empty. Setting the property persists the value across
	 * restarts, which setting the console variable directly does not.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ConfigSettingsToolset")
	FString ConsoleVariableName;
};

/// Tools for listing, inspecting, and editing Config Settings sections
UCLASS(BlueprintType, MinimalAPI)
class UConfigSettingsToolset59 : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()
public:

	// --- Discovery ---

	/**
	 * Lists the names of all known settings containers, sorted alphabetically.
	 * Common containers are "Editor" and "Project".
	 * @return Sorted array of container names.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "ConfigSettingsToolset")
	static TArray<FString> ListContainers();

	/**
	 * Lists the names of all categories within a settings container, sorted alphabetically.
	 * @param ContainerName The name of the container (e.g. "Project").
	 * @return Sorted array of category names.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "ConfigSettingsToolset")
	static TArray<FString> ListCategories(const FString& ContainerName);

	/**
	 * Lists the names of all sections within a settings category, sorted alphabetically.
	 * @param ContainerName The name of the container (e.g. "Project").
	 * @param CategoryName The name of the category (e.g. "Engine").
	 * @return Sorted array of section names.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "ConfigSettingsToolset")
	static TArray<FString> ListSections(const FString& ContainerName, const FString& CategoryName);

	/**
	 * Finds settings by the label shown in the Settings editor, the property name, the tooltip, or the console
	 * variable the property is bound to, across every container. Every word in the query must match, ignoring
	 * case. A section whose own name matches is returned with an empty PropertyName.
	 * @param Query Space-separated words to search for, e.g. "window title" or "gc.TimeBetweenPurgingPendingKillObjects".
	 * @return Matches sorted by container, category, section, and property.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "ConfigSettingsToolset")
	static TArray<FConfigSettingsSearchResult> SearchSettings(const FString& Query);

	// --- Schema & Values ---

	/**
	 * Returns a JSON Schema describing the user-visible properties of a settings section.
	 * The schema maps each property name to its type, description, and constraints.
	 * A section that uses a custom widget has no settings object and so no schema.
	 * @param ContainerName The name of the container (e.g. "Project").
	 * @param CategoryName The name of the category (e.g. "Engine").
	 * @param SectionName The name of the section (e.g. "General").
	 * @return JSON Schema string describing the section's properties, or empty string on failure.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "ConfigSettingsToolset")
	static FString GetSectionSchema(
		const FString& ContainerName,
		const FString& CategoryName,
		const FString& SectionName);

	/**
	 * Returns the current values of the specified properties as a JSON object.
	 * @param ContainerName The name of the container (e.g. "Project").
	 * @param CategoryName The name of the category (e.g. "Engine").
	 * @param SectionName The name of the section (e.g. "General").
	 * @param PropertyNames The names of the properties to read.
	 * @return JSON object mapping property names to their current values, or empty string on failure.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "ConfigSettingsToolset")
	static FString GetSectionPropertyValues(
		const FString& ContainerName,
		const FString& CategoryName,
		const FString& SectionName,
		const TArray<FString>& PropertyNames);

	// --- Editing ---

	/**
	 * Sets one or more properties on a settings section from a JSON object and saves.
	 * PropertiesJson must be a JSON object mapping property names to new values,
	 * in the same format returned by GetSectionPropertyValues.
	 * If any value is rejected, none are applied and nothing is saved.
	 * The default config file is checked out first when source control is enabled.
	 * @param ContainerName The name of the container (e.g. "Project").
	 * @param CategoryName The name of the category (e.g. "Engine").
	 * @param SectionName The name of the section (e.g. "General").
	 * @param PropertiesJson JSON object with property name to new value pairs.
	 * @return True if all properties successfully set.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "ConfigSettingsToolset")
	static bool SetSectionProperties(
		const FString& ContainerName,
		const FString& CategoryName,
		const FString& SectionName,
		const FString& PropertiesJson);

	/**
	 * Saves the settings in a section. The default config file is checked out first when source control is enabled.
	 * @param ContainerName The name of the container (e.g. "Project").
	 * @param CategoryName The name of the category (e.g. "Engine").
	 * @param SectionName The name of the section (e.g. "General").
	 * @return True on success.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "ConfigSettingsToolset")
	static bool SaveSection(
		const FString& ContainerName,
		const FString& CategoryName,
		const FString& SectionName);

	/**
	 * Resets the settings in a section to their default values. Project sections saved to a Default*.ini
	 * cannot be reset, matching the Settings editor.
	 * @param ContainerName The name of the container (e.g. "Project").
	 * @param CategoryName The name of the category (e.g. "Engine").
	 * @param SectionName The name of the section (e.g. "General").
	 * @return True on success.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "ConfigSettingsToolset")
	static bool ResetSectionToDefaults(
		const FString& ContainerName,
		const FString& CategoryName,
		const FString& SectionName);

	// --- Supported Platforms ---

	/**
	 * Lists the platforms a project can declare support for, the choices in the Project Settings Supported
	 * Platforms section.
	 * @return Platform names, e.g. "Windows", "Android".
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "ConfigSettingsToolset")
	static TArray<FString> ListAvailablePlatforms();

	/**
	 * Returns the platforms the project declares support for in its .uproject file.
	 * @return Platform names. Empty means every platform is supported ("All Platforms").
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "ConfigSettingsToolset")
	static TArray<FString> GetSupportedPlatforms();

	/**
	 * Replaces the platforms the project declares support for and saves the .uproject file, checking it out
	 * first when source control is enabled. Packaging, running or cooking for a platform not in the list warns.
	 * @param Platforms Names from ListAvailablePlatforms, or ones the project already lists whose platform data
	 *     isn't installed on this machine. Empty supports every platform ("All Platforms").
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "ConfigSettingsToolset")
	static void SetSupportedPlatforms(const TArray<FString>& Platforms);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface

private:
	friend class FConfigSettingsToolsetSpec59; // UE 5.8: spec renamed ...59

	static TSharedPtr<ISettingsSection> FindSection(
		const FString& ContainerName,
		const FString& CategoryName,
		const FString& SectionName);
};
