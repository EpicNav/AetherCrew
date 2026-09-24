// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioImageGenToolset.h"

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentImage.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioImageGenCookbook.h"
#include "HyperAIStudioImageGenGate.h"
#include "HyperAIStudioSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::ImageGen::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FHyperAIImageSlot Slot(const TCHAR* Name, const TCHAR* Value)
	{
		FHyperAIImageSlot Result;
		Result.Name = Name;
		Result.Value = Value;
		return Result;
	}

	TArray<uint8> ToBytes(const FString& Text)
	{
		TArray<uint8> Bytes;
		const FTCHARToUTF8 Utf8(*Text);
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Bytes;
	}

	/** A one-pixel PNG, base64-encoded the way a service returns one. */
	FString PngBase64()
	{
		const TArray<uint8> Png = {
			0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
			0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
			0x89};
		return FBase64::Encode(Png);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageGenCohortTest,
	"HyperAIStudio.ImageGen.Contracts.ExactTwoToolCohort",
	HyperAIStudio::ImageGen::Tests::Flags)

bool FHyperAIStudioImageGenCohortTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Names = FHyperAIStudioImageGenContracts::GetToolNames();
	TestEqual(TEXT("two tools"), Names.Num(), 2);
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = FHyperAIStudioImageGenContracts::GetAdapterDescriptor();
	TestEqual(TEXT("one variant per tool"), Descriptor.Variants.Num(), Names.Num());
	int32 Callables = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioImageGenToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->HasMetaData(TEXT("AICallable"))) continue;
		++Callables;
		TestTrue(*FString::Printf(TEXT("%s is a manifest tool"), *It->GetName()), Names.Contains(It->GetName()));
		const FString ToolTip = It->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description within 400 characters"), *It->GetName()), !ToolTip.IsEmpty() && ToolTip.Len() <= 400);
	}
	TestEqual(TEXT("AICallable functions match the manifest"), Callables, Names.Num());

	TestTrue(TEXT("the cookbook is in the project"), HyperAIStudio::ImageGen::EnsureCookbook());
	FString Cookbook;
	FFileHelper::LoadFileToString(Cookbook, *HyperAIStudio::ImageGen::GetCookbookPath());
	TestTrue(TEXT("and covers configuring a model, the base64 rule and what a reference is not"),
		Cookbook.Contains(TEXT("response_image_path")) && Cookbook.Contains(TEXT("b64_json"))
			&& Cookbook.Contains(TEXT("not a plan")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageGenSpendingNeedsAClickTest,
	"HyperAIStudio.ImageGen.Contracts.SpendingNeedsAClick",
	HyperAIStudio::ImageGen::Tests::Flags)

bool FHyperAIStudioImageGenSpendingNeedsAClickTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// The whole consent model rests on this pair: generate is an external effect, and an external effect
	// always needs approval whatever the agent-edit setting says. A safety class demoted to Edit here would
	// silently let an agent spend money, and nothing else in the pack would notice.
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = FHyperAIStudioImageGenContracts::GetAdapterDescriptor();
	const FHyperAIStudioDomainVariantDescriptor* Generate = Descriptor.Variants.FindByPredicate(
		[](const FHyperAIStudioDomainVariantDescriptor& Variant)
		{
			return Variant.ToolName == FHyperAIStudioImageGenContracts::GenerateToolName;
		});
	if (!TestNotNull(TEXT("generate is a variant"), Generate)) return false;
	TestTrue(TEXT("classed as an external effect"), Generate->Safety == EHyperAIStudioDomainSafety::ExternalEffect);
	TestTrue(TEXT("which always requires approval"),
		FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::ExternalEffect));

	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!TestNotNull(TEXT("settings exist"), Settings)) return false;
	const bool bRestore = Settings->bRequireApprovalForAgentEdits;
	ON_SCOPE_EXIT { Settings->bRequireApprovalForAgentEdits = bRestore; };
	Settings->bRequireApprovalForAgentEdits = false;
	TestTrue(TEXT("even with agent edits trusted"),
		FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::ExternalEffect));
	TestFalse(TEXT("while a plain edit follows the setting"),
		FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageGenPromptTest,
	"HyperAIStudio.ImageGen.Prompt.TemplatesDropWhatIsUnfilled",
	HyperAIStudio::ImageGen::Tests::Flags)

bool FHyperAIStudioImageGenPromptTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::ImageGen::Tests;
	namespace Gate = HyperAIStudio::ImageGen::Gate;

	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!TestNotNull(TEXT("settings exist"), Settings)) return false;
	const TArray<FHyperAIStudioImagePromptTemplateSettings> Restore = Settings->ImagePromptTemplates;
	ON_SCOPE_EXIT { Settings->ImagePromptTemplates = Restore; };
	Settings->ImagePromptTemplates.Reset();
	TestTrue(TEXT("an empty list is seeded"), Settings->EnsureDefaultImagePromptTemplates());
	TestFalse(TEXT("and seeding does not repeat"), Settings->EnsureDefaultImagePromptTemplates());

	const TArray<FHyperAIImageTemplateInfo> Templates = Gate::ReadTemplates();
	const FHyperAIImageTemplateInfo* Scene = Templates.FindByPredicate(
		[](const FHyperAIImageTemplateInfo& Info) { return Info.Id == TEXT("scene_reference"); });
	if (!TestNotNull(TEXT("the scene reference template is seeded"), Scene)) return false;
	for (const TCHAR* Name : {TEXT("subject"), TEXT("key_light"), TEXT("contrast"), TEXT("palette")})
	{
		TestTrue(*FString::Printf(TEXT("it takes a %s slot, which the lighting compare measures"), Name),
			Scene->Slots.Contains(Name));
	}

	FString Prompt;
	TArray<FString> Unfilled;
	FString Error;
	const TArray<FHyperAIImageSlot> Full = {
		Slot(TEXT("subject"), TEXT("a flooded marble subway platform")),
		Slot(TEXT("time_of_day"), TEXT("an hour before sunrise")),
		Slot(TEXT("weather"), TEXT("still air, thin mist")),
		Slot(TEXT("key_light"), TEXT("low from the left")),
		Slot(TEXT("contrast"), TEXT("high")),
		Slot(TEXT("palette"), TEXT("amber and teal")),
		Slot(TEXT("mood"), TEXT("abandoned")),
		Slot(TEXT("lens"), TEXT("35mm"))};
	if (!TestTrue(TEXT("a filled template composes"), Gate::ComposePrompt(TEXT("scene_reference"), Full, FString(), Prompt, Unfilled, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("with nothing unfilled"), Unfilled.IsEmpty());
	TestTrue(TEXT("and every value in it"), Prompt.Contains(TEXT("flooded marble")) && Prompt.Contains(TEXT("low from the left")));
	TestFalse(TEXT("and no leftover placeholder"), Prompt.Contains(TEXT("{")));

	// A half-answered template is the normal case, not an error: the clause goes, not the prompt.
	const TArray<FHyperAIImageSlot> Partial = {
		Slot(TEXT("subject"), TEXT("a flooded marble subway platform")),
		Slot(TEXT("contrast"), TEXT("high")),
		Slot(TEXT("palette"), TEXT("amber and teal"))};
	if (!TestTrue(TEXT("a half-filled template composes"), Gate::ComposePrompt(TEXT("scene_reference"), Partial, FString(), Prompt, Unfilled, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("leaving no placeholder behind"), Prompt.Contains(TEXT("{")));
	TestTrue(TEXT("reporting what was dropped"), Unfilled.Contains(TEXT("mood")) && Unfilled.Contains(TEXT("key_light")));
	TestTrue(TEXT("keeping the filled clauses"), Prompt.Contains(TEXT("high contrast")) && Prompt.Contains(TEXT("amber and teal palette")));
	TestFalse(TEXT("dropping the clause whose slot was empty"), Prompt.Contains(TEXT(" mood")));
	TestFalse(TEXT("and the sentence that held nothing else"), Prompt.Contains(TEXT("Key light")));
	TestTrue(TEXT("while the unslotted tail survives"), Prompt.Contains(TEXT("no watermark")));

	TestTrue(TEXT("extra prompt text is appended"),
		Gate::ComposePrompt(TEXT("scene_reference"), Partial, TEXT("Wet stone."), Prompt, Unfilled, Error)
			&& Prompt.EndsWith(TEXT("Wet stone.")));
	TestTrue(TEXT("and with no template it is the whole prompt"),
		Gate::ComposePrompt(FString(), {}, TEXT("Just this."), Prompt, Unfilled, Error) && Prompt == TEXT("Just this."));
	TestFalse(TEXT("an empty composition is refused"), Gate::ComposePrompt(FString(), {}, FString(), Prompt, Unfilled, Error));
	TestFalse(TEXT("an unknown template is refused"),
		Gate::ComposePrompt(TEXT("no_such_template"), Full, FString(), Prompt, Unfilled, Error));
	TestFalse(TEXT("a slot name that is not an identifier is refused"),
		Gate::ComposePrompt(TEXT("scene_reference"), {Slot(TEXT("sub ject"), TEXT("x"))}, FString(), Prompt, Unfilled, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageGenRequestBodyTest,
	"HyperAIStudio.ImageGen.Request.TemplatesSubstituteAndEscape",
	HyperAIStudio::ImageGen::Tests::Flags)

bool FHyperAIStudioImageGenRequestBodyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Gate = HyperAIStudio::ImageGen::Gate;

	FHyperAIStudioImageModelSettings Model;
	Model.Id = TEXT("test_model");
	Model.Model = TEXT("some-image-model");
	Model.RequestTemplate = TEXT("{\"model\": \"{model}\", \"prompt\": \"{prompt}\", \"size\": \"{width}x{height}\", \"seed\": {seed}}");
	FString Body;
	FString Error;
	// A prompt is user text going into a JSON string; a bare quote in it would end the field early and the
	// service would reject a body nobody could explain. It is escaped on the way in.
	const FString Prompt = TEXT("a \"grand\" hall\\atrium\nwith light");
	if (!TestTrue(TEXT("a body is built"), Gate::BuildRequestBody(Model, Prompt, FString(), 1024, 768, 42, Body, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("with the model substituted"), Body.Contains(TEXT("some-image-model")));
	TestTrue(TEXT("the size substituted outside quotes too"), Body.Contains(TEXT("1024x768")));
	TestTrue(TEXT("the seed substituted as a number"), Body.Contains(TEXT("\"seed\": 42")));
	TestTrue(TEXT("the quote escaped"), Body.Contains(TEXT("\\\"grand\\\"")));
	TestTrue(TEXT("the backslash escaped"), Body.Contains(TEXT("hall\\\\atrium")));
	TestFalse(TEXT("and no raw newline left in the body"), Body.Contains(TEXT("\n")));

	Model.RequestTemplate = TEXT("{\"prompt\": {prompt}}");
	TestFalse(TEXT("a template that cannot produce valid JSON is refused"),
		Gate::BuildRequestBody(Model, TEXT("a hall"), FString(), 1024, 768, -1, Body, Error));
	TestTrue(TEXT("naming the setting to fix"), Error.Contains(TEXT("request_template")));
	Model.RequestTemplate.Reset();
	TestFalse(TEXT("and an empty template is refused"),
		Gate::BuildRequestBody(Model, TEXT("a hall"), FString(), 1024, 768, -1, Body, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageGenResponseTest,
	"HyperAIStudio.ImageGen.Response.DottedPathsAndBase64",
	HyperAIStudio::ImageGen::Tests::Flags)

bool FHyperAIStudioImageGenResponseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::ImageGen::Tests;
	namespace Gate = HyperAIStudio::ImageGen::Gate;

	const FString Encoded = PngBase64();
	TArray<uint8> Bytes;
	FString Error;

	const FString OpenAIShaped = FString::Printf(TEXT("{\"data\": [{\"b64_json\": \"%s\"}]}"), *Encoded);
	if (!TestTrue(TEXT("an OpenAI-shaped reply yields the image"),
		Gate::ExtractImageBytes(ToBytes(OpenAIShaped), TEXT("data.0.b64_json"), Bytes, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("sniffed as a png"), Gate::SniffImageExtension(Bytes), FString(TEXT("png")));

	const FString A1111Shaped = FString::Printf(TEXT("{\"images\": [\"%s\"]}"), *Encoded);
	TestTrue(TEXT("an Automatic1111-shaped reply yields the image"),
		Gate::ExtractImageBytes(ToBytes(A1111Shaped), TEXT("images.0"), Bytes, Error));
	const FString GeminiShaped = FString::Printf(
		TEXT("{\"candidates\": [{\"content\": {\"parts\": [{\"inlineData\": {\"data\": \"%s\"}}]}}]}"), *Encoded);
	TestTrue(TEXT("a Gemini-shaped reply yields the image"),
		Gate::ExtractImageBytes(ToBytes(GeminiShaped), TEXT("candidates.0.content.parts.0.inlineData.data"), Bytes, Error));
	TestTrue(TEXT("a data uri has its prefix stripped"),
		Gate::ExtractImageBytes(ToBytes(FString::Printf(TEXT("{\"images\": [\"data:image/png;base64,%s\"]}"), *Encoded)),
			TEXT("images.0"), Bytes, Error));

	// A link is the default for several services and the one failure worth naming precisely, because the fix
	// is a setting rather than anything wrong with the prompt.
	TestFalse(TEXT("a link is refused rather than fetched"),
		Gate::ExtractImageBytes(ToBytes(TEXT("{\"data\": [{\"url\": \"https://example.invalid/a.png\"}]}")),
			TEXT("data.0.url"), Bytes, Error));
	TestTrue(TEXT("saying how to ask for base64"), Error.Contains(TEXT("b64_json")));

	TestFalse(TEXT("a path that hits nothing is refused"),
		Gate::ExtractImageBytes(ToBytes(OpenAIShaped), TEXT("data.3.b64_json"), Bytes, Error));
	TestFalse(TEXT("a reply that is not JSON is refused"),
		Gate::ExtractImageBytes(ToBytes(TEXT("502 Bad Gateway")), TEXT("data.0.b64_json"), Bytes, Error));
	TestFalse(TEXT("and a model with no response path is refused"),
		Gate::ExtractImageBytes(ToBytes(OpenAIShaped), FString(), Bytes, Error));

	TestEqual(TEXT("a jpeg is sniffed"), Gate::SniffImageExtension({0xFF, 0xD8, 0xFF, 0xE0}), FString(TEXT("jpg")));
	TestEqual(TEXT("a webp is sniffed"),
		Gate::SniffImageExtension({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'}), FString(TEXT("webp")));
	TestTrue(TEXT("and anything else is not an image"),
		Gate::SniffImageExtension(ToBytes(TEXT("<html>error</html>"))).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageGenFrontDoorTest,
	"HyperAIStudio.ImageGen.FrontDoor.RefusesBeforeSpending",
	HyperAIStudio::ImageGen::Tests::Flags)

bool FHyperAIStudioImageGenFrontDoorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::ImageGen::Tests;

	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	if (!TestNotNull(TEXT("settings exist"), Settings)) return false;
	const TArray<FHyperAIStudioImageModelSettings> RestoreModels = Settings->ImageModels;
	const TArray<FHyperAIStudioExternalServiceSettings> RestoreServices = Settings->ExternalServices;
	ON_SCOPE_EXIT
	{
		Settings->ImageModels = RestoreModels;
		Settings->ExternalServices = RestoreServices;
	};

	auto Generate = [](const TFunctionRef<void(FHyperAIImageGenerateRequest&)> Configure)
	{
		FHyperAIImageGenerateRequest Request;
		Request.ModelId = TEXT("test_model");
		Request.BatchName = TEXT("test_batch");
		Request.Prompt = TEXT("a flooded marble subway platform");
		Request.Count = 1;
		Configure(Request);
		return UHyperAIStudioImageGenToolset::hyper_image_generate(Request);
	};

	// Both lists, so the test reads the same whatever the project has configured.
	Settings->ImageModels.Reset();
	Settings->ExternalServices.Reset();
	FHyperAIImageGenerateReport Report = Generate([](FHyperAIImageGenerateRequest&) {});
	TestFalse(TEXT("with nothing configured, generating is refused"), Report.bOk);
	TestEqual(TEXT("as a missing model"), Report.Status, FString(TEXT("model_not_found")));
	TestTrue(TEXT("pointing at the cookbook rather than looking broken"), Report.Diagnostic.Contains(TEXT("Cookbook")));

	FHyperAIStudioImageModelSettings& Model = Settings->ImageModels.AddDefaulted_GetRef();
	Model.Id = TEXT("test_model");
	Model.ServiceId = TEXT("test_service");
	Model.Path = TEXT("/v1/images/generations");
	Model.RequestTemplate = TEXT("{\"prompt\": \"{prompt}\", \"size\": \"{width}x{height}\"}");
	Model.ResponseImagePath = TEXT("data.0.b64_json");
	Model.Width = 1024;
	Model.Height = 1024;

	Report = Generate([](FHyperAIImageGenerateRequest&) {});
	TestFalse(TEXT("a model naming no service is refused"), Report.bOk);
	TestEqual(TEXT("as a missing service"), Report.Status, FString(TEXT("service_not_found")));

	FHyperAIStudioExternalServiceSettings& Service = Settings->ExternalServices.AddDefaulted_GetRef();
	Service.Id = TEXT("test_service");
	Service.BaseUrl = TEXT("http://127.0.0.1:59999");
	Service.ApiKeyVariable = TEXT("HYPERAI_TEST_KEY_THAT_IS_NOT_SET");
	Service.CostPerCall = 0.04f;
	Report = Generate([](FHyperAIImageGenerateRequest&) {});
	TestFalse(TEXT("a service whose key variable is empty is refused"), Report.bOk);
	TestEqual(TEXT("as a missing key"), Report.Status, FString(TEXT("api_key_missing")));
	TestTrue(TEXT("naming the variable to set"), Report.Diagnostic.Contains(TEXT("HYPERAI_TEST_KEY_THAT_IS_NOT_SET")));
	Service.ApiKeyVariable.Reset();

	// Everything below is refused on the request alone, before the service is ever consulted.
	TestEqual(TEXT("a dry run carrying submission fields is refused"),
		Generate([](FHyperAIImageGenerateRequest& Request) { Request.OperationId = TEXT("op-1"); }).Status,
		FString(TEXT("unexpected_submission_fields")));
	TestEqual(TEXT("a submission with no operation id is refused"),
		Generate([](FHyperAIImageGenerateRequest& Request) { Request.bDryRun = false; }).Status,
		FString(TEXT("invalid_submission_fields")));
	TestEqual(TEXT("a batch name that is not a folder-safe identifier is refused"),
		Generate([](FHyperAIImageGenerateRequest& Request) { Request.BatchName = TEXT("../escape"); }).Status,
		FString(TEXT("invalid_batch_name")));
	TestEqual(TEXT("an empty batch name is refused"),
		Generate([](FHyperAIImageGenerateRequest& Request) { Request.BatchName.Reset(); }).Status,
		FString(TEXT("invalid_batch_name")));
	TestEqual(TEXT("a count past the batch limit is refused"),
		Generate([](FHyperAIImageGenerateRequest& Request) { Request.Count = 99; }).Status,
		FString(TEXT("invalid_count")));
	TestEqual(TEXT("a count of nothing is refused"),
		Generate([](FHyperAIImageGenerateRequest& Request) { Request.Count = 0; }).Status,
		FString(TEXT("invalid_count")));
	TestEqual(TEXT("an output bound outside the closed range is refused"),
		Generate([](FHyperAIImageGenerateRequest& Request) { Request.MaxOutputBytes = 8; }).Status,
		FString(TEXT("invalid_bounds")));
	TestEqual(TEXT("a size outside the closed range is refused"),
		Generate([](FHyperAIImageGenerateRequest& Request) { Request.Width = 16; }).Status,
		FString(TEXT("invalid_size")));
	TestEqual(TEXT("and a prompt with nothing in it is refused"),
		Generate([](FHyperAIImageGenerateRequest& Request) { Request.Prompt.Reset(); }).Status,
		FString(TEXT("invalid_prompt")));

	FHyperAIImageInspectRequest Inspect;
	const FHyperAIImageInspectReport Seen = UHyperAIStudioImageGenToolset::hyper_image_inspect(Inspect);
	TestTrue(TEXT("inspect works with a model configured"), Seen.bOk);
	TestTrue(TEXT("reporting it"), Seen.Models.ContainsByPredicate(
		[](const FHyperAIImageModelInfo& Info) { return Info.Id == TEXT("test_model"); }));
	TestFalse(TEXT("and naming where images land"), Seen.OutputFolder.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageGenAgentModelsTest,
	"HyperAIStudio.ImageGen.AgentModels.CodexDrawsForAgentsWithout",
	HyperAIStudio::ImageGen::Tests::Flags)

bool FHyperAIStudioImageGenAgentModelsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::ImageGen;

	FHyperAIImageInspectRequest Inspect;
	Inspect.bIncludeGenerated = false;
	const FHyperAIImageInspectReport Seen = UHyperAIStudioImageGenToolset::hyper_image_inspect(Inspect);
	const FHyperAIImageModelInfo* Codex = Seen.Models.FindByPredicate([](const FHyperAIImageModelInfo& Info) { return Info.Id == TEXT("codex_image_gen"); });
	const FHyperAIImageModelInfo* Gemini = Seen.Models.FindByPredicate([](const FHyperAIImageModelInfo& Info) { return Info.Id == TEXT("gemini_image_gen"); });
	if (!TestTrue(TEXT("Both agent generators are listed with nothing configured"), Codex && Gemini))
	{
		return false;
	}
	FString Issue;
	const bool bCodexDraws = FHyperAIStudioAgentImage::CanDraw(TEXT("codex"), Issue);
	TestEqual(TEXT("Codex's health says whether this machine can draw with it"), Codex->Health, FString(bCodexDraws ? TEXT("ready") : TEXT("unavailable")));
	TestEqual(TEXT("It costs nothing here: it spends the user's own plan"), Codex->CostPerCall, 0.0f);

	FHyperAIImageGenerateRequest Request;
	Request.ModelId = TEXT("codex_image_gen");
	Request.BatchName = TEXT("agent_plan_check");
	Request.Prompt = TEXT("a lighthouse at dusk");
	const FHyperAIImageGenerateReport Plan = UHyperAIStudioImageGenToolset::hyper_image_generate(Request);
	if (bCodexDraws)
	{
		TestTrue(TEXT("A dry run with Codex plans"), Plan.bOk && Plan.Status == TEXT("planned"));
		TestEqual(TEXT("costing nothing here"), Plan.EstimatedCost, 0.0f);
		TestTrue(TEXT("and says whose plan it spends"), Plan.Diagnostic.Contains(TEXT("your own Codex plan")));
	}
	else
	{
		TestEqual(TEXT("Without Codex the dry run says so"), Plan.Status, FString(TEXT("agent_unavailable")));
	}
	if (!FHyperAIStudioAgentImage::CanDraw(TEXT("gemini"), Issue))
	{
		Request.ModelId = TEXT("gemini_image_gen");
		const FHyperAIImageGenerateReport GeminiPlan = UHyperAIStudioImageGenToolset::hyper_image_generate(Request);
		TestEqual(TEXT("Gemini without its image extension is refused before anything runs"), GeminiPlan.Status, FString(TEXT("agent_unavailable")));
		TestTrue(TEXT("pointing at codex_image_gen"), GeminiPlan.Diagnostic.Contains(TEXT("codex_image_gen")) || GeminiPlan.Diagnostic.Contains(TEXT("not installed")));
	}

	// A batch drawn by a stand-in, straight through the gate; the approval in front of it is SpendingNeedsAClick's business.
	int32 Code = -1;
	FPlatformProcess::ExecProcess(TEXT("python"), TEXT("--version"), &Code, nullptr, nullptr);
	if (Code != 0)
	{
		AddWarning(TEXT("python is not on PATH, so the Codex stand-in cannot draw."));
		return true;
	}
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()) / (TEXT("ImageGenAgent-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString BatchId = TEXT("agent_test_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	ON_SCOPE_EXIT
	{
		FHyperAIStudioAgentImage::SetCommandForTests(FString());
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
		IFileManager::Get().DeleteDirectory(*(Gate::OutputRootOnDisk() / BatchId), false, true);
	};
	const FString Script = Dir / TEXT("fake_codex.py");
	FFileHelper::SaveStringToFile(TEXT(R"PY(import json, sys, os, zlib, struct
HERE = os.path.dirname(os.path.abspath(__file__))
def png(path):
    raw = b"".join(b"\x00" + bytes((30, 90, 200)) * 6 for _ in range(4))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 6, 4, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
for line in iter(sys.stdin.readline, ""):
    m = json.loads(line); method = m.get("method"); i = m.get("id")
    if method == "initialize":
        send({"id": i, "result": {}})
    elif method == "thread/start":
        send({"id": i, "result": {"thread": {"id": "t"}}} if m["params"].get("ephemeral") else {"id": i, "error": {"message": "not headless"}})
    elif method == "turn/start":
        saved = os.path.join(HERE, "drawn_%d.png" % len(os.listdir(HERE))); png(saved)
        send({"id": i, "result": {"turn": {"id": "u"}}})
        send({"method": "item/completed", "params": {"item": {"type": "imageGeneration", "id": "g", "status": "completed", "revisedPrompt": "r", "result": "", "savedPath": saved}}})
        send({"method": "turn/completed", "params": {"turn": {"id": "u", "status": "completed"}}})
)PY"), *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FHyperAIStudioAgentImage::SetCommandForTests(FString::Printf(TEXT("python -u \"%s\""), *Script));

	FHyperAIStudioImageModelSettings Model;
	if (!TestTrue(TEXT("codex_image_gen resolves without settings"), Gate::FindModel(TEXT("codex_image_gen"), Model)))
	{
		return false;
	}
	FString Error;
	if (!TestTrue(TEXT("A two-picture batch starts"), Gate::StartBatch(BatchId, Model, FString(), TEXT("a red cube"), TEXT("text"), 2, 1024, 768, -1, Error)))
	{
		AddError(Error);
		return false;
	}
	FHyperAIImageBatchInfo Info;
	const double End = FPlatformTime::Seconds() + 60.0;
	while (Gate::FindBatch(BatchId, Info) && Info.State == TEXT("running") && FPlatformTime::Seconds() < End)
	{
		FTSTicker::GetCoreTicker().Tick(0.05f);
		FPlatformProcess::Sleep(0.02f);
	}
	TestEqual(TEXT("The batch completes"), Info.State, FString(TEXT("complete")));
	if (!Info.Diagnostic.IsEmpty())
	{
		AddInfo(TEXT("Batch said: ") + Info.Diagnostic);
	}
	const TArray<FHyperAIImageRecord> Records = Gate::ReadGenerated(BatchId, 8);
	TestEqual(TEXT("Both pictures landed in the project with sidecars"), Records.Num(), 2);
	for (const FHyperAIImageRecord& Record : Records)
	{
		TestTrue(TEXT("each one Codex's, readable by the Preview tab and the compare tools"), Record.ModelId == TEXT("codex_image_gen")
			&& Record.Prompt == TEXT("a red cube") && IFileManager::Get().FileExists(*(FPaths::ProjectDir() / Record.File)));
	}
	return true;
}

#endif
