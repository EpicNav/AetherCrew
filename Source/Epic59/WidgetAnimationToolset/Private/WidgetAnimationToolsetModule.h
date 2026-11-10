// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * WidgetAnimationToolset module - AI assistant tools for authoring UWidgetAnimation
 * (lifecycle + bindings) on UMG Widget Blueprints.
 */
class FWidgetAnimationToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void OnAllModuleLoadingPhasesComplete();
	void OnPreExit();
};
