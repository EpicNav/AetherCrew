// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioImageGenToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIImageModelInfo
{
	GENERATED_BODY()

	/** Pass this as model_id. */
	UPROPERTY() FString Id;
	UPROPERTY() FString Label;
	UPROPERTY() FString ServiceId;
	/** The model name sent to the service. */
	UPROPERTY() FString Model;
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
	/** True when this endpoint is on this machine, and so shares the graphics card with the editor. */
	UPROPERTY() bool bRunsOnThisMachine = false;
	/** ready, unreachable or unknown, from the service's health probe. */
	UPROPERTY() FString Health;
	UPROPERTY() float CostPerCall = 0.0f;
	/** False when the service names an environment variable that is not set. */
	UPROPERTY() bool bHasApiKey = false;
	/** Why this model cannot be used, when it cannot. */
	UPROPERTY() FString Issue;
};

USTRUCT(BlueprintType)
struct FHyperAIImageTemplateInfo
{
	GENERATED_BODY()

	/** Pass this as template_id. */
	UPROPERTY() FString Id;
	UPROPERTY() FString Label;
	UPROPERTY() FString Template;
	/** The slot names this template takes, in the order they appear. */
	UPROPERTY() TArray<FString> Slots;
};

USTRUCT(BlueprintType)
struct FHyperAIImageRecord
{
	GENERATED_BODY()

	/** Project-relative path, ready for the Preview tab and hyper_lighting_compare. */
	UPROPERTY() FString File;
	UPROPERTY() FString BatchId;
	UPROPERTY() FString Prompt;
	UPROPERTY() FString ModelId;
	UPROPERTY() int32 Seed = -1;
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
	UPROPERTY() FString GeneratedUtc;
	UPROPERTY() int64 SizeBytes = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIImageBatchInfo
{
	GENERATED_BODY()

	UPROPERTY() FString BatchId;
	/** running, complete or failed. */
	UPROPERTY() FString State;
	UPROPERTY() FString Progress;
	UPROPERTY() FString Folder;
	UPROPERTY() FString Diagnostic;
};

USTRUCT(BlueprintType)
struct FHyperAIImageInspectRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bIncludeModels = true;
	UPROPERTY() bool bIncludeTemplates = true;
	/** Images already generated in this project, newest first. */
	UPROPERTY() bool bIncludeGenerated = true;
	/** Narrow the images and batches to one batch. */
	UPROPERTY() FString BatchId;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIImageInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAIImageModelInfo> Models;
	UPROPERTY() TArray<FHyperAIImageTemplateInfo> Templates;
	UPROPERTY() TArray<FHyperAIImageRecord> Images;
	UPROPERTY() TArray<FHyperAIImageBatchInfo> Batches;
	/** Project-relative folder generated images land in. */
	UPROPERTY() FString OutputFolder;
	UPROPERTY() float SessionSpend = 0.0f;
	UPROPERTY() float SessionSpendCap = 0.0f;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/** One named value filling a {slot} in a prompt template. */
USTRUCT(BlueprintType)
struct FHyperAIImageSlot
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Value;
};

USTRUCT(BlueprintType)
struct FHyperAIImageGenerateRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	/** From hyper_image_inspect. */
	UPROPERTY() FString ModelId;
	/** Template to fill from slots. Leave empty to send prompt exactly as written. */
	UPROPERTY() FString TemplateId;
	UPROPERTY() TArray<FHyperAIImageSlot> Slots;
	/** Appended after the filled template, or the whole prompt when there is no template. */
	UPROPERTY() FString Prompt;
	/** What to keep out, for the models whose request template uses it. */
	UPROPERTY() FString Negative;
	/** Names the folder the images land in; a short suffix keeps batches apart. */
	UPROPERTY() FString BatchName;
	UPROPERTY() int32 Count = 1;
	/** 0 for the model's configured size. */
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
	/** -1 for a random seed. A fixed seed re-renders the same image; a batch increments from it. */
	UPROPERTY() int32 Seed = -1;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIImageGenerateReport
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
	/** Exactly what will be sent. Read it on the dry run, where it costs nothing. */
	UPROPERTY() FString ComposedPrompt;
	/** Slot names the template wanted and the request did not fill. */
	UPROPERTY() TArray<FString> UnfilledSlots;
	UPROPERTY() FString ModelId;
	UPROPERTY() FString BatchId;
	/** Project-relative folder the images land in. */
	UPROPERTY() FString Folder;
	UPROPERTY() int32 Count = 0;
	UPROPERTY() int32 Width = 0;
	UPROPERTY() int32 Height = 0;
	UPROPERTY() int32 Seed = -1;
	UPROPERTY() float EstimatedCost = 0.0f;
	/** True when this model shares the editor's graphics card, which makes it far slower. */
	UPROPERTY() bool bRunsOnThisMachine = false;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() TArray<FString> Capabilities;
};

/** Reference images from a configured image model, written into the project as files with their prompts beside them. */
UCLASS()
class HYPERAISTUDIOIMAGEGEN_API UHyperAIStudioImageGenToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Configured image models with their service health and cost, the prompt templates and their slots, images already generated, and running batches. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|ImageGen")
	static FHyperAIImageInspectReport hyper_image_inspect(const FHyperAIImageInspectRequest& Request);

	/**
	 * Generate reference images from a prompt, written into the project as PNG files with a JSON sidecar
	 * recording the prompt, model and seed. Read .hyperai/docs/HyperAIStudio-ImageGenCookbook.md first.
	 * This spends money, so it always waits for approval. Dry-run to read the composed prompt for free,
	 * then resubmit; watch the batch with hyper_image_inspect.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|ImageGen")
	static FHyperAIImageGenerateReport hyper_image_generate(const FHyperAIImageGenerateRequest& Request);
};

class FHyperAIStudioImageGenInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIImageInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioImageGenGeneratePayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString BatchId;
	/** Project-relative output folder; also the effect target. */
	FString Folder;
	FString ModelId;
	FString TemplateId;
	FString Prompt;
	FString Negative;
	int32 Count = 1;
	int32 Width = 0;
	int32 Height = 0;
	int32 Seed = -1;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

class FHyperAIStudioImageGenInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIImageInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioImageGenGenerateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioImageGenDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioImageGenFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
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

class FHyperAIStudioImageGenContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("image_generation");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioimagegentoolset.v1");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.image_generation_model");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_image_inspect");
	static constexpr const TCHAR* GenerateToolName = TEXT("hyper_image_generate");
	static constexpr const TCHAR* InspectVariantId = TEXT("image_generation_capture.v1");
	static constexpr const TCHAR* GenerateVariantId = TEXT("image_generation_render.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.image_generation.inspect.v1");
	static constexpr const TCHAR* GeneratePayloadTypeId = TEXT("hyperai.payload.image_generation.render.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.image_generation.inspect.v1");
	static constexpr const TCHAR* GenerateResultTypeId = TEXT("hyperai.result.image_generation.render.v1");
	static constexpr int32 MaxImagesPerBatch = 8;
	static constexpr int32 MaxPromptCharacters = 4000;
	static constexpr int32 MaxSlots = 16;
	static constexpr int32 MaxSlotValueCharacters = 400;
	static constexpr int32 MinImageSize = 64;
	static constexpr int32 MaxImageSize = 4096;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FString> GetCapabilities();
	static bool IsCanonicalSha256(const FString& Value);
	/** Letters, digits and underscore, 1-48 characters: safe as a folder name and as a batch id. */
	static bool IsCanonicalName(const FString& Value, int32 MaxCharacters = 48);
	static FString InspectPayloadSchemaFingerprint();
	static FString GeneratePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString GenerateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAIImageInspectReport Inspect(const FHyperAIImageInspectRequest& Request);
	static FHyperAIImageGenerateReport BuildPlan(const FHyperAIImageGenerateRequest& Request);
	static FString ComputeGenerateSemanticFingerprint(const FHyperAIStudioImageGenGeneratePayload& Payload);
};

class FHyperAIStudioImageGenRegistration final
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
	TSharedPtr<FHyperAIStudioImageGenDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
