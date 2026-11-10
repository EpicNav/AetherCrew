// Games by Hyper 2026.

#include "HyperAIStudioLandscapeToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLandscapeModule, Log, All);

/** Owner of the landscape cohort: making, sculpting and painting landscapes. */
class FHyperAIStudioLandscapeModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioLandscapeToolset::StaticClass(),
			FHyperAIStudioLandscapeContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioLandscapeModule, Error, TEXT("Landscape optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioLandscapeRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioLandscapeToolset::StaticClass(),
				FHyperAIStudioLandscapeContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioLandscapeRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioLandscapeModule, HyperAIStudioLandscape)
