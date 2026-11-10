// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameFeaturesToolset59.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FGameFeaturesToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UGameFeaturesToolset59::StaticClass());
	}

	virtual void ShutdownModule() override
	{
		UToolsetRegistry::UnregisterToolsetClass(UGameFeaturesToolset59::StaticClass());
	}
};

IMPLEMENT_MODULE(FGameFeaturesToolsetModule, GameFeaturesToolset59);
