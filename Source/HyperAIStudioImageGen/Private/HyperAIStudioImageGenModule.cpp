// Games by Hyper 2026.

#include "HyperAIStudioImageGenToolset.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioShell.h"
#include "Modules/ModuleManager.h"
#include "SHyperAIStudioImagesPanel.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioImageGenModule"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioImageGenModule, Log, All);

/** Explicit owner for one LoadingPhase=None, atomic two-tool image generation source cohort. */
class FHyperAIStudioImageGenModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::PublishOptionalToolset(
			UHyperAIStudioImageGenToolset::StaticClass(),
			FHyperAIStudioImageGenContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioImageGenModule, Error, TEXT("Image generation optional publication failed closed: %s"), *Error);
			return;
		}
		bRuntimeToolsetPublished = true;
		Registration = MakeUnique<FHyperAIStudioImageGenRegistration>();
		Registration->Startup();

		FHyperAIStudioPage Page;
		Page.Id = TEXT("Images");
		Page.Label = LOCTEXT("ImagesPage", "Images");
		Page.Tooltip = LOCTEXT("ImagesPageTooltip", "Generate reference images, with Codex, Gemini or a configured model, and browse the ones the project has.");
		Page.Group = TEXT("Create");
		Page.Order = 10;
		Page.Icon = TEXT("HyperAIStudio.Icon.Images");
		Page.Create = [](const TSharedRef<SDockTab>&) { return SNew(SHyperAIStudioImagesPanel); };
		FHyperAIStudioShell::RegisterPage(Page);
		bPageRegistered = true;
	}

	virtual void ShutdownModule() override
	{
		if (bPageRegistered)
		{
			FHyperAIStudioShell::UnregisterPage(TEXT("Images"));
			bPageRegistered = false;
		}
		if (Registration)
		{
			Registration->Shutdown();
			if (Registration->HasLiveOwnership())
			{
				UE_LOG(LogHyperAIStudioImageGenModule, Error,
					TEXT("Image generation module shutdown retained live ownership after a busy fail-closed rollback."));
				return;
			}
			Registration.Reset();
		}
		if (bRuntimeToolsetPublished)
		{
			FHyperAIStudioCapabilityRuntimeIndex::WithdrawOptionalToolset(
				UHyperAIStudioImageGenToolset::StaticClass(),
				FHyperAIStudioImageGenContracts::GetQualifiedToolsetName());
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
	TUniquePtr<FHyperAIStudioImageGenRegistration> Registration;
	bool bRuntimeToolsetPublished = false;
	bool bPageRegistered = false;
};

IMPLEMENT_MODULE(FHyperAIStudioImageGenModule, HyperAIStudioImageGen)

#undef LOCTEXT_NAMESPACE
