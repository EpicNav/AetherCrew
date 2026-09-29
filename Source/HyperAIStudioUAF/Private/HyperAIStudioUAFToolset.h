// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "ToolsetRegistry/ToolCallAsyncResult.h"
#include "HyperAIStudioUAFToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIUAFVariable
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** bool, float, int, name, vector, or other for types this toolset does not author. */
	UPROPERTY() FString Type;
	/** True when a getter for it is wired into a trait pin somewhere in the graph. */
	UPROPERTY() bool bUsed = false;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFPin
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** float, bool, int32, FName, or the object or struct type the pin takes. */
	UPROPERTY() FString Type;
	/** The pin's value, or the variable or node feeding it when it is linked. */
	UPROPERTY() FString Value;
	/** True for a pose input: link another trait stack's result into it. */
	UPROPERTY() bool bPose = false;
	/** True when it is fed by a link rather than a value. */
	UPROPERTY() bool bLinked = false;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFTrait
{
	GENERATED_BODY()

	/** Pass this to add_trait, e.g. FSequencePlayerTrait. */
	UPROPERTY() FString Name;
	/** base (one per stack, at the bottom) or additive (layered on top of the base). */
	UPROPERTY() FString Mode;
	UPROPERTY() TArray<FHyperAIUAFPin> Pins;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFNode
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() TArray<FHyperAIUAFTrait> Traits;
	/** True when its result reaches the graph's output pose. */
	UPROPERTY() bool bReachesOutput = false;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFInspectRequest
{
	GENERATED_BODY()

	/** /Game object path of a UAF animation graph. Leave empty to return only the trait catalog. */
	UPROPERTY() FString TargetPath;
	UPROPERTY() bool bLoad = false;
	/** Every registered trait and its pins: the vocabulary add_trait and set_pin accept. */
	UPROPERTY() bool bIncludeTraitCatalog = false;
	/** Case-insensitive substring filter over trait names, for a narrower catalog. */
	UPROPERTY() FString TraitFilter;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString TargetPath;
	/** Pass to apply_plan as expected_revision. */
	UPROPERTY() FString Revision;
	/** compiled, warnings, errors or not_compiled. */
	UPROPERTY() FString CompileStatus;
	UPROPERTY() bool bOpenInEditor = false;
	UPROPERTY() bool bPackageDirty = false;
	/** The trait stack whose result feeds the graph's output pose; empty when nothing does. */
	UPROPERTY() FString OutputNode;
	UPROPERTY() TArray<FHyperAIUAFVariable> Variables;
	UPROPERTY() TArray<FHyperAIUAFNode> Nodes;
	UPROPERTY() TArray<FHyperAIUAFTrait> TraitCatalog;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/**
 * One closed UAF animation graph edit. Node and variable names are identifiers (letters, digits, _; up to 64).
 *   add_variable: Name, Value = bool, float, int, name or vector
 *   add_node: Node - an empty trait stack, the UAF equivalent of one animation node
 *   add_trait: Node, Trait - one base trait at the bottom, then additive traits on top; no trait twice per stack
 *   set_pin: Node, Trait, Pin, Value - a literal, or an asset's /Game object path
 *   bind_pin: Node, Trait, Pin, Value = variable name - a getter for it drives the pin every evaluation
 *   link: Node (the source stack), To, Trait, Pin - feeds the source's pose into that pose pin
 *   link_output: Node - feeds the stack's result to the graph's output pose
 *   remove_node: Node, with its links
 */
USTRUCT(BlueprintType)
struct FHyperAIUAFOp
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString Node;
	UPROPERTY() FString Trait;
	UPROPERTY() FString Pin;
	UPROPERTY() FString To;
	UPROPERTY() FString Name;
	UPROPERTY() FString Value;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() FString TargetPath;
	/** Create the animation graph at target_path. */
	UPROPERTY() bool bCreate = false;
	/** From hyper_uaf_inspect; empty when creating. */
	UPROPERTY() FString ExpectedRevision;
	/** Applied in order as one undo step, then compiled. */
	UPROPERTY() TArray<FHyperAIUAFOp> Ops;
	UPROPERTY() bool bSave = false;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFApplyPlanReport
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
	UPROPERTY() FString TargetPath;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() TArray<FString> Capabilities;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	/** error or warning. */
	UPROPERTY() FString Severity;
	UPROPERTY() FString Subject;
	UPROPERTY() FString Message;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString TargetPath;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIUAFValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString CompileStatus;
	/** True when the compiled graph allocates a runnable instance, the strongest check short of playing it. */
	UPROPERTY() bool bAllocatesInstance = false;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() TArray<FHyperAIUAFIssue> Issues;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FString> Capabilities;
};

/** hyper_uaf_apply_plan's reply, held open until the edit it submitted has finished: one call instead of an apply and a wait. */
UCLASS()
class UHyperAIUAFApplyPlanResult final : public UToolCallAsyncResult
{
	GENERATED_BODY()

public:
	bool SetValue(const FHyperAIUAFApplyPlanReport& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(FHyperAIUAFApplyPlanReport(InValue), Value);
	}

	UPROPERTY()
	FHyperAIUAFApplyPlanReport Value;
};

/** UAF (Unreal Animation Framework) animation graphs: trait stacks, their pins, variables and the output pose. */
UCLASS()
class HYPERAISTUDIOUAF_API UHyperAIStudioUAFToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** A UAF animation graph's trait stacks, their pin values and links, variables, output pose, compile status and revision; with no target_path, the trait catalog on its own. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|UAF")
	static FHyperAIUAFInspectReport hyper_uaf_inspect(const FHyperAIUAFInspectRequest& Request);

	/**
	 * Closed UAF graph edits, then a compile, as one undo step: create a graph, add variables, trait stacks and
	 * traits, set or bind their pins, link poses and the output. Read .hyperai/docs/HyperAIStudio-UAFCookbook.md
	 * first. Dry-run, then resubmit: the reply waits for the edit. Then validate.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|UAF")
	static UHyperAIUAFApplyPlanResult* hyper_uaf_apply_plan(const FHyperAIUAFApplyPlanRequest& Request);

	/** Compile status plus graph checks: the output pose is wired, every stack has one base trait, pose pins are fed, no orphan stacks, assets and variables resolve, and the graph allocates a runnable instance. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|UAF")
	static FHyperAIUAFValidateReport hyper_uaf_validate(const FHyperAIUAFValidateRequest& Request);
};

class FHyperAIStudioUAFInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIUAFInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUAFValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIUAFValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUAFEditOpsPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TargetPath;
	FString BaseRevision;
	bool bCreate = false;
	TArray<FHyperAIUAFOp> Ops;
	bool bSave = false;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

class FHyperAIStudioUAFInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIUAFInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUAFValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIUAFValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUAFMutationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString ContentKey;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioUAFDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;
};

class FHyperAIStudioUAFFreshVerifier final : public IHyperAIStudioTrustedFreshVerifier
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

class FHyperAIStudioUAFContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("uaf_graph");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiouaftoolset.v1");
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.uaf_animation_graph");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_uaf_inspect");
	static constexpr const TCHAR* MutationToolName = TEXT("hyper_uaf_apply_plan");
	static constexpr const TCHAR* ValidateToolName = TEXT("hyper_uaf_validate");
	static constexpr const TCHAR* InspectVariantId = TEXT("uaf_graph_capture.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("uaf_graph_edit_ops.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("uaf_graph_checks.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.uaf_graph.inspect.v1");
	static constexpr const TCHAR* EditOpsPayloadTypeId = TEXT("hyperai.payload.uaf_graph.edit_ops.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.uaf_graph.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.uaf_graph.inspect.v1");
	static constexpr const TCHAR* MutationResultTypeId = TEXT("hyperai.result.uaf_graph.edit_ops.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.uaf_graph.validate.v1");
	static constexpr int32 MaxOpsPerPlan = 48;
	static constexpr int32 MaxPathCharacters = 512;
	static constexpr int32 MinOutputBytes = 16 * 1024;
	static constexpr int32 MaxOutputBytes = 128 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static TArray<FString> GetCapabilities();
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString InspectPayloadSchemaFingerprint();
	static FString EditOpsPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString MutationResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetAdapterDescriptor();
	static FHyperAIUAFInspectReport Inspect(const FHyperAIUAFInspectRequest& Request);
	static FHyperAIUAFValidateReport Validate(const FHyperAIUAFValidateRequest& Request);
	/** hyper_uaf_apply_plan's plan with an agent's near misses fitted first; the tool then waits for the edit it submits. */
	static FHyperAIUAFApplyPlanReport BuildFittedPlan(const FHyperAIUAFApplyPlanRequest& Request);
	static FHyperAIUAFApplyPlanReport BuildPlan(const FHyperAIUAFApplyPlanRequest& Request);
	static FString ComputeEditOpsSemanticFingerprint(const FHyperAIStudioUAFEditOpsPayload& Payload);
};

class FHyperAIStudioUAFRegistration final
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
	TSharedPtr<FHyperAIStudioUAFDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
