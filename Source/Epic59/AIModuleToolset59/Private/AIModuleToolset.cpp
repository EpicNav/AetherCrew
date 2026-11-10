// Copyright Epic Games, Inc. All Rights Reserved.

#include "AIModuleToolset.h"

#include "BehaviorTreeToolset.h"
#include "EnvironmentQueryToolset.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

/*virtual*/ void FAIModuleToolsetModule::StartupModule()
{
	UToolsetRegistry::RegisterToolsetClass(UBehaviorTreeToolset::StaticClass());
	UToolsetRegistry::RegisterToolsetClass(UEnvironmentQueryToolset::StaticClass());
}

/*virtual*/ void FAIModuleToolsetModule::ShutdownModule()
{
	UToolsetRegistry::UnregisterToolsetClass(UBehaviorTreeToolset::StaticClass());
	UToolsetRegistry::UnregisterToolsetClass(UEnvironmentQueryToolset::StaticClass());
}

IMPLEMENT_MODULE(FAIModuleToolsetModule, AIModuleToolset59);
DEFINE_LOG_CATEGORY(LogAIModuleToolset);
