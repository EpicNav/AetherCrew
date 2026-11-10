// Games by Hyper 2026.

#include "HyperAIStudioImageGenToolset.h"

#include "HAL/IConsoleManager.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioShell.h"
#include "Modules/ModuleManager.h"
#include "SHyperAIStudioImagesPanel.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioImageGenModule"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioImageGenModule, Log, All);

namespace HyperAIStudio::ImageGen::Private
{
	/**
	 * HyperAIStudio.Relight [model=<id>] [shape=<3:2 and so on>] <how it should be lit>: Relight this view from the console,
	 * generating at once as Generate does.
	 */
	void RunRelight(const TArray<FString>& Args)
	{
		TArray<FString> Words = Args;
		FString ModelId;
		FString Shape;
		while (!Words.IsEmpty() && (Words[0].StartsWith(TEXT("model="), ESearchCase::IgnoreCase) || Words[0].StartsWith(TEXT("shape="), ESearchCase::IgnoreCase)))
		{
			(Words[0].StartsWith(TEXT("model="), ESearchCase::IgnoreCase) ? ModelId : Shape) = Words[0].RightChop(6);
			Words.RemoveAt(0);
		}
		const FString Prompt = FString::Join(Words, TEXT(" ")).TrimStartAndEnd();
		SHyperAIStudioImagesPanel::OpenOnPreset(TEXT("relight_view"), Prompt, ModelId, /*bGenerate=*/!Prompt.IsEmpty(), Shape);
	}

	FAutoConsoleCommand RelightCommand(TEXT("HyperAIStudio.Relight"),
		TEXT("Relight this view: HyperAIStudio.Relight [model=<id>] [shape=<16:9, 3:2 and so on>] <how it should be lit>. Takes the view through HyperAI_MatchCamera, has the image model relight it, which spends as the Images page's Generate does, then matches the level's lighting to it. With no words it only opens the page on the preset."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunRelight));
}

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

		// By this id the Match page offers Relight this view... beside Browse, for a user with no reference image.
		FHyperAIStudioAction Relight;
		Relight.Id = TEXT("RelightView");
		Relight.Label = LOCTEXT("RelightAction", "Relight this view");
		Relight.Description = LOCTEXT("RelightActionDescription", "No reference image? Make one from the level itself: an image model relights the view as you describe, then your preferred agent matches the level's lighting to it.");
		Relight.Icon = TEXT("HyperAIStudio.Icon.Images");
		Relight.Run = []() { SHyperAIStudioImagesPanel::OpenOnPreset(TEXT("relight_view")); };
		FHyperAIStudioShell::RegisterAction(Relight);
	}

	virtual void ShutdownModule() override
	{
		if (bPageRegistered)
		{
			FHyperAIStudioShell::UnregisterAction(TEXT("RelightView"));
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
