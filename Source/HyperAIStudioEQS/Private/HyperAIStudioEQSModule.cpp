// Games by Hyper 2026.

#include "HyperAIStudioEQSToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioEQSModule, Log, All);

/** Owner of the EQS cohort: Environment Queries made, tuned, wired into Behavior Trees and run. */
class FHyperAIStudioEQSModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioEQSToolset::StaticClass(),
			FHyperAIStudioEQSContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioEQSModule, Error, TEXT("EQS optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioEQSRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioEQSToolset::StaticClass(),
				FHyperAIStudioEQSContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioEQSRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioEQSModule, HyperAIStudioEQS)
