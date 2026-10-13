// Games by Hyper 2026.

#include "HyperAIStudioMotionWarpingToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMotionWarpingModule, Log, All);

/** Owner of the motion warping cohort: warp windows on montages and the warping component on characters. */
class FHyperAIStudioMotionWarpingModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioMotionWarpingToolset::StaticClass(),
			FHyperAIStudioMotionWarpingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioMotionWarpingModule, Error, TEXT("Motion warping optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioMotionWarpingRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioMotionWarpingToolset::StaticClass(),
				FHyperAIStudioMotionWarpingContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioMotionWarpingRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioMotionWarpingModule, HyperAIStudioMotionWarping)
