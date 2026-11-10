// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetAnimationToolsetModule.h"
#include "Misc/CoreDelegates.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "WidgetAnimationToolset.h"

#define LOCTEXT_NAMESPACE "WidgetAnimationToolset"

void FWidgetAnimationToolsetModule::StartupModule()
{
	FCoreDelegates::OnAllModuleLoadingPhasesComplete.AddRaw(
		this, &FWidgetAnimationToolsetModule::OnAllModuleLoadingPhasesComplete);
	FCoreDelegates::OnPreExit.AddRaw(
		this, &FWidgetAnimationToolsetModule::OnPreExit);
}

void FWidgetAnimationToolsetModule::ShutdownModule()
{
	FCoreDelegates::OnAllModuleLoadingPhasesComplete.RemoveAll(this);
	FCoreDelegates::OnPreExit.RemoveAll(this);
}

void FWidgetAnimationToolsetModule::OnAllModuleLoadingPhasesComplete()
{
	UToolsetRegistry::RegisterToolsetClass(UWidgetAnimationToolset::StaticClass());
}

void FWidgetAnimationToolsetModule::OnPreExit()
{
	UToolsetRegistry::UnregisterToolsetClass(UWidgetAnimationToolset::StaticClass());
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FWidgetAnimationToolsetModule, WidgetAnimationToolset)
