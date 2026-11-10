// Copyright Epic Games, Inc. All Rights Reserved.

#include "EnvironmentQueryToolset.h"

#include "AIGraphTypes.h"
#include "AIToolsetUtils.h"
#include "AssetToolsModule.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_EnvironmentQuery.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryNode.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQueryFactory.h"
#include "EnvironmentQueryGraph.h"
#include "EnvironmentQueryGraphNode.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "EnvironmentQueryGraphNode_Root.h"
#include "EnvironmentQueryGraphNode_Test.h"
#include "IAssetTools.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "UObject/UObjectHash.h"
#include "ToolsetRegistry/ToolsetLibrary59.h" // UE 5.8: 5.9 UToolsetLibrary additions

#include UE_INLINE_GENERATED_CPP_BY_NAME(EnvironmentQueryToolset)

using namespace UE::AIModuleToolset::Graph;

namespace
{
	constexpr float OptionXSpacing = 400.0f;

	UEnvironmentQueryGraph* GetEQSGraph(UEnvQuery* Query)
	{
		return Query ? Cast<UEnvironmentQueryGraph>(Query->EdGraph) : nullptr;
	}

	UEnvironmentQueryGraphNode_Root* GetRoot(UEnvQuery* Query)
	{
		UEnvironmentQueryGraph* Graph = GetEQSGraph(Query);
		if (!Graph)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UEnvironmentQueryGraphNode_Root* Root = Cast<UEnvironmentQueryGraphNode_Root>(Node))
			{
				return Root;
			}
		}
		return nullptr;
	}

	/** Returns the root's option nodes, ordered as they appear on its output pin. */
	TArray<UEnvironmentQueryGraphNode*> GetOrderedOptions(UEnvironmentQueryGraphNode_Root* Root)
	{
		TArray<UEnvironmentQueryGraphNode*> Result;
		UEdGraphPin* OutPin = FindPin(Root, EGPD_Output);
		if (!OutPin)
		{
			return Result;
		}
		Result.Reserve(OutPin->LinkedTo.Num());
		for (UEdGraphPin* LinkedPin : OutPin->LinkedTo)
		{
			if (LinkedPin)
			{
				if (UEnvironmentQueryGraphNode_Option* Option = Cast<UEnvironmentQueryGraphNode_Option>(LinkedPin->GetOwningNode()))
				{
					Result.Add(Option);
				}
			}
		}
		return Result;
	}

	/** Computes an X position that, when sorted with existing options' X, lands at TargetIndex. */
	float ComputeOptionXPosition(UEnvironmentQueryGraphNode_Root* Root, int32 TargetIndex)
	{
		TArray<UEnvironmentQueryGraphNode*> Siblings = GetOrderedOptions(Root);
		const int32 Count = Siblings.Num();
		const float RootX = static_cast<float>(Root->NodePosX);

		if (Count == 0)
		{
			return RootX;
		}
		// Options are ordered by X, so -1 (and anything past the end) maps to the rightmost slot to
		// append; only an explicit 0 prepends.
		if (TargetIndex < 0 || TargetIndex >= Count)
		{
			return static_cast<float>(Siblings[Count - 1]->NodePosX) + OptionXSpacing;
		}
		if (TargetIndex == 0)
		{
			return static_cast<float>(Siblings[0]->NodePosX) - OptionXSpacing;
		}
		const float Left = static_cast<float>(Siblings[TargetIndex - 1]->NodePosX);
		const float Right = static_cast<float>(Siblings[TargetIndex]->NodePosX);
		return 0.5f * (Left + Right);
	}

	// Move a test to Index within its option's SubNodes list, the single order that drives the built
	// query (UpdateAsset assigns TestOrder from this position). Index is the final array slot; a
	// negative Index, or one past the end, appends.
	void MoveTestInBucket(UEnvironmentQueryGraphNode* Option, UAIGraphNode* Test, int32 Index)
	{
		const int32 CurrentIdx = Option->SubNodes.IndexOfByKey(Test);
		if (CurrentIdx == INDEX_NONE)
		{
			return;
		}
		Option->SubNodes.RemoveAt(CurrentIdx);
		const int32 Count = Option->SubNodes.Num();
		const int32 Target = (Index < 0 || Index > Count) ? Count : Index;
		Option->SubNodes.Insert(Test, Target);
	}
}

// ------------------------------------------------------------------ //
// Asset
// ------------------------------------------------------------------ //

UEnvQuery* UEnvironmentQueryToolset::CreateEnvironmentQuery(const FDirectoryPath& FolderPath, const FString& AssetName)
{
	if (FolderPath.Path.IsEmpty() || AssetName.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CreateEnvironmentQuery: FolderPath and AssetName are required."));
		return nullptr;
	}

	if (!FToolsetLibrary59::ValidateAssetPath(FolderPath.Path)) // UE 5.8: 5.9 UToolsetLibrary addition
	{
		return nullptr;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UEnvironmentQueryFactory* Factory = NewObject<UEnvironmentQueryFactory>();
	UObject* NewAsset = AssetToolsModule.Get().CreateAsset(AssetName, FolderPath.Path, UEnvQuery::StaticClass(), Factory);
	UEnvQuery* Query = Cast<UEnvQuery>(NewAsset);
	if (!Query)
	{
		return nullptr;
	}

	// Mirror FEnvironmentQueryEditor::SpawnTab_UpdateGraph's fresh-graph setup so the asset is
	// editable immediately without needing the EQS editor window to be opened.
	UEnvironmentQueryGraph* MyGraph = NewObject<UEnvironmentQueryGraph>(Query, NAME_None, RF_Transactional);
	Query->EdGraph = MyGraph;

	const UEdGraphSchema* Schema = MyGraph->GetSchema();
	Schema->CreateDefaultNodesForGraph(*MyGraph);
	MyGraph->OnCreated();
	MyGraph->Initialize();

	Query->MarkPackageDirty();
	return Query;
}

UEnvironmentQueryGraphNode* UEnvironmentQueryToolset::GetRootGraphNode(UEnvQuery* Query)
{
	return GetRoot(Query);
}

// ------------------------------------------------------------------ //
// Options
// ------------------------------------------------------------------ //

UEnvironmentQueryGraphNode* UEnvironmentQueryToolset::AddOption(UEnvQuery* Query, TSubclassOf<UEnvQueryGenerator> GeneratorClass, int32 Index)
{
	if (!Query)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddOption: Query is null."));
		return nullptr;
	}
	if (!IsPlaceableClass(*GeneratorClass, UEnvQueryGenerator::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddOption: %s is not a placeable generator; pass a concrete UEnvQueryGenerator subclass."),
			*GetNameSafe(*GeneratorClass)));
		return nullptr;
	}
	UEnvironmentQueryGraph* Graph = GetEQSGraph(Query);
	UEnvironmentQueryGraphNode_Root* Root = GetRoot(Query);
	if (!Graph || !Root)
	{
		return nullptr;
	}
	UEdGraphPin* RootOut = FindPin(Root, EGPD_Output);
	if (!RootOut)
	{
		return nullptr;
	}

	const float TargetX = ComputeOptionXPosition(Root, Index);

	UEnvironmentQueryGraphNode_Option* Option = NewObject<UEnvironmentQueryGraphNode_Option>(Graph, NAME_None, RF_Transactional);
	Option->ClassData = FGraphNodeClassData(*GeneratorClass, FString());
	Option->CreateNewGuid();
	Graph->AddNode(Option, /*bUserAction*/false, /*bSelectNewNode*/false);
	Option->PostPlacedNewNode();
	Option->AllocateDefaultPins();

	Option->NodePosX = static_cast<int32>(TargetX);
	Option->NodePosY = Root->NodePosY + 100;

	if (UEdGraphPin* OptionIn = FindPin(Option, EGPD_Input))
	{
		RootOut->MakeLinkTo(OptionIn);
	}

	Graph->NotifyGraphChanged();
	Graph->UpdateAsset();
	Graph->MarkPackageDirty();
	return Option;
}

void UEnvironmentQueryToolset::MoveOption(UEnvQuery* Query, UEnvironmentQueryGraphNode* Option, int32 NewIndex)
{
	if (!Query || !Option)
	{
		return;
	}
	UEnvironmentQueryGraph* Graph = GetEQSGraph(Query);
	UEnvironmentQueryGraphNode_Root* Root = GetRoot(Query);
	if (!Graph || !Root)
	{
		return;
	}
	TArray<UEnvironmentQueryGraphNode*> Siblings = GetOrderedOptions(Root);
	const int32 CurrentIndex = Siblings.IndexOfByKey(Option);
	if (CurrentIndex == INDEX_NONE)
	{
		return;
	}
	// Compute the X position without the option being considered.
	Siblings.RemoveAt(CurrentIndex);
	const int32 Count = Siblings.Num();
	const int32 ClampedTarget = FMath::Clamp(NewIndex, 0, Count);

	float TargetX;
	if (Count == 0)
	{
		TargetX = static_cast<float>(Root->NodePosX);
	}
	else if (ClampedTarget == 0)
	{
		TargetX = static_cast<float>(Siblings[0]->NodePosX) - OptionXSpacing;
	}
	else if (ClampedTarget >= Count)
	{
		TargetX = static_cast<float>(Siblings[Count - 1]->NodePosX) + OptionXSpacing;
	}
	else
	{
		const float Left = static_cast<float>(Siblings[ClampedTarget - 1]->NodePosX);
		const float Right = static_cast<float>(Siblings[ClampedTarget]->NodePosX);
		TargetX = 0.5f * (Left + Right);
	}

	Option->Modify();
	Option->NodePosX = static_cast<int32>(TargetX);
	Graph->UpdateAsset();
	Graph->MarkPackageDirty();
}

void UEnvironmentQueryToolset::RemoveOption(UEnvironmentQueryGraphNode* Option)
{
	if (!Option)
	{
		return;
	}
	if (Option->IsA(UEnvironmentQueryGraphNode_Root::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveOption: cannot remove the root node."));
		return;
	}
	UEnvironmentQueryGraph* Graph = Cast<UEnvironmentQueryGraph>(Option->GetGraph());
	if (!Graph)
	{
		return;
	}

	Option->Modify();
	Option->DestroyNode();

	Graph->NotifyGraphChanged();
	Graph->UpdateAsset();
	Graph->MarkPackageDirty();
}

TArray<UEnvironmentQueryGraphNode*> UEnvironmentQueryToolset::GetOptionGraphNodes(UEnvQuery* Query)
{
	UEnvironmentQueryGraphNode_Root* Root = GetRoot(Query);
	return Root ? GetOrderedOptions(Root) : TArray<UEnvironmentQueryGraphNode*>();
}

// ------------------------------------------------------------------ //
// Tests
// ------------------------------------------------------------------ //

UEnvironmentQueryGraphNode* UEnvironmentQueryToolset::AddTest(UEnvironmentQueryGraphNode* Option, TSubclassOf<UEnvQueryTest> TestClass, int32 Index)
{
	if (!Option)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddTest: Option graph node is null."));
		return nullptr;
	}
	if (!Option->IsA(UEnvironmentQueryGraphNode_Option::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddTest: Tests can only be added to an option node."));
		return nullptr;
	}
	if (!IsPlaceableClass(*TestClass, UEnvQueryTest::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddTest: %s is not a placeable test; pass a concrete UEnvQueryTest subclass."),
			*GetNameSafe(*TestClass)));
		return nullptr;
	}
	UEnvironmentQueryGraph* Graph = Cast<UEnvironmentQueryGraph>(Option->GetGraph());
	if (!Graph)
	{
		return nullptr;
	}

	UEnvironmentQueryGraphNode_Test* Test = NewObject<UEnvironmentQueryGraphNode_Test>(Graph, NAME_None, RF_Transactional);
	Test->ClassData = FGraphNodeClassData(*TestClass, FString());
	Option->AddSubNode(Test, Graph);
	MoveTestInBucket(Option, Test, Index);

	Graph->UpdateAsset();
	Graph->MarkPackageDirty();
	return Test;
}

void UEnvironmentQueryToolset::MoveTest(UEnvironmentQueryGraphNode* Option, UEnvironmentQueryGraphNode* Test, int32 Index)
{
	if (!Option || !Test)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("MoveTest: Option or Test is null."));
		return;
	}
	UEnvironmentQueryGraph* Graph = Cast<UEnvironmentQueryGraph>(Option->GetGraph());
	if (!Graph)
	{
		return;
	}
	if (!Option->SubNodes.Contains(Test))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("MoveTest: Test is not a test of Option."));
		return;
	}

	Option->Modify();
	MoveTestInBucket(Option, Test, Index);

	Graph->NotifyGraphChanged();
	Graph->UpdateAsset();
	Graph->MarkPackageDirty();
}

void UEnvironmentQueryToolset::RemoveTest(UEnvironmentQueryGraphNode* Option, UEnvironmentQueryGraphNode* Test)
{
	if (!Option || !Test)
	{
		return;
	}
	UEnvironmentQueryGraph* Graph = Cast<UEnvironmentQueryGraph>(Option->GetGraph());
	if (!Graph)
	{
		return;
	}
	Option->Modify();
	Test->Modify();
	Option->RemoveSubNode(Test);
	Test->DestroyNode();

	Graph->NotifyGraphChanged();
	Graph->UpdateAsset();
	Graph->MarkPackageDirty();
}

void UEnvironmentQueryToolset::SetTestEnabled(UEnvironmentQueryGraphNode* Test, bool bEnabled)
{
	UEnvironmentQueryGraphNode_Test* TestNode = Cast<UEnvironmentQueryGraphNode_Test>(Test);
	if (!TestNode)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetTestEnabled: node is not a test."));
		return;
	}
	UEnvironmentQueryGraph* Graph = Cast<UEnvironmentQueryGraph>(TestNode->GetGraph());
	if (!Graph)
	{
		return;
	}
	TestNode->Modify();
	TestNode->bTestEnabled = bEnabled;

	Graph->UpdateAsset();
	Graph->MarkPackageDirty();
}

TArray<UEnvironmentQueryGraphNode*> UEnvironmentQueryToolset::GetTestGraphNodes(UEnvironmentQueryGraphNode* Option)
{
	TArray<UEnvironmentQueryGraphNode*> Result;
	if (Option)
	{
		Result.Reserve(Option->SubNodes.Num());
		for (UAIGraphNode* SubNode : Option->SubNodes)
		{
			if (UEnvironmentQueryGraphNode* Test = Cast<UEnvironmentQueryGraphNode>(SubNode))
			{
				Result.Add(Test);
			}
		}
	}
	return Result;
}

// ------------------------------------------------------------------ //
// Read
// ------------------------------------------------------------------ //

UEnvQueryGenerator* UEnvironmentQueryToolset::GetGenerator(UEnvironmentQueryGraphNode* Option)
{
	UEnvQueryOption* OptionInstance = Option ? Cast<UEnvQueryOption>(Option->NodeInstance) : nullptr;
	return OptionInstance ? OptionInstance->Generator : nullptr;
}

UEnvQueryOption* UEnvironmentQueryToolset::GetOptionInstance(UEnvironmentQueryGraphNode* Option)
{
	return Option ? Cast<UEnvQueryOption>(Option->NodeInstance) : nullptr;
}

UEnvQueryTest* UEnvironmentQueryToolset::GetTestInstance(UEnvironmentQueryGraphNode* Test)
{
	return Test ? Cast<UEnvQueryTest>(Test->NodeInstance) : nullptr;
}

bool UEnvironmentQueryToolset::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
