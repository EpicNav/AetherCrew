// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: compatibility stand-in for UE 5.9's ToolsetRegistryEditor module. It re-exports 5.8's
// ToolsetRegistry (whose ToolsetLibrary.h and ToolsetRegistrySubsystem.h already sit at the 5.9
// include paths), adds the 5.9-only ToolsetRegistry headers, and registers a toolset registrar so
// UE::Toolset::FToolsetRegistration works against 5.8's registry. See Source/Epic59/PORTING.md.

#include "ToolsetRegistryEditor/Module.h"

#include "Features/IModularFeatures.h"
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

class FToolsetRegistryEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		IModularFeatures::Get().RegisterModularFeature(
			UE::Toolset::IToolsetRegistrar::ModularFeatureName, &Registrar);
	}

	virtual void ShutdownModule() override
	{
		IModularFeatures::Get().UnregisterModularFeature(
			UE::Toolset::IToolsetRegistrar::ModularFeatureName, &Registrar);
	}

private:
	UE::ToolsetRegistry::Epic59::FToolsetRegistrar Registrar;
};

IMPLEMENT_MODULE(FToolsetRegistryEditorModule, ToolsetRegistryEditor);
