// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldConditionsToolset.h"

#include "Editor.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/ToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "WorldConditionQueryConverter.h"
#include "WorldConditionTools59.h"

/*virtual*/ void FWorldConditionsToolsetModule::StartupModule()
{
	WorldConditionQueryConverter = MakeShared<UE::WorldConditionsToolset::FWorldConditionQueryConverter>();
	Register();
}

/*virtual*/ void FWorldConditionsToolsetModule::ShutdownModule()
{
	if (UObjectInitialized())
	{
		UToolsetRegistry::UnregisterToolsetClass(UWorldConditionTools59::StaticClass());
	}

	if (WorldConditionQueryConverter.IsValid())
	{
		if (UE::ToolsetRegistry::FToolsetRegistry* Registry = UE::ToolsetRegistry::FToolsetRegistry::Get())
		{
			Registry->UnregisterConverter(WorldConditionQueryConverter);
		}
	}

	WorldConditionQueryConverter.Reset();
}

void FWorldConditionsToolsetModule::Register()
{
	if (GEditor == nullptr || !WorldConditionQueryConverter.IsValid())
	{
		return;
	}

	UToolsetRegistry::RegisterToolsetClass(UWorldConditionTools59::StaticClass());

	UToolsetRegistrySubsystem* const ToolsetRegistrySubsystem =
		GEditor->GetEditorSubsystem<UToolsetRegistrySubsystem>();
	if (ToolsetRegistrySubsystem != nullptr)
	{
		ToolsetRegistrySubsystem->ToolsetRegistry.RegisterConverter(WorldConditionQueryConverter);
	}
}

IMPLEMENT_MODULE(FWorldConditionsToolsetModule, WorldConditionsToolset59);
DEFINE_LOG_CATEGORY(LogWorldConditionsToolset);
