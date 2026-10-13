// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioAudioGenToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIAudioGenerateRequest
{
	GENERATED_BODY()

	/**
	 * elevenlabs (sound effects, 0.5-30 s), elevenlabs_music (3-300 s) or stability (Stable Audio, 1-190 s; 380 s with
	 * stable-audio-3). Empty picks the first whose key is set, in that order.
	 */
	UPROPERTY() FString Provider;
	/** What to make, such as "heavy wooden door slams shut in a stone hall": up to 1000 characters. */
	UPROPERTY() FString Prompt;
	/** Seconds per clip. 0 lets ElevenLabs sound effects pick (priced as 30 s); music and Stable Audio need a length. */
	UPROPERTY() float DurationSeconds = 0.f;
	/** How many takes to make, 1 to 8: each is its own call and its own price. */
	UPROPERTY() int32 Variations = 1;
	/** ElevenLabs sound effects: 0-1, how closely to follow the prompt (0.3 when below 0). */
	UPROPERTY() float PromptInfluence = -1.f;
	/** ElevenLabs sound effects: a clip that loops seamlessly. */
	UPROPERTY() bool bLoop = false;
	/** ElevenLabs music: no vocals. */
	UPROPERTY() bool bInstrumental = true;
	/** Stable Audio: a seed for repeatable takes (each take adds its index); 0 picks at random. */
	UPROPERTY() int32 Seed = 0;
	/** The provider's model; its default when empty (eleven_text_to_sound_v2, music_v1, stable-audio-2.5). Stable Audio also takes stable-audio-2 and stable-audio-3. */
	UPROPERTY() FString Model;
	/** The clips' base name, such as SFX_DoorSlam; the takes are <name>_01 and on. Made from the prompt when empty. */
	UPROPERTY() FString Name;
	/** Where they are imported, under /Game; Audio Import Folder in the settings when empty. */
	UPROPERTY() FString Folder;
	/**
	 * What the clips are wrapped in: metasound_one_shot, metasound_loop, metasound_random (one take at random per play,
	 * never the same twice running), sound_cue_random (a Sound Cue with a Random node), or none. Empty: one_shot for one
	 * take (loop when bLoop), metasound_random for several.
	 */
	UPROPERTY() FString Wrap;
	/** The wrapper's pitch range per play, in semitones, and volume range; 0, 0 and 1, 1 keep them fixed. */
	UPROPERTY() float PitchSemitonesMin = -1.f;
	UPROPERTY() float PitchSemitonesMax = 1.f;
	UPROPERTY() float VolumeMin = 0.9f;
	UPROPERTY() float VolumeMax = 1.f;
	/** Names the job; made when empty. Follow it with hyper_operation_wait (bWaitThroughApproval) or hyper_audio_generate_status. */
	UPROPERTY() FString OperationId;
	/** Only report the price, the provider and where the clips would land: nothing is spent. */
	UPROPERTY() bool bDryRun = true;
};

/** One take: its name, length and price. */
USTRUCT(BlueprintType)
struct FHyperAIAudioVariation
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() float Seconds = 0.f;
	/** USD, at most. */
	UPROPERTY() float Cost = 0.f;
	/** The file the provider sent, relative to the project. */
	UPROPERTY() FString File;
	/** The imported SoundWave. */
	UPROPERTY() FString AssetPath;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioGenerateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked (a dry run), awaiting_user_approval, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Provider;
	UPROPERTY() FString Model;
	/** Each take, with its length and price. */
	UPROPERTY() TArray<FHyperAIAudioVariation> Variations;
	/** What the job costs at most, in USD, against this session's spend cap. */
	UPROPERTY() float EstimatedCost = 0.f;
	/** The folder the takes and their wrapper are imported into. */
	UPROPERTY() FString Destination;
	UPROPERTY() FString Wrap;
	/** The wrapper asset it will make. */
	UPROPERTY() FString WrapperPath;
	/** Each provider, and whether its key's variable is set. */
	UPROPERTY() TArray<FString> Providers;
	/** The provider's documentation for the call it makes. */
	UPROPERTY() FString DocsUrl;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioJob
{
	GENERATED_BODY()

	UPROPERTY() FString OperationId;
	UPROPERTY() FString Provider;
	UPROPERTY() FString Model;
	/** awaiting_user_approval, rejected, generating, importing, wrapping, completed or failed. */
	UPROPERTY() FString State;
	/** Takes done, of Variations. */
	UPROPERTY() int32 Done = 0;
	UPROPERTY() FString Prompt;
	UPROPERTY() TArray<FHyperAIAudioVariation> Variations;
	UPROPERTY() FString Wrap;
	/** The MetaSound Source or Sound Cue playing the takes. */
	UPROPERTY() FString WrapperPath;
	/** USD booked so far. */
	UPROPERTY() float Cost = 0.f;
	UPROPERTY() FString Diagnostic;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioStatusRequest
{
	GENERATED_BODY()

	/** One job; empty lists this session's jobs, newest first. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioStatusReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAIAudioJob> Jobs;
};

/** Sound effects and music from ElevenLabs and Stable Audio, imported as SoundWaves and wrapped in a MetaSound or Sound Cue. */
UCLASS()
class HYPERAISTUDIOAUDIOGEN_API UHyperAIStudioAudioGenToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Makes sound effects (ElevenLabs, 0.5-30 s, loops too), music (ElevenLabs) or audio (Stable Audio) from words, N
	 * takes at once, imports them as SoundWaves and wraps them in a MetaSound Source (one-shot, loop, or a random-take
	 * player with pitch and volume ranges) or a Sound Cue with a Random node. It is paid, so it always waits for the
	 * user's approval: dry-run first (the default) to read each take's length and price. Keys come from
	 * ELEVENLABS_API_KEY or STABILITY_API_KEY.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|AudioGen")
	static FHyperAIAudioGenerateReport hyper_audio_generate(const FHyperAIAudioGenerateRequest& Request);

	/** This session's audio jobs: each one's state, its takes (file, SoundWave, length, price) and the wrapper it made. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|AudioGen")
	static FHyperAIAudioStatusReport hyper_audio_generate_status(const FHyperAIAudioStatusRequest& Request);
};

/** The HTTP request a take is made with: what the tests check, with no key in it. */
struct FHyperAIAudioGenCall
{
	FString Verb;
	/** Under the service's base URL, with any query. */
	FString Path;
	FString ContentType;
	TArray<uint8> Body;
	TMap<FString, FString> Headers;
	/** The provider answers with an id to poll rather than the audio. */
	bool bAsync = false;
};

class FHyperAIStudioAudioGenContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("audio_generation");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioaudiogentoolset.v1");
	static constexpr const TCHAR* GenerateToolName = TEXT("hyper_audio_generate");
	static constexpr const TCHAR* StatusToolName = TEXT("hyper_audio_generate_status");
	static constexpr int32 MaxPromptCharacters = 1000;
	static constexpr int32 MaxVariations = 8;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIAudioGenerateReport Generate(const FHyperAIAudioGenerateRequest& Request);
	static FHyperAIAudioStatusReport Status(const FString& OperationId);

	/** One take's request: endpoint, headers (without the key) and body, as the provider documents it. */
	static FHyperAIAudioGenCall BuildCall(const FString& Provider, const FString& Model, const FString& Prompt, float Seconds, float PromptInfluence,
		bool bLoop, bool bInstrumental, int32 Seed);
	/** What one take costs in USD: the service's price scaled by the take's length or the model. */
	static double PriceOf(const FString& Provider, const FString& Model, float Seconds, double ServicePrice);
	/** The audio's file extension from its first bytes (wav, mp3, ogg, flac); empty for anything else. */
	static FString SniffFormat(const TArray<uint8>& Bytes);
#if WITH_DEV_AUTOMATION_TESTS
	/** How often a running job asks its provider; tests shorten it. */
	static void SetPollSecondsForTests(double Seconds);
#endif
};

class FHyperAIStudioAudioGenRegistration final
{
public:
	void Startup();
	void Shutdown();

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
