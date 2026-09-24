// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioImageGenGate.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioWidgetShot.h"
#include "IHttpRouter.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioImagesPanel.h"

namespace HyperAIStudio::ImageGen::NanoBananaTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** A small PNG in base64, as Gemini sends a picture. */
	FString PngBase64()
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(40, 120, 200), 16 * 9);
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(16, 9, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		return FBase64::Encode(Png.GetData(), static_cast<uint32>(Png.Num()));
	}

	TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	/** Puts the model and service lists back, whatever a test did to them. */
	struct FSettingsGuard
	{
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		TArray<FHyperAIStudioImageModelSettings> Models = Settings->ImageModels;
		TArray<FHyperAIStudioExternalServiceSettings> Services = Settings->ExternalServices;
		~FSettingsGuard()
		{
			Settings->ImageModels = Models;
			Settings->ExternalServices = Services;
		}
	};

	FHyperAIStudioImageModelSettings Model(const TCHAR* Id)
	{
		FHyperAIStudioImageModelSettings Found;
		Gate::FindModel(Id, Found);
		return Found;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNanoBananaModelsTest,
	"HyperAIStudio.ImageGen.NanoBanana.Models",
	HyperAIStudio::ImageGen::NanoBananaTests::Flags)

bool FHyperAIStudioNanoBananaModelsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ImageGen;
	using namespace HyperAIStudio::ImageGen::NanoBananaTests;
	const FSettingsGuard Guard;
	Guard.Settings->ImageModels.Reset();
	Guard.Settings->ExternalServices.Reset();

	// Built in, with Google's current ids, through generateContent.
	const TPair<const TCHAR*, const TCHAR*> Expected[] = {
		{TEXT("nano_banana"), TEXT("gemini-2.5-flash-image")}, {TEXT("nano_banana_2"), TEXT("gemini-3.1-flash-image")},
		{TEXT("nano_banana_2_lite"), TEXT("gemini-3.1-flash-lite-image")}, {TEXT("nano_banana_pro"), TEXT("gemini-3-pro-image")}};
	for (const TPair<const TCHAR*, const TCHAR*>& Pair : Expected)
	{
		FHyperAIStudioImageModelSettings Found;
		TestTrue(FString::Printf(TEXT("%s is built in, as %s through the Gemini API"), Pair.Key, Pair.Value),
			Gate::FindModel(Pair.Key, Found) && Found.Model == Pair.Value && Found.ServiceId == TEXT("google_gemini")
			&& Found.Path == TEXT("/v1beta/models/{model}:generateContent") && Found.ResponseImagePath == TEXT("candidates.0.content.parts.*.inlineData.data"));
	}

	// Each is priced per image, by the size asked for.
	const FHyperAIStudioImageModelSettings Two = Model(TEXT("nano_banana_2"));
	const FHyperAIStudioImageModelSettings Pro = Model(TEXT("nano_banana_pro"));
	const FHyperAIStudioImageModelSettings Lite = Model(TEXT("nano_banana_2_lite"));
	const FHyperAIStudioImageModelSettings Banana = Model(TEXT("nano_banana"));
	TestEqual(TEXT("Nano Banana 2 costs 0.067 at 1K"), Gate::CostPerImage(Two, 1024, 1024), 0.067, 1e-6);
	TestEqual(TEXT("0.101 at 2K"), Gate::CostPerImage(Two, 1920, 1080), 0.101, 1e-6);
	TestEqual(TEXT("and 0.151 at 4K"), Gate::CostPerImage(Two, 3840, 2160), 0.151, 1e-6);
	TestEqual(TEXT("Nano Banana Pro costs 0.134 at 2K"), Gate::CostPerImage(Pro, 2048, 2048), 0.134, 1e-6);
	TestEqual(TEXT("and 0.24 at 4K"), Gate::CostPerImage(Pro, 4096, 4096), 0.24, 1e-6);
	TestEqual(TEXT("Nano Banana 2 Lite makes 1K only, so it costs the same asked bigger"), Gate::CostPerImage(Lite, 4096, 2304), 0.0336, 1e-6);
	TestEqual(TEXT("Nano Banana costs 0.039"), Gate::CostPerImage(Banana, 1024, 1024), 0.039, 1e-6);
	TestEqual(TEXT("Codex draws on the user's plan, for nothing here"), Gate::CostPerImage(Model(TEXT("codex_image_gen")), 1024, 1024), 0.0, 1e-9);

	// Width and height become what Gemini takes.
	TestEqual(TEXT("Square is 1:1"), Gate::AspectRatio(1024, 1024), FString(TEXT("1:1")));
	TestEqual(TEXT("1920 x 1080 is 16:9"), Gate::AspectRatio(1920, 1080), FString(TEXT("16:9")));
	TestEqual(TEXT("1080 x 1920 is 9:16"), Gate::AspectRatio(1080, 1920), FString(TEXT("9:16")));
	TestEqual(TEXT("2520 x 1080 is 21:9"), Gate::AspectRatio(2520, 1080), FString(TEXT("21:9")));
	TestEqual(TEXT("1200 x 1500 is 4:5"), Gate::AspectRatio(1200, 1500), FString(TEXT("4:5")));
	TestTrue(TEXT("The longer side picks 1K, 2K or 4K"), Gate::ImageSize(1024, 768) == TEXT("1K") && Gate::ImageSize(1920, 1080) == TEXT("2K")
		&& Gate::ImageSize(2560, 1440) == TEXT("4K"));

	FString Body;
	FString Error;
	TestTrue(TEXT("Nano Banana 2's request builds: ") + Error,
		Gate::BuildRequestBody(Two, TEXT("a lighthouse at dusk"), TEXT("text, watermark"), 1920, 1080, -1, Body, Error));
	AddInfo(TEXT("Nano Banana 2 request: ") + Body);
	TestTrue(TEXT("with the prompt, and the negative after it since Gemini has no field for one"),
		Body.Contains(TEXT("\"text\": \"a lighthouse at dusk Avoid: text, watermark\"")));
	TestTrue(TEXT("asking for a picture back"), Body.Contains(TEXT("\"responseModalities\": [\"IMAGE\"]")));
	TestTrue(TEXT("at 16:9 and 2K"), Body.Contains(TEXT("\"aspectRatio\": \"16:9\"")) && Body.Contains(TEXT("\"imageSize\": \"2K\"")));
	TestTrue(TEXT("Nano Banana's request builds"), Gate::BuildRequestBody(Banana, TEXT("a lighthouse"), FString(), 1024, 1024, -1, Body, Error));
	TestTrue(TEXT("with no size, which it does not take"), Body.Contains(TEXT("\"aspectRatio\": \"1:1\"")) && !Body.Contains(TEXT("imageSize")));

	// Model names are data: an entry with a built-in id replaces it.
	FHyperAIStudioImageModelSettings Renamed = Pro;
	Renamed.Model = TEXT("gemini-3-pro-image-preview");
	Guard.Settings->ImageModels = {Renamed};
	TestEqual(TEXT("An Image Models entry with a built-in id replaces it"), Model(TEXT("nano_banana_pro")).Model, FString(TEXT("gemini-3-pro-image-preview")));
	TestEqual(TEXT("and is listed once"), Gate::ReadModels().FilterByPredicate([](const FHyperAIImageModelInfo& Info) { return Info.Id == TEXT("nano_banana_pro"); }).Num(), 1);
	Guard.Settings->ImageModels.Reset();

	// What the Images page shows: the price, and the key.
	const TArray<FHyperAIImageModelInfo> Listed = Gate::ReadModels();
	TArray<FString> Ids;
	for (const FHyperAIImageModelInfo& Info : Listed)
	{
		Ids.Add(Info.Id);
	}
	AddInfo(TEXT("Models: ") + FString::Join(Ids, TEXT(", ")));
	TestTrue(TEXT("Nano Banana 2 comes first, then Pro"), Ids.Num() >= 4 && Ids[0] == TEXT("nano_banana_2") && Ids[1] == TEXT("nano_banana_pro"));
	const FHyperAIImageModelInfo* Listed2 = Listed.FindByPredicate([](const FHyperAIImageModelInfo& Info) { return Info.Id == TEXT("nano_banana_2"); });
	TestTrue(TEXT("with its price at its own size"), Listed2 && FMath::IsNearlyEqual(Listed2->CostPerCall, 0.067f, 1e-4f));
	if (FPlatformMisc::GetEnvironmentVariable(TEXT("GEMINI_API_KEY")).IsEmpty())
	{
		TestTrue(TEXT("and without GEMINI_API_KEY, saying how to get one"), Listed2 && !Listed2->bHasApiKey
			&& Listed2->Issue.Contains(TEXT("GEMINI_API_KEY")) && Listed2->Issue.Contains(TEXT("aistudio.google.com")));
		if (Listed2)
		{
			AddInfo(TEXT("Without a key: ") + Listed2->Issue);
		}
	}
	else
	{
		TestTrue(TEXT("and with GEMINI_API_KEY set, as having its key"), Listed2 && Listed2->bHasApiKey);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNanoBananaRepliesTest,
	"HyperAIStudio.ImageGen.NanoBanana.Replies",
	HyperAIStudio::ImageGen::NanoBananaTests::Flags)

bool FHyperAIStudioNanoBananaRepliesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ImageGen;
	using namespace HyperAIStudio::ImageGen::NanoBananaTests;
	const FString Path = TEXT("candidates.0.content.parts.*.inlineData.data");
	TArray<uint8> Bytes;
	FString Error;
	// Gemini may put a sentence before the picture.
	const FString Reply = FString::Printf(TEXT(R"({"candidates": [{"content": {"parts": [{"text": "Here is your lighthouse."}, {"inlineData": {"mimeType": "image/png", "data": "%s"}}]}, "finishReason": "STOP"}]})"),
		*PngBase64());
	TestTrue(TEXT("The picture after the text is found: ") + Error, Gate::ExtractImageBytes(Utf8(Reply), Path, Bytes, Error));
	TestEqual(TEXT("and it is a PNG"), Gate::SniffImageExtension(Bytes), FString(TEXT("png")));

	TestFalse(TEXT("A blocked prompt has no picture"), Gate::ExtractImageBytes(Utf8(TEXT(R"({"promptFeedback": {"blockReason": "SAFETY"}})")), Path, Bytes, Error));
	TestTrue(TEXT("and says why, in Gemini's words"), Error.Contains(TEXT("blocked the prompt (SAFETY)")));
	TestFalse(TEXT("Nor has a text-only answer"), Gate::ExtractImageBytes(Utf8(TEXT(R"({"candidates": [{"content": {"parts": [{"text": "I can't draw people."}]}, "finishReason": "STOP"}]})")), Path, Bytes, Error));
	TestTrue(TEXT("which is passed on"), Error.Contains(TEXT("answered with text instead of a picture: I can't draw people.")));
	TestFalse(TEXT("Nor a stop for safety"), Gate::ExtractImageBytes(Utf8(TEXT(R"({"candidates": [{"finishReason": "IMAGE_SAFETY"}]})")), Path, Bytes, Error));
	TestTrue(TEXT("which names the reason"), Error.Contains(TEXT("IMAGE_SAFETY")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNanoBananaEndToEndTest,
	"HyperAIStudio.ImageGen.NanoBanana.EndToEnd",
	HyperAIStudio::ImageGen::NanoBananaTests::Flags)

bool FHyperAIStudioNanoBananaEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ImageGen;
	using namespace HyperAIStudio::ImageGen::NanoBananaTests;
	// Google's endpoint, played by a server on this machine: what reaches it, and a picture back.
	constexpr uint32 Port = 18321;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18321 is taken, so the stand-in for Google's endpoint could not start."));
		return true;
	}
	struct FSeen
	{
		FString Path;
		FString Key;
		FString Body;
		int32 Calls = 0;
	};
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	const FString Picture = PngBase64();
	const FHttpRouteHandle Route = Router->BindRoute(FHttpPath(TEXT("/v1beta/models")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([Seen, Picture](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			++Seen->Calls;
			Seen->Path = Request.RelativePath.GetPath();
			for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
			{
				if (Header.Key.Equals(TEXT("x-goog-api-key"), ESearchCase::IgnoreCase) && !Header.Value.IsEmpty())
				{
					Seen->Key = Header.Value[0];
				}
			}
			FUTF8ToTCHAR Body(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
			Seen->Body = FString(Body.Length(), Body.Get());
			OnComplete(FHttpServerResponse::Create(FString::Printf(
				TEXT(R"({"candidates": [{"content": {"parts": [{"text": "A lighthouse."}, {"inlineData": {"mimeType": "image/png", "data": "%s"}}]}, "finishReason": "STOP"}]})"),
				*Picture), TEXT("application/json")));
			return true;
		}));
	FHttpServerModule::Get().StartAllListeners();

	const FSettingsGuard Guard;
	FHyperAIStudioExternalServiceSettings Local = UHyperAIStudioSettings::GetGeminiService();
	Local.BaseUrl = FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	Local.ApiKeyVariable = TEXT("HYPERAI_TEST_GEMINI_KEY");
	Guard.Settings->ExternalServices = {Local};
	Guard.Settings->ImageModels.Reset();
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_GEMINI_KEY"), TEXT("test-key-4821"));
	FHyperAIStudioExternalServices::ResetSessionSpend();
	const FString BatchId = TEXT("nano_e2e_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	ON_SCOPE_EXIT
	{
		Router->UnbindRoute(Route);
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_GEMINI_KEY"), TEXT(""));
		FHyperAIStudioExternalServices::ResetSessionSpend();
		IFileManager::Get().DeleteDirectory(*(Gate::OutputRootOnDisk() / BatchId), /*RequireExists=*/false, /*Tree=*/true);
	};

	const FHyperAIStudioImageModelSettings Two = Model(TEXT("nano_banana_2"));
	FString Error;
	if (!TestTrue(TEXT("A Nano Banana 2 batch starts: ") + Error,
		Gate::StartBatch(BatchId, Two, FString(), TEXT("A lighthouse on a rocky shore at dusk"), TEXT("text"), 1, 1920, 1080, -1, Error)))
	{
		return false;
	}
	FHyperAIImageBatchInfo Batch;
	const double End = FPlatformTime::Seconds() + 30.0;
	while (FPlatformTime::Seconds() < End && (!Gate::FindBatch(BatchId, Batch) || (Batch.State != TEXT("complete") && Batch.State != TEXT("failed"))))
	{
		FTSTicker::GetCoreTicker().Tick(0.02f);
		FPlatformProcess::Sleep(0.01f);
	}
	AddInfo(FString::Printf(TEXT("Batch %s: %s %s"), *Batch.State, *Batch.Progress, *Batch.Diagnostic));
	AddInfo(TEXT("Google's endpoint saw ") + Seen->Path + TEXT(" with ") + Seen->Body.Left(300));
	TestEqual(TEXT("The batch completes"), Batch.State, FString(TEXT("complete")));
	TestEqual(TEXT("with one call to Google"), Seen->Calls, 1);
	TestEqual(TEXT("at the model's own path"), Seen->Path, FString(TEXT("/gemini-3.1-flash-image:generateContent")));
	TestEqual(TEXT("with the key in Google's header"), Seen->Key, FString(TEXT("test-key-4821")));
	TestTrue(TEXT("asking for 16:9 at 2K"), Seen->Body.Contains(TEXT("\"aspectRatio\": \"16:9\"")) && Seen->Body.Contains(TEXT("\"imageSize\": \"2K\"")));
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Gate::OutputRootOnDisk() / BatchId / TEXT("*.png")), /*Files=*/true, /*Directories=*/false);
	TestEqual(TEXT("The picture lands in the project"), Files.Num(), 1);
	FString Sidecar;
	TestTrue(TEXT("with a sidecar naming the model and its price"), FFileHelper::LoadFileToString(Sidecar, *(Gate::OutputRootOnDisk() / BatchId / (BatchId + TEXT("_01.json"))))
		&& Sidecar.Contains(TEXT("\"model\": \"gemini-3.1-flash-image\"")) && Sidecar.Contains(TEXT("\"cost\": 0.1010")));
	TestEqual(TEXT("and the session's spend counts it at the 2K price"), FHyperAIStudioExternalServices::GetSessionSpend(), 0.101, 1e-4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensNanoBananaTest,
	"HyperAIStudio.Screens.NanoBanana",
	HyperAIStudio::ImageGen::NanoBananaTests::Flags)

bool FHyperAIStudioScreensNanoBananaTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ImageGen::NanoBananaTests;
	const FSettingsGuard Guard;
	Guard.Settings->ImageModels.Reset();
	const TCHAR* Prompt = TEXT("A lighthouse on a rocky shore at dusk, warm sky fading to violet, calm sea, room for a title on the right.");

	// As a user without a key sees it: the models are offered, and the page says what they need.
	Guard.Settings->ExternalServices.Reset();
	const TSharedRef<SHyperAIStudioImagesPanel> Without = SNew(SHyperAIStudioImagesPanel);
	for (const TCHAR* Id : {TEXT("nano_banana"), TEXT("nano_banana_2"), TEXT("nano_banana_2_lite"), TEXT("nano_banana_pro")})
	{
		TestTrue(FString::Printf(TEXT("The Images page offers %s"), Id), Without->SelectModel(Id));
	}
	Without->SetPrompt(Prompt);
	Without->SetCount(2);
	const bool bHasKey = !FPlatformMisc::GetEnvironmentVariable(TEXT("GEMINI_API_KEY")).IsEmpty();
	const FHyperAIImageGenerateReport Refused = Without->PreviewPrompt();
	if (!bHasKey)
	{
		TestEqual(TEXT("Without GEMINI_API_KEY, the dry run says the key is missing"), Refused.Status, FString(TEXT("api_key_missing")));
	}
	HyperAIStudio::Tests::SaveWidgetShot(Without, FIntPoint(1280, 880), TEXT("step16-images-nano-banana-pro"), 8);

	// With a key (here a test one, for a server on this machine that stands in for Google), the dry run prices it.
	constexpr uint32 Port = 18322;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	FHttpServerModule::Get().StartAllListeners();
	FHyperAIStudioExternalServiceSettings Local = UHyperAIStudioSettings::GetGeminiService();
	Local.BaseUrl = FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	Local.ApiKeyVariable = TEXT("HYPERAI_TEST_GEMINI_KEY");
	Guard.Settings->ExternalServices = {Local};
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_GEMINI_KEY"), TEXT("test-key-4821"));
	ON_SCOPE_EXIT { FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_GEMINI_KEY"), TEXT("")); };
	const TSharedRef<SHyperAIStudioImagesPanel> With = SNew(SHyperAIStudioImagesPanel);
	TestTrue(TEXT("Nano Banana Pro can be picked"), With->SelectModel(TEXT("nano_banana_pro")));
	With->SetPrompt(Prompt);
	With->SetCount(2);
	const FHyperAIImageGenerateReport Plan = With->PreviewPrompt();
	AddInfo(TEXT("Dry run: ") + Plan.Status + TEXT(": ") + Plan.Diagnostic.Left(300));
	TestTrue(TEXT("With a key, the dry run plans it"), Plan.bOk && Plan.Status == TEXT("planned"));
	TestEqual(TEXT("at Nano Banana Pro's price, two at 1K"), static_cast<double>(Plan.EstimatedCost), 0.268, 1e-3);
	HyperAIStudio::Tests::SaveWidgetShot(With, FIntPoint(1280, 880), TEXT("step16-images-nano-banana-ready"), 8);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18322 was taken; the stand-in did not answer health checks."));
	}
	return true;
}

#endif
