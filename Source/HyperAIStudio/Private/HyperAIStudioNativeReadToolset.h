// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioCapabilityCatalogCounts.h"
#include "HyperAIStudioOperationJournal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "ToolsetRegistry/ToolCallAsyncResult.h"
#include "HyperAIStudioNativeReadToolset.generated.h"

class FProperty;

/** One known HyperAI tool with explicit admission, registration, and callable state. */
USTRUCT(BlueprintType)
struct FHyperAINativeToolSummary
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Toolset;

	UPROPERTY()
	FString PackId;

	UPROPERTY()
	FString AtomicCohortId;

	/** Generated catalog policy only; deliberately not a read/edit/destructive safety class. */
	UPROPERTY()
	FString ExternalEffectPolicy;

	/** Clear product-facing support level: Read, Validate, PlanOnly, LimitedDirectEdit, or OperationSpecific. */
	UPROPERTY()
	FString ExecutionSupport;

	UPROPERTY()
	FString AdmissionState;

	UPROPERTY()
	FString Availability;

	UPROPERTY()
	bool bToolsetRegistered = false;

	/** Exact Epic ToolsetRegistry per-tool enabled/filter state. */
	UPROPERTY()
	bool bRuntimeEnabled = false;

	UPROPERTY()
	bool bImplementationLoaded = false;

	UPROPERTY()
	bool bCallable = false;

	UPROPERTY()
	int32 SourceArtifactCount = 0;

	UPROPERTY()
	FString Description;
};

/** Bounded summary of one toolset from HyperAIStudio's existing cached Epic inventory. */
USTRUCT(BlueprintType)
struct FHyperAIEpicToolsetSummary
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Version;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString SchemaHash;

	UPROPERTY()
	int32 ToolCount = 0;

	UPROPERTY()
	FString InventoryState;
};

/** Installed capability and health report. Scope and admission come from the complete generated catalog. */
USTRUCT(BlueprintType)
struct FHyperAICapabilityReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	/** Concise user/agent guidance without internal lifecycle terminology. */
	UPROPERTY()
	FString DiagnosticSummary;

	UPROPERTY()
	FString SnapshotUtc;

	UPROPERTY()
	FString EngineVersion;

	UPROPERTY()
	FString HyperAIPluginVersion;

	UPROPERTY()
	FString EpicMcpPluginVersion;

	UPROPERTY()
	FString ProjectName;

	UPROPERTY()
	FString ProjectIdentityHash;

	/** Explicit versioned meaning of ProjectIdentityHash; the opaque identifier itself stays journal-compatible. */
	UPROPERTY()
	FString ProjectIdentityScheme;

	UPROPERTY()
	FString ProjectIdentityHashAlgorithm;

	UPROPERTY()
	FString ProjectIdentityStatus;

	UPROPERTY()
	FString CapabilityCatalogFingerprint;

	UPROPERTY()
	int32 TotalKnownHyperAIToolCount = 0;

	/** HyperAI tools included by the selected product tool set; zero for Unreal MCP Only. */
	UPROPERTY()
	int32 IncludedHyperAIToolCount = 0;

	UPROPERTY()
	FString NativeToolset;

	UPROPERTY()
	bool bNativeToolsetRegistered = false;

	UPROPERTY()
	FString DependencyGraphToolset;

	UPROPERTY()
	bool bDependencyGraphToolsetRegistered = false;

	UPROPERTY()
	int32 CallableHyperAIToolCount = 0;

	UPROPERTY()
	TArray<FHyperAINativeToolSummary> CallableHyperAITools;

	UPROPERTY()
	FString NativeToolChannel;

	/** Product-facing alias for whether Unreal MCP + Extended Hyper Tools is selected. */
	UPROPERTY()
	bool bExtendedHyperToolsEnabled = false;

	UPROPERTY()
	bool bSourceCandidateToolsEnabled = false;

	UPROPERTY()
	bool bRuntimeIndexValid = false;

	UPROPERTY()
	int32 RuntimeImplementedHyperAIToolCount = 0;

	UPROPERTY()
	int32 LoadedHyperAIToolsetCount = 0;

	UPROPERTY()
	int32 RegisteredHyperAIToolsetCount = 0;

	UPROPERTY()
	int32 RegisteredHyperAIToolCount = 0;

	/** Loaded implementations whose generated admission state requires ToolsetRegistry registration now. */
	UPROPERTY()
	int32 ExpectedRegisteredHyperAIToolCount = 0;

	/** Actual-vs-expected registration differences; unloaded optional cohorts are not mismatches. */
	UPROPERTY()
	int32 RegistrationMismatchCount = 0;

	UPROPERTY()
	TArray<FString> RuntimeIndexDiagnostics;

	UPROPERTY()
	int32 PlannedHyperAIToolCount = 0;

	UPROPERTY()
	int32 SourceCandidateHyperAIToolCount = 0;

	UPROPERTY()
	int32 AdmittedHyperAIToolCount = 0;

	/** Every known implemented/planned HyperAI entry, including non-callable source candidates. */
	UPROPERTY()
	TArray<FHyperAINativeToolSummary> NativeTools;

	UPROPERTY()
	FString CurrentStatus;

	UPROPERTY()
	FString McpEndpoint;

	UPROPERTY()
	bool bMcpServerRunning = false;

	UPROPERTY()
	bool bMcpPortListening = false;

	UPROPERTY()
	bool bMcpToolsListReachable = false;

	UPROPERTY()
	bool bMcpToolSearchMode = false;

	UPROPERTY()
	int32 McpTopLevelRegisteredToolCount = 0;

	UPROPERTY()
	bool bCachedEpicInventoryAvailable = false;

	UPROPERTY()
	bool bCachedEpicInventoryStale = false;

	UPROPERTY()
	bool bCachedEpicInventoryRefreshing = false;

	UPROPERTY()
	bool bCachedEpicInventorySourceTruncated = false;

	UPROPERTY()
	FString CachedEpicInventoryFingerprint;

	UPROPERTY()
	FString CachedEpicInventoryCapturedUtc;

	UPROPERTY()
	FString CachedEpicDiscoveryMode;

	UPROPERTY()
	int32 CachedEpicToolsetCount = 0;

	UPROPERTY()
	int32 CachedEpicDescribedToolCount = 0;

	UPROPERTY()
	int32 PageOffset = 0;

	UPROPERTY()
	int32 PageSize = 0;

	UPROPERTY()
	int32 ReturnedEpicToolsetCount = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIEpicToolsetSummary> CachedEpicToolsets;
};

/** Reconciled state for one client-known HyperAI mutation operation. */
USTRUCT(BlueprintType)
struct FHyperAIOperationStatus
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString SnapshotUtc;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	bool bFound = false;

	UPROPERTY()
	FString State;

	UPROPERTY()
	FString RollbackState;

	UPROPERTY()
	FString StatusCode;

	UPROPERTY()
	FString PlanHash;

	UPROPERTY()
	FString CapabilityHash;

	UPROPERTY()
	FString CreatedUtc;

	UPROPERTY()
	FString UpdatedUtc;

	UPROPERTY()
	bool bTerminal = false;

	UPROPERTY()
	bool bNeedsReconciliation = false;

	UPROPERTY()
	bool bRetrySafe = false;

	UPROPERTY()
	bool bPartialCommit = false;

	UPROPERTY()
	FString ResolutionValidatorHash;

	UPROPERTY()
	FString ResolutionPostconditionHash;

	UPROPERTY()
	FString JournalAccessMode;

	UPROPERTY()
	bool bJournalLoadMayReconcile = true;

	UPROPERTY()
	int32 ReconciledRecordCount = 0;

	UPROPERTY()
	int64 JournalGeneration = 0;

	UPROPERTY()
	FString ProjectIdentityHash;

	/** The executor phase running now (apply, compile, validate, save, verify_fresh), while it runs. */
	UPROPERTY()
	FString Phase;

	UPROPERTY()
	int32 PhasesDone = 0;

	UPROPERTY()
	int32 PhasesTotal = 0;

	/** Progress of a background job that belongs to this operation, such as a capture or a batch. */
	UPROPERTY()
	FString JobProgress;

	/** What to do next, in one sentence. */
	UPROPERTY()
	FString NextAction;
};

USTRUCT(BlueprintType)
struct FHyperAIOperationWaitReport
{
	GENERATED_BODY()

	/** The operation's status when the wait ended, exactly as hyper_operation_status reports it. */
	UPROPERTY()
	FHyperAIOperationStatus Status;

	/** terminal, awaiting_user_approval, rejected, not_found, timeout, invalid_request or too_many_waiters. */
	UPROPERTY()
	FString CompletedBecause;

	UPROPERTY()
	int32 WaitedMs = 0;
};

/**
 * The pending result of hyper_operation_wait. The MCP server holds the agent's request open until this
 * completes, while the editor keeps ticking, so the executor finishes the very operation being waited on.
 */
UCLASS()
class UHyperAIOperationWaitResult final : public UToolCallAsyncResult
{
	GENERATED_BODY()

public:
	bool SetValue(const FHyperAIOperationWaitReport& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(FHyperAIOperationWaitReport(InValue), Value);
	}

	UPROPERTY()
	FHyperAIOperationWaitReport Value;
};

/** One remembered fact, as hyper_memory_recall returns it. */
USTRUCT(BlueprintType)
struct FHyperAIMemoryFactView
{
	GENERATED_BODY()

	/** Pass it as Replaces to update the fact, or to hyper_memory_forget. */
	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Title;

	/** convention, decision, fact or gotcha. */
	UPROPERTY()
	FString Type;

	/** user or agent. */
	UPROPERTY()
	FString Source;

	/** False for an agent's fact the user has not reviewed yet. */
	UPROPERTY()
	bool bConfirmed = true;

	/** YYYY-MM-DD. */
	UPROPERTY()
	FString Updated;

	UPROPERTY()
	FString Text;
};

USTRUCT(BlueprintType)
struct FHyperAIMemoryRecallReport
{
	GENERATED_BODY()

	/** ok or invalid_request. */
	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	/** Best matches first. */
	UPROPERTY()
	TArray<FHyperAIMemoryFactView> Facts;

	/** What agents changed lately on this machine, newest first, one line each. */
	UPROPERTY()
	TArray<FString> RecentChanges;
};

USTRUCT(BlueprintType)
struct FHyperAIMemoryWriteReport
{
	GENERATED_BODY()

	/** saved, forgotten, refused or invalid_request. */
	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	/** The fact's name. */
	UPROPERTY()
	FString Name;
};

/** PIE identity attached to a debug object or returned as a PIE world result. */
USTRUCT(BlueprintType)
struct FHyperAIPIEWorldIdentity
{
	GENERATED_BODY()

	UPROPERTY()
	bool bPresent = false;

	UPROPERTY()
	FString ContextHandle;

	UPROPERTY()
	int32 PieInstance = INDEX_NONE;

	UPROPERTY()
	int32 PackagePieInstance = INDEX_NONE;

	UPROPERTY()
	bool bPrimaryInstance = false;

	UPROPERTY()
	bool bDedicatedServer = false;

	UPROPERTY()
	FString WorldType;

	UPROPERTY()
	FString NetMode;

	UPROPERTY()
	FString WorldPath;

	UPROPERTY()
	FString WorldPackageName;

	UPROPERTY()
	FString SourcePackageName;

	UPROPERTY()
	bool bHasAuthority = false;

	UPROPERTY()
	bool bBegunPlay = false;

	UPROPERTY()
	bool bPaused = false;

	UPROPERTY()
	int32 ActorCount = 0;

	UPROPERTY()
	int32 PlayerControllerCount = 0;

	UPROPERTY()
	int32 LocalPlayerCount = 0;

	UPROPERTY()
	FString GameModeClass;

	UPROPERTY()
	FString GameStateClass;

	UPROPERTY()
	double TimeSeconds = 0.0;

	UPROPERTY()
	double RealTimeSeconds = 0.0;

	UPROPERTY()
	double DeltaTimeSeconds = 0.0;
};

/** Stable PIE identity used by paged Blueprint debugger snapshots; excludes ticking runtime state. */
USTRUCT(BlueprintType)
struct FHyperAIPIEDebugIdentity
{
	GENERATED_BODY()

	UPROPERTY()
	bool bPresent = false;

	UPROPERTY()
	FString ContextHandle;

	UPROPERTY()
	int32 PieInstance = INDEX_NONE;

	UPROPERTY()
	int32 PackagePieInstance = INDEX_NONE;

	UPROPERTY()
	bool bPrimaryInstance = false;

	UPROPERTY()
	bool bDedicatedServer = false;

	UPROPERTY()
	FString WorldType;

	UPROPERTY()
	FString NetMode;

	UPROPERTY()
	FString WorldPath;

	UPROPERTY()
	FString WorldPackageName;

	UPROPERTY()
	FString SourcePackageName;
};

/** One bounded Blueprint debugger observation. */
USTRUCT(BlueprintType)
struct FHyperAIBlueprintDebugItem
{
	GENERATED_BODY()

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString State;

	UPROPERTY()
	FString GraphName;

	UPROPERTY()
	FString GraphPath;

	UPROPERTY()
	FString GraphGuid;

	UPROPERTY()
	FString NodeGuid;

	UPROPERTY()
	FString PinGuid;

	UPROPERTY()
	FString NodeClass;

	UPROPERTY()
	FString NodeTitle;

	UPROPERTY()
	FString PinName;

	UPROPERTY()
	FString PinDirection;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	FString Message;

	UPROPERTY()
	bool bEnabled = false;

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	FString PropertyPath;

	UPROPERTY()
	bool bPropertyPathTruncated = false;

	UPROPERTY()
	FString ValuePreview;

	UPROPERTY()
	bool bValueTruncated = false;
};

/** Bounded Blueprint compile/debug snapshot with stable graph, node, pin, and PIE identities. */
USTRUCT(BlueprintType)
struct FHyperAIBlueprintDebugReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString SnapshotUtc;

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString AssetClass;

	UPROPERTY()
	FString CompileStatus;

	UPROPERTY()
	FString GeneratedClassPath;

	UPROPERTY()
	bool bHasDebuggingData = false;

	UPROPERTY()
	FString DebugObjectPath;

	UPROPERTY()
	FHyperAIPIEDebugIdentity DebugPieIdentity;

	UPROPERTY()
	FString GraphFilter;

	UPROPERTY()
	int32 GraphCount = 0;

	UPROPERTY()
	int32 ScannedGraphCount = 0;

	UPROPERTY()
	int32 ScannedNodeCount = 0;

	UPROPERTY()
	int32 BreakpointCount = 0;

	UPROPERTY()
	int32 WatchCount = 0;

	/** identity_and_bounded_scalar: containers/complex or oversized strings are never exported. */
	UPROPERTY()
	FString WatchValuePolicy = TEXT("identity_and_bounded_scalar");

	UPROPERTY()
	int32 DiagnosticNodeCount = 0;

	UPROPERTY()
	int32 TotalItemCount = 0;

	UPROPERTY()
	int32 PageOffset = 0;

	UPROPERTY()
	int32 PageSize = 0;

	UPROPERTY()
	int32 ReturnedItemCount = 0;

	UPROPERTY()
	bool bScanTruncated = false;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIBlueprintDebugItem> Items;
};

/** Explicit snapshot of every observable in-process PIE world. */
USTRUCT(BlueprintType)
struct FHyperAIPIEQueryReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString SnapshotUtc;

	UPROPERTY()
	bool bPlaySessionActive = false;

	UPROPERTY()
	bool bRunUnderOneProcess = true;

	UPROPERTY()
	bool bMultiprocessConfiguration = false;

	UPROPERTY()
	FString TopologySource;

	UPROPERTY()
	bool bFrozenTopologyAvailable = false;

	UPROPERTY()
	bool bServerWasLaunched = false;

	UPROPERTY()
	FString ConfiguredPlayNetMode;

	UPROPERTY()
	int32 ConfiguredClientCount = 0;

	UPROPERTY()
	int32 PieInstanceFilter = INDEX_NONE;

	UPROPERTY()
	int32 ObservableWorldCount = 0;

	UPROPERTY()
	int32 MatchingWorldCount = 0;

	UPROPERTY()
	int32 ReturnedWorldCount = 0;

	UPROPERTY()
	bool bSourceTruncated = false;

	UPROPERTY()
	TArray<FHyperAIPIEWorldIdentity> Worlds;
};

/**
 * First clean-room native HyperAI toolset. Its four functions are one atomic
 * ToolsetRegistry admission cohort: every member needs packaged/live evidence
 * before the cohort can move from SourceCandidate to Admitted.
 */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioNativeReadToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/** Return installed HyperAI tools, current health, and a page of the existing cached Epic inventory. No I/O refresh is started. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Capabilities")
	static FHyperAICapabilityReport hyper_capability_report(
		int32 PageSize = 32,
		const FString& Cursor = TEXT(""));

	/**
	 * Read one operation state from the live registry or a validated immutable journal generation.
	 * This read never takes mutation ownership, reconciles records, or writes journal state.
	 * A concurrent mutation/config owner returns structured status `busy`.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Workflow")
	static FHyperAIOperationStatus hyper_operation_status(const FString& OperationId);

	/**
	 * Wait for one operation to finish, be rejected, or need approval, then return its status: one call
	 * instead of polling. TimeoutMs 1000-120000, default 30000. Set bWaitThroughApproval to keep waiting
	 * while the user decides. The editor keeps running while this waits.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Workflow")
	static UHyperAIOperationWaitResult* hyper_operation_wait(
		const FString& OperationId,
		int32 TimeoutMs = 30000,
		bool bWaitThroughApproval = false);

	/** Inspect bounded Blueprint node diagnostics, breakpoints, watched pins, and the selected debug object's PIE identity. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintDebugReport hyper_blueprint_debug_inspect(
		const FString& BlueprintAssetPath,
		const FString& GraphName = TEXT(""),
		int32 PageSize = 32,
		const FString& Cursor = TEXT(""));

	/** Return one complete bounded snapshot of every matching in-process PIE world. Active multiprocess PIE fails closed because other processes are not observable. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|PIE")
	static FHyperAIPIEQueryReport hyper_pie_query(int32 PieInstance = -1);

	/**
	 * Project memory: facts about this project every agent shares, best matches first, plus what agents changed
	 * lately. Query is a few keywords; empty returns the newest facts. Target is an asset path to narrow the
	 * changes to, or all.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Memory")
	static FHyperAIMemoryRecallReport hyper_memory_recall(const FString& Query, int32 Limit = 10, const FString& Target = TEXT("all"));

	/**
	 * Save a lasting fact about this project for every future chat: a convention, a decision or a gotcha, never task
	 * progress. Title is one line; Text up to 4000 characters; Type is convention, decision, fact or gotcha. Replaces
	 * is new, or the name of a fact you saved earlier, to update it. The user reviews what agents save.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Memory")
	static FHyperAIMemoryWriteReport hyper_memory_remember(
		const FString& Title,
		const FString& Text,
		const FString& Type = TEXT("fact"),
		const FString& Replaces = TEXT("new"));

	/** Delete a fact an agent saved, by name. Facts the user wrote stay; ask the user to remove those. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Memory")
	static FHyperAIMemoryWriteReport hyper_memory_forget(const FString& Name);
};

struct FHyperAIStudioNativeToolManifestEntry
{
	FString Name;
	FString Toolset;
	FString Safety;
	FString Description;
	enum class EAdmissionState : uint8
	{
		Planned,
		SourceCandidate,
		Admitted
	} AdmissionState = EAdmissionState::Planned;
};

/** Value-only PIE topology input used to test active-session/current-settings separation. */
struct FHyperAIStudioPIETopologyConfig
{
	FString PlayNetMode = TEXT("standalone");
	bool bRunUnderOneProcess = true;
	int32 ClientCount = 1;
	bool bLaunchSeparateServer = false;
	bool bServerWasLaunched = false;
	bool bExternalSessionDestination = false;
};

struct FHyperAIStudioPIETopologyResolution
{
	bool bValid = false;
	bool bUsedFrozenActiveSession = false;
	bool bMultiprocess = true;
	FString Source;
	FHyperAIStudioPIETopologyConfig Effective;
};

/** Pure contract helpers kept public within the module for automation tests. */
class FHyperAIStudioNativeReadContracts final
{
public:
	static constexpr int32 MaxPageSize = 64;
	static constexpr int32 MaxCatalogToolCount = HyperAIStudio::CapabilityCatalog::GeneratedToolCount;
	static constexpr int32 MaxCursorCharacters = 160;
	static constexpr int32 MaxDiagnosticCharacters = 512;
	static constexpr int32 MaxPathCharacters = 1024;
	static constexpr int32 MaxBlueprintGraphs = 256;
	static constexpr int32 MaxBlueprintNodes = 10000;
	static constexpr int32 MaxBlueprintDebugItems = 2048;
	static constexpr int32 MaxBlueprintBreakpoints = 512;
	static constexpr int32 MaxBlueprintWatches = 512;
	static constexpr int32 MaxPieWorlds = 64;

	static const TArray<FHyperAIStudioNativeToolManifestEntry>& GetCallableManifest();
	static FString GetQualifiedToolsetName();
	static FString GetDependencyGraphQualifiedToolsetName();
	static FString AdmissionStateToString(FHyperAIStudioNativeToolManifestEntry::EAdmissionState State);
	static bool IsPendingNativeToolsTestEnabled();
	/** ToolsetRegistry admission is class-wide; every entry in one toolset cohort must share one non-Planned state. */
	static bool IsToolsetRegistrationAllowed(const FString& Toolset, bool bAllowPendingForTests);
	static bool IsAdmissionCohortRegistrationAllowed(
		const TArray<FHyperAIStudioNativeToolManifestEntry>& Manifest,
		const FString& Toolset,
		bool bAllowPendingForTests);
	/** Registration is expected only for a loaded implementation whose generated admission permits calls. */
	static bool IsCapabilityRegistrationExpected(
		const FHyperAINativeToolSummary& Tool,
		bool bAllowSourceCandidatesForTests);
	/** Counts actual-vs-expected registration differences without penalizing unloaded optional packs. */
	static int32 CountCapabilityRegistrationMismatches(
		const TArray<FHyperAINativeToolSummary>& Tools,
		bool bAllowSourceCandidatesForTests,
		int32& OutExpectedRegisteredToolCount);
	/** Verifies per-row callable/registration truth without requiring unavailable optional modules to load. */
	static bool AreCapabilityRuntimeRowsConsistent(
		const TArray<FHyperAINativeToolSummary>& Tools,
		bool bAllowSourceCandidatesForTests,
		int32& OutExpectedRegisteredToolCount,
		int32& OutRegistrationMismatchCount);
	static FHyperAIStudioPIETopologyResolution ResolvePieTopology(
		bool bActiveSession,
		bool bFrozenConfigAvailable,
		const FHyperAIStudioPIETopologyConfig& FrozenConfig,
		const FHyperAIStudioPIETopologyConfig& CurrentConfig);
	static bool ValidatePageRequest(int32 PageSize, const FString& Cursor, FString& OutDiagnostic);
	static FString MakeCursor(const FString& SnapshotFingerprint, int32 Offset);
	static bool ParseCursor(
		const FString& Cursor,
		const FString& ExpectedSnapshotFingerprint,
		int32 TotalCount,
		int32& OutOffset,
		FString& OutStatus,
		FString& OutDiagnostic);
	static FString HashTokens(const TArray<FString>& Tokens);
	static FString ClipText(const FString& Value, int32 MaxCharacters, bool* bOutTruncated = nullptr);
	static FString OperationStateToStatus(EHyperAIStudioOperationState State);
	static FString RollbackStateToStatus(EHyperAIStudioRollbackState State);
	static FString ClassifyJournalLoadFailure(const FString& LoadError);
	static FString ClassifyWatchPreviewProperty(const FProperty* Property);
	static FString MakeBlueprintDebugFingerprint(
		const FHyperAIBlueprintDebugReport& Report,
		int32 RequestedPageSize,
		const TArray<FHyperAIBlueprintDebugItem>& Items);
};

/** Defers owned HyperAI toolset registrations until PostEngineInit and unregisters exactly the classes it registered. */
class FHyperAIStudioNativeReadToolRegistration final
{
public:
	void Startup();
	void Shutdown();
	bool IsRegistered() const;

private:
	void RegisterAfterEngineInit();
	void RefreshMcpToolsIfSafe() const;

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsNativeReadRegistration = false;
	bool bOwnsDependencyGraphRegistration = false;
};
