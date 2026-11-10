// Games by Hyper 2026.

#include "HyperAIStudioEpic59Modules.h"

#include "CoreGlobals.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioEpic59, Log, All);

namespace HyperAIStudio::Epic59Modules
{
	const TArray<TPair<FName, TArray<FString>>>& GetGuardedModules()
	{
		// Module name (as in HyperAIStudio.uplugin) and the optional plugins its Build.cs links against. Tests modules
		// follow the module they test.
		static const TArray<TPair<FName, TArray<FString>>> Modules = {
			{ TEXT("RewindDebuggerToolset"), { TEXT("GameplayInsights") } },
			{ TEXT("RewindDebuggerToolsetTests"), { TEXT("GameplayInsights") } },
			{ TEXT("WorldConditionsToolset59"), { TEXT("WorldConditions") } },
			{ TEXT("DataRegistryToolset59"), { TEXT("DataRegistry") } },
		};
		return Modules;
	}

	static void LoadGuardedModules()
	{
		TArray<FString> Loaded;
		for (const TPair<FName, TArray<FString>>& Module : GetGuardedModules())
		{
			TArray<FString> Missing;
			for (const FString& PluginName : Module.Value)
			{
				const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
				if (!Plugin.IsValid() || !Plugin->IsEnabled())
				{
					Missing.Add(PluginName);
				}
			}
			if (!Missing.IsEmpty())
			{
				UE_LOG(LogHyperAIStudioEpic59, Display,
					TEXT("Ported 5.9 toolset module %s stays unloaded until these project plugins are enabled: %s."),
					*Module.Key.ToString(), *FString::Join(Missing, TEXT(", ")));
				continue;
			}
			if (FModuleManager::Get().LoadModule(Module.Key) == nullptr)
			{
				UE_LOG(LogHyperAIStudioEpic59, Warning,
					TEXT("Ported 5.9 toolset module %s did not load; check the log above for its startup error and that it was built."),
					*Module.Key.ToString());
				continue;
			}
			Loaded.Add(Module.Key.ToString());
		}
		UE_LOG(LogHyperAIStudioEpic59, Display, TEXT("Ported 5.9 toolset modules loaded on demand: %d of %d%s"),
			Loaded.Num(), GetGuardedModules().Num(),
			Loaded.IsEmpty() ? TEXT(".") : *FString::Printf(TEXT(" (%s)."), *FString::Join(Loaded, TEXT(", "))));
	}

	void Startup()
	{
		if (GIsRunning)
		{
			LoadGuardedModules();
		}
		else
		{
			FCoreDelegates::GetOnPostEngineInit().AddStatic(&LoadGuardedModules);
		}
	}
}
