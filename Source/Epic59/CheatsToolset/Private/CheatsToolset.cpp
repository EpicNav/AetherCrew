// Copyright Epic Games, Inc. All Rights Reserved.

#include "CheatsToolset.h"

#include "CheatsToolsetDefinition.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

/*virtual*/ void FCheatsToolsetModule::StartupModule()
{
	UToolsetRegistry::RegisterToolsetClass(UCheatsToolset::StaticClass());
}

/*virtual*/ void FCheatsToolsetModule::ShutdownModule()
{
	if (UObjectInitialized())
	{
		UToolsetRegistry::UnregisterToolsetClass(UCheatsToolset::StaticClass());
	}
}

IMPLEMENT_MODULE(FCheatsToolsetModule, CheatsToolset);
DEFINE_LOG_CATEGORY(LogCheatsToolset);
