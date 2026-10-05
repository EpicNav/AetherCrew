// Games by Hyper 2026.

#include "HyperAIStudioSceneSpatialToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioSceneSpatialModule, Log, All);

/** Owner of the scene cost and spatial cohort: what a level costs to render, where things are, and where they go. */
class FHyperAIStudioSceneSpatialModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioSceneSpatialToolset::StaticClass(),
			FHyperAIStudioSceneSpatialContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioSceneSpatialModule, Error, TEXT("Scene spatial optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioSceneSpatialRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioSceneSpatialToolset::StaticClass(),
				FHyperAIStudioSceneSpatialContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioSceneSpatialRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioSceneSpatialModule, HyperAIStudioSceneSpatial)
