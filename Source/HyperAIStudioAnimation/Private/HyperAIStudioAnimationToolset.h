// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioDomainAdapter.h"
#include "HyperAIStudioLiveProbePublisher.h"
#include "HyperAIStudioTrustedExecution.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioAnimationToolset.generated.h"

/** One stable, bounded Animation/Rigging finding. */
USTRUCT(BlueprintType)
struct FHyperAIAnimationIssue
{
	GENERATED_BODY()

	UPROPERTY() FString Code;
	UPROPERTY() FString Severity;
	UPROPERTY() FString Variant;
	UPROPERTY() FString AssetPath;
	UPROPERTY() FString StableId;
	UPROPERTY() int32 OperationIndex = -1;
	UPROPERTY() FString Message;
};

/** Exact prerequisite and implementation state for one typed family. */
USTRUCT(BlueprintType)
struct FHyperAIAnimationVariantStatus
{
	GENERATED_BODY()

	UPROPERTY() FString Variant;
	UPROPERTY() TArray<FString> RequiredPlugins;
	UPROPERTY() TArray<FString> RequiredModules;
	UPROPERTY() bool bPluginsInstalled = false;
	UPROPERTY() bool bPluginsEnabled = false;
	UPROPERTY() bool bModulesLoaded = false;
	UPROPERTY() bool bInspectImplemented = false;
	UPROPERTY() bool bPlanSchemaImplemented = false;
	UPROPERTY() bool bApplyBackendExecutable = false;
	UPROPERTY() TArray<FString> SupportedCases;
	UPROPERTY() TArray<FString> DelegatedEpicCases;
	UPROPERTY() TArray<FString> UnsupportedCases;
	UPROPERTY() FString State;
	UPROPERTY() FString Remediation;
};

/** A graph, state, transition, section, notify, curve, sample, pose, bone, or compatibility edge. */
USTRUCT(BlueprintType)
struct FHyperAIAnimationElementView
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString StableId;
	UPROPERTY() FString Name;
	UPROPERTY() FString SecondaryName;
	UPROPERTY() FString ContainerName;
	UPROPERTY() FString ClassPath;
	UPROPERTY() FString ReferencePath;
	UPROPERTY() int32 Index = -1;
	UPROPERTY() int32 SecondaryIndex = -1;
	UPROPERTY() double TimeSeconds = -1.0;
	UPROPERTY() double DurationSeconds = -1.0;
	UPROPERTY() bool bFlag = false;
};

/** Immutable loaded-only projection of one supported base animation asset. */
USTRUCT(BlueprintType)
struct FHyperAIAnimationAssetRecord
{
	GENERATED_BODY()

	UPROPERTY() FString Variant;
	UPROPERTY() FString StableId;
	UPROPERTY() FString AssetPath;
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() bool bPackageDirty = false;
	UPROPERTY() bool bTemplate = false;
	UPROPERTY() bool bOutputDetailsTruncated = false;
	UPROPERTY() FString SkeletonPath;
	UPROPERTY() FString PreviewMeshPath;
	UPROPERTY() FString ParentClassPath;
	UPROPERTY() FString GeneratedClassPath;
	UPROPERTY() FString BlueprintStatus;
	UPROPERTY() double PlayLengthSeconds = 0.0;
	UPROPERTY() int32 SampledKeyCount = 0;
	UPROPERTY() int32 GraphCount = 0;
	UPROPERTY() int32 StateMachineCount = 0;
	UPROPERTY() int32 StateCount = 0;
	UPROPERTY() int32 TransitionCount = 0;
	UPROPERTY() int32 SectionCount = 0;
	UPROPERTY() int32 SlotCount = 0;
	UPROPERTY() int32 SegmentCount = 0;
	UPROPERTY() int32 NotifyCount = 0;
	UPROPERTY() int32 CurveCount = 0;
	UPROPERTY() int32 SampleCount = 0;
	UPROPERTY() int32 PoseCount = 0;
	UPROPERTY() int32 BoneCount = 0;
	UPROPERTY() TArray<FHyperAIAnimationElementView> Elements;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationInspectRequest
{
	GENERATED_BODY()

	/** Exact /Game object paths. Empty means a bounded scan of already-loaded objects. */
	UPROPERTY() TArray<FString> AssetPaths;
	/** all or one exact variant from the prerequisite matrix. */
	UPROPERTY() FString Variant = TEXT("all");
	UPROPERTY() bool bIncludeDetails = true;
	UPROPERTY() bool bIncludePrerequisites = true;
	UPROPERTY() int32 PageSize = 64;
	UPROPERTY() FString Cursor;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Scope = TEXT("loaded_only");
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() int32 LoadedObjectsScanned = 0;
	UPROPERTY() int32 TotalRecords = 0;
	UPROPERTY() int32 ReturnedRecords = 0;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString NextCursor;
	UPROPERTY() TArray<FHyperAIAnimationAssetRecord> Records;
	UPROPERTY() TArray<FHyperAIAnimationIssue> Issues;
	UPROPERTY() TArray<FHyperAIAnimationVariantStatus> Variants;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationValidateRequest
{
	GENERATED_BODY()

	UPROPERTY() TArray<FString> AssetPaths;
	UPROPERTY() FString Variant = TEXT("all");
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() bool bRequirePackagesClean = false;
	UPROPERTY() bool bIncludePrerequisites = true;
	UPROPERTY() int32 MaxIssues = 128;
	UPROPERTY() int32 MaxGameThreadMs = 100;
	UPROPERTY() int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationValidateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bValid = false;
	UPROPERTY() bool bFreshCapture = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Scope = TEXT("fresh_loaded_state");
	UPROPERTY() FString Revision;
	UPROPERTY() bool bRevisionComplete = false;
	UPROPERTY() int32 ErrorCount = 0;
	UPROPERTY() int32 WarningCount = 0;
	UPROPERTY() int32 InfoCount = 0;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() TArray<FHyperAIAnimationIssue> Issues;
	UPROPERTY() TArray<FHyperAIAnimationVariantStatus> Variants;
};

/** Closed rule vocabulary; never an expression, code snippet, or class name. */
USTRUCT(BlueprintType)
struct FHyperAIAnimationTransitionRuleSpec
{
	GENERATED_BODY()

	/** bool_parameter, time_remaining_ratio, or automatic_sequence_end. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString ParameterName;
	UPROPERTY() double Threshold = -1.0;
	UPROPERTY() bool bExpectedValue = true;
};

/** Closed notify shape. Named events are data and cannot instantiate arbitrary classes. */
USTRUCT(BlueprintType)
struct FHyperAIAnimationNotifySpec
{
	GENERATED_BODY()

	/** named_event only in the base adapter. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Name;
	UPROPERTY() double TimeSeconds = -1.0;
	UPROPERTY() double DurationSeconds = 0.0;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationSegmentSpec
{
	GENERATED_BODY()

	UPROPERTY() FString SequencePath;
	UPROPERTY() double StartSeconds = 0.0;
	UPROPERTY() double EndSeconds = -1.0;
	UPROPERTY() double PlayRate = 1.0;
	UPROPERTY() int32 LoopCount = 1;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationBlendSampleSpec
{
	GENERATED_BODY()

	UPROPERTY() FString SequencePath;
	UPROPERTY() FVector Position = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationNameMapping
{
	GENERATED_BODY()

	UPROPERTY() FString Source;
	UPROPERTY() FString Target;
};

/** Strict discriminated operation. Unused fields must remain at their defaults. */
USTRUCT(BlueprintType)
struct FHyperAIAnimationPlanOperation
{
	GENERATED_BODY()

	/**
	 * Base operations: animbp.create|set_skeleton|add_state_machine|add_state|add_transition|
	 * set_transition_rule|add_interface|add_layer|compile_repair; montage.create|add_section|
	 * remove_section|link_sections|add_notify|remove_notify; composite.create|set_segments;
	 * blend_space.create|set_axis|set_samples (executable for one blend space per plan; bFlag on create makes it
	 * 1D); pose_asset.create|set_source|set_pose_names;
	 * sequence.add_curve|remove_curve|add_notify|remove_notify; skeleton.add_compatible|
	 * remove_compatible. Optional ControlRig/IK/PoseSearch prefixes are recognized and fail
	 * with variant_adapter_unavailable until their exact typed facade is linked.
	 */
	UPROPERTY() FString Type;
	UPROPERTY() FString TargetPath;
	/** Exact inspector-issued element identity where an operation requires one. */
	UPROPERTY() FString StableId;
	/** Exact CAS for existing targets; empty for create and same-plan post-create configuration. */
	UPROPERTY() FString ExpectedRevision;
	UPROPERTY() FString ReferencePath;
	UPROPERTY() FString Name;
	UPROPERTY() FString SecondaryName;
	UPROPERTY() int32 AxisIndex = -1;
	UPROPERTY() double Minimum = 0.0;
	UPROPERTY() double Maximum = 0.0;
	UPROPERTY() bool bFlag = false;
	UPROPERTY() FHyperAIAnimationTransitionRuleSpec TransitionRule;
	UPROPERTY() FHyperAIAnimationNotifySpec Notify;
	UPROPERTY() TArray<FHyperAIAnimationSegmentSpec> Segments;
	UPROPERTY() TArray<FHyperAIAnimationBlendSampleSpec> Samples;
	UPROPERTY() TArray<FHyperAIAnimationNameMapping> Mappings;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationApplyPlanRequest
{
	GENERATED_BODY()

	UPROPERTY() bool bDryRun = true;
	/** Required only for idempotent staging. Edit plans expose no client authorization token. */
	UPROPERTY() FString OperationId;
	/** Echo the exact dry-run plan hash before staging. */
	UPROPERTY() FString ExpectedPlanHash;
	UPROPERTY() int32 DeadlineMs = 2000;
	UPROPERTY() int32 MaxGameThreadMs = 250;
	UPROPERTY() int32 MaxOutputBytes = 65536;
	UPROPERTY() TArray<FHyperAIAnimationPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationPlanEffects
{
	GENERATED_BODY()

	UPROPERTY() int32 OperationCount = 0;
	UPROPERTY() int32 TargetCount = 0;
	UPROPERTY() int32 AssetsCreated = 0;
	UPROPERTY() int32 AssetsUpdated = 0;
	UPROPERTY() int32 GraphChanges = 0;
	UPROPERTY() int32 TimelineChanges = 0;
	UPROPERTY() int32 CompatibilityChanges = 0;
	UPROPERTY() bool bTransactionOnce = false;
	UPROPERTY() bool bCompileOnce = false;
	UPROPERTY() bool bSaveOnce = false;
	UPROPERTY() bool bValidateOnce = false;
	UPROPERTY() bool bFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIAnimationApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bStaged = false;
	UPROPERTY() bool bExecutionSubmitted = false;
	UPROPERTY() bool bReplay = false;
	UPROPERTY() bool bFallbackPermitted = false;
	UPROPERTY() bool bTruncated = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString BaseRevision;
	UPROPERTY() FString PlanHash;
	UPROPERTY() FString AuthorizationPlanHash;
	UPROPERTY() FString CapabilityHash;
	UPROPERTY() FString EffectFingerprint;
	UPROPERTY() FString StageId;
	UPROPERTY() FHyperAIAnimationPlanEffects Effects;
	UPROPERTY() TArray<FHyperAIAnimationIssue> Issues;
	UPROPERTY() TArray<FHyperAIAnimationVariantStatus> Variants;
};

/** Exactly three tools form the Animation/Rigging atomic cohort. */
UCLASS()
class HYPERAISTUDIOANIMATION_API UHyperAIStudioAnimationToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Loaded animation assets (montages, composites, blend spaces, pose assets, sequences, skeletons) with revisions and elements. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIAnimationInspectReport hyper_animation_inspect(
		const FHyperAIAnimationInspectRequest& Request);

	/**
	 * Closed animation asset plans. One blend space's blend_space.create (ReferencePath = Skeleton; bFlag true for 1D),
	 * set_axis (AxisIndex, Name, Minimum, Maximum) and set_samples (SequencePath and Position each) run for real: submit
	 * with bDryRun false and an operation_id, then hyper_operation_wait. Other types only dry-run; use
	 * hyper_anim_blueprint_apply_plan for Animation Blueprints.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIAnimationApplyPlanReport hyper_animation_apply_plan(
		const FHyperAIAnimationApplyPlanRequest& Request);

	/** Fresh checks of loaded animation assets: skeletons, blend space samples, montage sections, notifies. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIAnimationValidateReport hyper_animation_validate(
		const FHyperAIAnimationValidateRequest& Request);
};

enum class EHyperAIStudioAnimationOperationKind : uint8
{
	AnimBlueprintCreate,
	AnimBlueprintSetSkeleton,
	AnimBlueprintAddStateMachine,
	AnimBlueprintAddState,
	AnimBlueprintAddTransition,
	AnimBlueprintSetTransitionRule,
	AnimBlueprintAddInterface,
	AnimBlueprintAddLayer,
	AnimBlueprintCompileRepair,
	MontageCreate,
	MontageAddSection,
	MontageRemoveSection,
	MontageLinkSections,
	MontageAddNotify,
	MontageRemoveNotify,
	CompositeCreate,
	CompositeSetSegments,
	BlendSpaceCreate,
	BlendSpaceSetAxis,
	BlendSpaceSetSamples,
	PoseAssetCreate,
	PoseAssetSetSource,
	PoseAssetSetPoseNames,
	SequenceAddCurve,
	SequenceRemoveCurve,
	SequenceAddNotify,
	SequenceRemoveNotify,
	SkeletonAddCompatible,
	SkeletonRemoveCompatible
};

struct FHyperAIStudioAnimationBackendOperation
{
	EHyperAIStudioAnimationOperationKind Kind = EHyperAIStudioAnimationOperationKind::AnimBlueprintCreate;
	FString Type;
	FString TargetPath;
	FString StableId;
	FString ExpectedRevision;
	FString ReferencePath;
	FString Name;
	FString SecondaryName;
	int32 AxisIndex = -1;
	double Minimum = 0.0;
	double Maximum = 0.0;
	bool bFlag = false;
	FHyperAIAnimationTransitionRuleSpec TransitionRule;
	FHyperAIAnimationNotifySpec Notify;
	TArray<FHyperAIAnimationSegmentSpec> Segments;
	TArray<FHyperAIAnimationBlendSampleSpec> Samples;
	TArray<FHyperAIAnimationNameMapping> Mappings;
	bool bCreatesTarget = false;
};

struct FHyperAIStudioAnimationValueSnapshot
{
	FString ScopeFingerprint;
	FString Revision;
	bool bComplete = true;
	int32 LoadedObjectsScanned = 0;
	double WorkDeadlineSeconds = 0.0;
	TArray<FHyperAIAnimationAssetRecord> Records;
	TArray<FHyperAIAnimationIssue> CaptureIssues;
};

/** Immutable closed DTO accepted only by the shared typed-artifact executor. */
class FHyperAIStudioAnimationTypedPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	TArray<FHyperAIStudioAnimationBackendOperation> Operations;
	FString BaseRevision;
	FString SemanticFingerprint;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe>
		CloneImmutable() const override;
};

class FHyperAIStudioAnimationResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FString Phase;
	FString Revision;
	bool bValid = false;
	int32 ErrorCount = 0;

	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

/** Inspect and validate reach the adapter too: a registered adapter must cover its whole cohort. */
class FHyperAIStudioAnimationInspectPayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIAnimationInspectRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioAnimationValidatePayload final : public IHyperAIStudioDomainRequestPayload
{
public:
	FHyperAIAnimationValidateRequest Request;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioAnimationInspectResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIAnimationInspectReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

class FHyperAIStudioAnimationValidateResultPayload final : public IHyperAIStudioDomainResultPayload
{
public:
	FHyperAIAnimationValidateReport Report;
	virtual FString GetTypeId() const override;
	virtual FString GetSchemaFingerprint() const override;
	virtual int32 GetBoundedByteSize() const override;
};

/** Executes blend space plans and the two reads; every other plan stays staged-only. */
class FHyperAIStudioAnimationDomainAdapter final : public IHyperAIStudioDomainAdapter
{
public:
	FHyperAIStudioAnimationDomainAdapter();
	virtual const FHyperAIStudioDomainAdapterDescriptor& GetDescriptor() const override;
	virtual FHyperAIStudioDomainAdapterResult Execute(
		const FHyperAIStudioDomainDispatchContext& Context,
		const IHyperAIStudioDomainRequestPayload& Payload) override;

private:
	FHyperAIStudioDomainAdapterDescriptor Descriptor;
};

struct FHyperAIStudioAnimationVariantDescriptor
{
	FString Variant;
	TArray<FString> RequiredPlugins;
	TArray<FString> RequiredModules;
	bool bInspectImplemented = false;
	bool bPlanSchemaImplemented = false;
	bool bApplyBackendExecutable = false;
	TArray<FString> SupportedCases;
	TArray<FString> DelegatedEpicCases;
	TArray<FString> UnsupportedCases;
};

class FHyperAIStudioAnimationFacade final
{
public:
	static const TArray<FHyperAIStudioAnimationVariantDescriptor>& GetVariantDescriptors();
	static TArray<FHyperAIAnimationVariantStatus> ResolveVariantStatuses();
	static bool CaptureLoaded(
		const TArray<FString>& AssetPaths,
		const FString& Variant,
		bool bIncludeDetails,
		FHyperAIStudioAnimationValueSnapshot& OutSnapshot,
		FString& OutStatus,
		FString& OutDiagnostic,
		int32 MaxWorkMs = 100);
};

struct FHyperAIStudioAnimationStagedArtifact
{
	FHyperAIStudioPreparedTypedArtifact Prepared;
	TSharedPtr<const FHyperAIStudioAnimationTypedPayload, ESPMode::ThreadSafe> Payload;
	FString CanonicalProjectId;
	FString OperationId;
	FString StageId;
	int64 ExpiresMonotonicMs = 0;
};

/** Bounded side-effect-free staging. There is deliberately no public claim/execute route. */
class FHyperAIStudioAnimationStagingService final
{
public:
	static bool Stage(
		const FHyperAIStudioAnimationStagedArtifact& Artifact,
		bool& bOutReplay,
		FString& OutError);
	static int32 NumStaged();
	static void Reset();
};

struct FHyperAIStudioAnimationManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioAnimationContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("animation_rigging");
	static constexpr const TCHAR* AtomicCohortId =
		TEXT("cohort.source.hyperaistudioanimationtoolset.v1");
	static constexpr const TCHAR* MutationVariantId = TEXT("base_animation_assets.v1");
	/** The catalog's animation_backend prerequisite this pack satisfies once its blend space backend is registered. */
	static constexpr const TCHAR* LiveProbeId = TEXT("probe.animation_editor");
	static constexpr const TCHAR* PayloadTypeId =
		TEXT("hyperai.payload.animation.base_animation_assets.v1");
	static constexpr const TCHAR* ResultTypeId =
		TEXT("hyperai.result.animation.base_animation_assets.v1");
	static constexpr const TCHAR* InspectVariantId = TEXT("animation_inspect.v1");
	static constexpr const TCHAR* ValidateVariantId = TEXT("animation_validate.v1");
	static constexpr const TCHAR* InspectPayloadTypeId = TEXT("hyperai.payload.animation.inspect.v1");
	static constexpr const TCHAR* ValidatePayloadTypeId = TEXT("hyperai.payload.animation.validate.v1");
	static constexpr const TCHAR* InspectResultTypeId = TEXT("hyperai.result.animation.inspect.v1");
	static constexpr const TCHAR* ValidateResultTypeId = TEXT("hyperai.result.animation.validate.v1");
	static constexpr int32 MaxAssetPaths = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxStableIdCharacters = 2048;
	static constexpr int32 MaxNameCharacters = 128;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxNestedItems = 256;
	static constexpr int32 MaxRecords = 4096;
	static constexpr int32 MaxElementsPerAsset = 2048;
	static constexpr int32 MaxLoadedObjectsScanned = 8192;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 192;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int64 StageLifetimeMs = 15000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioAnimationManifestEntry>& GetManifest();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsCanonicalProjectObjectPath(const FString& Path);
	static bool IsCanonicalSha256(const FString& Value);
	static FString PayloadSchemaFingerprint();
	static FString ResultSchemaFingerprint();
	static FString InspectPayloadSchemaFingerprint();
	static FString ValidatePayloadSchemaFingerprint();
	static FString InspectResultSchemaFingerprint();
	static FString ValidateResultSchemaFingerprint();
	static const FHyperAIStudioDomainAdapterDescriptor& GetBaseAdapterDescriptor();
	static bool ValidateOperationShape(
		const FHyperAIAnimationPlanOperation& Operation,
		FHyperAIStudioAnimationBackendOperation& OutOperation,
		FString& OutErrorCode,
		FString& OutError);
	static FString ComputeSnapshotRevision(FHyperAIStudioAnimationValueSnapshot& Snapshot);
	static TArray<FHyperAIAnimationIssue> ValidateValueSnapshot(
		const FHyperAIStudioAnimationValueSnapshot& Snapshot,
		bool bRequirePackagesClean,
		int32 MaxIssueCount,
		bool& bOutTruncated);
	static FString ComputePayloadSemanticFingerprint(
		const TArray<FHyperAIStudioAnimationBackendOperation>& Operations,
		const FString& BaseRevision);
	static FString ComputeStageId(
		const FString& CanonicalProjectId,
		const FString& OperationId,
		const FString& PlanHash,
		const FString& EffectFingerprint);
	static FHyperAIAnimationApplyPlanReport BuildPlan(
		const FHyperAIAnimationApplyPlanRequest& Request);
};

class FHyperAIStudioAnimationRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;

private:
	void RegisterAfterEngineInit();
	void RegisterBlendSpaceBackend();
	void RollBackBlendSpaceBackend();

	FDelegateHandle PostEngineInitHandle;
	TSharedPtr<FHyperAIStudioAnimationDomainAdapter, ESPMode::ThreadSafe> Adapter;
	FHyperAIStudioDomainRegistrationHandle AdapterHandle;
	FHyperAIStudioTrustedProbeRegistrationHandle ProbeHandle;
	FHyperAIStudioLiveProbePublisher ProbePublisher;
	bool bStarted = false;
	bool bOwnsRegistration = false;
};
