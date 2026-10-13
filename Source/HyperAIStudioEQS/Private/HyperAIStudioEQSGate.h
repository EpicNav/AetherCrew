// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioEQSToolset.h"

class FProperty;
class UBehaviorTree;
class UEnvQuery;
class UEnvQueryGenerator;
class UEnvQueryTest;

/**
 * The EQS plan's kinds, the settings it writes by reflection onto generators, tests and Behavior Tree nodes, and the
 * ops themselves. Ops run the same way on a transient copy of the query (the check and the dry run) and on the asset
 * inside the plan's undo step. Game thread only.
 */
namespace HyperAIStudio::EQS::Gate
{
	/** A generator, test or context the plan names by a short word. */
	struct FKindInfo
	{
		const TCHAR* Kind = TEXT("");
		UClass* Class = nullptr;
		const TCHAR* Summary = TEXT("");
	};

	const TArray<FKindInfo>& Generators();
	const TArray<FKindInfo>& Tests();
	const TArray<FKindInfo>& Contexts();
	/** The word a generator, test or context class goes by, or its class path when it has none. */
	FString KindOf(const UClass* Class);
	/** A generator by word or Blueprint generator class path; null with OutError listing the words. */
	UClass* ResolveGenerator(const FString& Type, FString& OutError);
	UClass* ResolveTest(const FString& Type, FString& OutError);

	/** "Name=Value; Name=Value" split in order; a value may hold '=' but not ';'. */
	bool SplitParams(const FString& Text, TArray<TPair<FString, FString>>& Out, FString& OutError);

	/**
	 * Writes one setting onto Node (an EQS node or a Behavior Tree node) by reflection: a property by name, case
	 * insensitive, through nested structs with dots, with the short names the op documents. Modify Node first.
	 */
	bool SetParam(UObject& Node, const FString& Name, const FString& Value, FString& OutError);
	/** Every "Name=Value" of Params onto Node, in order; stops at the first refusal. */
	bool ApplyParams(UObject& Node, const FString& Params, FString& OutError);

	/** A value as the plan writes it: querier, 500, param:Range, InverseLinear, any:A.B. */
	FString ValueText(const FProperty& Property, const void* Value, const UObject* Owner);
	/** "Name=Value" for each editable setting; only those differing from the class defaults when bOnlyChanged. */
	TArray<FString> DescribeSettings(const UObject& Node, bool bOnlyChanged);
	/** "Name: type, default Value" for each editable setting of Class. */
	TArray<FString> DescribeKindSettings(UClass* Class);

	/** The query's options and tests, as the report shows them. */
	TArray<FHyperAIEQSOptionView> Describe(const UEnvQuery& Query);
	/** Named params the query reads. */
	TArray<FString> QueryParams(const UEnvQuery& Query);
	/** What could go wrong when it runs. */
	TArray<FString> Warn(const UEnvQuery& Query);

	/**
	 * The query ops of Ops (create and the bt.* ops are skipped) on Query, in order, each object modified before it
	 * changes. Notes say what each did. False with OutError (naming the op) at the first refusal.
	 */
	bool ApplyQueryOps(UEnvQuery& Query, const TArray<FHyperAIEQSOp>& Ops, TArray<FString>& OutNotes, FString& OutError);

	/** A Behavior Tree under /Game by path, loaded; null with OutError. */
	UBehaviorTree* LoadTree(const FString& Path, FString& OutError);
	/**
	 * The bt.* ops checked against their trees without changing them: the tree, its Blackboard and key (a Vector or an
	 * Actor key), and the parent (composites the plan adds earlier count by their node name).
	 */
	bool CheckTreeOps(const TArray<FHyperAIEQSOp>& Ops, FString& OutError);
	/** One bt.* op, into the tree's graph (made when the tree has none), which then rebuilds the runtime tree. */
	bool ApplyTreeOp(UBehaviorTree& Tree, const FHyperAIEQSOp& Op, UEnvQuery& Query, FString& OutNote, FString& OutError);
	/** The tree's composites "#index name (class), N children", Blackboard keys "name: type" and EQS nodes. */
	void DescribeTree(UBehaviorTree& Tree, TArray<FString>& OutComposites, TArray<FString>& OutKeys, TArray<FString>& OutEQSNodes);

	FString VectorText(const FVector& Value);
}
