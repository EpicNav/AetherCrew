// Games by Hyper 2026.

#include "HyperAIStudioDestructionToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDestructionModule, Log, All);

/** Owner of the destruction cohort: geometry collections fractured, clustered and tuned for Chaos. */
class FHyperAIStudioDestructionModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioDestructionToolset::StaticClass(),
			FHyperAIStudioDestructionContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioDestructionModule, Error, TEXT("Destruction optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioDestructionRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioDestructionToolset::StaticClass(),
				FHyperAIStudioDestructionContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioDestructionRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioDestructionModule, HyperAIStudioDestruction)
