// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioSettings.h"

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::Services::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FHyperAIStudioExternalServiceSettings Paid(const TCHAR* Id, const float CostPerCall, const TCHAR* KeyVariable = TEXT(""))
	{
		FHyperAIStudioExternalServiceSettings Service;
		Service.Id = Id;
		Service.Label = Id;
		Service.BaseUrl = TEXT("http://127.0.0.1:9");
		Service.ApiKeyVariable = KeyVariable;
		Service.CostPerCall = CostPerCall;
		return Service;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioServiceDefaultsTest,
	"HyperAIStudio.Services.DefaultsAndLookup",
	HyperAIStudio::Services::Tests::Flags)

bool FHyperAIStudioServiceDefaultsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Services::Tests;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioExternalServiceSettings> Restore = Settings->ExternalServices;
	ON_SCOPE_EXIT { Settings->ExternalServices = Restore; };

	// Built in: Google's Gemini API for the Nano Banana models, Tripo, Meshy and Hi3D for 3D models, and ElevenLabs and
	// Stability for audio. Keys come from the environment.
	Settings->ExternalServices.Reset();
	const TArray<FHyperAIStudioExternalServiceSettings> Unconfigured = FHyperAIStudioExternalServices::GetServices();
	TestTrue(TEXT("an unconfigured project has the Gemini API, the 3D services and the audio services"), Unconfigured.Num() == 7
		&& Unconfigured[0].Id == TEXT("google_gemini") && Unconfigured[1].Id == TEXT("tripo") && Unconfigured[2].Id == TEXT("meshy")
		&& Unconfigured[3].Id == TEXT("hi3d") && Unconfigured[4].Id == TEXT("elevenlabs") && Unconfigured[5].Id == TEXT("elevenlabs_music")
		&& Unconfigured[6].Id == TEXT("stability"));
	FHyperAIStudioExternalServiceSettings ElevenLabs;
	TestTrue(TEXT("ElevenLabs takes its key in xi-api-key, with no prefix"), FHyperAIStudioExternalServices::Find(TEXT("elevenlabs"), ElevenLabs)
		&& ElevenLabs.ApiKeyHeader == TEXT("xi-api-key") && ElevenLabs.ApiKeyPrefix.IsEmpty() && ElevenLabs.ApiKeyVariable == TEXT("ELEVENLABS_API_KEY"));
	FHyperAIStudioExternalServiceSettings Gemini;
	TestTrue(TEXT("which is found by id"), FHyperAIStudioExternalServices::Find(TEXT("google_gemini"), Gemini));
	TestTrue(TEXT("with Google's key header and variable"), Gemini.ApiKeyHeader == TEXT("x-goog-api-key") && Gemini.ApiKeyPrefix.IsEmpty()
		&& Gemini.ApiKeyVariable == TEXT("GEMINI_API_KEY") && Gemini.BaseUrl == TEXT("https://generativelanguage.googleapis.com"));
	FHyperAIStudioExternalServiceSettings Proxy = Gemini;
	Proxy.BaseUrl = TEXT("http://127.0.0.1:8123");
	Settings->ExternalServices = {Proxy};
	TestTrue(TEXT("and a project's own entry with its id replaces it"), FHyperAIStudioExternalServices::GetServices().Num() == 7
		&& FHyperAIStudioExternalServices::Find(TEXT("google_gemini"), Gemini) && Gemini.BaseUrl == Proxy.BaseUrl);

	Settings->ExternalServices = {Paid(TEXT("local_model"), 0.0f)};
	FHyperAIStudioExternalServiceSettings Local;
	if (!TestTrue(TEXT("a configured service is found by id"), FHyperAIStudioExternalServices::Find(TEXT("local_model"), Local)))
	{
		return false;
	}
	TestTrue(TEXT("with its url"), Local.BaseUrl.Contains(TEXT("127.0.0.1")));
	TestTrue(TEXT("and needing no key when it names no variable"), FHyperAIStudioExternalServices::HasApiKey(Local));
	TestTrue(TEXT("found whatever the case"), FHyperAIStudioExternalServices::Find(TEXT("LOCAL_MODEL"), Local));

	FHyperAIStudioExternalServiceSettings Missing;
	TestFalse(TEXT("an unknown id is not found"), FHyperAIStudioExternalServices::Find(TEXT("nope"), Missing));
	TestEqual(TEXT("and its health says so"),
		FHyperAIStudioExternalServices::QueryHealth(TEXT("nope")).Health, EHyperAIStudioServiceHealth::Unreachable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioServiceKeysTest,
	"HyperAIStudio.Services.KeysComeFromTheEnvironment",
	HyperAIStudio::Services::Tests::Flags)

bool FHyperAIStudioServiceKeysTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Services::Tests;
	// A key in project config would be committed, so a service names the variable and never the key.
	const FHyperAIStudioExternalServiceSettings NeedsKey = Paid(TEXT("keyed"), 0.0f, TEXT("HYPERAI_TEST_KEY_ABSENT"));
	TestFalse(TEXT("a service with an empty variable has no key"), FHyperAIStudioExternalServices::HasApiKey(NeedsKey));

	const FHyperAIStudioExternalServiceSettings NoKeyNeeded = Paid(TEXT("open"), 0.0f);
	TestTrue(TEXT("a service that needs none always has one"), FHyperAIStudioExternalServices::HasApiKey(NoKeyNeeded));

	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_KEY_PRESENT"), TEXT("secret-value"));
	ON_SCOPE_EXIT { FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_KEY_PRESENT"), TEXT("")); };
	FHyperAIStudioExternalServiceSettings HasKey = Paid(TEXT("keyed"), 0.0f, TEXT("HYPERAI_TEST_KEY_PRESENT"));
	TestTrue(TEXT("a set variable counts as a key"), FHyperAIStudioExternalServices::HasApiKey(HasKey));

	// Whatever a service reports about itself must never carry the key it was given.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioExternalServiceSettings> Restore = Settings->ExternalServices;
	ON_SCOPE_EXIT { Settings->ExternalServices = Restore; };
	HasKey.BaseUrl.Reset();
	Settings->ExternalServices = {HasKey};
	const FHyperAIStudioServiceStatus Status = FHyperAIStudioExternalServices::QueryHealth(TEXT("keyed"));
	TestFalse(TEXT("and no status carries the key value"), Status.Diagnostic.Contains(TEXT("secret-value")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioServiceSpendTest,
	"HyperAIStudio.Services.SpendIsCappedPerSession",
	HyperAIStudio::Services::Tests::Flags)

bool FHyperAIStudioServiceSpendTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Services::Tests;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioExternalServiceSettings> RestoreServices = Settings->ExternalServices;
	const float RestoreCap = Settings->ExternalServiceSessionSpendCap;
	ON_SCOPE_EXIT
	{
		Settings->ExternalServices = RestoreServices;
		Settings->ExternalServiceSessionSpendCap = RestoreCap;
		FHyperAIStudioExternalServices::ResetSessionSpend();
	};

	Settings->ExternalServices = {Paid(TEXT("free_local"), 0.0f), Paid(TEXT("paid_api"), 0.10f)};
	Settings->ExternalServiceSessionSpendCap = 1.0f;
	FHyperAIStudioExternalServices::ResetSessionSpend();

	double Cost = -1.0;
	FString Error;
	TestTrue(TEXT("a local service reserves freely"), FHyperAIStudioExternalServices::Reserve(TEXT("free_local"), 500, Cost, Error));
	TestEqual(TEXT("at no cost"), Cost, 0.0);

	TestTrue(TEXT("a paid call inside the cap reserves"), FHyperAIStudioExternalServices::Reserve(TEXT("paid_api"), 5, Cost, Error));
	TestEqual(TEXT("with the cost stated up front"), Cost, 0.5);

	TestFalse(TEXT("a batch past the cap is refused"), FHyperAIStudioExternalServices::Reserve(TEXT("paid_api"), 50, Cost, Error));
	TestTrue(TEXT("naming the cap and the setting that raises it"),
		Error.Contains(TEXT("cap")) && Error.Contains(TEXT("Spend Cap")));
	TestEqual(TEXT("and it spent nothing"), FHyperAIStudioExternalServices::GetSessionSpend(), 0.0);

	// Reserving is a plan; the cap follows what actually went out.
	FHyperAIStudioExternalServices::RecordSpend(TEXT("paid_api"), 8);
	TestEqual(TEXT("recorded spend accumulates"), FHyperAIStudioExternalServices::GetSessionSpend(), 0.8);
	FHyperAIStudioExternalServices::RecordSpend(TEXT("free_local"), 100);
	TestEqual(TEXT("a local service never adds to it"), FHyperAIStudioExternalServices::GetSessionSpend(), 0.8);

	TestFalse(TEXT("what is left is what can still be reserved"),
		FHyperAIStudioExternalServices::Reserve(TEXT("paid_api"), 3, Cost, Error));
	TestTrue(TEXT("a call that fits still reserves"), FHyperAIStudioExternalServices::Reserve(TEXT("paid_api"), 1, Cost, Error));

	Settings->ExternalServiceSessionSpendCap = 0.0f;
	TestFalse(TEXT("a zero cap refuses every paid call"), FHyperAIStudioExternalServices::Reserve(TEXT("paid_api"), 1, Cost, Error));
	TestTrue(TEXT("while local work is unaffected"), FHyperAIStudioExternalServices::Reserve(TEXT("free_local"), 1, Cost, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioServiceCallsTest,
	"HyperAIStudio.Services.CallsRefuseBeforeSending",
	HyperAIStudio::Services::Tests::Flags)

bool FHyperAIStudioServiceCallsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Services::Tests;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioExternalServiceSettings> Restore = Settings->ExternalServices;
	ON_SCOPE_EXIT
	{
		Settings->ExternalServices = Restore;
		FHyperAIStudioExternalServices::ResetSessionSpend();
	};

	FHyperAIStudioExternalServiceSettings NoUrl = Paid(TEXT("no_url"), 0.0f);
	NoUrl.BaseUrl.Reset();
	Settings->ExternalServices = {NoUrl, Paid(TEXT("needs_key"), 0.0f, TEXT("HYPERAI_TEST_KEY_ABSENT")), Paid(TEXT("ok"), 0.0f)};

	// A call that cannot succeed is refused before anything is sent, so the callback never runs.
	const auto NeverRuns = [this](bool, const TArray<uint8>&, const TMap<FString, FString>&, const FString&)
	{
		AddError(TEXT("A refused request must not call back."));
	};
	FString Error;
	TestFalse(TEXT("an unknown service is refused"),
		FHyperAIStudioExternalServices::PostJson(TEXT("nope"), TEXT("/x"), TEXT("{}"), NeverRuns, Error));
	TestTrue(TEXT("by name"), Error.Contains(TEXT("nope")));
	TestFalse(TEXT("a service with no url is refused"),
		FHyperAIStudioExternalServices::PostJson(TEXT("no_url"), TEXT("/x"), TEXT("{}"), NeverRuns, Error));
	TestFalse(TEXT("a missing key is refused, naming the variable"),
		FHyperAIStudioExternalServices::PostJson(TEXT("needs_key"), TEXT("/x"), TEXT("{}"), NeverRuns, Error));
	TestTrue(TEXT("so the fix is obvious"), Error.Contains(TEXT("HYPERAI_TEST_KEY_ABSENT")));
	TestFalse(TEXT("an empty body is refused"),
		FHyperAIStudioExternalServices::PostJson(TEXT("ok"), TEXT("/x"), FString(), NeverRuns, Error));
	return true;
}

#endif
