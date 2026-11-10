// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioChooserToolset.generated.h"

/**
 * One op of a chooser plan, in order. Columns and rows are counted from 0 as hyper_chooser_inspect lists them; cells are
 * text, one per column in column order (hyper_chooser_inspect with no Asset lists what each column kind's cells take).
 */
USTRUCT(BlueprintType)
struct FHyperAIChooserOp
{
	GENERATED_BODY()

	/** create, column.add, column.set, column.remove, row.add, row.set, row.remove, fallback.set. */
	UPROPERTY() FString Kind;
	/**
	 * create: object (each row gives an asset of OutputObjectType, the default), class (each row gives a subclass of
	 * OutputObjectType) or none (no result: the rows only write output columns).
	 */
	UPROPERTY() FString ResultType;
	/** create: the class of what the rows give, such as /Script/Engine.AnimSequence; UObject when empty. */
	UPROPERTY() FString OutputObjectType;
	/**
	 * create: the parameters the table reads and writes, classes first then structs; they are Context0, Context1... in that
	 * order. A class by path (/Script/Engine.CharacterMovementComponent, or a Blueprint /Game/BP_Hero); a struct by path
	 * (/Script/Engine.HitResult, or a user struct /Game/S_Locomotion).
	 */
	UPROPERTY() TArray<FString> ContextClasses;
	UPROPERTY() TArray<FString> ContextStructs;
	/** create: input (read, the default), output (written) or both, one per parameter in Context order. */
	UPROPERTY() TArray<FString> ContextDirections;
	/** column.set, column.remove: the column's index. */
	UPROPERTY() int32 Column = -1;
	/**
	 * column.add: bool, enum, multi_enum, float_range, float_distance, gameplay_tag, gameplay_tag_query, object, object_class,
	 * name, randomize, output_float, output_bool, output_enum, output_object, output_name, output_gameplay_tag or pose_match
	 * (needs the PoseSearch plugin).
	 */
	UPROPERTY() FString ColumnKind;
	/**
	 * column.add, column.set: the property the column reads (a filter) or writes (an output), by its path on a parameter:
	 * Speed (the first parameter that has it), Context1.Speed, or a struct member Context0.Velocity.X.
	 */
	UPROPERTY() FString Binding;
	/** column.add, column.set: the column's settings, "Name=Value; Name=Value" (bWrapInput, MaxDistance, TagMatchType, Disabled...). */
	UPROPERTY() FString Params;
	/** row.set, row.remove: the row's index; row.add: where it goes (-1, the default, at the end). */
	UPROPERTY() int32 Row = -1;
	/**
	 * row.add, row.set, fallback.set: what the row gives: an asset path (/Game/Anim/Walk), soft:/Game/Anim/Walk for a soft
	 * reference, a class (/Script/Engine.Character or /Game/BP_Hero), chooser:/Game/CT_Other to evaluate another chooser
	 * table, or nested:Name for a table nested inside this one. fallback.set takes none to clear the fallback.
	 */
	UPROPERTY() FString Result;
	/**
	 * row.add, row.set: one cell per column in column order, empty for any (bool true/false/any, float_range 0..300, enum
	 * Walking or !Walking, gameplay_tag A.B, C.D...). column.add, column.set: one cell per row for that column.
	 * fallback.set: the output columns' values when no row passes (filter columns stay empty). An empty list keeps them.
	 */
	UPROPERTY() TArray<FString> Cells;
	/** row.add, row.set: true or false to disable or enable the row (a disabled row is never chosen); empty keeps it. */
	UPROPERTY() FString Disabled;
};

USTRUCT(BlueprintType)
struct FHyperAIChooserPlanRequest
{
	GENERATED_BODY()

	/** The chooser table the plan makes (with create) or changes, under /Game: /Game/Animation/CT_Locomotion. */
	UPROPERTY() FString Asset;
	UPROPERTY() TArray<FHyperAIChooserOp> Operations;
	/** Check every op on a copy in memory and report what would change; nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Save the table. Off, nothing is saved and one undo takes the whole plan back. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIChooserPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, approval_not_queued, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Asset;
	/** The table's columns and rows as the plan leaves it. */
	UPROPERTY() int32 Columns = 0;
	UPROPERTY() int32 Rows = 0;
	/** What each op did, with the index each new column or row got. */
	UPROPERTY() TArray<FString> Notes;
	/** What could go wrong in play: rows that can never be chosen, no fallback, outputs bound to an input parameter... */
	UPROPERTY() TArray<FString> Warnings;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIChooserInspectRequest
{
	GENERATED_BODY()

	/** A chooser table by path. Empty: the column kinds this editor has, the op kinds, and the chooser tables under /Game. */
	UPROPERTY() FString Asset;
	/** List at most this many rows; 500 at most. */
	UPROPERTY() int32 MaxRows = 200;
};

/** A parameter the table reads or writes. */
USTRUCT(BlueprintType)
struct FHyperAIChooserParameter
{
	GENERATED_BODY()

	/** Its name in bindings: Context0, Context1... */
	UPROPERTY() FString Name;
	/** class or struct. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Type;
	/** input, output or both. */
	UPROPERTY() FString Direction;
};

USTRUCT(BlueprintType)
struct FHyperAIChooserColumnInfo
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = 0;
	/** The plan's word for it (float_range...), or the column struct's name when the plan has no word for it. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Struct;
	/** filter, scoring, output or random. */
	UPROPERTY() FString Role;
	/** The property it reads or writes, as Context0.Speed; empty when unbound. */
	UPROPERTY() FString Binding;
	/** Why the binding does not resolve, as the Chooser editor says it; empty when it does. */
	UPROPERTY() FString Problem;
	/** Its settings as Name = Value, the names column.set takes in Params. */
	UPROPERTY() TArray<FString> Settings;
	/** An output column's value when the fallback is used. */
	UPROPERTY() FString Fallback;
	UPROPERTY() bool bDisabled = false;
};

USTRUCT(BlueprintType)
struct FHyperAIChooserRowInfo
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = 0;
	/** asset <path>, soft asset <path>, class <path>, chooser <path> (another table), nested <name>, or none. */
	UPROPERTY() FString Result;
	/** One per column in column order, as row.add takes them; any where the cell matches anything. */
	UPROPERTY() TArray<FString> Cells;
	UPROPERTY() bool bDisabled = false;
};

/** A column type this editor has. */
USTRUCT(BlueprintType)
struct FHyperAIChooserColumnKind
{
	GENERATED_BODY()

	/** The word column.add takes; empty for a type the plan cannot add. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Struct;
	UPROPERTY() FString DisplayName;
	/** filter, scoring, output or random. */
	UPROPERTY() FString Role;
	/** The type of property its Binding takes (bool, double, enum, FGameplayTagContainer, object, FName...). */
	UPROPERTY() FString Input;
	/** What its cells take as text. */
	UPROPERTY() FString Cells;
	/** Its settings, "Name: type, default", the names Params takes. */
	UPROPERTY() TArray<FString> Settings;
};

USTRUCT(BlueprintType)
struct FHyperAIChooserInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Asset;
	/** object, class or none. */
	UPROPERTY() FString ResultType;
	UPROPERTY() FString OutputObjectType;
	UPROPERTY() TArray<FHyperAIChooserParameter> Parameters;
	UPROPERTY() TArray<FHyperAIChooserColumnInfo> Columns;
	UPROPERTY() int32 RowCount = 0;
	UPROPERTY() TArray<FHyperAIChooserRowInfo> Rows;
	/** What is given when no row passes; none when there is no fallback. */
	UPROPERTY() FString Fallback;
	/** Tables nested inside this one (nested:Name), and other tables its rows evaluate (chooser:path). */
	UPROPERTY() TArray<FString> NestedChoosers;
	UPROPERTY() TArray<FString> Warnings;
	/** Without Asset: the column types, the ops hyper_chooser_apply_plan takes with their fields, and the tables under /Game. */
	UPROPERTY() TArray<FHyperAIChooserColumnKind> ColumnKinds;
	UPROPERTY() TArray<FString> Ops;
	UPROPERTY() TArray<FString> Assets;
};

USTRUCT(BlueprintType)
struct FHyperAIChooserEvaluateRequest
{
	GENERATED_BODY()

	UPROPERTY() FString Asset;
	/**
	 * The parameters' property values, "Name=Value; Name=Value": MaxWalkSpeed=450; Context1.Stance=Crouch. A name without
	 * ContextN. is set on the first parameter that has it; the rest keep their class or struct defaults.
	 */
	UPROPERTY() FString Values;
	/** Seeds a Randomize column's pick, the same seed the same pick; -1 picks at random. */
	UPROPERTY() int32 Seed = -1;
};

/** What one filter column did to the rows on its own. */
USTRUCT(BlueprintType)
struct FHyperAIChooserColumnFilter
{
	GENERATED_BODY()

	UPROPERTY() int32 Column = 0;
	UPROPERTY() FString Kind;
	UPROPERTY() FString Binding;
	/** The value it read from the parameters. */
	UPROPERTY() FString Input;
	/** The rows it turns away. */
	UPROPERTY() TArray<int32> RowsOut;
};

USTRUCT(BlueprintType)
struct FHyperAIChooserEvaluateReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Asset;
	/** The row that won, as the engine chose it; -1 when the fallback was used or nothing was. */
	UPROPERTY() int32 Row = -1;
	UPROPERTY() bool bFallback = false;
	/** The asset or class it gave; empty when nothing. */
	UPROPERTY() FString Result;
	/** A nested or evaluated table the winning row ran, and the row that won there. */
	UPROPERTY() FString Nested;
	/** The rows that passed every filter column, in row order (scoring columns order them by cost). */
	UPROPERTY() TArray<int32> Passed;
	UPROPERTY() TArray<FHyperAIChooserColumnFilter> Filtered;
	/** Each output column's value after the choice: "Column 3 (output_float Context0.PlayRate) = 1.2". */
	UPROPERTY() TArray<FString> Outputs;
	/** The parameter values the table was given. */
	UPROPERTY() TArray<FString> Inputs;
	/** A Randomize column picked among the rows that passed; without Seed another call may pick another. */
	UPROPERTY() bool bRandom = false;
	UPROPERTY() TArray<FString> Notes;
};

/**
 * Chooser tables: animation (or any asset or class) chosen by state. Columns filter rows by a property of the context
 * (bool, enum, float range, gameplay tags, objects, names), score them (float distance, pose match), pick randomly, and
 * write output values; rows give an asset, a class, or another chooser table. Built in one undo step and evaluated
 * against chosen context values to show which row wins and why.
 */
UCLASS()
class HYPERAISTUDIOCHOOSER_API UHyperAIStudioChooserToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * A chooser table, read only: its parameters (classes and structs, input or output), result type, columns (kind,
	 * binding, settings, disabled), rows (result and one cell per column), fallback and nested tables. Without Asset: the
	 * column kinds this editor has with what their cells take, the plan's ops, and the chooser tables under /Game.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIChooserInspectReport hyper_chooser_inspect(const FHyperAIChooserInspectRequest& Request);

	/**
	 * Makes or changes a chooser table in one plan, one undo step: create it (result type, output class, the classes and
	 * structs it reads as parameters); column.add a filter, scoring, random or output column bound to a parameter's
	 * property (bool, enum, float_range, gameplay_tag, output_float, pose_match...); column.set or column.remove; row.add
	 * rows with a result (asset, class, or another chooser) and one cell per column (true, 0..300, Walking, A.B); row.set,
	 * row.remove; fallback.set. bDryRun checks it all on a copy first.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIChooserPlanReport hyper_chooser_apply_plan(const FHyperAIChooserPlanRequest& Request);

	/**
	 * Runs a chooser table as the game does, with the parameter values given (Values "MaxWalkSpeed=450; Stance=Crouch"),
	 * and reports the row that won, what it gave, the output columns' values, the rows that passed, and the rows each
	 * filter column turned away. Seed fixes a Randomize column's pick. Changes nothing.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Animation")
	static FHyperAIChooserEvaluateReport hyper_chooser_evaluate(const FHyperAIChooserEvaluateRequest& Request);
};

class FHyperAIStudioChooserContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("animation_rigging");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiochoosertoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_chooser_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_chooser_apply_plan");
	static constexpr const TCHAR* EvaluateToolName = TEXT("hyper_chooser_evaluate");
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxColumns = 32;
	static constexpr int32 MaxRows = 512;
	/** Parameters a table may have; the evaluation context holds at most 4 objects in place. */
	static constexpr int32 MaxParameters = 8;
	static constexpr int32 MaxObjectParameters = 4;
	static constexpr int32 MaxListedRows = 500;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIChooserInspectReport Inspect(const FHyperAIChooserInspectRequest& Request);
	static FHyperAIChooserPlanReport ApplyPlan(const FHyperAIChooserPlanRequest& Request);
	static FHyperAIChooserEvaluateReport Evaluate(const FHyperAIChooserEvaluateRequest& Request);
};

class FHyperAIStudioChooserRegistration final
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
