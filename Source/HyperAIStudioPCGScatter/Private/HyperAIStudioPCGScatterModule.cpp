// Games by Hyper 2026.

#include "HyperAIStudioPCGScatterToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPCGScatterModule, Log, All);

/** Owner of the PCG scatter cohort: scatter graphs and the volumes that run them. */
class FHyperAIStudioPCGScatterModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioPCGScatterToolset::StaticClass(),
			FHyperAIStudioPCGScatterContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPCGScatterModule, Error, TEXT("PCG scatter optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioPCGScatterRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioPCGScatterToolset::StaticClass(),
				FHyperAIStudioPCGScatterContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioPCGScatterRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioPCGScatterModule, HyperAIStudioPCGScatter)
