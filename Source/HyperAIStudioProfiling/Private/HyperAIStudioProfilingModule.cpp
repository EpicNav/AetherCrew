// Games by Hyper 2026.

#include "HyperAIStudioProfilingToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioProfilingModule, Log, All);

/** Owner of the play-session profiling cohort: measure a play session, compare two, and read Nanite and Niagara live. */
class FHyperAIStudioProfilingModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioProfilingToolset::StaticClass(),
			FHyperAIStudioProfilingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioProfilingModule, Error, TEXT("Profiling optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioProfilingRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioProfilingToolset::StaticClass(),
				FHyperAIStudioProfilingContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioProfilingRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioProfilingModule, HyperAIStudioProfiling)
