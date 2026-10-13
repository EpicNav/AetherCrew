// Games by Hyper 2026.

#include "HyperAIStudioChooserToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioChooserModule, Log, All);

/** Owner of the chooser cohort: chooser tables read, built in plans, and evaluated against context values. */
class FHyperAIStudioChooserModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioChooserToolset::StaticClass(),
			FHyperAIStudioChooserContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioChooserModule, Error, TEXT("Chooser optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioChooserRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioChooserToolset::StaticClass(),
				FHyperAIStudioChooserContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioChooserRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioChooserModule, HyperAIStudioChooser)
