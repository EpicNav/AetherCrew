// Games by Hyper 2026.

#include "HyperAIStudioSettings.h"

#include "HyperAIStudioLocalization.h"
#include "HyperAIStudioSandbox.h"

FString UHyperAIStudioSettings::GetGeminiCliImageModelId() const
{
	switch (GeminiCliImageModel)
	{
	case EHyperAIStudioNanoBananaModel::NanoBanana: return TEXT("gemini-2.5-flash-image");
	case EHyperAIStudioNanoBananaModel::NanoBanana2: return TEXT("gemini-3.1-flash-image");
	case EHyperAIStudioNanoBananaModel::NanoBananaPro: return TEXT("gemini-3-pro-image");
	default: return FString();
	}
}

FHyperAIStudioExternalServiceSettings UHyperAIStudioSettings::GetGeminiService()
{
	FHyperAIStudioExternalServiceSettings Service;
	Service.Id = TEXT("google_gemini");
	Service.Label = TEXT("Google Gemini API");
	Service.BaseUrl = TEXT("https://generativelanguage.googleapis.com");
	Service.ApiKeyVariable = TEXT("GEMINI_API_KEY");
	Service.ApiKeyHeader = TEXT("x-goog-api-key");
	Service.ApiKeyPrefix = FString();
	// Each model has its own price per image; nothing is charged per call on top.
	Service.CostPerCall = 0.0f;
	Service.TimeoutSeconds = 180;
	return Service;
}

TArray<FHyperAIStudioImageModelSettings> UHyperAIStudioSettings::GetNanoBananaModels()
{
	// generateContent, which Google keeps supported beside its newer Interactions API. The picture is a part of the
	// first candidate, after any text; aspect ratio always, and size only where the model takes one.
	const TCHAR* Sized = TEXT(R"({"contents": [{"parts": [{"text": "{prompt}"}]}], "generationConfig": {"responseModalities": ["IMAGE"], "imageConfig": {"aspectRatio": "{aspect_ratio}", "imageSize": "{image_size}"}}})");
	const TCHAR* Fixed = TEXT(R"({"contents": [{"parts": [{"text": "{prompt}"}]}], "generationConfig": {"responseModalities": ["IMAGE"], "imageConfig": {"aspectRatio": "{aspect_ratio}"}}})");
	auto Model = [](const TCHAR* Id, const TCHAR* Label, const TCHAR* Gemini, const TCHAR* Request, const float Cost1K, const float Cost2K, const float Cost4K)
	{
		FHyperAIStudioImageModelSettings Out;
		Out.Id = Id;
		Out.Label = Label;
		Out.ServiceId = TEXT("google_gemini");
		Out.Path = TEXT("/v1beta/models/{model}:generateContent");
		Out.RequestTemplate = Request;
		Out.ResponseImagePath = TEXT("candidates.0.content.parts.*.inlineData.data");
		Out.Model = Gemini;
		Out.Width = 1024;
		Out.Height = 1024;
		Out.CostPerImage = Cost1K;
		Out.CostPerImage2K = Cost2K;
		Out.CostPerImage4K = Cost4K;
		return Out;
	};
	// Google's standard prices per image, 2026-09.
	return {
		Model(TEXT("nano_banana_2"), TEXT("Nano Banana 2 (Gemini 3.1 Flash Image)"), TEXT("gemini-3.1-flash-image"), Sized, 0.067f, 0.101f, 0.151f),
		Model(TEXT("nano_banana_pro"), TEXT("Nano Banana Pro (Gemini 3 Pro Image)"), TEXT("gemini-3-pro-image"), Sized, 0.134f, 0.134f, 0.24f),
		Model(TEXT("nano_banana_2_lite"), TEXT("Nano Banana 2 Lite (Gemini 3.1 Flash Lite Image)"), TEXT("gemini-3.1-flash-lite-image"), Fixed, 0.0336f, 0.0f, 0.0f),
		Model(TEXT("nano_banana"), TEXT("Nano Banana (Gemini 2.5 Flash Image)"), TEXT("gemini-2.5-flash-image"), Fixed, 0.039f, 0.0f, 0.0f)};
}

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
