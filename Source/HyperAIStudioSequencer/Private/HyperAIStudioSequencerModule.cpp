// Games by Hyper 2026.

#include "HyperAIStudioSequencerToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioSequencerModule, Log, All);

/** Owner of the Sequencer cohort: level sequences cut like a film. */
class FHyperAIStudioSequencerModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioSequencerToolset::StaticClass(),
			FHyperAIStudioSequencerContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioSequencerModule, Error, TEXT("Sequencer optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioSequencerRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioSequencerToolset::StaticClass(),
				FHyperAIStudioSequencerContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioSequencerRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioSequencerModule, HyperAIStudioSequencer)
