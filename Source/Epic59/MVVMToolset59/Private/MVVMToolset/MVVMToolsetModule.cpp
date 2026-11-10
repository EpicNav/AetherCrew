// Copyright Epic Games, Inc. All Rights Reserved.

#include "Editor.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "MVVMToolset/MVVMToolset59.h"

class FMVVMToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UToolsetRegistry::RegisterToolsetClass(UMVVMToolset59::StaticClass());
	}

	virtual void ShutdownModule() override
	{
		UToolsetRegistry::UnregisterToolsetClass(UMVVMToolset59::StaticClass());
	}
};

IMPLEMENT_MODULE(FMVVMToolsetModule, MVVMToolset59);
