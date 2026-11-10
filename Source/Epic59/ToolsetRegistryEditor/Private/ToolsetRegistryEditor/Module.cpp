// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: compatibility stand-in for UE 5.9's ToolsetRegistryEditor module. It re-exports 5.8's
// ToolsetRegistry (whose ToolsetLibrary.h and ToolsetRegistrySubsystem.h already sit at the 5.9
// include paths), adds the 5.9-only ToolsetRegistry headers, and registers a toolset registrar so
// UE::Toolset::FToolsetRegistration works against 5.8's registry. See Source/Epic59/PORTING.md.

#include "ToolsetRegistryEditor/Module.h"

#include "Algo/AllOf.h"
#include "Features/IModularFeatures.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ToolsetCore/ToolsetRegistrar.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

DEFINE_LOG_CATEGORY(LogToolsetRegistryEditor);

namespace UE::ToolsetRegistry::Epic59
{
	// Forwards FToolsetRegistration to 5.8's registry, which accepts UToolsetDefinition classes only.
	class FToolsetRegistrar final : public UE::Toolset::IToolsetRegistrar
	{
	public:
		virtual bool RegisterToolsetClass(UClass* ProviderClass) override
		{
			if (!ProviderClass || !ProviderClass->IsChildOf<UToolsetDefinition>())
			{
				UE_LOGF(LogToolsetRegistryEditor, Warning,
					"Cannot register '%ls' as a toolset: UE 5.8's registry only accepts UToolsetDefinition classes.",
					ProviderClass ? *ProviderClass->GetPathName() : TEXT("null"));
				return false;
			}
			if (!UToolsetRegistry::IsToolsetClassRegistered(ProviderClass))
			{
				UToolsetRegistry::RegisterToolsetClass(ProviderClass);
			}
			return UToolsetRegistry::IsToolsetClassRegistered(ProviderClass);
		}

		virtual void UnregisterToolsetClass(UClass* ProviderClass) override
		{
			if (ProviderClass && ProviderClass->IsChildOf<UToolsetDefinition>()
				&& UToolsetRegistry::IsToolsetClassRegistered(ProviderClass))
			{
				UToolsetRegistry::UnregisterToolsetClass(ProviderClass);
			}
		}
	};
}

namespace UE::ToolsetRegistry::Epic59
{
	// Unregisters each UE 5.8 toolset class that a loaded port supersedes (EPIC59_SUPERSEDES rows), so agents see one
	// version. A registry name filter would do the same, but HyperAI refuses its own runtime toolset registrations
	// while the registry has any filter (see PORTING.md), so the 5.8 class is unregistered instead.
	void HideSupersededToolsets()
	{
		struct FSuperseded
		{
			const TCHAR* ModuleName;
			const TCHAR* ToolsetClassPath;
		};
		const FSuperseded Rows[] = {
#define EPIC59_OPTIONAL_MODULE(Module, ...)
#define EPIC59_SUPERSEDES(Module, ToolsetClassPath) { TEXT(#Module), ToolsetClassPath },
#include "ToolsetRegistryEditor/OptionalPortedModules.inl"
#undef EPIC59_SUPERSEDES
#undef EPIC59_OPTIONAL_MODULE
			{ nullptr, nullptr }
		};
		for (const FSuperseded& Row : Rows)
		{
			if (!Row.ModuleName || !FModuleManager::Get().IsModuleLoaded(Row.ModuleName))
			{
				continue;
			}
			UClass* OldClass = FindObject<UClass>(nullptr, Row.ToolsetClassPath);
			if (OldClass && UToolsetRegistry::IsToolsetClassRegistered(OldClass))
			{
				UToolsetRegistry::UnregisterToolsetClass(OldClass);
				UE_LOGF(LogToolsetRegistryEditor, Log, "'%ls' supersedes UE 5.8's '%ls'; the 5.8 toolset is unregistered.",
					Row.ModuleName, Row.ToolsetClassPath);
			}
		}
	}

	// Loads each ported module of OptionalPortedModules.inl whose plugins are all enabled.
	void LoadOptionalPortedModules()
	{
		struct FOptionalModule
		{
			const TCHAR* ModuleName;
			TArray<const TCHAR*> Plugins;
		};
		const FOptionalModule Modules[] = {
#define EPIC59_OPTIONAL_MODULE(Module, ...) { TEXT(#Module), { __VA_ARGS__ } },
#define EPIC59_SUPERSEDES(Module, ToolsetClassPath)
#include "ToolsetRegistryEditor/OptionalPortedModules.inl"
#undef EPIC59_SUPERSEDES
#undef EPIC59_OPTIONAL_MODULE
			{ nullptr, {} }
		};
		for (const FOptionalModule& Module : Modules)
		{
			if (!Module.ModuleName)
			{
				continue;
			}
			const bool bPluginsEnabled = Algo::AllOf(Module.Plugins, [](const TCHAR* Plugin)
			{
				const TSharedPtr<IPlugin> Found = IPluginManager::Get().FindPlugin(Plugin);
				return Found.IsValid() && Found->IsEnabled();
			});
			if (!bPluginsEnabled)
			{
				UE_LOGF(LogToolsetRegistryEditor, Verbose, "Not loading '%ls': a plugin it needs is not enabled.", Module.ModuleName);
				continue;
			}
			if (!FModuleManager::Get().LoadModule(Module.ModuleName))
			{
				UE_LOGF(LogToolsetRegistryEditor, Warning, "Could not load the ported toolset module '%ls'.", Module.ModuleName);
			}
		}
		HideSupersededToolsets();
	}
}

class FToolsetRegistryEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		IModularFeatures::Get().RegisterModularFeature(
			UE::Toolset::IToolsetRegistrar::ModularFeatureName, &Registrar);
		// UE 5.8: ported modules with an optional plugin dependency load once every plugin module has.
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&UE::ToolsetRegistry::Epic59::LoadOptionalPortedModules);
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		IModularFeatures::Get().UnregisterModularFeature(
			UE::Toolset::IToolsetRegistrar::ModularFeatureName, &Registrar);
	}

private:
	FDelegateHandle PostEngineInitHandle;
	UE::ToolsetRegistry::Epic59::FToolsetRegistrar Registrar;
};

IMPLEMENT_MODULE(FToolsetRegistryEditorModule, ToolsetRegistryEditor);
