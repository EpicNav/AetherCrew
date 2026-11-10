// Games by Hyper 2026.

#include "HyperAIStudioFoliageToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioFoliageModule, Log, All);

/** Owner of the foliage cohort: foliage types, scatter, placement and erasing. */
class FHyperAIStudioFoliageModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioFoliageToolset::StaticClass(),
			FHyperAIStudioFoliageContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioFoliageModule, Error, TEXT("Foliage optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioFoliageRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioFoliageToolset::StaticClass(),
				FHyperAIStudioFoliageContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioFoliageRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioFoliageModule, HyperAIStudioFoliage)
