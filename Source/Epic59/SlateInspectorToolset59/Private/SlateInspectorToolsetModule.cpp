// Copyright Epic Games, Inc. All Rights Reserved.

#include "SlateInspectorToolsetModule.h"

#include "Modules/ModuleManager.h"
#include "SlateInspectorToolsetHyperAIGuard.h" // UE 5.8: HyperAI Studio guard

IMPLEMENT_MODULE(FSlateInspectorToolsetModule, SlateInspectorToolset59)

void FSlateInspectorToolsetModule::StartupModule()
{
}

void FSlateInspectorToolsetModule::ShutdownModule()
{
	UE::SlateInspectorToolset::HyperAIGuard::Shutdown(); // UE 5.8: remove the guard's input pre-processor
}
