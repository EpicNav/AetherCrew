// Games by Hyper 2026.

#include "HyperAIStudioSettings.h"

#include "HyperAIStudioLocalization.h"
#include "HyperAIStudioSandbox.h"

FString UHyperAIStudioSettings::GetAutoModeArguments(const FString& AgentName)
{
	if (!GetDefault<UHyperAIStudioSettings>()->bAgentAutoMode)
	{
		return FString();
	}
	// Both agents' own reviewed modes: each still stops what it judges risky.
	if (AgentName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase))
	{
		return TEXT("--permission-mode auto");
	}
	// A root override, so it reaches Codex's resume subcommand too; a bare word is taken as the string it spells.
	if (AgentName.Equals(TEXT("Codex"), ESearchCase::IgnoreCase))
	{
		return TEXT("-c approvals_reviewer=auto_review");
	}
	return FString();
}

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

TArray<FHyperAIStudioExternalServiceSettings> UHyperAIStudioSettings::GetMeshServices()
{
	auto Make = [](const TCHAR* Id, const TCHAR* Label, const TCHAR* BaseUrl, const TCHAR* KeyVariable, const float Cost)
	{
		FHyperAIStudioExternalServiceSettings Service;
		Service.Id = Id;
		Service.Label = Label;
		Service.BaseUrl = BaseUrl;
		Service.ApiKeyVariable = KeyVariable;
		Service.CostPerCall = Cost;
		Service.TimeoutSeconds = 120;
		return Service;
	};
	// A textured model from a picture, at list price (September 2026): Tripo 30 credits at $0.01, Meshy 30 at about
	// $0.02, Hi3D v3.0 65 at $0.02. Hi3D's key is client_id:client_secret in one variable. Viggle makes only animation
	// from words: a flat 10 credits at $0.01 a clip.
	return {
		Make(TEXT("tripo"), TEXT("Tripo 3D"), TEXT("https://openapi.tripo3d.ai/v3"), TEXT("TRIPO_API_KEY"), 0.30f),
		Make(TEXT("meshy"), TEXT("Meshy"), TEXT("https://api.meshy.ai"), TEXT("MESHY_API_KEY"), 0.60f),
		Make(TEXT("hi3d"), TEXT("Hi3D (Hitem3D)"), TEXT("https://api.hitem3d.ai"), TEXT("HITEM3D_API_KEY"), 1.30f),
		Make(TEXT("viggle"), TEXT("Viggle"), TEXT("https://apis.viggle.ai"), TEXT("VIGGLE_API_KEY"), 0.10f)};
}

TArray<FHyperAIStudioMeshTaskPrice> UHyperAIStudioSettings::GetMeshTaskPrices()
{
	auto Price = [](const TCHAR* Provider, const TCHAR* Task, const float Cost)
	{
		FHyperAIStudioMeshTaskPrice Out;
		Out.Provider = Provider;
		Out.Task = Task;
		Out.Cost = Cost;
		return Out;
	};
	// The providers' own price lists: Tripo's (developers.tripo3d.ai/en/pricing: rig 25, retarget 10 an animation,
	// texture 10, HD 20, 8K 30), Meshy's (docs.meshy.ai/api/pricing: rig 5, 3 an action, text to motion 10 or 3 swift,
	// retexture 10, 8K 15) and Viggle's (10 a clip).
	// The model creator's Tripo v3 prices (October 2026, credits at $0.01): H3.1 from words 10/20/30/40 and from pictures
	// 20/30/40/50 by texture tier (none, standard or fast, detailed, extreme); P2 100/110/120/130; add-ons detailed geometry
	// 20, quads 5, smart low poly 10, parts 20; convert 5 or 10 with mesh options; retopology v3.0 35 or 40 for quads
	// (mesh-decimate); Smart UV 20; segmentation 40; completion 50, quick cap 30. Multiview pictures are priced unverified.
	return {
		Price(TEXT("meshy"), TEXT("rig"), 0.10f),
		Price(TEXT("meshy"), TEXT("animate"), 0.06f),
		Price(TEXT("meshy"), TEXT("motion"), 0.20f),
		Price(TEXT("meshy"), TEXT("motion_swift"), 0.06f),
		Price(TEXT("meshy"), TEXT("motion_apply"), 0.06f),
		Price(TEXT("meshy"), TEXT("texture"), 0.20f),
		Price(TEXT("meshy"), TEXT("texture_8k"), 0.30f),
		Price(TEXT("tripo"), TEXT("rig"), 0.25f),
		Price(TEXT("tripo"), TEXT("animate"), 0.10f),
		Price(TEXT("tripo"), TEXT("texture"), 0.10f),
		Price(TEXT("tripo"), TEXT("texture_hd"), 0.20f),
		Price(TEXT("tripo"), TEXT("texture_8k"), 0.30f),
		Price(TEXT("tripo"), TEXT("h3_text"), 0.10f),
		Price(TEXT("tripo"), TEXT("h3_text_standard"), 0.20f),
		Price(TEXT("tripo"), TEXT("h3_text_detailed"), 0.30f),
		Price(TEXT("tripo"), TEXT("h3_text_extreme"), 0.40f),
		Price(TEXT("tripo"), TEXT("h3_image"), 0.20f),
		Price(TEXT("tripo"), TEXT("h3_image_standard"), 0.30f),
		Price(TEXT("tripo"), TEXT("h3_image_detailed"), 0.40f),
		Price(TEXT("tripo"), TEXT("h3_image_extreme"), 0.50f),
		Price(TEXT("tripo"), TEXT("p2"), 1.00f),
		Price(TEXT("tripo"), TEXT("p2_standard"), 1.10f),
		Price(TEXT("tripo"), TEXT("p2_detailed"), 1.20f),
		Price(TEXT("tripo"), TEXT("p2_extreme"), 1.30f),
		Price(TEXT("tripo"), TEXT("addon_detailed_geometry"), 0.20f),
		Price(TEXT("tripo"), TEXT("addon_quad"), 0.05f),
		Price(TEXT("tripo"), TEXT("addon_smart_low_poly"), 0.10f),
		Price(TEXT("tripo"), TEXT("addon_parts"), 0.20f),
		Price(TEXT("tripo"), TEXT("convert"), 0.05f),
		Price(TEXT("tripo"), TEXT("convert_advanced"), 0.10f),
		Price(TEXT("tripo"), TEXT("retopo"), 0.35f),
		Price(TEXT("tripo"), TEXT("retopo_quad"), 0.40f),
		Price(TEXT("tripo"), TEXT("smart_uv"), 0.20f),
		Price(TEXT("tripo"), TEXT("segment"), 0.40f),
		Price(TEXT("tripo"), TEXT("complete"), 0.50f),
		Price(TEXT("tripo"), TEXT("complete_quick"), 0.30f),
		Price(TEXT("tripo"), TEXT("image_to_multiview"), 0.10f),
		Price(TEXT("tripo"), TEXT("edit_multiview"), 0.10f),
		Price(TEXT("viggle"), TEXT("motion"), 0.10f)};
}

float UHyperAIStudioSettings::FindMeshTaskPrice(const FString& Provider, const FString& Task)
{
	auto Find = [&Provider, &Task](const TArray<FHyperAIStudioMeshTaskPrice>& Prices) -> const FHyperAIStudioMeshTaskPrice*
	{
		return Prices.FindByPredicate([&](const FHyperAIStudioMeshTaskPrice& Price)
		{
			return Price.Provider.Equals(Provider, ESearchCase::IgnoreCase) && Price.Task.Equals(Task, ESearchCase::IgnoreCase);
		});
	};
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	if (const FHyperAIStudioMeshTaskPrice* Own = Settings ? Find(Settings->MeshTaskPrices) : nullptr)
	{
		return FMath::Max(Own->Cost, 0.0f);
	}
	const TArray<FHyperAIStudioMeshTaskPrice> BuiltIn = GetMeshTaskPrices();
	const FHyperAIStudioMeshTaskPrice* Default = Find(BuiltIn);
	return Default ? Default->Cost : -1.0f;
}

TArray<FHyperAIStudioExternalServiceSettings> UHyperAIStudioSettings::GetAudioServices()
{
	auto Make = [](const TCHAR* Id, const TCHAR* Label, const TCHAR* BaseUrl, const TCHAR* KeyVariable, const TCHAR* KeyHeader, const TCHAR* KeyPrefix,
		const float Cost)
	{
		FHyperAIStudioExternalServiceSettings Service;
		Service.Id = Id;
		Service.Label = Label;
		Service.BaseUrl = BaseUrl;
		Service.ApiKeyVariable = KeyVariable;
		Service.ApiKeyHeader = KeyHeader;
		Service.ApiKeyPrefix = KeyPrefix;
		Service.CostPerCall = Cost;
		Service.TimeoutSeconds = 180;
		return Service;
	};
	// List prices (October 2026): ElevenLabs' API bills sound effects at $0.12 and music at $0.15 a minute of audio, so
	// their price is a minute's; Stability bills 8 credits at $0.01 for a Stable Audio 2.5 or 3 generation.
	return {
		Make(TEXT("elevenlabs"), TEXT("ElevenLabs Sound Effects"), TEXT("https://api.elevenlabs.io"), TEXT("ELEVENLABS_API_KEY"), TEXT("xi-api-key"), TEXT(""), 0.12f),
		Make(TEXT("elevenlabs_music"), TEXT("ElevenLabs Music"), TEXT("https://api.elevenlabs.io"), TEXT("ELEVENLABS_API_KEY"), TEXT("xi-api-key"), TEXT(""), 0.15f),
		Make(TEXT("stability"), TEXT("Stability AI (Stable Audio)"), TEXT("https://api.stability.ai"), TEXT("STABILITY_API_KEY"), TEXT("Authorization"), TEXT("Bearer "), 0.08f)};
}

TArray<FHyperAIStudioImageModelSettings> UHyperAIStudioSettings::GetNanoBananaModels()
{
	// generateContent, which Google keeps supported beside its newer Interactions API. The picture is a part of the
	// first candidate, after any text; aspect ratio always, and size only where the model takes one.
	const TCHAR* Sized = TEXT(R"({"contents": [{"parts": [{"text": "{prompt}"}]}], "generationConfig": {"responseModalities": ["IMAGE"], "imageConfig": {"aspectRatio": "{aspect_ratio}", "imageSize": "{image_size}"}}})");
	const TCHAR* Fixed = TEXT(R"({"contents": [{"parts": [{"text": "{prompt}"}]}], "generationConfig": {"responseModalities": ["IMAGE"], "imageConfig": {"aspectRatio": "{aspect_ratio}"}}})");
	// A picture to change goes beside the words, as a part of its own.
	const TCHAR* SizedEdit = TEXT(R"({"contents": [{"parts": [{"text": "{prompt}"}, {"inlineData": {"mimeType": "{input_mime_type}", "data": "{input_image}"}}]}], "generationConfig": {"responseModalities": ["IMAGE"], "imageConfig": {"aspectRatio": "{aspect_ratio}", "imageSize": "{image_size}"}}})");
	const TCHAR* FixedEdit = TEXT(R"({"contents": [{"parts": [{"text": "{prompt}"}, {"inlineData": {"mimeType": "{input_mime_type}", "data": "{input_image}"}}]}], "generationConfig": {"responseModalities": ["IMAGE"], "imageConfig": {"aspectRatio": "{aspect_ratio}"}}})");
	auto Model = [](const TCHAR* Id, const TCHAR* Label, const TCHAR* Gemini, const TCHAR* Request, const TCHAR* Edit, const float Cost1K, const float Cost2K, const float Cost4K)
	{
		FHyperAIStudioImageModelSettings Out;
		Out.Id = Id;
		Out.Label = Label;
		Out.ServiceId = TEXT("google_gemini");
		Out.Path = TEXT("/v1beta/models/{model}:generateContent");
		Out.RequestTemplate = Request;
		Out.EditRequestTemplate = Edit;
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
		Model(TEXT("nano_banana_2"), TEXT("Nano Banana 2 (Gemini 3.1 Flash Image)"), TEXT("gemini-3.1-flash-image"), Sized, SizedEdit, 0.067f, 0.101f, 0.151f),
		Model(TEXT("nano_banana_pro"), TEXT("Nano Banana Pro (Gemini 3 Pro Image)"), TEXT("gemini-3-pro-image"), Sized, SizedEdit, 0.134f, 0.134f, 0.24f),
		Model(TEXT("nano_banana_2_lite"), TEXT("Nano Banana 2 Lite (Gemini 3.1 Flash Lite Image)"), TEXT("gemini-3.1-flash-lite-image"), Fixed, FixedEdit, 0.0336f, 0.0f, 0.0f),
		Model(TEXT("nano_banana"), TEXT("Nano Banana (Gemini 2.5 Flash Image)"), TEXT("gemini-2.5-flash-image"), Fixed, FixedEdit, 0.039f, 0.0f, 0.0f)};
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
