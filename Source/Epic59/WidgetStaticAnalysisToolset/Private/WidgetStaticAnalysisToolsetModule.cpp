// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetStaticAnalysisToolsetModule.h"
#include "Misc/CoreDelegates.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "WidgetStaticAnalysisToolset.h"

#define LOCTEXT_NAMESPACE "WidgetStaticAnalysisToolset"

void FWidgetStaticAnalysisToolsetModule::StartupModule()
{
	FCoreDelegates::OnAllModuleLoadingPhasesComplete.AddRaw(
		this, &FWidgetStaticAnalysisToolsetModule::OnAllModuleLoadingPhasesComplete);
	FCoreDelegates::OnPreExit.AddRaw(
		this, &FWidgetStaticAnalysisToolsetModule::OnPreExit);
}

void FWidgetStaticAnalysisToolsetModule::ShutdownModule()
{
	FCoreDelegates::OnAllModuleLoadingPhasesComplete.RemoveAll(this);
	FCoreDelegates::OnPreExit.RemoveAll(this);

	UToolsetRegistry::UnregisterToolsetClass(UWidgetStaticAnalysisToolset::StaticClass());
}

void FWidgetStaticAnalysisToolsetModule::OnAllModuleLoadingPhasesComplete()
{
	UToolsetRegistry::RegisterToolsetClass(UWidgetStaticAnalysisToolset::StaticClass());
}

void FWidgetStaticAnalysisToolsetModule::OnPreExit()
{
	UToolsetRegistry::UnregisterToolsetClass(UWidgetStaticAnalysisToolset::StaticClass());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FWidgetStaticAnalysisToolsetModule, WidgetStaticAnalysisToolset)
