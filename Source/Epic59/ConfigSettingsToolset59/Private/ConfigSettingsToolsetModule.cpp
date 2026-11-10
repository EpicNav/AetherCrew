// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "ConfigSettingsToolset59.h"

#define LOCTEXT_NAMESPACE "FConfigSettingsToolsetModule"

class FConfigSettingsToolsetModule : public IModuleInterface
{
	void StartupModule()
	{
		UToolsetRegistry::RegisterToolsetClass(UConfigSettingsToolset59::StaticClass());
	}

	void ShutdownModule()
	{
		UToolsetRegistry::UnregisterToolsetClass(UConfigSettingsToolset59::StaticClass());
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FConfigSettingsToolsetModule, ConfigSettingsToolset59)
