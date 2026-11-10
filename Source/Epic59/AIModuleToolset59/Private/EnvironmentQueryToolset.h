// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions
#include "UObject/SoftObjectPath.h"

#include "EnvironmentQueryToolset.generated.h"

class UEnvironmentQueryGraphNode;
class UEnvQuery;
class UEnvQueryContext;
class UEnvQueryGenerator;
class UEnvQueryOption;
class UEnvQueryTest;

/**
 * Inspect and edit Environment Query (EQS) assets in the editor: create a query, add and arrange its
 * options (each option runs one generator) and each option's tests, enable or disable tests, and read
 * the generator and test instances that hold the settings.
 */
UCLASS(BlueprintType, MinimalAPI)
class UEnvironmentQueryToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------- //
	// Asset
	// ---------------------------------------------------------------- //

	/**
	 * Creates a new Environment Query asset.
	 * @param FolderPath Content folder to create in (Path e.g. "/Game/AI").
	 * @param AssetName Name for the new asset.
	 * @return The created query.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EnvironmentQueryToolset")
	static UEnvQuery* CreateEnvironmentQuery(const FDirectoryPath& FolderPath, const FString& AssetName);

	/**
	 * Returns the query's root graph node. Options connect to it.
	 * @param Query The query to inspect.
	 * @return The root graph node.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "EnvironmentQueryToolset")
	static UEnvironmentQueryGraphNode* GetRootGraphNode(UEnvQuery* Query);

	// ---------------------------------------------------------------- //
	// Options (each wraps one generator; take and return graph nodes)
	// ---------------------------------------------------------------- //

	/**
	 * Adds an option to the query. An option wraps one generator that produces the items to score.
	 * @param Query The query to modify.
	 * @param GeneratorClass A concrete, placeable generator class.
	 * @param Index Option position; -1 appends.
	 * @return The new option's graph node.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EnvironmentQueryToolset")
	static UEnvironmentQueryGraphNode* AddOption(UEnvQuery* Query, TSubclassOf<UEnvQueryGenerator> GeneratorClass, int32 Index = -1);

	/**
	 * Reorders an option among its siblings. Option order is the tie-break order the query runs them in.
	 * @param Query The query that owns the option.
	 * @param Option The option graph node to move.
	 * @param NewIndex Target option position.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EnvironmentQueryToolset")
	static void MoveOption(UEnvQuery* Query, UEnvironmentQueryGraphNode* Option, int32 NewIndex);

	/**
	 * Removes an option and its tests.
	 * @param Option The option graph node to remove.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EnvironmentQueryToolset")
	static void RemoveOption(UEnvironmentQueryGraphNode* Option);

	/**
	 * Returns the query's option graph nodes, in order.
	 * @param Query The query to query.
	 * @return The option graph nodes.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "EnvironmentQueryToolset")
	static TArray<UEnvironmentQueryGraphNode*> GetOptionGraphNodes(UEnvQuery* Query);

	// ---------------------------------------------------------------- //
	// Tests (subnodes of an option; take and return graph nodes)
	// ---------------------------------------------------------------- //

	/**
	 * Adds a test to an option. Tests filter and score the option's generated items.
	 * @param Option The option graph node to add the test to.
	 * @param TestClass A concrete, placeable test class.
	 * @param Index Position among the option's tests; -1 appends.
	 * @return The new test's graph node.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EnvironmentQueryToolset")
	static UEnvironmentQueryGraphNode* AddTest(UEnvironmentQueryGraphNode* Option, TSubclassOf<UEnvQueryTest> TestClass, int32 Index = -1);

	/**
	 * Reorders a test among its siblings without recreating it, so its settings are kept. Test order is
	 * the order they run in (and, with auto-sort off, affects performance).
	 * @param Option The option graph node that owns the test.
	 * @param Test The test graph node to move.
	 * @param Index Target position among the option's tests; -1 or past the end appends.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EnvironmentQueryToolset")
	static void MoveTest(UEnvironmentQueryGraphNode* Option, UEnvironmentQueryGraphNode* Test, int32 Index);

	/**
	 * Removes a test from its option.
	 * @param Option The option graph node that owns the test.
	 * @param Test The test graph node to remove.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EnvironmentQueryToolset")
	static void RemoveTest(UEnvironmentQueryGraphNode* Option, UEnvironmentQueryGraphNode* Test);

	/**
	 * Enables or disables a test. A disabled test stays on the option but is excluded from the built query.
	 * @param Test The test graph node to toggle.
	 * @param bEnabled Whether the test is enabled.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "EnvironmentQueryToolset")
	static void SetTestEnabled(UEnvironmentQueryGraphNode* Test, bool bEnabled);

	/**
	 * Returns the test subnode graph nodes attached to an option, in order.
	 * @param Option The option graph node to query.
	 * @return The test graph nodes.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "EnvironmentQueryToolset")
	static TArray<UEnvironmentQueryGraphNode*> GetTestGraphNodes(UEnvironmentQueryGraphNode* Option);

	// ---------------------------------------------------------------- //
	// Read (node instances hold the settings)
	// ---------------------------------------------------------------- //

	/**
	 * Returns the generator instance wrapped by an option. This is the object that holds the
	 * generator's settings.
	 * @param Option The option graph node to read.
	 * @return The generator instance, or null if the node is not an option.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "EnvironmentQueryToolset")
	static UEnvQueryGenerator* GetGenerator(UEnvironmentQueryGraphNode* Option);

	/**
	 * Returns the option instance a graph node wraps.
	 * @param Option The option graph node to read.
	 * @return The option instance, or null if the node is not an option.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "EnvironmentQueryToolset")
	static UEnvQueryOption* GetOptionInstance(UEnvironmentQueryGraphNode* Option);

	/**
	 * Returns the test instance a graph node wraps. This is the object that holds the test's settings.
	 * @param Test The test graph node to read.
	 * @return The test instance, or null if the node is not a test.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "EnvironmentQueryToolset")
	static UEnvQueryTest* GetTestInstance(UEnvironmentQueryGraphNode* Test);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface
};
