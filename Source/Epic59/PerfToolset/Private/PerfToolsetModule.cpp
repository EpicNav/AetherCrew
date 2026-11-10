// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#include "CsvProfilerToolset.h"
#include "GpuProfilerToolset.h"
#include "StatsToolset.h"

#define LOCTEXT_NAMESPACE "FPerfToolsetModule"

class FPerfToolsetModule : public IModuleInterface
{
	void StartupModule()
	{
		UToolsetRegistry::RegisterToolsetClass(UCsvProfilerToolset::StaticClass());
#if STATS
		UToolsetRegistry::RegisterToolsetClass(UStatsToolset::StaticClass());
#endif
#if WITH_PROFILEGPU
		UToolsetRegistry::RegisterToolsetClass(UGpuProfilerToolset::StaticClass());
#endif
		// UE 5.8: no cooked-runtime toolsets in 5.8. UToolsetRegistry::EnableCookedRuntimeSupport(TEXT("PerfToolset"));
	}

	void ShutdownModule()
	{
#if WITH_PROFILEGPU
		UToolsetRegistry::UnregisterToolsetClass(UGpuProfilerToolset::StaticClass());
#endif
#if STATS
		UToolsetRegistry::UnregisterToolsetClass(UStatsToolset::StaticClass());
#endif
		UToolsetRegistry::UnregisterToolsetClass(UCsvProfilerToolset::StaticClass());
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPerfToolsetModule, PerfToolset)
