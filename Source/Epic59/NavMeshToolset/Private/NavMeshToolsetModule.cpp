// Copyright Epic Games, Inc. All Rights Reserved.

#include "NavMeshToolsetModule.h"

#include "Modules/ModuleManager.h"
#include "NavMeshToolset.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

DEFINE_LOG_CATEGORY(LogNavMeshToolset);

void FNavMeshToolsetModule::StartupModule()
{
	// Never register the toolset in Shipping, whatever a target enables. Recast-less
	// targets compile stubs only.
#if WITH_RECAST && !UE_BUILD_SHIPPING
	UToolsetRegistry::RegisterToolsetClass(UNavMeshToolset::StaticClass());

	// Also register the AIRuntimeCallable tools outside the editor, where the live navmesh is.
	// EnableCookedRuntimeSupport routes through a callback that ToolsetRegistryEditor installs in
	// its own StartupModule; this module loads at PostEngineInit (see .uplugin) so that callback
	// is guaranteed installed, in editors and commandlets alike, before this runs.
	// UE 5.8: no cooked-runtime toolsets in 5.8. UToolsetRegistry::EnableCookedRuntimeSupport(TEXT("NavMeshToolset"));
#elif !WITH_RECAST
	UE_LOGF(LogNavMeshToolset, Display,
		"NavMeshToolset tools are unavailable: this target was built without Recast (bCompileRecast=false).");
#endif
}

void FNavMeshToolsetModule::ShutdownModule()
{
#if WITH_RECAST && !UE_BUILD_SHIPPING
	UToolsetRegistry::UnregisterToolsetClass(UNavMeshToolset::StaticClass());
#endif
}

// Defined here rather than in a NavMeshToolset_*.cpp because those only compile with Recast, while
// the class (and this virtual) exists on every target.
bool UNavMeshToolset::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}

IMPLEMENT_MODULE(FNavMeshToolsetModule, NavMeshToolset);
