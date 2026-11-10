// Copyright Epic Games, Inc. All Rights Reserved.

#include "TextureGraphToolset.h"
#include "TextureGraphTools.h"

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

void FTextureGraphToolsetModule::StartupModule()
{
	UToolsetRegistry::RegisterToolsetClass(UTextureGraphTools::StaticClass());
}

void FTextureGraphToolsetModule::ShutdownModule()
{
	UToolsetRegistry::UnregisterToolsetClass(UTextureGraphTools::StaticClass());
}

IMPLEMENT_MODULE(FTextureGraphToolsetModule, TextureGraphToolset);
DEFINE_LOG_CATEGORY(LogTextureGraphToolset);
