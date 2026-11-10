// Copyright Epic Games, Inc. All Rights Reserved.

#include "BehaviorTreeToolset.h"

#include "AIGraphTypes.h"
#include "AIToolsetUtils.h"
#include "AssetToolsModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTreeFactory.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_SimpleParallel.h"
#include "BehaviorTreeGraphNode_SubtreeTask.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BlackboardDataFactory.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "IAssetTools.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"
#include "ToolsetRegistry/ToolsetLibrary59.h" // UE 5.8: 5.9 UToolsetLibrary additions

#include UE_INLINE_GENERATED_CPP_BY_NAME(BehaviorTreeToolset)

using namespace UE::AIModuleToolset::Graph;

namespace
{
	constexpr float ChildXSpacing = 400.0f;

	UBehaviorTreeGraph* GetBTGraph(UBehaviorTree* BehaviorTree)
	{
		return BehaviorTree ? Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph) : nullptr;
	}

	UBehaviorTreeGraphNode_Root* GetRoot(UBehaviorTree* BehaviorTree)
	{
		UBehaviorTreeGraph* Graph = GetBTGraph(BehaviorTree);
		if (!Graph)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node))
			{
				return Root;
			}
		}
		return nullptr;
	}

	UClass* ChooseGraphNodeClassForComposite(TSubclassOf<UBTCompositeNode> CompositeClass)
	{
		if (*CompositeClass == UBTComposite_SimpleParallel::StaticClass())
		{
			return UBehaviorTreeGraphNode_SimpleParallel::StaticClass();
		}
		return UBehaviorTreeGraphNode_Composite::StaticClass();
	}

	UClass* ChooseGraphNodeClassForTask(TSubclassOf<UBTTaskNode> TaskClass)
	{
		if (*TaskClass == UBTTask_RunBehavior::StaticClass())
		{
			return UBehaviorTreeGraphNode_SubtreeTask::StaticClass();
		}
		return UBehaviorTreeGraphNode_Task::StaticClass();
	}

	/** Returns the parent's existing children (graph nodes) ordered as they appear on the output pin. */
	TArray<UBehaviorTreeGraphNode*> GetOrderedChildren(UBehaviorTreeGraphNode* Parent)
	{
		TArray<UBehaviorTreeGraphNode*> Result;
		UEdGraphPin* OutPin = FindPin(Parent, EGPD_Output);
		if (!OutPin)
		{
			return Result;
		}
		Result.Reserve(OutPin->LinkedTo.Num());
		for (UEdGraphPin* LinkedPin : OutPin->LinkedTo)
		{
			if (LinkedPin)
			{
				if (UBehaviorTreeGraphNode* Child = Cast<UBehaviorTreeGraphNode>(LinkedPin->GetOwningNode()))
				{
					Result.Add(Child);
				}
			}
		}
		return Result;
	}

	/** Computes an X position that, when sorted with existing children's X, lands at TargetIndex. */
	float ComputeChildXPosition(UBehaviorTreeGraphNode* Parent, int32 TargetIndex)
	{
		TArray<UBehaviorTreeGraphNode*> Siblings = GetOrderedChildren(Parent);
		const int32 Count = Siblings.Num();
		const float ParentX = static_cast<float>(Parent->NodePosX);

		if (Count == 0)
		{
			return ParentX;
		}
		// Children are ordered by X, so -1 (and anything past the end) must map to the rightmost
		// slot to append; only an explicit 0 prepends.
		if (TargetIndex < 0 || TargetIndex >= Count)
		{
			return static_cast<float>(Siblings[Count - 1]->NodePosX) + ChildXSpacing;
		}
		if (TargetIndex == 0)
		{
			return static_cast<float>(Siblings[0]->NodePosX) - ChildXSpacing;
		}
		const float Left = static_cast<float>(Siblings[TargetIndex - 1]->NodePosX);
		const float Right = static_cast<float>(Siblings[TargetIndex]->NodePosX);
		return 0.5f * (Left + Right);
	}

	UBehaviorTreeGraphNode* SpawnTopLevelNode(UClass* GraphNodeClass, UClass* InstanceClass, UBehaviorTreeGraph& Graph)
	{
		UBehaviorTreeGraphNode* GraphNode = NewObject<UBehaviorTreeGraphNode>(&Graph, GraphNodeClass, NAME_None, RF_Transactional);
		GraphNode->ClassData = FGraphNodeClassData(InstanceClass, FString());
		GraphNode->CreateNewGuid();
		Graph.AddNode(GraphNode, /*bUserAction*/false, /*bSelectNewNode*/false);
		GraphNode->PostPlacedNewNode();
		GraphNode->AllocateDefaultPins();
		return GraphNode;
	}

	UBehaviorTreeGraphNode* AddChildNode(UBehaviorTreeGraphNode* Parent, UClass* GraphNodeClass, UClass* InstanceClass, int32 Index)
	{
		if (!Parent || !GraphNodeClass || !InstanceClass)
		{
			return nullptr;
		}
		UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Parent->GetGraph());
		if (!Graph)
		{
			return nullptr;
		}
		UEdGraphPin* ParentOut = FindPin(Parent, EGPD_Output);
		if (!ParentOut)
		{
			return nullptr;
		}

		const float TargetX = ComputeChildXPosition(Parent, Index);
		UBehaviorTreeGraphNode* Child = SpawnTopLevelNode(GraphNodeClass, InstanceClass, *Graph);
		if (!Child)
		{
			return nullptr;
		}
		Child->NodePosX = static_cast<int32>(TargetX);
		Child->NodePosY = Parent->NodePosY + 200;

		UEdGraphPin* ChildIn = FindPin(Child, EGPD_Input);
		if (ChildIn)
		{
			ParentOut->MakeLinkTo(ChildIn);
		}

		Graph->NotifyGraphChanged();
		Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
		Graph->MarkPackageDirty();
		return Child;
	}

	// Move a subnode to position Index within its bucket (decorators or services), reusing the same
	// reorder the editor runs on a drag & drop so the typed bucket and the combined SubNodes list
	// stay in sync. We hand the engine an anchor sibling and let it work out where the node lands;
	// Index stays a plain bucket position for the caller. A negative Index, or one past the last
	// slot, appends.
	void ReorderSubNode(UBehaviorTreeGraphNode* Owner, UBehaviorTreeGraphNode* SubNode, const TArray<TObjectPtr<UBehaviorTreeGraphNode>>& Bucket, int32 Index)
	{
		const int32 CurrentIdx = Bucket.IndexOfByKey(SubNode);
		if (CurrentIdx == INDEX_NONE)
		{
			return;
		}

		// A negative Index, or one past the end, means "append" (the last slot).
		const int32 LastIdx = Bucket.Num() - 1;
		const int32 Target = (Index < 0 || Index > LastIdx) ? LastIdx : Index;
		if (Target == CurrentIdx)
		{
			return;
		}

		// The sibling the node should sit in front of, as a drag & drop would land it: moving earlier
		// drops before the node now at Target; moving later drops before the node just past it, or
		// appends when Target is the last slot.
		const int32 AnchorIdx = (Target < CurrentIdx) ? Target : Target + 1;
		UBehaviorTreeGraphNode* Anchor = Bucket.IsValidIndex(AnchorIdx) ? Bucket[AnchorIdx].Get() : nullptr;

		Owner->RemoveSubNode(SubNode);
		Owner->InsertSubNodeAt(SubNode, Anchor ? Owner->FindSubNodeDropIndex(Anchor) : 0);
	}

	// Blackboard-key selector helpers, shared by GetAllowedBlackboardKeys and SetNodeBlackboardKey.

	FStructProperty* FindKeySelectorProperty(const UBTNode* Node, FName SelectorPropertyName)
	{
		FStructProperty* Prop = FindFProperty<FStructProperty>(Node->GetClass(), SelectorPropertyName);
		return (Prop && Prop->Struct == FBlackboardKeySelector::StaticStruct()) ? Prop : nullptr;
	}

	FString ListKeySelectorNames(const UClass* NodeClass)
	{
		TArray<FString> Names;
		for (TFieldIterator<FStructProperty> It(NodeClass); It; ++It)
		{
			if (It->Struct == FBlackboardKeySelector::StaticStruct())
			{
				Names.Add(It->GetName());
			}
		}
		return Names.Num() ? FString::Join(Names, TEXT(", ")) : TEXT("none");
	}

	// Whether a key of KeyType passes the selector's type filter, matching the editor dropdown's rule:
	// an empty AllowedTypes accepts any key, otherwise the key must satisfy one of the filters.
	bool IsKeyAllowedBySelector(const FBlackboardKeySelector& Selector, const UBlackboardKeyType& KeyType)
	{
		if (Selector.AllowedTypes.Num() == 0)
		{
			return true;
		}
		for (const TObjectPtr<UBlackboardKeyType>& Filter : Selector.AllowedTypes)
		{
			if (Filter && KeyType.IsAllowedByFilter(Filter))
			{
				return true;
			}
		}
		return false;
	}
}

// ------------------------------------------------------------------ //
// Asset
// ------------------------------------------------------------------ //

UBehaviorTree* UBehaviorTreeToolset::CreateBehaviorTree(const FDirectoryPath& FolderPath, const FString& AssetName)
{
	if (FolderPath.Path.IsEmpty() || AssetName.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CreateBehaviorTree: FolderPath and AssetName are required."));
		return nullptr;
	}

	if (!FToolsetLibrary59::ValidateAssetPath(FolderPath.Path)) // UE 5.8: 5.9 UToolsetLibrary addition
	{
		return nullptr;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UBehaviorTreeFactory* Factory = NewObject<UBehaviorTreeFactory>();
	UObject* NewAsset = AssetToolsModule.Get().CreateAsset(AssetName, FolderPath.Path, UBehaviorTree::StaticClass(), Factory);
	UBehaviorTree* BehaviorTree = Cast<UBehaviorTree>(NewAsset);
	if (!BehaviorTree)
	{
		return nullptr;
	}

	// Mirror FBehaviorTreeEditor::RestoreBehaviorTree's graph setup so the asset is editable
	// immediately without needing the BT editor window to be opened.
	const FName GraphName(TEXT("BehaviorTree"));
	const TSubclassOf<UEdGraphSchema> SchemaClass = GetDefault<UBehaviorTreeGraph>(UBehaviorTreeGraph::StaticClass())->Schema;
	BehaviorTree->BTGraph = FBlueprintEditorUtils::CreateNewGraph(
		BehaviorTree, GraphName, UBehaviorTreeGraph::StaticClass(), SchemaClass);

	UBehaviorTreeGraph* MyGraph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	if (MyGraph)
	{
		const UEdGraphSchema* Schema = MyGraph->GetSchema();
		Schema->CreateDefaultNodesForGraph(*MyGraph);
		MyGraph->OnCreated();
		MyGraph->Initialize();
		MyGraph->UpdateAsset(UBehaviorTreeGraph::ClearDebuggerFlags | UBehaviorTreeGraph::KeepRebuildCounter);
	}

	BehaviorTree->MarkPackageDirty();
	return BehaviorTree;
}

void UBehaviorTreeToolset::SetBlackboard(UBehaviorTree* BehaviorTree, UBlackboardData* Blackboard)
{
	if (!BehaviorTree)
	{
		return;
	}
	UBehaviorTreeGraphNode_Root* Root = GetRoot(BehaviorTree);
	if (!Root)
	{
		// No graph yet: assign directly. The next graph creation copies it back via the root's PostPlacedNewNode.
		BehaviorTree->BlackboardAsset = Blackboard;
	}
	else
	{
		Root->Modify();
		Root->BlackboardAsset = Blackboard;
		Root->UpdateBlackboard();
	}
	BehaviorTree->MarkPackageDirty();
}

UBehaviorTreeGraphNode* UBehaviorTreeToolset::GetRootGraphNode(UBehaviorTree* BehaviorTree)
{
	return GetRoot(BehaviorTree);
}

// ------------------------------------------------------------------ //
// Composites / Tasks
// ------------------------------------------------------------------ //

UBehaviorTreeGraphNode* UBehaviorTreeToolset::AddRootComposite(UBehaviorTree* BehaviorTree, TSubclassOf<UBTCompositeNode> CompositeClass)
{
	if (!BehaviorTree)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddRootComposite: BehaviorTree is null."));
		return nullptr;
	}
	if (!IsPlaceableClass(*CompositeClass, UBTCompositeNode::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddRootComposite: %s is not a placeable composite; pass a concrete UBTCompositeNode subclass."),
			*GetNameSafe(*CompositeClass)));
		return nullptr;
	}
	UBehaviorTreeGraphNode_Root* Root = GetRoot(BehaviorTree);
	if (!Root)
	{
		return nullptr;
	}
	UEdGraphPin* RootOut = FindPin(Root, EGPD_Output);
	if (!RootOut || RootOut->LinkedTo.Num() > 0)
	{
		// Tree already has a root-attached composite; require explicit removal first.
		return nullptr;
	}
	return AddChildNode(Root, ChooseGraphNodeClassForComposite(CompositeClass), *CompositeClass, /*Index*/0);
}

UBehaviorTreeGraphNode* UBehaviorTreeToolset::AddComposite(UBehaviorTreeGraphNode* Parent, TSubclassOf<UBTCompositeNode> CompositeClass, int32 Index)
{
	if (!Parent)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddComposite: Parent graph node is null."));
		return nullptr;
	}
	if (!IsPlaceableClass(*CompositeClass, UBTCompositeNode::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddComposite: %s is not a placeable composite; pass a concrete UBTCompositeNode subclass."),
			*GetNameSafe(*CompositeClass)));
		return nullptr;
	}
	return AddChildNode(Parent, ChooseGraphNodeClassForComposite(CompositeClass), *CompositeClass, Index);
}

UBehaviorTreeGraphNode* UBehaviorTreeToolset::AddTask(UBehaviorTreeGraphNode* Parent, TSubclassOf<UBTTaskNode> TaskClass, int32 Index)
{
	if (!Parent)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddTask: Parent graph node is null."));
		return nullptr;
	}
	if (!IsPlaceableClass(*TaskClass, UBTTaskNode::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddTask: %s is not a placeable task; pass a concrete UBTTaskNode subclass."),
			*GetNameSafe(*TaskClass)));
		return nullptr;
	}
	return AddChildNode(Parent, ChooseGraphNodeClassForTask(TaskClass), *TaskClass, Index);
}

void UBehaviorTreeToolset::MoveGraphNode(UBehaviorTreeGraphNode* Parent, UBehaviorTreeGraphNode* Child, int32 NewIndex)
{
	if (!Parent || !Child)
	{
		return;
	}
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Parent->GetGraph());
	if (!Graph)
	{
		return;
	}
	TArray<UBehaviorTreeGraphNode*> Siblings = GetOrderedChildren(Parent);
	const int32 CurrentIndex = Siblings.IndexOfByKey(Child);
	if (CurrentIndex == INDEX_NONE)
	{
		return;
	}
	// Compute the X position without the child being considered.
	Siblings.RemoveAt(CurrentIndex);
	const int32 Count = Siblings.Num();
	const int32 ClampedTarget = FMath::Clamp(NewIndex, 0, Count);

	float TargetX;
	if (Count == 0)
	{
		TargetX = static_cast<float>(Parent->NodePosX);
	}
	else if (ClampedTarget == 0)
	{
		TargetX = static_cast<float>(Siblings[0]->NodePosX) - ChildXSpacing;
	}
	else if (ClampedTarget >= Count)
	{
		TargetX = static_cast<float>(Siblings[Count - 1]->NodePosX) + ChildXSpacing;
	}
	else
	{
		const float Left = static_cast<float>(Siblings[ClampedTarget - 1]->NodePosX);
		const float Right = static_cast<float>(Siblings[ClampedTarget]->NodePosX);
		TargetX = 0.5f * (Left + Right);
	}

	Child->Modify();
	Child->NodePosX = static_cast<int32>(TargetX);
	Graph->RebuildChildOrder(Parent);
	Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
	Graph->MarkPackageDirty();
}

void UBehaviorTreeToolset::RemoveGraphNode(UBehaviorTreeGraphNode* Node)
{
	if (!Node)
	{
		return;
	}
	if (Node->IsA(UBehaviorTreeGraphNode_Root::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveGraphNode: cannot remove the root node."));
		return;
	}
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Node->GetGraph());
	if (!Graph)
	{
		return;
	}

	Node->Modify();
	Node->DestroyNode();

	Graph->NotifyGraphChanged();
	Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
	Graph->MarkPackageDirty();
}

// ------------------------------------------------------------------ //
// Decorators / Services
// ------------------------------------------------------------------ //

UBehaviorTreeGraphNode* UBehaviorTreeToolset::AddDecorator(UBehaviorTreeGraphNode* Owner, TSubclassOf<UBTDecorator> DecoratorClass, int32 Index)
{
	if (!Owner)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddDecorator: Owner graph node is null."));
		return nullptr;
	}
	if (!IsPlaceableClass(*DecoratorClass, UBTDecorator::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddDecorator: %s is not a placeable decorator; pass a concrete UBTDecorator subclass."),
			*GetNameSafe(*DecoratorClass)));
		return nullptr;
	}
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Owner->GetGraph());
	if (!Graph)
	{
		return nullptr;
	}
	UBehaviorTreeGraphNode_Decorator* SubNode = NewObject<UBehaviorTreeGraphNode_Decorator>(Graph, NAME_None, RF_Transactional);
	SubNode->ClassData = FGraphNodeClassData(*DecoratorClass, FString());
	Owner->AddSubNode(SubNode, Graph);
	ReorderSubNode(Owner, SubNode, Owner->Decorators, Index);
	Graph->MarkPackageDirty();
	return SubNode;
}

UBehaviorTreeGraphNode* UBehaviorTreeToolset::AddService(UBehaviorTreeGraphNode* Owner, TSubclassOf<UBTService> ServiceClass, int32 Index)
{
	if (!Owner)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddService: Owner graph node is null."));
		return nullptr;
	}
	if (!IsPlaceableClass(*ServiceClass, UBTService::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddService: %s is not a placeable service; pass a concrete UBTService subclass."),
			*GetNameSafe(*ServiceClass)));
		return nullptr;
	}
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Owner->GetGraph());
	if (!Graph)
	{
		return nullptr;
	}
	UBehaviorTreeGraphNode_Service* SubNode = NewObject<UBehaviorTreeGraphNode_Service>(Graph, NAME_None, RF_Transactional);
	SubNode->ClassData = FGraphNodeClassData(*ServiceClass, FString());
	Owner->AddSubNode(SubNode, Graph);
	ReorderSubNode(Owner, SubNode, Owner->Services, Index);
	Graph->MarkPackageDirty();
	return SubNode;
}

UBehaviorTreeGraphNode* UBehaviorTreeToolset::AddRootDecorator(UBehaviorTree* BehaviorTree, TSubclassOf<UBTDecorator> DecoratorClass, int32 Index)
{
	if (!BehaviorTree)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddRootDecorator: BehaviorTree is null."));
		return nullptr;
	}
	if (!IsPlaceableClass(*DecoratorClass, UBTDecorator::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddRootDecorator: %s is not a placeable decorator; pass a concrete UBTDecorator subclass."),
			*GetNameSafe(*DecoratorClass)));
		return nullptr;
	}
	UBehaviorTreeGraphNode_Root* Root = GetRoot(BehaviorTree);
	if (!Root)
	{
		return nullptr;
	}
	return AddDecorator(Root, DecoratorClass, Index);
}

void UBehaviorTreeToolset::RemoveSubnode(UBehaviorTreeGraphNode* Owner, UBehaviorTreeGraphNode* Subnode)
{
	if (!Owner || !Subnode)
	{
		return;
	}
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Owner->GetGraph());
	if (!Graph)
	{
		return;
	}
	Owner->Modify();
	Subnode->Modify();
	Owner->RemoveSubNode(Subnode);
	Subnode->DestroyNode();

	Graph->NotifyGraphChanged();
	Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
	Graph->MarkPackageDirty();
}

void UBehaviorTreeToolset::MoveSubnode(UBehaviorTreeGraphNode* Owner, UBehaviorTreeGraphNode* Subnode, int32 Index)
{
	if (!Owner || !Subnode)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("MoveSubnode: Owner or Subnode is null."));
		return;
	}
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Owner->GetGraph());
	if (!Graph)
	{
		return;
	}

	Owner->Modify();
	if (Owner->Decorators.Contains(Subnode))
	{
		ReorderSubNode(Owner, Subnode, Owner->Decorators, Index);
	}
	else if (Owner->Services.Contains(Subnode))
	{
		ReorderSubNode(Owner, Subnode, Owner->Services, Index);
	}
	else
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("MoveSubnode: Subnode is not a decorator or service of Owner."));
		return;
	}

	Graph->NotifyGraphChanged();
	Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
	Graph->MarkPackageDirty();
}

// ------------------------------------------------------------------ //
// Read / traverse
// ------------------------------------------------------------------ //

UBlackboardData* UBehaviorTreeToolset::GetBlackboard(UBehaviorTree* BehaviorTree)
{
	return BehaviorTree ? BehaviorTree->GetBlackboardAsset() : nullptr;
}

TArray<UBehaviorTreeGraphNode*> UBehaviorTreeToolset::GetChildGraphNodes(UBehaviorTreeGraphNode* Parent)
{
	return Parent ? GetOrderedChildren(Parent) : TArray<UBehaviorTreeGraphNode*>();
}

TArray<UBehaviorTreeGraphNode*> UBehaviorTreeToolset::GetDecoratorGraphNodes(UBehaviorTreeGraphNode* Owner)
{
	TArray<UBehaviorTreeGraphNode*> Result;
	if (Owner)
	{
		for (UBehaviorTreeGraphNode* Decorator : Owner->Decorators)
		{
			Result.Add(Decorator);
		}
	}
	return Result;
}

TArray<UBehaviorTreeGraphNode*> UBehaviorTreeToolset::GetServiceGraphNodes(UBehaviorTreeGraphNode* Owner)
{
	TArray<UBehaviorTreeGraphNode*> Result;
	if (Owner)
	{
		for (UBehaviorTreeGraphNode* Service : Owner->Services)
		{
			Result.Add(Service);
		}
	}
	return Result;
}

UBTNode* UBehaviorTreeToolset::GetNodeInstance(UBehaviorTreeGraphNode* Node)
{
	return Node ? Cast<UBTNode>(Node->NodeInstance) : nullptr;
}

UBehaviorTree* UBehaviorTreeToolset::GetSubtree(UBTNode* Node)
{
	if (UBTTask_RunBehavior* RunBehavior = Cast<UBTTask_RunBehavior>(Node))
	{
		return RunBehavior->GetSubtreeAsset();
	}
	return nullptr;
}

// ------------------------------------------------------------------ //
// Blackboard
// ------------------------------------------------------------------ //

UBlackboardData* UBehaviorTreeToolset::CreateBlackboardData(const FDirectoryPath& FolderPath, const FString& AssetName)
{
	if (FolderPath.Path.IsEmpty() || AssetName.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CreateBlackboardData: FolderPath and AssetName are required."));
		return nullptr;
	}

	if (!FToolsetLibrary59::ValidateAssetPath(FolderPath.Path)) // UE 5.8: 5.9 UToolsetLibrary addition
	{
		return nullptr;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UBlackboardDataFactory* Factory = NewObject<UBlackboardDataFactory>();
	UObject* NewAsset = AssetToolsModule.Get().CreateAsset(AssetName, FolderPath.Path, UBlackboardData::StaticClass(), Factory);
	return Cast<UBlackboardData>(NewAsset);
}

UBlackboardKeyType* UBehaviorTreeToolset::AddBlackboardKey(UBlackboardData* Blackboard, FName KeyName, TSubclassOf<UBlackboardKeyType> KeyType, bool bInstanceSynced)
{
	if (!Blackboard || KeyName.IsNone() || !*KeyType || KeyType->HasAnyClassFlags(CLASS_Abstract))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddBlackboardKey: need a blackboard, a name, and a concrete key type (got %s)."),
			*GetNameSafe(*KeyType)));
		return nullptr;
	}
	for (const FBlackboardEntry& Existing : Blackboard->Keys)
	{
		if (Existing.EntryName == KeyName)
		{
			return nullptr;
		}
	}

	Blackboard->Modify();
	FBlackboardEntry Entry;
	Entry.EntryName = KeyName;
	Entry.bInstanceSynced = bInstanceSynced;
	UBlackboardKeyType* KeyTypeInstance = NewObject<UBlackboardKeyType>(Blackboard, *KeyType);
	Entry.KeyType = KeyTypeInstance;
	Blackboard->Keys.Add(Entry);

	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateParentKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	Blackboard->MarkPackageDirty();
	return KeyTypeInstance;
}

bool UBehaviorTreeToolset::RemoveBlackboardKey(UBlackboardData* Blackboard, FName KeyName)
{
	if (!Blackboard)
	{
		return false;
	}
	const int32 RemovedIndex = Blackboard->Keys.IndexOfByPredicate(
		[KeyName](const FBlackboardEntry& Entry) { return Entry.EntryName == KeyName; });
	if (RemovedIndex == INDEX_NONE)
	{
		return false;
	}

	Blackboard->Modify();
	Blackboard->Keys.RemoveAt(RemovedIndex);
	Blackboard->UpdateKeyIDs();
	Blackboard->UpdateParentKeys();
	Blackboard->PropagateKeyChangesToDerivedBlackboardAssets();
	Blackboard->MarkPackageDirty();
	return true;
}

TArray<FName> UBehaviorTreeToolset::GetBlackboardKeyNames(UBlackboardData* Blackboard)
{
	TArray<FName> Names;
	if (Blackboard)
	{
		Names.Reserve(Blackboard->Keys.Num());
		for (const FBlackboardEntry& Entry : Blackboard->Keys)
		{
			Names.Add(Entry.EntryName);
		}
	}
	return Names;
}

TArray<FName> UBehaviorTreeToolset::GetAllowedBlackboardKeys(UBTNode* Node, FName SelectorPropertyName)
{
	TArray<FName> Result;
	if (!Node)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetAllowedBlackboardKeys: Node is null."));
		return Result;
	}

	FStructProperty* SelectorProp = FindKeySelectorProperty(Node, SelectorPropertyName);
	if (!SelectorProp)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("GetAllowedBlackboardKeys: %s has no blackboard-key selector named '%s'. Available selectors: %s."),
			*GetNameSafe(Node->GetClass()), *SelectorPropertyName.ToString(), *ListKeySelectorNames(Node->GetClass())));
		return Result;
	}

	UBehaviorTree* Tree = Node->GetTypedOuter<UBehaviorTree>();
	UBlackboardData* Blackboard = Tree ? Tree->GetBlackboardAsset() : nullptr;
	if (!Blackboard)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetAllowedBlackboardKeys: the node's tree has no blackboard; call SetBlackboard first."));
		return Result;
	}

	// Walk the blackboard and its parents, keeping the keys the selector's filter accepts - the same
	// set the editor's key dropdown offers.
	const FBlackboardKeySelector& Selector = *SelectorProp->ContainerPtrToValuePtr<FBlackboardKeySelector>(Node);
	for (const UBlackboardData* It = Blackboard; It; It = It->Parent)
	{
		for (const FBlackboardEntry& Entry : It->Keys)
		{
			if (Entry.KeyType && IsKeyAllowedBySelector(Selector, *Entry.KeyType))
			{
				Result.AddUnique(Entry.EntryName);
			}
		}
	}
	return Result;
}

void UBehaviorTreeToolset::SetNodeBlackboardKey(UBTNode* Node, FName SelectorPropertyName, FName KeyName)
{
	if (!Node)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetNodeBlackboardKey: Node is null."));
		return;
	}

	FStructProperty* SelectorProp = FindKeySelectorProperty(Node, SelectorPropertyName);
	if (!SelectorProp)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("SetNodeBlackboardKey: %s has no blackboard-key selector named '%s'. Available selectors: %s."),
			*GetNameSafe(Node->GetClass()), *SelectorPropertyName.ToString(), *ListKeySelectorNames(Node->GetClass())));
		return;
	}

	UBehaviorTree* Tree = Node->GetTypedOuter<UBehaviorTree>();
	UBlackboardData* Blackboard = Tree ? Tree->GetBlackboardAsset() : nullptr;
	if (!Blackboard)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetNodeBlackboardKey: the node's tree has no blackboard; call SetBlackboard first."));
		return;
	}

	FBlackboardKeySelector& Selector = *SelectorProp->ContainerPtrToValuePtr<FBlackboardKeySelector>(Node);

	// Validate before mutating so a rejected key leaves the selector untouched.
	if (!KeyName.IsNone())
	{
		const FBlackboardEntry* Entry = Blackboard->GetKey(Blackboard->GetKeyID(KeyName));
		if (!Entry || !Entry->KeyType)
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("SetNodeBlackboardKey: key '%s' not found in blackboard '%s'. Call GetBlackboardKeyNames()."),
				*KeyName.ToString(), *GetNameSafe(Blackboard)));
			return;
		}
		if (!IsKeyAllowedBySelector(Selector, *Entry->KeyType))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("SetNodeBlackboardKey: key '%s' (%s) is not accepted by the '%s' selector. Call GetAllowedBlackboardKeys()."),
				*KeyName.ToString(), *GetNameSafe(Entry->KeyType->GetClass()), *SelectorPropertyName.ToString()));
			return;
		}
	}

	Node->Modify();
	// SelectedKeyID/Type are a transient cache derived from the name; resolve from the name rather than
	// writing a raw id. None clears it.
	Selector.SelectedKeyName = KeyName;
	Selector.InvalidateResolvedKey();
	if (!KeyName.IsNone())
	{
		Selector.ResolveSelectedKey(*Blackboard);
	}

#if WITH_EDITOR
	// Rebuild any cached, name-derived state (e.g. a decorator's description) through the node's normal edit path.
	if (FProperty* NameProp = FindFProperty<FProperty>(FBlackboardKeySelector::StaticStruct(), GET_MEMBER_NAME_CHECKED(FBlackboardKeySelector, SelectedKeyName)))
	{
		FPropertyChangedEvent ChangeEvent(NameProp);
		Node->PostEditChangeProperty(ChangeEvent);
	}
#endif

	Tree->MarkPackageDirty();
}

bool UBehaviorTreeToolset::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
