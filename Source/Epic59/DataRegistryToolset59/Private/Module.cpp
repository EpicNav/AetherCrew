// Copyright Epic Games, Inc. All Rights Reserved.

#include "DataRegistryTools59.h"

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FDataRegistryToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UDataRegistryTools59::StaticClass());
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized())
		{
			UToolsetRegistry::UnregisterToolsetClass(UDataRegistryTools59::StaticClass());
		}
	}
};

IMPLEMENT_MODULE(FDataRegistryToolsetModule, DataRegistryToolset59);
