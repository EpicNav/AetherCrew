// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HyperAIStudioBlueprintPatch.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioBlueprintWorkflowToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAIBlueprintGraphRef
{
	GENERATED_BODY()

	UPROPERTY()
	FString GraphGuid;

	UPROPERTY()
	FString GraphPath;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintNodeRef
{
	GENERATED_BODY()

	UPROPERTY()
	FHyperAIBlueprintGraphRef Graph;

	UPROPERTY()
	FString NodeGuid;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintPinRef
{
	GENERATED_BODY()

	UPROPERTY()
	FHyperAIBlueprintNodeRef Node;

	/** Optional. Empty selects the revision-bound name/direction/index identity. */
	UPROPERTY()
	FString PersistentGuid;

	UPROPERTY()
	FString PinName;

	/** input or output. */
	UPROPERTY()
	FString Direction;

	UPROPERTY()
	int32 PinIndex = -1;
};

/** Closed discriminated operation DTO. Fields not owned by Type must remain at their defaults. */
USTRUCT(BlueprintType)
struct FHyperAIBlueprintPatchOperationInput
{
	GENERATED_BODY()

	/** create_node, move_node, connect_pins, break_pin_link, set_literal_default, delete_node, or layout_nodes. */
	UPROPERTY()
	FString Type;

	UPROPERTY()
	FHyperAIBlueprintGraphRef Graph;

	UPROPERTY()
	FHyperAIBlueprintNodeRef Node;

	UPROPERTY()
	FHyperAIBlueprintPinRef PinA;

	UPROPERTY()
	FHyperAIBlueprintPinRef PinB;

	UPROPERTY()
	FString NewNodeGuid;

	/** branch, sequence, or reroute. */
	UPROPERTY()
	FString NodeKind;

	UPROPERTY()
	int32 X = 0;

	UPROPERTY()
	int32 Y = 0;

	UPROPERTY()
	FString Value;

	/** orphan_only or authorized. Either is destructive; authorized may remove linked nodes only with a server grant. */
	UPROPERTY()
	FString DeleteMode;

	UPROPERTY()
	TArray<FHyperAIBlueprintNodeRef> LayoutNodes;

	UPROPERTY()
	int32 Columns = 0;

	UPROPERTY()
	int32 HorizontalSpacing = 0;

	UPROPERTY()
	int32 VerticalSpacing = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintPatchInput
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString ExpectedRevision;

	UPROPERTY()
	TArray<FHyperAIBlueprintPatchOperationInput> Operations;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintMutationRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FHyperAIBlueprintPatchInput Patch;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString ExpectedPlanHash;

	/** Opaque, short-lived, server-issued token. It contains no client-supplied claims. */
	UPROPERTY()
	FString AuthorizationToken;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintSnapshotRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetPath;

	/** Optional exact graph path filter. */
	UPROPERTY()
	FString GraphPath;

	UPROPERTY()
	bool bIncludePins = true;

	UPROPERTY()
	bool bIncludeLinks = true;

	UPROPERTY()
	int32 PageSize = 64;

	UPROPERTY()
	FString Cursor;

	UPROPERTY()
	int32 MaxOutputBytes = 65536;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintSnapshotItem
{
	GENERATED_BODY()

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString ParentId;

	UPROPERTY()
	FString GraphPath;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString ClassPath;

	UPROPERTY()
	FString PinType;

	UPROPERTY()
	FString Direction;

	UPROPERTY()
	FString DefaultValue;

	UPROPERTY()
	int32 X = 0;

	UPROPERTY()
	int32 Y = 0;

	UPROPERTY()
	bool bCanUserDelete = false;

	UPROPERTY()
	bool bOrphaned = false;

	UPROPERTY()
	TArray<FString> LinkedPinIds;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintSnapshotReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	FString CompileStatus;

	/** Bounded process-local immutable value snapshot token for hyper_blueprint_diff. */
	UPROPERTY()
	FString SnapshotToken;

	UPROPERTY()
	int32 GraphCount = 0;

	UPROPERTY()
	int32 NodeCount = 0;

	UPROPERTY()
	int32 PinCount = 0;

	UPROPERTY()
	int32 LinkCount = 0;

	UPROPERTY()
	int32 TotalItems = 0;

	UPROPERTY()
	int32 ReturnedItems = 0;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	FString NextCursor;

	UPROPERTY()
	TArray<FHyperAIBlueprintSnapshotItem> Items;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintDiscoverActionsRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FHyperAIBlueprintGraphRef Graph;

	UPROPERTY()
	FString Query;

	UPROPERTY()
	int32 MaxActions = 16;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintActionRecord
{
	GENERATED_BODY()

	UPROPERTY()
	FString ActionId;

	UPROPERTY()
	FString NodeKind;

	UPROPERTY()
	FString Title;

	UPROPERTY()
	FString NodeClassPath;

	UPROPERTY()
	TArray<FHyperAIBlueprintSnapshotItem> Pins;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintActionReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString Revision;

	UPROPERTY()
	TArray<FHyperAIBlueprintActionRecord> Actions;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintWorkflowIssue
{
	GENERATED_BODY()

	UPROPERTY()
	FString Code;

	UPROPERTY()
	FString Severity;

	UPROPERTY()
	int32 OperationIndex = -1;

	UPROPERTY()
	FString Target;

	UPROPERTY()
	FString Message;

	UPROPERTY()
	double Confidence = 0.0;

	UPROPERTY()
	FString Evidence;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintEffectReport
{
	GENERATED_BODY()

	UPROPERTY()
	int32 OperationCount = 0;

	UPROPERTY()
	int32 NoOpCount = 0;

	UPROPERTY()
	int32 NodesCreated = 0;

	UPROPERTY()
	int32 NodesMoved = 0;

	UPROPERTY()
	int32 LinksAdded = 0;

	UPROPERTY()
	int32 LinksBroken = 0;

	UPROPERTY()
	int32 DefaultsChanged = 0;

	UPROPERTY()
	int32 LayoutNodesMoved = 0;

	UPROPERTY()
	bool bCompileOnce = false;

	UPROPERTY()
	bool bSaveOnce = false;

	UPROPERTY()
	bool bFreshVerifyOnce = false;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintPatchReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	bool bDryRun = true;

	UPROPERTY()
	bool bMutationStarted = false;

	UPROPERTY()
	bool bStaged = false;

	UPROPERTY()
	bool bExecutionSubmitted = false;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString RevisionBefore;

	UPROPERTY()
	FString RevisionAfter;

	UPROPERTY()
	FString PlanHash;

	/** Backend patch semantic hash embedded into the staged patch_id. */
	UPROPERTY()
	FString PatchPlanHash;

	UPROPERTY()
	FString EffectFingerprint;

	/** Backend effect digest; the TypedPlan effect fingerprint remains in EffectFingerprint. */
	UPROPERTY()
	FString PatchEffectFingerprint;

	UPROPERTY()
	FHyperAIBlueprintEffectReport Effects;

	UPROPERTY()
	TArray<FHyperAIBlueprintWorkflowIssue> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintCompileValidateRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString ExpectedRevision;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	FString AuthorizationToken;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintRepairRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString ExpectedRevision;

	/** suggest or stage_safe. Only deterministic confidence-1 layout repairs can be staged. */
	UPROPERTY()
	FString Mode = TEXT("suggest");

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	FString AuthorizationToken;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintDiffRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString BaseSnapshotToken;

	UPROPERTY()
	int32 MaxChanges = 256;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintDiffItem
{
	GENERATED_BODY()

	UPROPERTY()
	FString Kind;

	UPROPERTY()
	FString StableId;

	UPROPERTY()
	FString Field;

	UPROPERTY()
	FString Before;

	UPROPERTY()
	FString After;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintDiffReport
{
	GENERATED_BODY()

	UPROPERTY()
	bool bOk = false;

	UPROPERTY()
	FString Status;

	UPROPERTY()
	FString Diagnostic;

	UPROPERTY()
	FString BaseRevision;

	UPROPERTY()
	FString CurrentRevision;

	UPROPERTY()
	bool bTruncated = false;

	UPROPERTY()
	TArray<FHyperAIBlueprintDiffItem> Changes;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintLayoutRequest
{
	GENERATED_BODY()

	UPROPERTY()
	FString AssetPath;

	UPROPERTY()
	FString ExpectedRevision;

	UPROPERTY()
	FHyperAIBlueprintGraphRef Graph;

	UPROPERTY()
	TArray<FHyperAIBlueprintNodeRef> Nodes;

	UPROPERTY()
	int32 OriginX = 0;

	UPROPERTY()
	int32 OriginY = 0;

	UPROPERTY()
	int32 Columns = 4;

	UPROPERTY()
	int32 HorizontalSpacing = 400;

	UPROPERTY()
	int32 VerticalSpacing = 240;

	UPROPERTY()
	FString OperationId;

	UPROPERTY()
	FString ExpectedPlanHash;

	UPROPERTY()
	FString AuthorizationToken;
};

/** One Blueprint cohort: all nine names share the same exact source/admission evidence. */
UCLASS()
class HYPERAISTUDIO_API UHyperAIStudioBlueprintWorkflowToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * A loaded Blueprint's graphs, nodes, pins and links by stable id, paged with PageSize and Cursor (GraphPath keeps one
	 * graph), with its revision (the ExpectedRevision a patch needs) and compile status. SnapshotToken is what
	 * hyper_blueprint_diff compares against later. For node titles, search, timelines, interfaces and replicated events,
	 * use hyper_blueprint_structure_inspect.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintSnapshotReport hyper_blueprint_snapshot(const FHyperAIBlueprintSnapshotRequest& Request);

	/**
	 * The nodes a patch's create_node can add to a graph (branch, sequence and reroute) with their pins, filtered by
	 * Query. Calls, events, variables and every other node are written with Epic's write_graph_dsl.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintActionReport hyper_blueprint_discover_actions(const FHyperAIBlueprintDiscoverActionsRequest& Request);

	/**
	 * Checks a patch against the Blueprint's exact revision without changing anything and returns its PlanHash and effects.
	 * Operations: create_node (branch, sequence, reroute), move_node, connect_pins, break_pin_link, set_literal_default and
	 * layout_nodes, addressed by the stable ids hyper_blueprint_snapshot lists. delete_node is checked but not applied by
	 * this route; delete nodes with Epic's Blueprint tools.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintPatchReport hyper_blueprint_validate_patch(const FHyperAIBlueprintPatchInput& Patch);

	/**
	 * Applies a patch hyper_blueprint_validate_patch accepted: the same Patch, a new OperationId, and ExpectedPlanHash set to
	 * its PlanHash. It runs as one undo step, compiles once, saves and re-checks the saved Blueprint; follow it with
	 * hyper_operation_wait, and read compiler messages with hyper_blueprint_debug_inspect.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintPatchReport hyper_blueprint_apply_patch(const FHyperAIBlueprintMutationRequest& Request);

	/** hyper_blueprint_apply_patch for a patch that creates at least one node; refused otherwise. */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintPatchReport hyper_blueprint_build_batch(const FHyperAIBlueprintMutationRequest& Request);

	/**
	 * Compiles a Blueprint at its exact revision. Without ExpectedPlanHash it only previews and returns the PlanHash; send it
	 * again with that hash and an OperationId to compile, then hyper_operation_wait. Compiler messages come from
	 * hyper_blueprint_debug_inspect.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintPatchReport hyper_blueprint_compile_validate(const FHyperAIBlueprintCompileValidateRequest& Request);

	/**
	 * Finds layout damage (nodes stacked on the same spot) at the exact revision. Mode suggest lists the findings and the
	 * repair patch; stage_safe with ExpectedPlanHash and an OperationId applies the deterministic ones.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintPatchReport hyper_blueprint_repair(const FHyperAIBlueprintRepairRequest& Request);

	/**
	 * What changed in a Blueprint since a hyper_blueprint_snapshot: nodes, pins, links and defaults added, removed or
	 * changed, compared with that snapshot's SnapshotToken (kept for five minutes).
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintDiffReport hyper_blueprint_diff(const FHyperAIBlueprintDiffRequest& Request);

	/**
	 * Lays the listed nodes of one graph on a grid (Columns, spacing, origin) at the exact revision; previews without
	 * ExpectedPlanHash, applies with it and an OperationId. Nodes Epic's write_graph_dsl adds are already laid out when the
	 * Blueprint compiles.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintPatchReport hyper_blueprint_layout(const FHyperAIBlueprintLayoutRequest& Request);
};

struct FHyperAIStudioBlueprintWorkflowManifestEntry
{
	enum class ECompiledCeiling : uint8
	{
		Planned,
		SourceCandidate
	};

	FString Name;
	FString QualifiedToolset;
	ECompiledCeiling CompiledCeiling = ECompiledCeiling::Planned;
};

class FHyperAIStudioBlueprintWorkflowContracts final
{
public:
	static constexpr int32 MaxPageSize = 128;
	static constexpr int32 MaxCursorCharacters = 192;
	static constexpr int32 MaxOutputBytes = 256 * 1024;
	static constexpr int32 MaxQueryCharacters = 128;
	static constexpr int32 MaxSnapshotCacheEntries = 8;
	static constexpr int64 SnapshotCacheLifetimeMs = 5 * 60 * 1000;
	static constexpr int32 MaxDiffChanges = 1024;
	static constexpr int32 MaxRepairFindings = 128;
	static constexpr int32 MaxAuthorizationTokenCharacters = 256;
	static constexpr int32 MaxOperationIdCharacters = 128;

	static FString GetQualifiedToolsetName();
	static const TArray<FHyperAIStudioBlueprintWorkflowManifestEntry>& GetManifest();
	static bool IsPendingTestRegistrationEnabled();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);

	static bool ConvertGraphRef(const FHyperAIBlueprintGraphRef& Input, FHyperAIBlueprintGraphId& Out, FString& OutError);
	static bool ConvertNodeRef(const FHyperAIBlueprintNodeRef& Input, FHyperAIBlueprintNodeId& Out, FString& OutError);
	static bool ConvertPinRef(const FHyperAIBlueprintPinRef& Input, FHyperAIBlueprintPinId& Out, FString& OutError);
	static bool ConvertPatch(const FHyperAIBlueprintPatchInput& Input, FHyperAIBlueprintPatch& Out, FString& OutError);
	static FHyperAIBlueprintPatchReport ValidatePatchLoaded(
		const FHyperAIBlueprintPatchInput& Input,
		FHyperAIBlueprintPatch* OutPatch = nullptr,
		FHyperAIBlueprintPatchPlan* OutPlan = nullptr);
	static FHyperAIBlueprintDiffReport DiffSnapshots(
		const FHyperAIBlueprintPatchSnapshot& Base,
		const FHyperAIBlueprintPatchSnapshot& Current,
		int32 MaxChanges);
	static FHyperAIBlueprintPatch BuildDeterministicRepairPatch(
		const FHyperAIBlueprintPatchSnapshot& Snapshot,
		TArray<FHyperAIBlueprintWorkflowIssue>& OutFindings);

};

class FHyperAIStudioBlueprintWorkflowRegistration final
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
