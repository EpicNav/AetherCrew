// Games by Hyper 2026.

#include "HyperAIStudioAudioAuthoringToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAudioAuthoringModule, Log, All);

/** Owner of the audio authoring cohort: Sound Cues, the mix and procedural MetaSound recipes. */
class FHyperAIStudioAudioAuthoringModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioAudioAuthoringToolset::StaticClass(),
			FHyperAIStudioAudioAuthoringContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAudioAuthoringModule, Error, TEXT("Audio authoring optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioAudioAuthoringRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioAudioAuthoringToolset::StaticClass(),
				FHyperAIStudioAudioAuthoringContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioAudioAuthoringRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioAudioAuthoringModule, HyperAIStudioAudioAuthoring)
