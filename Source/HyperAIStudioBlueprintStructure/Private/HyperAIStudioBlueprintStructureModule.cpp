// Games by Hyper 2026.

#include "HyperAIStudioBlueprintStructureToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioBlueprintStructureModule, Log, All);

/** Owner of the Blueprint structure cohort: timelines, interfaces, replication and collapsed graphs. */
class FHyperAIStudioBlueprintStructureModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioBlueprintStructureToolset::StaticClass(),
			FHyperAIStudioBlueprintStructureContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioBlueprintStructureModule, Error, TEXT("Blueprint structure optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioBlueprintStructureRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioBlueprintStructureToolset::StaticClass(),
				FHyperAIStudioBlueprintStructureContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioBlueprintStructureRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioBlueprintStructureModule, HyperAIStudioBlueprintStructure)
