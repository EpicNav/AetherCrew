// Games by Hyper 2026.

#include "HyperAIStudioWorldPartitionToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioWorldPartitionModule, Log, All);

/** Owner of the World Partition cohort: data layers, HLOD layers, streaming and editor regions. */
class FHyperAIStudioWorldPartitionModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(UHyperAIStudioWorldPartitionToolset::StaticClass(),
			FHyperAIStudioWorldPartitionContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioWorldPartitionModule, Error, TEXT("World Partition optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioWorldPartitionRegistration>();
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
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(UHyperAIStudioWorldPartitionToolset::StaticClass(),
				FHyperAIStudioWorldPartitionContracts::GetQualifiedToolsetName());
			bRuntimeToolsetPublished = false;
		}
	}

	virtual bool SupportsDynamicReloading() override { return false; }

private:
	TUniquePtr<FHyperAIStudioWorldPartitionRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
};

IMPLEMENT_MODULE(FHyperAIStudioWorldPartitionModule, HyperAIStudioWorldPartition)
