// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioTypedArtifactExecution.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "ToolsetRegistry/ToolCallAsyncResult.h"
#include "HyperAIStudioAudioToolset.generated.h"

struct FMetasoundFrontendDocument;

USTRUCT(BlueprintType)
struct FHyperAIAudioIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	int32 OperationIndex = -1;

	UPROPERTY()
	FString Message;
};

/** Per-family capability evidence. State is available, disabled, not_loaded, or unavailable. */
USTRUCT(BlueprintType)
struct FHyperAIAudioPrerequisite
{
	GENERATED_BODY()

	UPROPERTY()
	FString Family;

	UPROPERTY()
	FString Plugin;

	UPROPERTY()
	FString Module;

	UPROPERTY()
	FString State;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioRecord
{
	GENERATED_BODY()

	UPROPERTY()
	FString Variant;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString ObjectPath;

	UPROPERTY()
	FString PackageName;

	UPROPERTY()
	FString ClassPath;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bLoaded = false;

	UPROPERTY()
	bool bDirty = false;

	UPROPERTY()
	bool bEnabled = false;

	UPROPERTY()
	int32 PrimaryCount = 0;

	UPROPERTY()
	int32 SecondaryCount = 0;

	UPROPERTY()
	int32 TertiaryCount = 0;

	UPROPERTY()
	double DurationSeconds = 0.0;

	UPROPERTY()
	double SampleRate = 0.0;

	UPROPERTY()
	int32 NumChannels = 0;

	UPROPERTY()
	TArray<FString> References;

	UPROPERTY()
	TArray<FString> Details;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioInspectRequest
{
	GENERATED_BODY()

	/** loaded_only, on_disk_index, or loaded_and_on_disk. */
	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	/** Empty selects every closed audio family. */
	UPROPERTY()
	TArray<FString> Variants;

	/** Exact /Game object paths only. */
	UPROPERTY()
	TArray<FString> ObjectPaths;

	/** Allowlist: identity, package, metrics, graph, references, details. */
	UPROPERTY()
	TArray<FString> Projection;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;

	UPROPERTY()
	int32 DeadlineMs = 100;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioInspectReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString ObservationScope;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	int32 SourceObjectsScanned = 0;

	UPROPERTY()
	int32 TotalRecords = 0;

	UPROPERTY()
	int32 ReturnedRecords = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIAudioPrerequisite> Prerequisites;

	UPROPERTY()
	TArray<FHyperAIAudioRecord> Records;

	UPROPERTY()
	TArray<FHyperAIAudioIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioValidateRequest
{
	GENERATED_BODY()

	/** loaded_only, on_disk_index, or loaded_and_on_disk. */
	UPROPERTY()
	FString Scope = TEXT("loaded_only");

	/** Families to check, as in hyper_audio_inspect; empty checks them all. */
	UPROPERTY()
	TArray<FString> Variants;

	/** Exact /Game object paths only. */
	UPROPERTY()
	TArray<FString> ObjectPaths;

	UPROPERTY()
	int32 MaxIssues = 128;

	UPROPERTY()
	int32 DeadlineMs = 200;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioValidateReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	bool bRevisionComplete = false;

	UPROPERTY()
	int32 ErrorCount = 0;

	UPROPERTY()
	int32 WarningCount = 0;

	UPROPERTY()
	int32 InfoCount = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	TArray<FHyperAIAudioPrerequisite> Prerequisites;

	UPROPERTY()
	TArray<FHyperAIAudioIssue> Issues;
};

/** Closed typed value; exactly one value field is admitted by Type. */
USTRUCT(BlueprintType)
struct FHyperAIAudioParameterValue
{
	GENERATED_BODY()

	/** The semantic setting, such as volume, falloff_distance or attenuation. */
	UPROPERTY()
	FString Name;

	/** bool, int, float, string, or object. */
	UPROPERTY()
	FString Type;

	UPROPERTY()
	bool BoolValue = false;

	UPROPERTY()
	int32 IntValue = 0;

	UPROPERTY()
	double FloatValue = 0.0;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	FString ObjectPath;
};

/**
 * Closed audio plan operation. There is no raw DSP, sample buffer, filesystem path,
 * script, console command, arbitrary class, arbitrary MetaSound registry key, or playback command.
 */
USTRUCT(BlueprintType)
struct FHyperAIAudioPlanOperation
{
	GENERATED_BODY()

	/** The operation, such as sound_cue.create, sound_class.configure, sound_mix.add_adjuster or metasound.connect; hyper_audio_apply_plan lists them. */
	UPROPERTY()
	FString Variant;

	/** The asset changed or made, as an object path: /Game/Audio/SC_Steps.SC_Steps. */
	UPROPERTY()
	FString TargetPath;

	/** Required loaded-state CAS for existing targets; prohibited for creates. */
	UPROPERTY()
	FString ExpectedRevision;

	/** The asset a reference variant points at: set_root_wave a SoundWave, set_parent and add_adjuster a Sound Class, add_context a DialogueVoice. */
	UPROPERTY()
	FString ReferencePath;

	/** sound_cue_template, sound_utility.sound_simple and soundscape.palette: the sounds they play, as object paths. */
	UPROPERTY()
	TArray<FString> SourcePaths;

	/** metasound.add_input, add_output, add_variable, remove_member: the member name; declare_interface: source or source_one_shot. */
	UPROPERTY()
	FString MemberName;

	/** dialogue_wave contexts: the target. */
	UPROPERTY()
	FString SecondaryName;

	/** metasound members: bool, int32, float, string, trigger, audio, object, wave_asset or time; remove_member: input, output or variable. */
	UPROPERTY()
	FString DataType;

	/** Closed semantic allowlist, never an arbitrary class or registry key. */
	UPROPERTY()
	FString NodeKind;

	/** Exact closed descriptor schema version; currently 1 for add/configure_node only. */
	UPROPERTY()
	int32 NodeDescriptorVersion = 0;

	/** metasound.add_node: a new GUID for the node; configure_node, remove_node, set_literal: the existing node. */
	UPROPERTY()
	FGuid NodeId;

	UPROPERTY()
	FGuid VertexId;

	/** Semantic alternative to VertexId for a versioned descriptor node. */
	UPROPERTY()
	FString VertexName;

	UPROPERTY()
	FGuid FromNodeId;

	UPROPERTY()
	FGuid FromVertexId;

	/** Semantic alternative to FromVertexId for a versioned descriptor node. */
	UPROPERTY()
	FString FromPortName;

	UPROPERTY()
	FGuid ToNodeId;

	UPROPERTY()
	FGuid ToVertexId;

	/** Semantic alternative to ToVertexId for a versioned descriptor node. */
	UPROPERTY()
	FString ToPortName;

	/** The position in an ordered list (mix stages, effect chains, mutators, components); removals need it. */
	UPROPERTY()
	int32 Index = -1;

	UPROPERTY()
	int32 Count = 0;

	/** sound_mix adjusters: volume (0-4); modulation stages, effects, mutators and MotorSim components: their scalar. */
	UPROPERTY()
	double Value = 0.0;

	/** sound_mix adjusters: pitch (0-8). */
	UPROPERTY()
	double SecondaryValue = 0.0;

	UPROPERTY()
	bool bSetEnabled = false;

	UPROPERTY()
	bool bEnabled = false;

	/** audio_gameplay_volume.create: where the volume goes and its half size. */
	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FVector Extent = FVector::ZeroVector;

	/** Typed settings by semantic name (volume, pitch, falloff_distance...); set_literal takes one named value. */
	UPROPERTY()
	TArray<FHyperAIAudioParameterValue> Parameters;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioApplyPlanRequest
{
	GENERATED_BODY()

	/** true checks the plan and returns PlanHash; false (with ExpectedPlanHash) submits it for approval and waits for the edit. */
	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	FString OperationId;

	/** The PlanHash from the dry run of the same Operations. */
	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	int32 DeadlineMs = 1000;

	/** Applied in order in one undo step, up to 64. */
	UPROPERTY()
	TArray<FHyperAIAudioPlanOperation> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioPlanEffects
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 CreateCount = 0;

	UPROPERTY()
	int32 UpdateCount = 0;

	UPROPERTY()
	int32 RemoveCount = 0;

	UPROPERTY()
	int32 MetaSoundGraphOperationCount = 0;

	UPROPERTY()
	int32 OptionalPluginOperationCount = 0;

	UPROPERTY()
	bool bMayRemovePersistedData = false;

	UPROPERTY()
	bool bPlaybackRequested = false;

	UPROPERTY()
	bool bMicrophoneCaptureRequested = false;

	UPROPERTY()
	bool bTransactionOnce = true;

	UPROPERTY()
	bool bSaveOnce = true;

	UPROPERTY()
	bool bCompileOnce = true;

	UPROPERTY()
	bool bValidateOnce = true;

	UPROPERTY()
	bool bFreshVerifyOnce = true;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioApplyPlanReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	bool bRequiresTrustedAuthorization = true;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString SafetyClass = TEXT("external_effect");

	UPROPERTY()
	FString BaseRevision;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString AuthorizationPlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString EffectFingerprint;

	UPROPERTY()
	FString PreparedContractFingerprint;

	UPROPERTY()
	FHyperAIAudioPlanEffects Effects;

	UPROPERTY()
	TArray<FHyperAIAudioPrerequisite> Prerequisites;

	UPROPERTY()
	TArray<FHyperAIAudioIssue> Issues;
};

/** hyper_audio_apply_plan's reply, held open until the edit it submitted has finished: one call instead of an apply and a wait. */
UCLASS()
class UHyperAIAudioApplyPlanResult final : public UToolCallAsyncResult
{
	GENERATED_BODY()

public:
	bool SetValue(const FHyperAIAudioApplyPlanReport& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(FHyperAIAudioApplyPlanReport(InValue), Value);
	}

	UPROPERTY()
	FHyperAIAudioApplyPlanReport Value;
};

UCLASS()
class HYPERAISTUDIOAUDIO_API UHyperAIStudioAudioToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Lists the project's audio assets as records: SoundWaves, Sound Cues, Sound Classes, Sound Mixes, attenuation,
	 * concurrency, dialogue, MetaSound Sources and Patches, and the optional plugins' assets (modulation, wave tables,
	 * synthesis presets, Synesthesia analyzers, Soundscape, Audio Gameplay Volumes, MotorSim). Each record carries its
	 * Revision, the CAS that hyper_audio_apply_plan's ExpectedRevision takes. Variants filters by family (sound_wave,
	 * sound_cue, metasound_source...); ObjectPaths by exact /Game/Folder/Asset.Asset paths. Scope loaded_only (the
	 * default) reads loaded assets; Prerequisites says which optional audio plugins are enabled. Changes nothing.
	 * For a Sound Cue's graph, a mix's settings or a MetaSound's inputs and nodes, hyper_sound_asset_inspect is simpler.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static FHyperAIAudioInspectReport hyper_audio_inspect(const FHyperAIAudioInspectRequest& Request);

	/**
	 * Low-level typed audio edits, a list of Operations applied in one undo step: first with bDryRun true (the default),
	 * which returns PlanHash, then the same Operations with bDryRun false and ExpectedPlanHash, which waits for approval.
	 * Each operation's Variant decides which fields it uses (others must stay empty): *.create (TargetPath a free
	 * /Game/Folder/Name.Name, no ExpectedRevision) and *.configure (TargetPath, ExpectedRevision from hyper_audio_inspect,
	 * Parameters by semantic name: sound_wave volume, pitch, looping, streaming, compression_quality; sound_cue
	 * volume_multiplier, pitch_multiplier, attenuation, concurrency; sound_class volume, pitch, low_pass_frequency, parent,
	 * default_submix; sound_mix initial_delay, duration, fade_in_time, fade_out_time, apply_eq; attenuation inner_radius,
	 * falloff_distance, spatialize...; concurrency max_count, resolution_rule, retrigger_time...). Reference variants take
	 * ReferencePath: sound_cue.set_root_wave (a SoundWave), sound_class.set_parent, sound_mix.add_adjuster and
	 * remove_adjuster (a Sound Class; Value volume 0-4, SecondaryValue pitch 0-8). MetaSound variants (metasound.source.create,
	 * add_input/add_output/add_variable with MemberName and DataType bool, int32, float, string, trigger, audio, wave_asset
	 * or time, add_node and configure_node with NodeKind oscillator, gain, random, math_add or math_multiply and
	 * NodeDescriptorVersion 1, connect and disconnect by node GUIDs and port names, set_literal) cover only a fixed handful of
	 * nodes: build real MetaSounds with hyper_metasound_graph_plan (any registered node) or hyper_metasound_recipe instead,
	 * and Sound Cues with hyper_sound_cue_plan. Never plays audio.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static UHyperAIAudioApplyPlanResult* hyper_audio_apply_plan(const FHyperAIAudioApplyPlanRequest& Request);

	/**
	 * Checks audio assets for broken references, empty or dangling Sound Cue graphs, MetaSound documents that do not
	 * validate, and disabled optional plugins, as Issues with a severity (error, warning, info). Same Scope, Variants and
	 * ObjectPaths as hyper_audio_inspect. Changes nothing. To hear whether a sound works, render and measure it with
	 * hyper_audio_analyze.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static FHyperAIAudioValidateReport hyper_audio_validate(const FHyperAIAudioValidateRequest& Request);
};

struct FHyperAIAudioBackendMetaPort
{
	FString Name;
	FName TypeName;
	FGuid VertexId;
	int32 AccessType = 0;
	bool bLiteralSettable = false;
	FString ConfigureParameter;
};

struct FHyperAIAudioBackendOperation
{
	FString Variant;
	FString TargetPath;
	FString ExpectedRevision;
	FString ReferencePath;
	TArray<FString> SourcePaths;
	FString MemberName;
	FString SecondaryName;
	FString DataType;
	FString NodeKind;
	int32 NodeDescriptorVersion = 0;
	/** Exact UE registry key and reflected class-interface seal used for authoring/replay. */
	FString MetaSoundRegistryKey;
	FString MetaSoundInterfaceFingerprint;
	FGuid NodeId;
	FGuid VertexId;
	FString VertexName;
	FGuid FromNodeId;
	FGuid FromVertexId;
	FString FromPortName;
	FGuid ToNodeId;
	FGuid ToVertexId;
	FString ToPortName;
	TArray<FHyperAIAudioBackendMetaPort> DescriptorInputs;
	TArray<FHyperAIAudioBackendMetaPort> DescriptorOutputs;
	int32 Index = -1;
	int32 Count = 0;
	double Value = 0.0;
	double SecondaryValue = 0.0;
	bool bSetEnabled = false;
	bool bEnabled = false;
	FVector Location = FVector::ZeroVector;
	FVector Extent = FVector::ZeroVector;
	TArray<FHyperAIAudioParameterValue> Parameters;
	FString OptionalFamily;
};

class FHyperAIAudioTypedArtifactPayload final : public IHyperAIStudioTypedArtifactPayload
{
public:
	FString TypeId;
	FString SchemaFingerprint;
	FString PackId;
	FString BaseRevision;
	TArray<FString> CanonicalOperations;
	/** Final per-target typed state after ordered semantic replay. */
	TArray<FString> CanonicalShadowStates;

	virtual FString GetTypeId() const override { return TypeId; }
	virtual FString GetSchemaFingerprint() const override { return SchemaFingerprint; }
	virtual int32 GetBoundedByteSize() const override;
	virtual FString GetSemanticFingerprint() const override;
	virtual TSharedRef<const IHyperAIStudioTypedArtifactPayload, ESPMode::ThreadSafe> CloneImmutable() const override;
};

struct FHyperAIAudioValueSnapshot
{
	FString Scope;
	FString Revision;
	bool bComplete = true;
	int32 Scanned = 0;
	TArray<FHyperAIAudioPrerequisite> Prerequisites;
	TArray<FHyperAIAudioRecord> Records;
	TArray<FHyperAIAudioIssue> Issues;
};

struct FHyperAIAudioManifestEntry
{
	FString Name;
	FString QualifiedToolset;
};

class FHyperAIStudioAudioContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("audio_metasound");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioaudiotoolset.v1");
	static constexpr int32 MaxPaths = 128;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxVariants = 32;
	static constexpr int32 MaxProjectionFields = 8;
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxParametersPerOperation = 64;
	static constexpr int32 MaxTotalParameters = 512;
	static constexpr int32 MaxRecords = 4096;
	static constexpr int32 MaxScannedObjects = 8192;
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxIssues = 128;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MaxInspectDeadlineMs = 500;
	static constexpr int32 MaxApplyDeadlineMs = 2000;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIAudioManifestEntry>& GetManifest();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static bool IsInspectVariant(const FString& Variant);
	static bool IsCanonicalProjectAssetPath(const FString& Path);
	static bool ValidateOperationShape(const FHyperAIAudioPlanOperation& In, FHyperAIAudioBackendOperation& Out, FString& OutError);
	static FString ComputeLoadedTargetRevision(const FString& ExactObjectPath);
	static bool Capture(const FHyperAIAudioInspectRequest& Request, FHyperAIAudioValueSnapshot& Out, FString& OutError);
	static FString ComputeSnapshotRevision(FHyperAIAudioValueSnapshot& Snapshot);
	static TArray<FHyperAIAudioIssue> ValidateSnapshot(const FHyperAIAudioValueSnapshot& Snapshot, int32 MaxIssueCount, bool& bOutTruncated);
	/** hyper_audio_apply_plan's plan with an agent's near misses fitted first; the tool then waits for the edit it submits. */
	static FHyperAIAudioApplyPlanReport BuildFittedPlan(const FHyperAIAudioApplyPlanRequest& Request);
	static FHyperAIAudioApplyPlanReport BuildPlan(const FHyperAIAudioApplyPlanRequest& Request);
#if WITH_DEV_AUTOMATION_TESTS
	static FString ComputeOnDiskEvidenceRevisionForTest(
		const FString& ObjectPath, const FString& PackageName, const FString& ClassPath,
		int64 DiskSize, const FString& SavedHash, bool bHasPackageData);
	static FString ClassifyClassEvidenceForTest(
		const FString& ExactClassPath, const TArray<FString>& AncestorClassPaths);
	static bool IsClassEvidenceCompatibleForTest(
		const FString& ParameterName, const FString& ExactClassPath,
		const TArray<FString>& AncestorClassPaths);
	static bool ValidateMetaSoundDocumentForTest(
		const FMetasoundFrontendDocument& Document, FString& OutRevision,
		bool& bOutTraversalComplete);
		static bool IsCreateExistenceStateAdmittedForTest(
			int32 ExistsState, bool bRegistryAvailable);
	static bool IsSparseContainerShapeBoundedForTest(int32 Num, int32 MaxIndex);
	static bool ValidateCoupledTupleForTest(
		const FString& ClassKind, const TMap<FString, double>& KnownValues,
		const TSet<FString>& TouchedKeys, FString& OutError);
	#endif
};

class FHyperAIStudioAudioRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsRegistration = false;
};
