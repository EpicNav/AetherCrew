// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: copied from UE 5.9's GameFeatures plugin (Public/Tests/GameFeaturePluginTestHelper.h), which 5.8 lacks;
// private to this test module.

#pragma once

#include "GameFeaturesSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace UE::GameFeatures::Testing
{
	enum class EShouldActivate : uint8
	{
		No,
		Yes
	};

	struct FGameFeatureDependsProperties
	{
		FString PluginName;
		EShouldActivate ShouldActivate = EShouldActivate::No;
	};

	struct FGameFeatureProperties
	{
		FString PluginName;
		EGameFeatureTargetState	BuiltinAutoState = EGameFeatureTargetState::Installed;
		TArray<FGameFeatureDependsProperties> Depends;
	};

	/**
	 * Writes a synthetic, content-free Game Feature Plugin to ProjectPluginsDir()/GameFeatures for
	 * a test to drive through its lifecycle, and returns its URL.
	 *
	 * The plugin is written to disk only. To reach it by name, register it with
	 * IPluginManager::AddToPluginsList followed by RefreshPluginsList.
	 *
	 * DESTRUCTIVE: an existing directory of the same plugin name is deleted first. Callers are
	 * responsible for removing the directory afterwards, and should do so from an ON_SCOPE_EXIT
	 * registered before any state is mutated so that early-return failures still clean up.
	 */
	bool CreateGameFeaturePlugin(FGameFeatureProperties Properties, FString& OutPluginURL);
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
