// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAudioGenToolset.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioSettings.h"
#include "IHttpRouter.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundWave.h"

namespace HyperAIStudio::AudioGenTests
{
	using C = FHyperAIStudioAudioGenContracts;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** The fixture every stand-in sends: a quarter second of a mono 16-bit sine at 22.05 kHz. */
	TArray<uint8> TinyWav()
	{
		constexpr int32 SampleRate = 22050;
		constexpr int32 Frames = SampleRate / 4;
		TArray<uint8> Bytes;
		auto U32 = [&Bytes](const uint32 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 4); };
		auto U16 = [&Bytes](const uint16 V) { Bytes.Append(reinterpret_cast<const uint8*>(&V), 2); };
		Bytes.Append(reinterpret_cast<const uint8*>("RIFF"), 4);
		U32(36 + Frames * 2);
		Bytes.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8);
		U32(16);
		U16(1);
		U16(1);
		U32(SampleRate);
		U32(SampleRate * 2);
		U16(2);
		U16(16);
		Bytes.Append(reinterpret_cast<const uint8*>("data"), 4);
		U32(Frames * 2);
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			const int16 Value = static_cast<int16>(FMath::Sin(2.f * PI * 440.f * Frame / SampleRate) * 12000.f);
			Bytes.Append(reinterpret_cast<const uint8*>(&Value), 2);
		}
		return Bytes;
	}

	FString Text(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	/** What the stand-in services saw. */
	struct FSeen
	{
		TArray<FString> Calls;
		TMap<FString, FString> Keys;
		TMap<FString, FString> Queries;
		TArray<FString> Bodies;
		int32 Polls = 0;
	};

	FString Header(const FHttpServerRequest& Request, const TCHAR* Name)
	{
		for (const TPair<FString, TArray<FString>>& Pair : Request.Headers)
		{
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase) && !Pair.Value.IsEmpty())
			{
				return Pair.Value[0];
			}
		}
		return FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioGenRequestsTest, "HyperAIStudio.AudioGen.Requests", HyperAIStudio::AudioGenTests::Flags)

bool FHyperAIStudioAudioGenRequestsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioGenTests;
	// ElevenLabs sound effects: JSON with the documented fields, MP3 at 44.1 kHz asked for in the query, no key in the call.
	const FHyperAIAudioGenCall Effect = C::BuildCall(TEXT("elevenlabs"), TEXT("eleven_text_to_sound_v2"), TEXT("glass shatters"), 2.5f, 0.6f, true, true, 0);
	const FString EffectBody = Text(Effect.Body);
	TestTrue(TEXT("Sound effects post JSON to /v1/sound-generation, MP3 at 44.1 kHz: ") + Effect.Path, Effect.Verb == TEXT("POST")
		&& Effect.Path == TEXT("/v1/sound-generation?output_format=mp3_44100_128") && Effect.ContentType == TEXT("application/json") && !Effect.bAsync);
	TestTrue(TEXT("with text, model_id, duration_seconds, prompt_influence and loop: ") + EffectBody, EffectBody.Contains(TEXT("\"text\":\"glass shatters\""))
		&& EffectBody.Contains(TEXT("\"model_id\":\"eleven_text_to_sound_v2\"")) && EffectBody.Contains(TEXT("\"duration_seconds\":2.5"))
		&& EffectBody.Contains(TEXT("\"prompt_influence\":0.6")) && EffectBody.Contains(TEXT("\"loop\":true")));
	TestFalse(TEXT("A length ElevenLabs picks sends no duration"), Text(C::BuildCall(TEXT("elevenlabs"), TEXT("eleven_text_to_sound_v2"), TEXT("x"), 0.f, 0.3f, false, true, 0).Body)
		.Contains(TEXT("duration_seconds")));
	const FString Music = Text(C::BuildCall(TEXT("elevenlabs_music"), TEXT("music_v1"), TEXT("calm harp"), 45.f, 0.3f, false, true, 0).Body);
	TestTrue(TEXT("Music posts the prompt, its length in ms and instrumental: ") + Music, Music.Contains(TEXT("\"music_length_ms\":45000"))
		&& Music.Contains(TEXT("\"force_instrumental\":true")) && Music.Contains(TEXT("\"prompt\":\"calm harp\"")));
	const FHyperAIAudioGenCall Stable = C::BuildCall(TEXT("stability"), TEXT("stable-audio-2.5"), TEXT("rain on a tin roof"), 12.f, 0.3f, false, true, 42);
	const FString StableBody = Text(Stable.Body);
	TestTrue(TEXT("Stable Audio 2.5 posts a multipart form to stable-audio-2, asking for WAV bytes"), Stable.Path == TEXT("/v2beta/audio/stable-audio-2/text-to-audio")
		&& Stable.ContentType.StartsWith(TEXT("multipart/form-data; boundary=")) && Stable.Headers.FindRef(TEXT("Accept")) == TEXT("audio/*") && !Stable.bAsync);
	TestTrue(TEXT("with prompt, duration, model, output_format wav and the seed"), StableBody.Contains(TEXT("name=\"prompt\"\r\n\r\nrain on a tin roof"))
		&& StableBody.Contains(TEXT("name=\"duration\"\r\n\r\n12")) && StableBody.Contains(TEXT("name=\"model\"\r\n\r\nstable-audio-2.5"))
		&& StableBody.Contains(TEXT("name=\"output_format\"\r\n\r\nwav")) && StableBody.Contains(TEXT("name=\"seed\"\r\n\r\n42")));
	const FHyperAIAudioGenCall Three = C::BuildCall(TEXT("stability"), TEXT("stable-audio-3"), TEXT("x"), 20.f, 0.3f, false, true, 0);
	TestTrue(TEXT("Stable Audio 3 is the asynchronous endpoint, with no seed when none is given"), Three.bAsync && Three.Path == TEXT("/v2beta/audio/stable-audio/text-to-audio")
		&& !Text(Three.Body).Contains(TEXT("name=\"seed\"")));
	for (const FHyperAIAudioGenCall& Call : {Effect, Stable})
	{
		TestFalse(TEXT("No call carries a key; it is added when sent"), Call.Headers.Contains(TEXT("xi-api-key")) || Call.Headers.Contains(TEXT("Authorization")));
	}

	// Prices: ElevenLabs by the minute of audio (an automatic length priced at 30 s), Stability by the generation.
	TestTrue(TEXT("Ten seconds of sound effects at $0.12 a minute is $0.02"), FMath::IsNearlyEqual(C::PriceOf(TEXT("elevenlabs"), FString(), 10.f, 0.12), 0.02, 1e-9));
	TestTrue(TEXT("a length ElevenLabs picks is priced as 30 s"), FMath::IsNearlyEqual(C::PriceOf(TEXT("elevenlabs"), FString(), 0.f, 0.12), 0.06, 1e-9));
	TestTrue(TEXT("a minute of music at $0.15"), FMath::IsNearlyEqual(C::PriceOf(TEXT("elevenlabs_music"), FString(), 60.f, 0.15), 0.15, 1e-9));
	TestTrue(TEXT("Stable Audio 2.5 at 8 credits, Stable Audio 2 at 20"), FMath::IsNearlyEqual(C::PriceOf(TEXT("stability"), TEXT("stable-audio-2.5"), 30.f, 0.08), 0.08, 1e-9)
		&& FMath::IsNearlyEqual(C::PriceOf(TEXT("stability"), TEXT("stable-audio-2"), 30.f, 0.08), 0.20, 1e-9));

	// The audio's format from its first bytes.
	TestEqual(TEXT("WAV"), C::SniffFormat(TinyWav()), FString(TEXT("wav")));
	TArray<uint8> Mp3 = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0, 0, 0};
	TestEqual(TEXT("MP3 with a tag"), C::SniffFormat(Mp3), FString(TEXT("mp3")));
	Mp3 = {0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0, 0, 0, 0, 0};
	TestEqual(TEXT("MP3 from its frame sync"), C::SniffFormat(Mp3), FString(TEXT("mp3")));
	const TArray<uint8> Json = {'{', '"', 'd', 'e', 't', 'a', 'i', 'l', '"', ':', '1', '}'};
	TestTrue(TEXT("A JSON error is not audio"), C::SniffFormat(Json).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioAudioGenProvidersTest, "HyperAIStudio.AudioGen.Providers", HyperAIStudio::AudioGenTests::Flags)

bool FHyperAIStudioAudioGenProvidersTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::AudioGenTests;
	constexpr uint32 Port = 18352;
	const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, /*bFailOnBindFailure=*/true);
	if (!Router.IsValid())
	{
		AddWarning(TEXT("Port 18352 is taken, so the stand-ins for ElevenLabs and Stability could not start."));
		return true;
	}
	const FString Base = FString::Printf(TEXT("http://127.0.0.1:%u"), Port);
	const TSharedRef<FSeen> Seen = MakeShared<FSeen>();
	const TSharedRef<TArray<uint8>> Wav = MakeShared<TArray<uint8>>(TinyWav());
	TArray<FHttpRouteHandle> Routes;
	auto Bind = [&](const TCHAR* Path, const EHttpServerRequestVerbs Verb, TFunction<TUniquePtr<FHttpServerResponse>(const FHttpServerRequest&)> Answer)
	{
		Routes.Add(Router->BindRoute(FHttpPath(Path), Verb, FHttpRequestHandler::CreateLambda(
			[Seen, Path = FString(Path), Answer](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				Seen->Calls.Add(Path);
				Seen->Keys.Add(Path, Header(Request, TEXT("xi-api-key")) + Header(Request, TEXT("Authorization")));
				Seen->Queries.Add(Path, Request.QueryParams.FindRef(TEXT("output_format")));
				Seen->Bodies.Add(Text(Request.Body).Left(4000));
				OnComplete(Answer(Request));
				return true;
			})));
	};
	auto Audio = [Wav](const FHttpServerRequest&) { return FHttpServerResponse::Create(TArray<uint8>(*Wav), TEXT("audio/wav")); };
	auto Json = [](const FString& Body, const EHttpServerResponseCodes Code)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = Code;
		return Response;
	};
	// Each answers as its provider documents: audio bytes at once, or (Stable Audio 3) an id, in progress once, then the audio.
	Bind(TEXT("/elevenlabs/v1/sound-generation"), EHttpServerRequestVerbs::VERB_POST, Audio);
	Bind(TEXT("/elevenlabs_music/v1/music"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&)
	{
		return Json(TEXT(R"({"detail":{"status":"invalid_api_key","message":"Invalid API key"}})"), EHttpServerResponseCodes::Denied);
	});
	Bind(TEXT("/stability/v2beta/audio/stable-audio-2/text-to-audio"), EHttpServerRequestVerbs::VERB_POST, Audio);
	Bind(TEXT("/stability/v2beta/audio/stable-audio/text-to-audio"), EHttpServerRequestVerbs::VERB_POST, [Json](const FHttpServerRequest&)
	{
		return Json(TEXT(R"({"id":"gen_rain"})"), EHttpServerResponseCodes::Accepted);
	});
	Bind(TEXT("/stability/v2beta/audio/results/gen_rain"), EHttpServerRequestVerbs::VERB_GET, [Seen, Json, Audio](const FHttpServerRequest& Request)
	{
		return Seen->Polls++ == 0 ? Json(TEXT(R"({"id":"gen_rain","status":"in-progress"})"), EHttpServerResponseCodes::Accepted) : Audio(Request);
	});
	FHttpServerModule::Get().StartAllListeners();

	// The providers, pointed at the stand-ins, with keys only this test sets.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FHyperAIStudioExternalServiceSettings> SavedServices = Settings->ExternalServices;
	const FString SavedFolder = Settings->AudioImportFolder;
	const float SavedCap = Settings->ExternalServiceSessionSpendCap;
	for (FHyperAIStudioExternalServiceSettings Service : UHyperAIStudioSettings::GetAudioServices())
	{
		Service.BaseUrl = Base / Service.Id;
		Service.ApiKeyVariable = Service.Id == TEXT("stability") ? TEXT("HYPERAI_TEST_STABILITY_KEY") : TEXT("HYPERAI_TEST_ELEVENLABS_KEY");
		Settings->ExternalServices.Add(Service);
	}
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_ELEVENLABS_KEY"), TEXT("eleven-test-key"));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_STABILITY_KEY"), TEXT("stability-test-key"));
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/AudioGen_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Settings->AudioImportFolder = Folder;
	Settings->ExternalServiceSessionSpendCap = 5.f;
	C::SetPollSecondsForTests(0.1);
	FHyperAIStudioExternalServices::ResetSessionSpend();
	TArray<FString> Operations;
	ON_SCOPE_EXIT
	{
		for (const FHttpRouteHandle& Route : Routes)
		{
			Router->UnbindRoute(Route);
		}
		Settings->ExternalServices = SavedServices;
		Settings->AudioImportFolder = SavedFolder;
		Settings->ExternalServiceSessionSpendCap = SavedCap;
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_ELEVENLABS_KEY"), TEXT(""));
		FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_STABILITY_KEY"), TEXT(""));
		C::SetPollSecondsForTests(0.0);
		FHyperAIStudioExternalServices::ResetSessionSpend();
		for (const FString& Operation : Operations)
		{
			IFileManager::Get().DeleteDirectory(*(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Audio") / Operation), false, true);
		}
		TArray<FAssetData> Assets;
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPath(FName(*Folder), Assets, /*bRecursive=*/true);
		// The wrappers first: a wave a cue's Wave Player still holds is "in use", and its package stays behind.
		TArray<UObject*> Users;
		TArray<UObject*> Waves;
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* Object = Asset.GetAsset())
			{
				(Object->GetClass() == USoundWave::StaticClass() ? Waves : Users).Add(Object);
			}
		}
		for (const TArray<UObject*>* Objects : {&Users, &Waves})
		{
			if (!Objects->IsEmpty())
			{
				ObjectTools::ForceDeleteObjects(*Objects, /*ShowConfirmation=*/false);
			}
		}
	};

	// A dry run prices each take and spends nothing.
	FHyperAIAudioGenerateRequest Request;
	Request.Provider = TEXT("elevenlabs");
	Request.Prompt = TEXT("heavy wooden door slams shut");
	Request.DurationSeconds = 2.f;
	Request.Variations = 3;
	const FHyperAIAudioGenerateReport Checked = C::Generate(Request);
	AddInfo(TEXT("Dry run: ") + Checked.Diagnostic);
	TestTrue(TEXT("A dry run names each take with its length and price, and the wrapper"), Checked.bOk && Checked.Status == TEXT("checked")
		&& Checked.Variations.Num() == 3 && Checked.Variations[0].Name == TEXT("SFX_HeavyWoodenDoorSlams_01") && FMath::IsNearlyEqual(Checked.Variations[0].Seconds, 2.f)
		&& FMath::IsNearlyEqual(Checked.Variations[2].Cost, 0.004f, 1e-6f) && FMath::IsNearlyEqual(Checked.EstimatedCost, 0.012f, 1e-6f)
		&& Checked.Wrap == TEXT("metasound_random") && Checked.WrapperPath == Folder / TEXT("MS_HeavyWoodenDoorSlams")
		&& Checked.DocsUrl.Contains(TEXT("text-to-sound-effects")));
	TestTrue(TEXT("and nothing was sent"), Seen->Calls.IsEmpty() && FMath::IsNearlyZero(FHyperAIStudioExternalServices::GetSessionSpend()));

	// Refused before anything is sent.
	auto Refused = [&](const TCHAR* What, const FHyperAIAudioGenerateRequest& Bad, const TCHAR* Status)
	{
		const FHyperAIAudioGenerateReport Report = C::Generate(Bad);
		TestTrue(FString(What) + TEXT(": ") + Report.Diagnostic, !Report.bOk && Report.Status == Status);
	};
	FHyperAIAudioGenerateRequest Long = Request;
	Long.DurationSeconds = 31.f;
	Refused(TEXT("A sound effect over 30 s"), Long, TEXT("invalid_request"));
	FHyperAIAudioGenerateRequest Short = Request;
	Short.DurationSeconds = 0.4f;
	Refused(TEXT("or under 0.5 s"), Short, TEXT("invalid_request"));
	FHyperAIAudioGenerateRequest Many = Request;
	Many.Variations = 9;
	Refused(TEXT("More than 8 takes"), Many, TEXT("invalid_request"));
	FHyperAIAudioGenerateRequest NoLength = Request;
	NoLength.Provider = TEXT("stability");
	NoLength.DurationSeconds = 0.f;
	Refused(TEXT("Stable Audio without a length"), NoLength, TEXT("invalid_request"));
	FHyperAIAudioGenerateRequest LoopStable = NoLength;
	LoopStable.DurationSeconds = 10.f;
	LoopStable.bLoop = true;
	Refused(TEXT("A provider loop from Stable Audio"), LoopStable, TEXT("invalid_request"));
	FHyperAIAudioGenerateRequest Model = Request;
	Model.Model = TEXT("eleven_v9");
	Refused(TEXT("A model the provider does not document"), Model, TEXT("invalid_request"));
	FHyperAIAudioGenerateRequest Wrap = Request;
	Wrap.Wrap = TEXT("blueprint");
	Refused(TEXT("An unknown wrapper"), Wrap, TEXT("invalid_request"));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_STABILITY_KEY"), TEXT(""));
	const FHyperAIAudioGenerateReport NoKey = C::Generate(LoopStable);
	TestTrue(TEXT("A provider without a key names the variable to set, never a key"), NoKey.Status == TEXT("missing_key")
		&& NoKey.Diagnostic.Contains(TEXT("HYPERAI_TEST_STABILITY_KEY")) && !NoKey.Diagnostic.Contains(TEXT("eleven-test-key")));
	FPlatformMisc::SetEnvironmentVar(TEXT("HYPERAI_TEST_STABILITY_KEY"), TEXT("stability-test-key"));

	// The spend cap: a job that would pass it is refused, nothing sent.
	Settings->ExternalServiceSessionSpendCap = 0.005f;
	FHyperAIAudioGenerateRequest Capped = Request;
	Capped.bDryRun = false;
	const FHyperAIAudioGenerateReport Over = C::Generate(Capped);
	TestTrue(TEXT("Over the session's cap, it is refused before asking the user: ") + Over.Diagnostic, !Over.bOk && Over.Status == TEXT("over_spend_cap")
		&& !FHyperAIStudioApprovalGate::HasPending(Over.OperationId) && Seen->Calls.IsEmpty());
	Settings->ExternalServiceSessionSpendCap = 5.f;

	// Turned down, nothing is sent.
	const FHyperAIAudioGenerateReport Asked = C::Generate(Capped);
	Operations.Add(Asked.OperationId);
	TestEqual(TEXT("For real, it waits for the user's approval"), Asked.Status, FString(TEXT("awaiting_user_approval")));
	FHyperAIStudioApprovalGate::Reject(Asked.OperationId, TEXT("Not now."));
	const FHyperAIAudioStatusReport Rejected = C::Status(Asked.OperationId);
	TestTrue(TEXT("A rejected job shows as rejected and reaches no provider"), !Rejected.Jobs.IsEmpty() && Rejected.Jobs[0].State == TEXT("rejected") && Seen->Calls.IsEmpty());

	// Each provider for real, through approval, to imported SoundWaves and their wrapper.
	auto Run = [&](FHyperAIAudioGenerateRequest Live) -> FHyperAIAudioJob
	{
		Live.bDryRun = false;
		const FHyperAIAudioGenerateReport Report = C::Generate(Live);
		Operations.Add(Report.OperationId);
		if (!TestEqual(Live.Provider + TEXT(" waits for approval: ") + Report.Diagnostic, Report.Status, FString(TEXT("awaiting_user_approval"))))
		{
			return FHyperAIAudioJob();
		}
		FString Error;
		TestTrue(Live.Provider + TEXT(" is approved: ") + Error, FHyperAIStudioApprovalGate::Approve(Report.OperationId, Error));
		FHyperAIAudioJob Job;
		const double End = FPlatformTime::Seconds() + 30.0;
		while (FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			FPlatformProcess::Sleep(0.01f);
			const FHyperAIAudioStatusReport Status = C::Status(Report.OperationId);
			Job = Status.Jobs.IsEmpty() ? FHyperAIAudioJob() : Status.Jobs[0];
			if (Job.State == TEXT("completed") || Job.State == TEXT("failed"))
			{
				break;
			}
		}
		AddInfo(FString::Printf(TEXT("%s: %s, %s"), *Live.Provider, *Job.State, *Job.Diagnostic));
		return Job;
	};
	FHyperAIAudioGenerateRequest Doors = Request;
	Doors.Variations = 2;
	Doors.PromptInfluence = 0.5f;
	const FHyperAIAudioJob DoorJob = Run(Doors);
	TestTrue(TEXT("Two ElevenLabs takes are imported as SoundWaves of the fixture's length"), DoorJob.State == TEXT("completed") && DoorJob.Variations.Num() == 2
		&& DoorJob.Variations.ContainsByPredicate([](const FHyperAIAudioVariation& Take) { return FMath::IsNearlyEqual(Take.Seconds, 0.25f, 0.01f); })
		&& !DoorJob.Variations.ContainsByPredicate([](const FHyperAIAudioVariation& Take) { return !StaticLoadObject(USoundWave::StaticClass(), nullptr, *Take.AssetPath); }));
	TestTrue(TEXT("and played by a random MetaSound"), !DoorJob.WrapperPath.IsEmpty() && StaticLoadObject(UObject::StaticClass(), nullptr, *DoorJob.WrapperPath) != nullptr);
	TestTrue(TEXT("ElevenLabs got its key in xi-api-key, MP3 asked for, and the documented fields"),
		Seen->Keys.FindRef(TEXT("/elevenlabs/v1/sound-generation")) == TEXT("eleven-test-key")
		&& Seen->Queries.FindRef(TEXT("/elevenlabs/v1/sound-generation")) == TEXT("mp3_44100_128")
		&& Seen->Bodies.ContainsByPredicate([](const FString& Body) { return Body.Contains(TEXT("\"duration_seconds\":2")) && Body.Contains(TEXT("\"prompt_influence\":0.5")); }));

	FHyperAIAudioGenerateRequest Rain;
	Rain.Provider = TEXT("stability");
	Rain.Model = TEXT("stable-audio-3");
	Rain.Prompt = TEXT("steady rain on leaves");
	Rain.DurationSeconds = 20.f;
	Rain.Wrap = TEXT("sound_cue_random");
	Rain.Name = TEXT("AMB_Rain");
	const FHyperAIAudioJob RainJob = Run(Rain);
	TestTrue(TEXT("Stable Audio 3 is polled until its audio is ready, then wrapped in a Sound Cue"), RainJob.State == TEXT("completed") && Seen->Polls >= 2
		&& Cast<USoundCue>(StaticLoadObject(UObject::StaticClass(), nullptr, *RainJob.WrapperPath)) != nullptr);
	TestTrue(TEXT("with the key as a bearer"), Seen->Keys.FindRef(TEXT("/stability/v2beta/audio/stable-audio/text-to-audio")) == TEXT("Bearer stability-test-key"));

	FHyperAIAudioGenerateRequest Hum = Rain;
	Hum.Model = FString();
	Hum.Wrap = TEXT("none");
	Hum.Name = TEXT("AMB_Hum");
	const FHyperAIAudioJob HumJob = Run(Hum);
	TestTrue(TEXT("Stable Audio 2.5 answers at once; with no wrapper, only the SoundWave"), HumJob.State == TEXT("completed") && HumJob.WrapperPath.IsEmpty()
		&& HumJob.Variations.Num() == 1 && !HumJob.Variations[0].AssetPath.IsEmpty());
	TestTrue(TEXT("Each take is booked against the session's cap"), FMath::IsNearlyEqual(FHyperAIStudioExternalServices::GetSessionSpend(), 2 * 0.004 + 0.08 + 0.08, 0.0001));

	FHyperAIAudioGenerateRequest Song;
	Song.Provider = TEXT("elevenlabs_music");
	Song.Prompt = TEXT("calm harp theme");
	Song.DurationSeconds = 30.f;
	const double SpentBefore = FHyperAIStudioExternalServices::GetSessionSpend();
	const FHyperAIAudioJob SongJob = Run(Song);
	TestTrue(TEXT("A provider's refusal fails the job in its own words, and nothing is booked: ") + SongJob.Diagnostic, SongJob.State == TEXT("failed")
		&& SongJob.Diagnostic.Contains(TEXT("401")) && SongJob.Diagnostic.Contains(TEXT("Invalid API key"))
		&& FMath::IsNearlyEqual(FHyperAIStudioExternalServices::GetSessionSpend(), SpentBefore, 0.0001));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
