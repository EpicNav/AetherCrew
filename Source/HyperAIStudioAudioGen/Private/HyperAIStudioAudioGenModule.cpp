// Games by Hyper 2026.

#include "HyperAIStudioAudioGenToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAudioGenModule, Log, All);

/** Owner of the audio generation cohort: sound effects and music from ElevenLabs and Stable Audio, imported into the project. */
class FHyperAIStudioAudioGenModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioAudioGenToolset::StaticClass(), FHyperAIStudioAudioGenContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAudioGenModule, Error, TEXT("Audio generation optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioAudioGenRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioAudioGenToolset::StaticClass(), FHyperAIStudioAudioGenContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioAudioGenRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioAudioGenModule, HyperAIStudioAudioGen)
