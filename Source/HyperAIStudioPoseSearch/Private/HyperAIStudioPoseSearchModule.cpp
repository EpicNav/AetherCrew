// Games by Hyper 2026.

#include "HyperAIStudioPoseSearchToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPoseSearchModule, Log, All);

/** Owner of the motion matching cohort: Pose Search schemas, databases and their search indexes. */
class FHyperAIStudioPoseSearchModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioPoseSearchToolset::StaticClass(),
			FHyperAIStudioPoseSearchContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPoseSearchModule, Error, TEXT("Pose Search optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioPoseSearchRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioPoseSearchToolset::StaticClass(),
				FHyperAIStudioPoseSearchContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioPoseSearchRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioPoseSearchModule, HyperAIStudioPoseSearch)
