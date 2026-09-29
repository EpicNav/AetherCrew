// Games by Hyper 2026.

#include "HyperAIStudioDecibelToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDecibelModule, Log, All);

/** Owner of the sound design cohort: sound effects, ambiance and music built with Decibel. */
class FHyperAIStudioDecibelModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if WITH_DECIBEL
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioDecibelToolset::StaticClass(), FHyperAIStudioDecibelContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioDecibelModule, Error, TEXT("Sound design optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioDecibelRegistration>();
		Registration->Startup();
#endif
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioDecibelToolset::StaticClass(), FHyperAIStudioDecibelContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioDecibelRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioDecibelModule, HyperAIStudioDecibel)
