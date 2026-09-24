// Games by Hyper 2026.

#include "HyperAIStudioSettings.h"

#include "HyperAIStudioLocalization.h"
#include "HyperAIStudioSandbox.h"

TArray<FString> UHyperAIStudioSettings::GetLanguageOptions()
{
	TArray<FString> Options = {TEXT("Editor"), TEXT("en")};
	Options.Append(HyperAIStudio::Localization::GetTranslatedCultures());
	return Options;
}

#if WITH_EDITOR
void UHyperAIStudioSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName Property = PropertyChangedEvent.GetMemberPropertyName();
	if (Property == GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, UILanguage))
	{
		HyperAIStudio::Localization::Refresh();
	}
	else if (Property == GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, bSandboxMode))
	{
		HyperAIStudio::Sandbox::OnModeChanged();
	}
}
#endif
