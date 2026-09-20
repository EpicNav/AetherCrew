// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioDialogueToolset.generated.h"

/** One spoken span and the unspoken cue that shapes it. */
USTRUCT(BlueprintType)
struct FHyperAIDialogueLine
{
	GENERATED_BODY()

	/** /Game object path of a HyperAIStudioVoice asset. */
	UPROPERTY() FString Voice;
	/** What is said. Only this is spoken. */
	UPROPERTY() FString Text;
	/** How it is said, in the third person: "her voice hardens", "a long pause". Never spoken. */
	UPROPERTY() FString Direction;
	/** Asset and file name for this line; numbered from the scene when empty. */
	UPROPERTY() FString Name;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	/** error or warning. */
	UPROPERTY() FString Severity;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueVoiceRecord
{
	GENERATED_BODY()

	UPROPERTY() FString Path;
	UPROPERTY() FString ReferenceClip;
	UPROPERTY() FString SpeakerDescription;
	/** False when the clip named above is not in the voice reference folder on this machine. */
	UPROPERTY() bool bClipPresent = false;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueRenderRecord
{
	GENERATED_BODY()

	UPROPERTY() FString RenderId;
	/** running, complete or failed. */
	UPROPERTY() FString State;
	UPROPERTY() FString Progress;
	UPROPERTY() int32 LinesDone = 0;
	UPROPERTY() int32 LinesTotal = 0;
	UPROPERTY() FString Folder;
	/** Pass to apply_plan as expected_render_key. */
	UPROPERTY() FString RenderKey;
	UPROPERTY() float SecondsOfAudio = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueInspectRequest
{
	GENERATED_BODY()

	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	/** reachable, unreachable, key_missing or unknown. */
	UPROPERTY() FString ServiceHealth;
	UPROPERTY() FString ServiceDiagnostic;
	UPROPERTY() FString VoiceReferenceFolder;
	UPROPERTY() TArray<FHyperAIDialogueVoiceRecord> Voices;
	/** Clip files sitting in the voice reference folder, ready to be named by a voice. */
	UPROPERTY() TArray<FString> AvailableClips;
	UPROPERTY() TArray<FHyperAIDialogueRenderRecord> Renders;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FHyperAIDialogueLine> Lines;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAIDialogueIssue> Issues;
	/** Roughly how long these lines take to render, so a long batch is a choice rather than a surprise. */
	UPROPERTY() float EstimatedRenderSeconds = 0.f;
	/** The prompt the first line would send, so the format can be checked without spending a render. */
	UPROPERTY() FString SamplePrompt;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueRenderRequest
{
	GENERATED_BODY()

	/** Names the folder the wavs land in; letters, digits and _ only. */
	UPROPERTY() FString SceneName;
	UPROPERTY() TArray<FHyperAIDialogueLine> Lines;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueRenderReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString RenderId;
	UPROPERTY() FString Folder;
	UPROPERTY() int32 LineCount = 0;
	UPROPERTY() float EstimatedRenderSeconds = 0.f;
	UPROPERTY() TArray<FString> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	/** A render that finished, from hyper_dialogue_render. */
	UPROPERTY() FString RenderId;
	/** From hyper_dialogue_inspect; proves the wavs have not changed since. */
	UPROPERTY() FString ExpectedRenderKey;
	/** /Game folder the sound waves are created in, e.g. /Game/Dialogue/Act1. */
	UPROPERTY() FString TargetFolder;
	/** Also wrap each wave in a Dialogue Wave with its speaker and spoken text, for localisation. */
	UPROPERTY() bool bCreateDialogueWaves = true;
	UPROPERTY() bool bSave = false;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIDialogueApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bTrustedPrepared = false;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString TargetFolder;
	UPROPERTY() FString RenderKey;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() TArray<FString> Capabilities;
};

/** Character dialogue: render lines to speech on the configured service, then bring them in as assets. */
UCLASS()
class HYPERAISTUDIODIALOGUE_API UHyperAIStudioDialogueToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** The speech service's health, the voices in the project and whether their reference clips are present, the clips available to name, and every render with its progress. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Dialogue")
	static FHyperAIDialogueInspectReport hyper_dialogue_inspect(const FHyperAIDialogueInspectRequest& Request);

	/** Checks lines against the speech model's rules before any render: quotes that would close a span early, actions written where they would be spoken aloud, missing voices or clips. Returns the prompt the first line would send. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Dialogue")
	static FHyperAIDialogueValidateReport hyper_dialogue_validate(const FHyperAIDialogueValidateRequest& Request);

	/**
	 * Renders each line to its own wav on the configured speech service, one at a time because the model owns
	 * the card. Returns at once with a render_id; watch it with hyper_dialogue_inspect. Read
	 * .hyperai/docs/HyperAIStudio-DialogueCookbook.md first, and validate before spending the time.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Dialogue")
	static FHyperAIDialogueRenderReport hyper_dialogue_render(const FHyperAIDialogueRenderRequest& Request);

	/** Brings a finished render into the project as sound waves, and optionally Dialogue Waves carrying the speaker and the spoken text, as one undo step. Dry-run, resubmit, poll. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Dialogue")
	static FHyperAIDialogueApplyPlanReport hyper_dialogue_apply_plan(const FHyperAIDialogueApplyPlanRequest& Request);
};

class FHyperAIStudioDialogueInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIDialogueInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioDialogueValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIDialogueValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioDialogueRenderPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIDialogueRenderRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioDialogueImportPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString RenderId;
	FString RenderKey;
	FString TargetFolder;
	bool bCreateDialogueWaves = true;
	bool bSave = false;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

class FHyperAIStudioDialogueInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIDialogueInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioDialogueValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIDialogueValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioDialogueRenderResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIDialogueRenderReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioDialogueMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioDialogueDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioDialogueFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
{
public:
	virtual FString GetOwnerAdapterFingerprint() const override;
	virtual bool ResolveCanonicalEffectTarget(
		const IHyperAIStudioTypedArtifactPayload& Request,
		FString& OutCanonicalEffectTarget,
		FString& OutError) override;
	virtual bool VerifyFreshExact(
		const IHyperAIStudioTypedArtifactPayload& Request,
		const IHyperAIStudioDomainResultPayload& Result,
		FString& OutPostconditionHash,
		FString& OutError) override;
};

class FHyperAIStudioDialogueContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("dialogue_speech");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiodialoguetoolset.v1");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.dialogue_speech_service");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_dialogue_inspect");
	static constexpr const TCHAR* ValidateToolName = TEXT("hyper_dialogue_validate");
	static constexpr const TCHAR* RenderToolName = TEXT("hyper_dialogue_render");
	static constexpr const TCHAR* MutationToolName = TEXT("hyper_dialogue_apply_plan");
	static constexpr const TCHAR* InspectVariantId = TEXT("dialogue_capture.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("dialogue_line_checks.v1");
	static constexpr const TCHAR* RenderVariantId = TEXT("dialogue_speech_render.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("dialogue_import_ops.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.dialogue.inspect.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.dialogue.validate.v1");
	static constexpr const TCHAR* RenderPayloadTypeId = TEXT("hyperai.payload.dialogue.render.v1");
	static constexpr const TCHAR* ImportPayloadTypeId = TEXT("hyperai.payload.dialogue.import.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.dialogue.inspect.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.dialogue.validate.v1");
	static constexpr const TCHAR* RenderResultTypeId = TEXT("hyperai.result.dialogue.render.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.dialogue.import.v1");
	static constexpr const TCHAR* ServiceId = TEXT("dramabox");
	static constexpr int32 MaxLinesPerRender = 64;
	static constexpr int32 MaxTextCharacters = 600;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;
	/**
	 * Measured on a 4090 with nothing else on the card: a line costs about ten seconds whatever its length,
	 * because the text encoder and thirty denoise steps run either way, plus roughly a sixth of a second per
	 * character. Sharing the card with the editor makes this far worse: the same line took five minutes when
	 * the editor held the GPU, nearly all of it in the text encoder waiting for memory.
	 */
	static constexpr float EstimatedFixedSecondsPerLine = 10.0f;
	static constexpr float EstimatedSecondsPerCharacter = 0.15f;
	/** A render gets the job host's whole budget, so refuse a batch that would not fit with room for contention. */
	static constexpr float MaxEstimatedRenderSeconds = 300.0f;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FString> GetCapabilities();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalProjectFolder(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString InspectPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString RenderPayloadSchemaFingerprint();
	static FString ImportPayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static FString RenderResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAIDialogueInspectReport Inspect(const FHyperAIDialogueInspectRequest& Request);
	static FHyperAIDialogueValidateReport Validate(const FHyperAIDialogueValidateRequest& Request);
	static FHyperAIDialogueRenderReport Render(const FHyperAIDialogueRenderRequest& Request);
	static FHyperAIDialogueApplyPlanReport BuildPlan(const FHyperAIDialogueApplyPlanRequest& Request);
	static FString ComputeImportSemanticFingerprint(const FHyperAIStudioDialogueImportPayload& Payload);
};

class FHyperAIStudioDialogueRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;
	bool HasLiveOwnership() const;

private:
	void RegisterAfterEngineInit();
	void RollBackRegistration();

	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FHyperAIStudioDialogueDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
