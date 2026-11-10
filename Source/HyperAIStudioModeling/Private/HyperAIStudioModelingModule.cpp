// Games by Hyper 2026.

#include "HyperAIStudioModelingToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioModelingModule, Log, All);

/** Owner of the modeling cohort: model plans built with Geometry Script and written as static meshes. */
class FHyperAIStudioModelingModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioModelingToolset::StaticClass(),
			FHyperAIStudioModelingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioModelingModule, Error, TEXT("Modeling optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioModelingRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioModelingToolset::StaticClass(),
				FHyperAIStudioModelingContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioModelingRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioModelingModule, HyperAIStudioModeling)
