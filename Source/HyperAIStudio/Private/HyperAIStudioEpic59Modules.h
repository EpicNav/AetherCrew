// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * Loads the ported UE 5.9 toolset modules (Source/Epic59) that link against an engine plugin HyperAI lists as optional.
 * Those modules have LoadingPhase None in HyperAIStudio.uplugin: loading one while its plugin is disabled would fail
 * the whole plugin, so they load here, after engine init, only when every plugin they need is enabled.
 */
namespace HyperAIStudio::Epic59Modules
{
	struct FGuardedModule
	{
		FName ModuleName;
		TArray<FString> Plugins;
	};

	/** Loads each guarded module whose plugins are enabled (at PostEngineInit, or now if that has passed). */
	void Startup();

	/** Names of the guarded modules and the plugins each needs, for tests and diagnostics. */
	const TArray<FGuardedModule>& GetGuardedModules();
}
