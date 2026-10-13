// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioEQSToolset.generated.h"

/**
 * One op of an EQS plan, in order. Settings are "Name=Value; Name=Value": the generator's or test's own properties by
 * name (hyper_eqs_inspect lists each kind's), nested ones with a dot (ProjectionData.TraceMode=None, LineA.LineFrom=item).
 */
USTRUCT(BlueprintType)
struct FHyperAIEQSOp
{
	GENERATED_BODY()

	/**
	 * create (makes the query asset), option.add, option.set, option.remove, test.add, test.set, test.remove,
	 * bt.add_composite, bt.run_eqs_task, bt.run_eqs_service.
	 */
	UPROPERTY() FString Kind;
	/**
	 * option.add: the generator, grid, circle, donut, cone, pathing_grid, actors_of_class, current_location, composite,
	 * perceived_actors, or a Blueprint generator class (/Game/AI/EQG_Cover.EQG_Cover_C). test.add: distance, dot, trace,
	 * pathfinding, pathfinding_batch, overlap, gameplay_tags, project, random or volume. bt.add_composite: selector or sequence.
	 */
	UPROPERTY() FString Type;
	/** The option, from 0 in the query's order; -1 (the default) is the last one, such as the one this plan just added. */
	UPROPERTY() int32 Option = -1;
	/** test.set and test.remove: the test within the option, from 0; -1 is the last one. */
	UPROPERTY() int32 Test = -1;
	/**
	 * "Name=Value; Name=Value". Contexts take querier, item or a context class (/Game/AI/EQC_Player.EQC_Player_C).
	 * Numbers that can be query params take param:Name (a named param the Behavior Tree or caller sets). Enums take
	 * their short names (TestPurpose=Score, FilterType=Range, ScoringEquation=InverseLinear). Tag queries take
	 * any:A.B,C.D, all:... or none:.... Short names: Purpose, Filter, Min, Max, Scoring, Factor, Clamp (sets ClampMinType
	 * and ClampMaxType), Context (the kind's main context).
	 */
	UPROPERTY() FString Params;
	/** composite: its child generators in order, each "kind" or "kind: Name=Value; Name=Value". */
	UPROPERTY() TArray<FString> Children;
	/** bt.*: the Behavior Tree, /Game/AI/BT_Guard. */
	UPROPERTY() FString BehaviorTree;
	/** bt.*: the composite the new node goes under or onto: root (the tree's first composite), its node name, or #index. */
	UPROPERTY() FString Parent;
	/** bt.run_eqs_task and bt.run_eqs_service: the tree's Blackboard key (a Vector or an Actor object key) the result goes to. */
	UPROPERTY() FString BlackboardKey;
};

USTRUCT(BlueprintType)
struct FHyperAIEQSPlanRequest
{
	GENERATED_BODY()

	/** The Environment Query the plan makes (start with create) or changes, under /Game: /Game/AI/EQS_FindCover. */
	UPROPERTY() FString Asset;
	UPROPERTY() TArray<FHyperAIEQSOp> Operations;
	/** Apply the plan to a copy and report the query it would leave; nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Save the query, and the Behavior Trees it wired. Off, nothing is saved and one undo takes the whole plan back. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIEQSTestView
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = 0;
	UPROPERTY() FString Kind;
	/** filter, score or filter_and_score. */
	UPROPERTY() FString Purpose;
	/** What the test checks and how it scores, as the EQS editor words it. */
	UPROPERTY() FString Description;
	/** Its settings that differ from the kind's defaults, "Name=Value". */
	UPROPERTY() TArray<FString> Settings;
};

USTRUCT(BlueprintType)
struct FHyperAIEQSOptionView
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = 0;
	/** The generator's kind (grid, circle...) or its class. */
	UPROPERTY() FString Generator;
	/** point or actor (what its items are). */
	UPROPERTY() FString ItemType;
	UPROPERTY() FString Description;
	UPROPERTY() TArray<FString> Settings;
	/** composite: its child generators. */
	UPROPERTY() TArray<FString> Children;
	UPROPERTY() TArray<FHyperAIEQSTestView> Tests;
};

USTRUCT(BlueprintType)
struct FHyperAIEQSPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, approval_not_queued, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Asset;
	/** The query's options as the plan leaves them. */
	UPROPERTY() TArray<FHyperAIEQSOptionView> Options;
	/** Named params the query reads (param:Name settings), which a Behavior Tree node or hyper_eqs_run sets. */
	UPROPERTY() TArray<FString> QueryParams;
	/** Behavior Tree nodes the plan added, "BT path: node (under parent)". */
	UPROPERTY() TArray<FString> TreeNodes;
	/** What each op did. */
	UPROPERTY() TArray<FString> Notes;
	/** What could go wrong when it runs: a test that cannot score these items, no navmesh for navigation... */
	UPROPERTY() TArray<FString> Warnings;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIEQSInspectRequest
{
	GENERATED_BODY()

	/** An Environment Query by path. Neither it nor BehaviorTree: the generator, test and context kinds, and the queries under /Game. */
	UPROPERTY() FString Asset;
	/** A Behavior Tree: its composites (for a plan's Parent), its Blackboard keys, and the EQS nodes it has. */
	UPROPERTY() FString BehaviorTree;
};

/** A generator or test kind a plan takes, with its settings. */
USTRUCT(BlueprintType)
struct FHyperAIEQSKind
{
	GENERATED_BODY()

	/** generator, test or context. */
	UPROPERTY() FString Role;
	UPROPERTY() FString Kind;
	UPROPERTY() FString Class;
	UPROPERTY() FString Summary;
	/** "Name: type, default value", one per setting. */
	UPROPERTY() TArray<FString> Settings;
};

USTRUCT(BlueprintType)
struct FHyperAIEQSInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Asset;
	UPROPERTY() TArray<FHyperAIEQSOptionView> Options;
	UPROPERTY() TArray<FString> QueryParams;
	UPROPERTY() TArray<FString> Warnings;
	/** BehaviorTree: its composites as "#index name (class), N children", its Blackboard keys as "name: type", its EQS nodes. */
	UPROPERTY() FString BehaviorTree;
	UPROPERTY() TArray<FString> Composites;
	UPROPERTY() TArray<FString> BlackboardKeys;
	UPROPERTY() TArray<FString> TreeNodes;
	/** Without Asset or BehaviorTree: the kinds hyper_eqs_apply_plan takes, and the queries under /Game. */
	UPROPERTY() TArray<FHyperAIEQSKind> Kinds;
	UPROPERTY() TArray<FString> Assets;
};

USTRUCT(BlueprintType)
struct FHyperAIEQSRunRequest
{
	GENERATED_BODY()

	/** The Environment Query to run. */
	UPROPERTY() FString Asset;
	/** The actor that asks, by label: an AI pawn, its controller, or any actor (its location is the querier's). Empty: the selected actor. */
	UPROPERTY() FString Querier;
	/** single (the best item, the default), all (every item that passed, best first), random_best_5 or random_best_25. */
	UPROPERTY() FString RunMode;
	/** Named query params, "Name=Value; Name=Value". */
	UPROPERTY() FString Params;
	/** Items listed, best first; 1 to 200, 10 by default. */
	UPROPERTY() int32 MaxItems = 10;
};

USTRUCT(BlueprintType)
struct FHyperAIEQSItem
{
	GENERATED_BODY()

	UPROPERTY() int32 Rank = 0;
	UPROPERTY() float Score = 0.f;
	/** "x,y,z" cm. */
	UPROPERTY() FString Location;
	/** Actor items: its label. */
	UPROPERTY() FString Actor;
};

USTRUCT(BlueprintType)
struct FHyperAIEQSRunReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** success, failed (no item passed), missing_param, invalid_request. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Asset;
	/** editor or play: the world it ran in (Play In Editor's while it runs). */
	UPROPERTY() FString World;
	UPROPERTY() FString Querier;
	/** The option whose items won (the first whose generator and tests left any). */
	UPROPERTY() int32 Option = -1;
	UPROPERTY() FString ItemType;
	/** Items that passed every filter. */
	UPROPERTY() int32 Passed = 0;
	UPROPERTY() TArray<FHyperAIEQSItem> Items;
	UPROPERTY() int32 Milliseconds = 0;
	UPROPERTY() TArray<FString> Warnings;
};

/**
 * Environment Queries (EQS): queries made with generators, contexts and tests with their scoring and filters in one
 * undoable plan, wired into Behavior Trees as a Run EQS Query task or a Run EQS service, inspected, and run from a
 * querier in the level or in Play In Editor to see the best items and their scores.
 */
UCLASS()
class HYPERAISTUDIOEQS_API UHyperAIStudioEQSToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * An Environment Query (Asset), read only: each option's generator with its settings and each test with its purpose,
	 * filter and scoring, and the named params it reads. A BehaviorTree: its composites, Blackboard keys and EQS nodes.
	 * Neither: the generator, test and context kinds hyper_eqs_apply_plan takes, with their settings and defaults.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|GameplayAI")
	static FHyperAIEQSInspectReport hyper_eqs_inspect(const FHyperAIEQSInspectRequest& Request);

	/**
	 * Makes or changes an Environment Query in one plan, one undo step: create; option.add a generator (grid, circle,
	 * donut, cone, pathing_grid, actors_of_class, current_location, composite, perceived_actors, or a Blueprint
	 * generator) around a context (querier, item, or a Blueprint context class); test.add distance, dot, trace,
	 * pathfinding, overlap, gameplay_tags, project, random or volume tests with their purpose, filter and scoring;
	 * option.set, test.set and the removes; and wires it into a Behavior Tree: bt.run_eqs_task or bt.run_eqs_service
	 * writing to a Blackboard key, bt.add_composite for the selector or sequence they go under. bDryRun reports the
	 * query it would leave.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|GameplayAI")
	static FHyperAIEQSPlanReport hyper_eqs_apply_plan(const FHyperAIEQSPlanRequest& Request);

	/**
	 * Runs an Environment Query now from a querier actor (by label) in the open level, or in Play In Editor while it
	 * runs, and reports the winning option, how many items passed, and the best items with their locations, actors and
	 * scores. Changes nothing.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|GameplayAI")
	static FHyperAIEQSRunReport hyper_eqs_run(const FHyperAIEQSRunRequest& Request);
};

class FHyperAIStudioEQSContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("gameplay_ai");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioeqstoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_eqs_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_eqs_apply_plan");
	static constexpr const TCHAR* RunToolName = TEXT("hyper_eqs_run");
	static constexpr int32 MaxOperations = 48;
	static constexpr int32 MaxOptions = 16;
	static constexpr int32 MaxTestsPerOption = 24;
	static constexpr int32 MaxChildren = 8;
	static constexpr int32 MaxRunItems = 200;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIEQSInspectReport Inspect(const FHyperAIEQSInspectRequest& Request);
	static FHyperAIEQSPlanReport ApplyPlan(const FHyperAIEQSPlanRequest& Request);
	static FHyperAIEQSRunReport Run(const FHyperAIEQSRunRequest& Request);
};

class FHyperAIStudioEQSRegistration final
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
