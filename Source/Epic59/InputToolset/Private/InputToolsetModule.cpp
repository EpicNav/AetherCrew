// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "InputToolset.h"
#include "InputToolset58.h" // UE 5.8: static front registered in place of the instance-dispatched subsystem

class FInputToolsetModule : public IModuleInterface
{
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UInputToolset58::StaticClass()); // UE 5.8: 5.8 registers UToolsetDefinition classes only and dispatches static tools
		// UE 5.8: no cooked-runtime toolsets in 5.8. UToolsetRegistry::EnableCookedRuntimeSupport(TEXT("InputToolset"));
	}

	virtual void ShutdownModule() override
	{
		UToolsetRegistry::UnregisterToolsetClass(UInputToolset58::StaticClass()); // UE 5.8
	}
};

IMPLEMENT_MODULE(FInputToolsetModule, InputToolset)
