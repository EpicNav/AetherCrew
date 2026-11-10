// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameplayTagsToolset59.h"

#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

class FGameplayTagsToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UGameplayTagsToolset59::StaticClass());
	}

	virtual void ShutdownModule() override
	{
		UToolsetRegistry::UnregisterToolsetClass(UGameplayTagsToolset59::StaticClass());
	}
};

IMPLEMENT_MODULE(FGameplayTagsToolsetModule, GameplayTagsToolset59);
