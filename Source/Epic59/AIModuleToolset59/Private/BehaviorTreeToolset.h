// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "Templates/SubclassOf.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions
#include "UObject/SoftObjectPath.h"

#include "BehaviorTreeToolset.generated.h"

class UBehaviorTree;
class UBehaviorTreeGraphNode;
class UBlackboardData;
class UBlackboardKeyType;
class UBTCompositeNode;
class UBTDecorator;
class UBTNode;
class UBTService;
class UBTTaskNode;

/**
 * Inspect and edit BehaviorTree assets in the editor: create trees and blackboards, build,
 * rearrange, and traverse the node graph, configure nodes, and manage blackboard keys.
 */
UCLASS(BlueprintType, MinimalAPI)
class UBehaviorTreeToolset : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------- //
	// Asset
	// ---------------------------------------------------------------- //

	/**
	 * Creates a new BehaviorTree asset, ready to edit.
	 * @param FolderPath Content folder to create in (Path e.g. "/Game/AI").
	 * @param AssetName Name for the new asset.
	 * @return The created BehaviorTree.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static UBehaviorTree* CreateBehaviorTree(const FDirectoryPath& FolderPath, const FString& AssetName);

	/**
	 * Sets the tree's blackboard asset.
	 * @param BehaviorTree The tree to modify.
	 * @param Blackboard The blackboard to assign, or null to clear.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static void SetBlackboard(UBehaviorTree* BehaviorTree, UBlackboardData* Blackboard);

	/**
	 * Returns the tree's root graph node.
	 * @param BehaviorTree The tree to query.
	 * @return The root graph node.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BehaviorTreeToolset")
	static UBehaviorTreeGraphNode* GetRootGraphNode(UBehaviorTree* BehaviorTree);

	// ---------------------------------------------------------------- //
	// Composites / Tasks (edit, take and return graph nodes)
	// ---------------------------------------------------------------- //

	/**
	 * Adds the tree's single root composite.
	 * @param BehaviorTree The tree to modify.
	 * @param CompositeClass A concrete, placeable composite class.
	 * @return The new composite's graph node, or null if a root composite already exists.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static UBehaviorTreeGraphNode* AddRootComposite(UBehaviorTree* BehaviorTree, TSubclassOf<UBTCompositeNode> CompositeClass);

	/**
	 * Adds a composite as a child of a composite (or root) parent.
	 * @param Parent Graph node to add the composite under.
	 * @param CompositeClass A concrete, placeable composite class.
	 * @param Index Sibling position; -1 appends.
	 * @return The new composite's graph node.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static UBehaviorTreeGraphNode* AddComposite(UBehaviorTreeGraphNode* Parent, TSubclassOf<UBTCompositeNode> CompositeClass, int32 Index = -1);

	/**
	 * Adds a task as a child of a composite parent.
	 * @param Parent Composite graph node to add the task under.
	 * @param TaskClass A concrete, placeable task class.
	 * @param Index Sibling position; -1 appends.
	 * @return The new task's graph node.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static UBehaviorTreeGraphNode* AddTask(UBehaviorTreeGraphNode* Parent, TSubclassOf<UBTTaskNode> TaskClass, int32 Index = -1);

	/**
	 * Reorders a child among its siblings.
	 * @param Parent The parent graph node.
	 * @param Child The child graph node to move.
	 * @param NewIndex Target sibling position.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static void MoveGraphNode(UBehaviorTreeGraphNode* Parent, UBehaviorTreeGraphNode* Child, int32 NewIndex);

	/**
	 * Removes a composite/task graph node, with its children and subnodes. The root cannot be removed.
	 * @param Node The graph node to remove.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static void RemoveGraphNode(UBehaviorTreeGraphNode* Node);

	// ---------------------------------------------------------------- //
	// Decorators / Services (edit, take and return graph nodes)
	// ---------------------------------------------------------------- //

	/**
	 * Adds a decorator subnode to an owner.
	 * @param Owner Graph node to attach the decorator to.
	 * @param DecoratorClass A concrete, placeable decorator class.
	 * @param Index Position among the owner's decorators; -1 appends.
	 * @return The new decorator's graph node.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static UBehaviorTreeGraphNode* AddDecorator(UBehaviorTreeGraphNode* Owner, TSubclassOf<UBTDecorator> DecoratorClass, int32 Index = -1);

	/**
	 * Adds a service subnode to a composite owner.
	 * @param Owner Composite graph node to attach the service to.
	 * @param ServiceClass A concrete, placeable service class.
	 * @param Index Position among the owner's services; -1 appends.
	 * @return The new service's graph node.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static UBehaviorTreeGraphNode* AddService(UBehaviorTreeGraphNode* Owner, TSubclassOf<UBTService> ServiceClass, int32 Index = -1);

	/**
	 * Adds a root-level decorator, used when the tree is referenced as a subtree.
	 * @param BehaviorTree The tree to modify.
	 * @param DecoratorClass A concrete, placeable decorator class.
	 * @param Index Position among the root decorators; -1 appends.
	 * @return The new decorator's graph node.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static UBehaviorTreeGraphNode* AddRootDecorator(UBehaviorTree* BehaviorTree, TSubclassOf<UBTDecorator> DecoratorClass, int32 Index = -1);

	/**
	 * Removes a decorator or service subnode from its owner.
	 * @param Owner The owner graph node.
	 * @param Subnode The decorator or service graph node to remove.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static void RemoveSubnode(UBehaviorTreeGraphNode* Owner, UBehaviorTreeGraphNode* Subnode);

	/**
	 * Reorders an existing decorator or service among its siblings without recreating it, so its
	 * configured settings are kept. Decorator order affects evaluation and abort priority.
	 * @param Owner The owner graph node.
	 * @param Subnode The decorator or service graph node to move.
	 * @param Index Target position among the owner's decorators or services; -1 or past the end appends.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static void MoveSubnode(UBehaviorTreeGraphNode* Owner, UBehaviorTreeGraphNode* Subnode, int32 Index);

	// ---------------------------------------------------------------- //
	// Read / traverse
	// ---------------------------------------------------------------- //

	/**
	 * Returns the tree's blackboard asset.
	 * @param BehaviorTree The tree to query.
	 * @return The blackboard, or null if unassigned.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BehaviorTreeToolset")
	static UBlackboardData* GetBlackboard(UBehaviorTree* BehaviorTree);

	/**
	 * Returns the child graph nodes of a composite (or root) parent, in sibling order.
	 * @param Parent The parent graph node.
	 * @return The child graph nodes.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BehaviorTreeToolset")
	static TArray<UBehaviorTreeGraphNode*> GetChildGraphNodes(UBehaviorTreeGraphNode* Parent);

	/**
	 * Returns the decorator subnode graph nodes attached to a graph node.
	 * @param Owner The graph node to query.
	 * @return The decorator subnode graph nodes.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BehaviorTreeToolset")
	static TArray<UBehaviorTreeGraphNode*> GetDecoratorGraphNodes(UBehaviorTreeGraphNode* Owner);

	/**
	 * Returns the service subnode graph nodes attached to a graph node.
	 * @param Owner The graph node to query.
	 * @return The service subnode graph nodes.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BehaviorTreeToolset")
	static TArray<UBehaviorTreeGraphNode*> GetServiceGraphNodes(UBehaviorTreeGraphNode* Owner);

	/**
	 * Returns the behavior node instance that a graph node wraps. This is the object that holds the
	 * node's settings.
	 * @param Node The graph node to read.
	 * @return The node instance.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BehaviorTreeToolset")
	static UBTNode* GetNodeInstance(UBehaviorTreeGraphNode* Node);

	/**
	 * Returns the BehaviorTree run by a RunBehavior task node instance.
	 * @param Node The node instance to inspect.
	 * @return The referenced subtree, or null if the node is not a RunBehavior task.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BehaviorTreeToolset")
	static UBehaviorTree* GetSubtree(UBTNode* Node);

	// ---------------------------------------------------------------- //
	// Blackboard
	// ---------------------------------------------------------------- //

	/**
	 * Creates a new BlackboardData asset.
	 * @param FolderPath Content folder to create in (Path e.g. "/Game/AI").
	 * @param AssetName Name for the new asset.
	 * @return The created BlackboardData.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static UBlackboardData* CreateBlackboardData(const FDirectoryPath& FolderPath, const FString& AssetName);

	/**
	 * Adds a key to the blackboard.
	 * @param Blackboard The blackboard to modify.
	 * @param KeyName Name for the new key.
	 * @param KeyType A concrete, placeable key-type class.
	 * @param bInstanceSynced Whether the key is synchronized across all instances of the blackboard.
	 * @return The new key's (instanced) key-type object, for further configuration; null if a key of that name already exists.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static UBlackboardKeyType* AddBlackboardKey(UBlackboardData* Blackboard, FName KeyName, TSubclassOf<UBlackboardKeyType> KeyType, bool bInstanceSynced = false);

	/**
	 * Removes a key from the blackboard.
	 * @param Blackboard The blackboard to modify.
	 * @param KeyName The key to remove.
	 * @return True if a key was removed, false if no key of that name exists.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static bool RemoveBlackboardKey(UBlackboardData* Blackboard, FName KeyName);

	/**
	 * Returns the blackboard's own key names, not including parent-chain keys.
	 * @param Blackboard The blackboard to query.
	 * @return The key names.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BehaviorTreeToolset")
	static TArray<FName> GetBlackboardKeyNames(UBlackboardData* Blackboard);

	/**
	 * Returns the blackboard keys a node's key selector accepts, matching the editor's key dropdown:
	 * the tree blackboard's own keys (and its parents') filtered by the selector's allowed types (an
	 * empty filter accepts any key).
	 * @param Node The node instance that owns the selector.
	 * @param SelectorPropertyName The name of the node's key-selector property (usually "BlackboardKey").
	 * @return The names of the keys the selector accepts.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "BehaviorTreeToolset")
	static TArray<FName> GetAllowedBlackboardKeys(UBTNode* Node, FName SelectorPropertyName);

	/**
	 * Sets which blackboard key a node's key selector points at, by name, resolving the binding against
	 * the tree's blackboard. Prefer this over writing the selector's fields directly, since the resolved
	 * key id is a transient cache derived from the name.
	 * @param Node The node instance that owns the selector.
	 * @param SelectorPropertyName The name of the node's key-selector property (usually "BlackboardKey").
	 * @param KeyName The blackboard key to bind to; None clears the binding.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Write"), Category = "BehaviorTreeToolset")
	static void SetNodeBlackboardKey(UBTNode* Node, FName SelectorPropertyName, FName KeyName);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface
};
