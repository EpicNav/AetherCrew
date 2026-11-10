// Games by Hyper 2026.

#include "HyperAIStudioScriptingToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioScriptingModule, Log, All);

/** Owner of the one-tool scripting cohort. The core loads it only while Python scripting is switched on. */
class FHyperAIStudioScriptingModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioScriptingToolset::StaticClass(),
			FHyperAIStudioScriptingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioScriptingModule, Error, TEXT("Scripting optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioScriptingRegistration>();
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
				UHyperAIStudioScriptingToolset::StaticClass(),
				FHyperAIStudioScriptingContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioScriptingRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioScriptingModule, HyperAIStudioScripting)
