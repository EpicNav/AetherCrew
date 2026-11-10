// Copyright Epic Games, Inc. All Rights Reserved.

#include "AsyncFunctionResult/AsyncFunctionResultCaptureStep.h"

#include "CoreGlobals.h"
#include "Features/IModularFeatures.h"
#include "Misc/AssertionMacros.h"
#include "Modules/ModuleManager.h"

#include "AsyncFunctionResultLog.h"

FName UE::AsyncFunctionResult::ICaptureStepProvider::GetModularFeatureName()
{
	static const FName FeatureName(TEXT("AsyncFunctionResultCaptureStepProvider"));
	return FeatureName;
}

TUniquePtr<UE::AsyncFunctionResult::ICaptureStep> UE::AsyncFunctionResult::BeginCaptureStep(UTransactor* Buffer)
{
	if (!Buffer)
	{
		return nullptr;
	}
	check(IsInGameThread());

	IModularFeatures& ModularFeatures = IModularFeatures::Get();
	const FName FeatureName = ICaptureStepProvider::GetModularFeatureName();
	if (!ModularFeatures.IsModularFeatureAvailable(FeatureName))
	{
		FModuleManager::Get().LoadModule(TEXT("AsyncFunctionResultEditor"));
	}
	if (!ModularFeatures.IsModularFeatureAvailable(FeatureName))
	{
		// UE59 compat (mcp-6): fail softly in every configuration. An agent's tool call reaches this in an
		// editor without the provider module (MCP and toolsets without AIAssistant); UE6 asserts here.
		UE_LOGF(
			LogAsyncFunctionResult, Error,
			"A capture step was requested but the AsyncFunctionResultEditor module could not be "
			"loaded; the step runs uncaptured");
		return nullptr;
	}
	return ModularFeatures.GetModularFeature<ICaptureStepProvider>(FeatureName).BeginCaptureStep(Buffer);
}
