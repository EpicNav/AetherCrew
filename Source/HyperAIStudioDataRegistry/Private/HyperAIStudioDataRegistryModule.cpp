// Games by Hyper 2026.

#include "HyperAIStudioDataRegistryToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDataRegistryModule, Log, All);

/** Owner of the data registry cohort: Data Registry assets made, given sources and looked up in the editor. */
class FHyperAIStudioDataRegistryModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioDataRegistryToolset::StaticClass(),
			FHyperAIStudioDataRegistryContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioDataRegistryModule, Error, TEXT("Data registry optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioDataRegistryRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioDataRegistryToolset::StaticClass(),
				FHyperAIStudioDataRegistryContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioDataRegistryRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioDataRegistryModule, HyperAIStudioDataRegistry)
