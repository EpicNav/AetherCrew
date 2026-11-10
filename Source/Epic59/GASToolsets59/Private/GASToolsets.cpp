// Copyright Epic Games, Inc. All Rights Reserved.

#include "GASToolsets.h"

#include "AbilitySystemInspectorToolset59.h"
#include "AttributeSetToolset59.h"
#include "Editor.h"
#include "GameplayAbilityToolset.h"
#include "GameplayCueToolset59.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

/*virtual*/ void FGASToolsetsModule::StartupModule()
{
	UToolsetRegistry::RegisterToolsetClass(UGameplayAbilityToolset::StaticClass());
	UToolsetRegistry::RegisterToolsetClass(UGameplayCueToolset59::StaticClass());
	UToolsetRegistry::RegisterToolsetClass(UAttributeSetToolset59::StaticClass());
	UToolsetRegistry::RegisterToolsetClass(UAbilitySystemInspectorToolset59::StaticClass());
}

/*virtual*/ void FGASToolsetsModule::ShutdownModule()
{
	if (UObjectInitialized())
	{
		UToolsetRegistry::UnregisterToolsetClass(UAbilitySystemInspectorToolset59::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UAttributeSetToolset59::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UGameplayCueToolset59::StaticClass());
		UToolsetRegistry::UnregisterToolsetClass(UGameplayAbilityToolset::StaticClass());
	}
}

IMPLEMENT_MODULE(FGASToolsetsModule, GASToolsets59);
DEFINE_LOG_CATEGORY(LogGASToolsets);
