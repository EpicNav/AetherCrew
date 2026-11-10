// Games by Hyper 2026.

#include "HyperAIStudioMeshTerrainToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMeshTerrainModule, Log, All);

/** Explicit owner for one LoadingPhase=None, atomic three-tool mesh terrain source cohort. */
class FHyperAIStudioMeshTerrainModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioMeshTerrainToolset::StaticClass(),
			FHyperAIStudioMeshTerrainContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioMeshTerrainModule, Error, TEXT("Mesh terrain optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioMeshTerrainRegistration>();
		Registration->Startup();
	}

	virtual void ShutdownModule() override
	{
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioMeshTerrainModule, Error,
					TEXT("Mesh terrain module shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioMeshTerrainToolset::StaticClass(),
				FHyperAIStudioMeshTerrainContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

	virtual bool SupportsAutomaticShutdown() override
	{
		// Registered adapter vtables may not outlive this DLL.
		return false;
	}

private:
	TUniquePtr<FHyperAIStudioMeshTerrainRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioMeshTerrainModule, HyperAIStudioMeshTerrain)
