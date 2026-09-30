// Games by Hyper 2026.

#include "HyperAIStudioMovieRenderToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMovieRenderModule, Log, All);

/** Owner of the Movie Render cohort: render configs, graphs and renders. */
class FHyperAIStudioMovieRenderModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioMovieRenderToolset::StaticClass(),
			FHyperAIStudioMovieRenderContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioMovieRenderModule, Error, TEXT("Movie Render optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioMovieRenderRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioMovieRenderToolset::StaticClass(),
				FHyperAIStudioMovieRenderContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioMovieRenderRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioMovieRenderModule, HyperAIStudioMovieRender)
