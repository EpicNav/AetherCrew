// Copyright Epic Games, Inc. All Rights Reserved.

#include "ToolsetCore/ToolsetCoreLog.h"
#include "ToolsetCore/ToolsetRegistrar.h"

#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, ToolsetCore);
DEFINE_LOG_CATEGORY(LogToolsetCore);

namespace UE::Toolset
{
	// Lives here rather than in the plugin implementation: a TOOLSETCORE_API static data member
	// can only be defined where the macro expands to dllexport, which is inside this module.
	const FName IToolsetRegistrar::ModularFeatureName(TEXT("ToolsetRegistrar"));
}
