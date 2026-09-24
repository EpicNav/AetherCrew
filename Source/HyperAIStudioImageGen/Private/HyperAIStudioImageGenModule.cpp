// Games by Hyper 2026.

#include "HyperAIStudioImageGenToolset.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "Modules/ModuleManager.h"
#include "SHyperAIStudioImagesPanel.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

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

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(SHyperAIStudioImagesPanel::GetTabName(), FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				.Label(LOCTEXT("ImagesTab", "HyperAI Images"))
				[
					SNew(SHyperAIStudioImagesPanel)
				];
		}))
		.SetDisplayName(LOCTEXT("ImagesTab", "HyperAI Images"))
		.SetTooltipText(LOCTEXT("ImagesTabTooltip", "Generate reference images, with Codex or a configured model, and browse the ones the project has."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
		bTabRegistered = true;
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FHyperAIStudioImageGenModule::RegisterMenus));
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this);
		if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
		{
			Menu->FindOrAddSection(TEXT("HyperAIStudio")).AddMenuEntry(
				TEXT("OpenHyperAIStudioImages"),
				LOCTEXT("ImagesMenu", "HyperAI Images"),
				LOCTEXT("ImagesMenuTooltip", "Generate reference images and browse the generated ones."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(SHyperAIStudioImagesPanel::GetTabName()); })));
		}
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized())
		{
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
		}
		if (bTabRegistered && FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SHyperAIStudioImagesPanel::GetTabName());
			bTabRegistered = false;
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
	bool bTabRegistered = false;
};

IMPLEMENT_MODULE(FHyperAIStudioImageGenModule, HyperAIStudioImageGen)

#undef LOCTEXT_NAMESPACE
