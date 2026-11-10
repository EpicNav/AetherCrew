// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "ConsoleToolset.h"
#include "LogsToolset59.h"

#define LOCTEXT_NAMESPACE "FConsoleToolsetModule"

class FConsoleToolsetModule : public IModuleInterface
{
	void StartupModule()
	{
		UToolsetRegistry::RegisterToolsetClass(UConsoleToolset::StaticClass());
		UToolsetRegistry::RegisterToolsetClass(ULogsToolset59::StaticClass());
		// UE 5.8: no cooked-runtime toolsets in 5.8. UToolsetRegistry::EnableCookedRuntimeSupport(TEXT("ConsoleToolset"));
	}

	void ShutdownModule()
	{
		UToolsetRegistry::UnregisterToolsetClass(ULogsToolset59::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UConsoleToolset::StaticClass());
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FConsoleToolsetModule, ConsoleToolset)
