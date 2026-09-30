// Games by Hyper 2026.

#include "HyperAIStudioRetargetToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioRetargetModule, Log, All);

/** Owner of the retargeting cohort: IK Rigs, IK Retargeters and batch retargeting. */
class FHyperAIStudioRetargetModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioRetargetToolset::StaticClass(),
			FHyperAIStudioRetargetContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioRetargetModule, Error, TEXT("Retargeting optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioRetargetRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioRetargetToolset::StaticClass(),
				FHyperAIStudioRetargetContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioRetargetRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioRetargetModule, HyperAIStudioRetarget)
