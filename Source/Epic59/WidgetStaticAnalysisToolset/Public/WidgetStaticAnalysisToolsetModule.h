// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

/** Provides AI assistant static-analysis toolsets for UMG widgets. */
class FWidgetStaticAnalysisToolsetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void OnAllModuleLoadingPhasesComplete();

	void OnPreExit();
};
