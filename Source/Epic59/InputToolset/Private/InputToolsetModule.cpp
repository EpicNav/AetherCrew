// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "InputToolset.h"

class FInputToolsetModule : public IModuleInterface
{
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UInputToolset::StaticClass());
		// UE 5.8: no cooked-runtime toolsets in 5.8. UToolsetRegistry::EnableCookedRuntimeSupport(TEXT("InputToolset"));
	}

	virtual void ShutdownModule() override
	{
		UToolsetRegistry::UnregisterToolsetClass(UInputToolset::StaticClass());
	}
};

IMPLEMENT_MODULE(FInputToolsetModule, InputToolset)
