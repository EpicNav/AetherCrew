// Games by Hyper 2026.

// UE 5.8: HyperAI Studio addition, not in Epic's 5.9 source. Two jobs for the ported toolsets that UE 5.8 cannot
// express in HyperAIStudio.uplugin:
//  - Optional plugins. A port that links a plugin HyperAI users may not enable (TextureGraph, ModelViewViewModel,
//    CommonUI) is listed with LoadingPhase "None" and loaded here once the PostEngineInit phase completes, only when
//    every plugin it needs is enabled; loading it without them would fail the editor's start-up. Its own
//    OnAllModuleLoadingPhasesComplete registration still runs, since that fires after this.
//  - 5.8 duplicates. A ported toolset whose 5.8 version still ships (MVVMToolset, SlateInspectorToolset) hides the
//    5.8 one through the registry's block list once the port is loaded, so agents see one version of each tool.
//    The 5.8 SlateInspectorToolset has no guard for HyperAI Studio's own UI, so hiding it also removes that bypass.
// See Source/Epic59/PORTING.md.

#include "ToolsetRegistryEditor/Epic59Ports.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "ToolsetRegistryEditor/Module.h"

namespace UE::ToolsetRegistry::Epic59
{
namespace Private
{
	struct FOptionalPort
	{
		const TCHAR* ModuleName;
		TArray<const TCHAR*> RequiredPlugins;
	};

	struct FHiddenDuplicate
	{
		/** The port; the 5.8 toolset is hidden only while this module is loaded. */
		const TCHAR* PortModuleName;
		/** Anchored block-list pattern naming the 5.8 toolset ("<Module>.<Class>"). */
		const TCHAR* BlockedPattern;
	};

	const TArray<FOptionalPort>& GetOptionalPorts()
	{
		static const TArray<FOptionalPort> Ports = {
			{ TEXT("TextureGraphToolset"), { TEXT("TextureGraph") } },
			{ TEXT("MVVMToolset59"), { TEXT("ModelViewViewModel") } },
			{ TEXT("WidgetStaticAnalysisToolset"), { TEXT("ModelViewViewModel") } },
		};
		return Ports;
	}

	const TArray<FHiddenDuplicate>& GetHiddenDuplicates()
	{
		static const TArray<FHiddenDuplicate> Duplicates = {
		};
		return Duplicates;
	}

	FDelegateHandle GLoadingPhaseHandle;
	FDelegateHandle GAllPhasesHandle;

	void LoadOptionalPorts()
	{
		for (const FOptionalPort& Port : GetOptionalPorts())
		{
			TArray<FString> Missing;
			for (const TCHAR* PluginName : Port.RequiredPlugins)
			{
				const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
				if (!Plugin.IsValid() || !Plugin->IsEnabled())
				{
					Missing.Add(PluginName);
				}
			}
			if (!Missing.IsEmpty())
			{
				UE_LOGF(LogToolsetRegistryEditor, Display,
					"UE 5.9 toolset port %ls stays unloaded until these plugins are enabled: %ls.",
					Port.ModuleName, *FString::Join(Missing, TEXT(", ")));
				continue;
			}
			if (!FModuleManager::Get().LoadModule(Port.ModuleName))
			{
				UE_LOGF(LogToolsetRegistryEditor, Warning, "UE 5.9 toolset port %ls failed to load.", Port.ModuleName);
			}
		}
	}

	void HideDuplicates()
	{
		const TArray<FHiddenDuplicate>& Duplicates = GetHiddenDuplicates();
		if (Duplicates.IsEmpty())
		{
			return;
		}
		auto Subsystem = UToolsetRegistrySubsystem::Get(TEXT("Hiding UE 5.8 toolsets that have UE 5.9 ports"));
		if (!Subsystem.HasValue())
		{
			return;
		}
		for (const FHiddenDuplicate& Duplicate : Duplicates)
		{
			if (FModuleManager::Get().IsModuleLoaded(Duplicate.PortModuleName))
			{
				Subsystem.GetValue()->ToolsetRegistry.AddBlockedName(Duplicate.BlockedPattern);
				UE_LOGF(LogToolsetRegistryEditor, Display, "Hid the UE 5.8 toolset %ls; %ls replaces it.",
					Duplicate.BlockedPattern, Duplicate.PortModuleName);
			}
		}
	}
}

void StartupPorts()
{
	Private::GLoadingPhaseHandle = IPluginManager::Get().OnLoadingPhaseComplete().AddLambda(
		[](const ELoadingPhase::Type LoadingPhase, const bool bSuccess)
		{
			if (LoadingPhase == ELoadingPhase::PostEngineInit)
			{
				Private::LoadOptionalPorts();
			}
		});
	Private::GAllPhasesHandle = FCoreDelegates::OnAllModuleLoadingPhasesComplete.AddStatic(&Private::HideDuplicates);
}

void ShutdownPorts()
{
	IPluginManager::Get().OnLoadingPhaseComplete().Remove(Private::GLoadingPhaseHandle);
	FCoreDelegates::OnAllModuleLoadingPhasesComplete.Remove(Private::GAllPhasesHandle);
}
}
