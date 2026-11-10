// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetPropertyTracer.h"
#include "WidgetPropertyReflectionUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_Knot.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_Select.h"
#include "K2Node_StructMemberGet.h"
#include "K2Node_IfThenElse.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UObjectIterator.h"

void FWidgetPropertyTracer::CollectAllGraphs(const UWidgetBlueprint* WidgetBlueprint, TArray<UEdGraph*>& OutGraphs)
{
	WidgetBlueprint->GetAllGraphs(OutGraphs);

	TSet<UEdGraph*> SeenGraphs(OutGraphs);
	ForEachObjectWithOuter(WidgetBlueprint, [&OutGraphs, &SeenGraphs](UObject* Inner)
	{
		if (UEdGraph* Graph = Cast<UEdGraph>(Inner))
		{
			bool bAlreadyIn = false;
			SeenGraphs.Add(Graph, &bAlreadyIn);
			if (!bAlreadyIn)
			{
				OutGraphs.Add(Graph);
				Graph->GetAllChildrenGraphs(OutGraphs);
			}
		}
	}, EGetObjectsFlags::IncludeNestedObjects);
}

namespace
{
	// Tree widgets count only if marked as variables; inherited ones come from the generated class.
	bool IsWidgetVariable(const UWidgetTree* WidgetTree, const UClass* GeneratedClass, FName VariableName)
	{
		const UWidget* TreeWidget = WidgetTree ? WidgetTree->FindWidget(VariableName) : nullptr;
		if (TreeWidget && TreeWidget->bIsVariable)
		{
			return true;
		}

		if (GeneratedClass)
		{
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(GeneratedClass->FindPropertyByName(VariableName)))
			{
				return ObjectProperty->PropertyClass && ObjectProperty->PropertyClass->IsChildOf(UWidget::StaticClass());
			}
		}

		return false;
	}

	// Like IsWidgetVariable, but returns the widget's class.
	const UClass* ResolveWidgetClassForVariable(const UWidgetTree* WidgetTree, const UClass* GeneratedClass, FName VariableName)
	{
		const UWidget* TreeWidget = WidgetTree ? WidgetTree->FindWidget(VariableName) : nullptr;
		if (TreeWidget && TreeWidget->bIsVariable)
		{
			return TreeWidget->GetClass();
		}

		if (GeneratedClass)
		{
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(GeneratedClass->FindPropertyByName(VariableName)))
			{
				return ObjectProperty->PropertyClass;
			}
		}

		return nullptr;
	}

	// Forward exec walk from the named event, following called function graphs.
	TSet<UEdGraphNode*> CollectReachableNodesFromEvent(const TArray<UEdGraph*>& AllGraphs, FName EventFunctionName)
	{
		TArray<UEdGraphNode*> Worklist;
		for (UEdGraph* Graph : AllGraphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
				{
					if (EventNode->EventReference.GetMemberName() == EventFunctionName)
					{
						Worklist.Add(EventNode);
					}
				}
			}
		}

		TSet<UEdGraphNode*> Reachable;
		for (int32 WorkIdx = 0; WorkIdx < Worklist.Num(); ++WorkIdx)
		{
			UEdGraphNode* Node = Worklist[WorkIdx];
			bool bAlreadyVisited = false;
			Reachable.Add(Node, &bAlreadyVisited);
			if (bAlreadyVisited)
			{
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					Worklist.Add(LinkedPin->GetOwningNode());
				}
			}

			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				const UEdGraphNode* CalledGraphEntryNode = nullptr;
				if (UEdGraph* CalledGraph = CallNode->GetFunctionGraph(CalledGraphEntryNode))
				{
					for (UEdGraphNode* CalledGraphNode : CalledGraph->Nodes)
					{
						Worklist.Add(CalledGraphNode);
					}
				}
			}
		}

		return Reachable;
	}

	// Branch edges not provably always taken; a connected condition excludes both, a literal one excludes one.
	bool IsUnevaluableBranchOutputPin(const UEdGraphPin* OutputPin)
	{
		const UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(OutputPin->GetOwningNode());
		if (!Branch)
		{
			return false;
		}
		if (OutputPin->PinName != UEdGraphSchema_K2::PN_Then && OutputPin->PinName != UEdGraphSchema_K2::PN_Else)
		{
			return false;
		}

		const UEdGraphPin* ConditionPin = Branch->GetConditionPin();
		if (!ConditionPin)
		{
			return false;
		}
		if (ConditionPin->LinkedTo.Num() > 0)
		{
			return true;
		}

		const bool bConditionLiteral = ConditionPin->DefaultValue.ToBool();
		const bool bIsThenEdge = OutputPin->PinName == UEdGraphSchema_K2::PN_Then;
		return bIsThenEdge != bConditionLiteral;
	}

	// Forward exec walk skipping unevaluable branch edges; unreached nodes are branch-gated.
	TSet<UEdGraphNode*> CollectUnconditionallyReachableNodes(const TArray<UEdGraph*>& AllGraphs)
	{
		TArray<UEdGraphNode*> Worklist;
		for (UEdGraph* Graph : AllGraphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Cast<UK2Node_Event>(Node))
				{
					Worklist.Add(Node);
				}
			}
		}

		TSet<UEdGraphNode*> Reachable;
		for (int32 WorkIdx = 0; WorkIdx < Worklist.Num(); ++WorkIdx)
		{
			UEdGraphNode* Node = Worklist[WorkIdx];
			bool bAlreadyVisited = false;
			Reachable.Add(Node, &bAlreadyVisited);
			if (bAlreadyVisited)
			{
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}
				if (IsUnevaluableBranchOutputPin(Pin))
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					Worklist.Add(LinkedPin->GetOwningNode());
				}
			}

			if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				const UEdGraphNode* CalledGraphEntryNode = nullptr;
				if (UEdGraph* CalledGraph = CallNode->GetFunctionGraph(CalledGraphEntryNode))
				{
					for (UEdGraphNode* CalledGraphNode : CalledGraph->Nodes)
					{
						Worklist.Add(CalledGraphNode);
					}
				}
			}
		}

		return Reachable;
	}

	// FunctionName -> property whose MD_PropertySetFunction names it. Rebuilt per call to avoid staleness.
	void BuildPropertySetterIndexForClass(const UClass* WidgetClass, TMap<FName, const FProperty*>& OutIndex)
	{
		if (!WidgetClass)
		{
			return;
		}

		for (TFieldIterator<FProperty> PropertyIt(WidgetClass); PropertyIt; ++PropertyIt)
		{
			const FProperty* Property = *PropertyIt;
			if (!Property->HasMetaData(FBlueprintMetadata::MD_PropertySetFunction))
			{
				continue;
			}

			const FString& SetterFunctionName = Property->GetMetaData(FBlueprintMetadata::MD_PropertySetFunction);
			if (!SetterFunctionName.IsEmpty())
			{
				// TFieldIterator is most-derived first; first match wins.
				const FName SetterFunctionFName(*SetterFunctionName);
				if (!OutIndex.Contains(SetterFunctionFName))
				{
					OutIndex.Add(SetterFunctionFName, Property);
				}
			}
		}
	}

	// Value from the archetype instance; empty if none or unresolved.
	FString ExportWidgetPropertyValue(const UWidgetTree* WidgetTree, FName WidgetVariableName, FName PropertyName)
	{
		UWidget* WidgetInstance = WidgetTree ? WidgetTree->FindWidget(WidgetVariableName) : nullptr;
		if (!WidgetInstance)
		{
			return FString();
		}

		const FProperty* Property = WidgetInstance->GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			return FString();
		}

		return FWidgetPropertyReflectionUtils::ExportPropertyValue(Property, WidgetInstance);
	}

	// Follows a widget's Get-node pin through pass-through nodes, recording VariableSet/CallFunction writes on self.
	void TraceWritesForWidget(const TArray<UEdGraph*>& AllGraphs, const UWidgetTree* WidgetTree, FName WidgetVariableName, const TSet<UEdGraphNode*>& PreConstructReachableNodes, const TSet<UEdGraphNode*>& ConstructReachableNodes, const TSet<UEdGraphNode*>& UnconditionallyReachableNodes, const TMap<FName, const FProperty*>& PropertySetterIndex, TArray<FTracedPropertyWrite>& OutWrites)
	{
		TArray<UEdGraphPin*> Worklist;
		for (UEdGraph* Graph : AllGraphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
				{
					if (VarGet->VariableReference.IsSelfContext() && VarGet->GetVarName() == WidgetVariableName)
					{
						if (UEdGraphPin* ValuePin = VarGet->GetValuePin())
						{
							Worklist.Append(ValuePin->LinkedTo);
						}
					}
				}
			}
		}

		// Pass-through nodes dedup by node; writes dedup by node and self pin.
		TSet<UEdGraphNode*> VisitedForPropagation;
		TSet<UEdGraphNode*> RecordedWriteNodes;
		for (int32 WorkIdx = 0; WorkIdx < Worklist.Num(); ++WorkIdx)
		{
			UEdGraphPin* CurrentPin = Worklist[WorkIdx];
			UEdGraphNode* Node = CurrentPin->GetOwningNode();

			if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
			{
				bool bAlreadyVisited = false;
				VisitedForPropagation.Add(Node, &bAlreadyVisited);
				if (bAlreadyVisited)
				{
					continue;
				}
				if (UEdGraphPin* Out = Knot->GetOutputPin())
				{
					Worklist.Append(Out->LinkedTo);
				}
			}
			else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
			{
				bool bAlreadyVisited = false;
				VisitedForPropagation.Add(Node, &bAlreadyVisited);
				if (bAlreadyVisited)
				{
					continue;
				}
				if (UEdGraphPin* CastResult = CastNode->GetCastResultPin())
				{
					Worklist.Append(CastResult->LinkedTo);
				}
			}
			else if (UK2Node_GetArrayItem* GetItem = Cast<UK2Node_GetArrayItem>(Node))
			{
				bool bAlreadyVisited = false;
				VisitedForPropagation.Add(Node, &bAlreadyVisited);
				if (bAlreadyVisited)
				{
					continue;
				}
				if (UEdGraphPin* Result = GetItem->GetResultPin())
				{
					Worklist.Append(Result->LinkedTo);
				}
			}
			else if (UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Node))
			{
				bool bAlreadyVisited = false;
				VisitedForPropagation.Add(Node, &bAlreadyVisited);
				if (bAlreadyVisited)
				{
					continue;
				}
				if (UEdGraphPin* Ret = SelectNode->GetReturnValuePin())
				{
					Worklist.Append(Ret->LinkedTo);
				}
			}
			else if (Cast<UK2Node_StructMemberGet>(Node) != nullptr)
			{
				bool bAlreadyVisited = false;
				VisitedForPropagation.Add(Node, &bAlreadyVisited);
				if (bAlreadyVisited)
				{
					continue;
				}
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin->Direction == EGPD_Output)
					{
						Worklist.Append(Pin->LinkedTo);
					}
				}
			}
			else if (UK2Node_VariableSet* VarSet = Cast<UK2Node_VariableSet>(Node))
			{
				// Require the self pin, so "Var = MyWidget" isn't a write.
				UEdGraphPin* SelfPin = VarSet->FindPin(UEdGraphSchema_K2::PN_Self);
				if (SelfPin && CurrentPin == SelfPin && !RecordedWriteNodes.Contains(Node))
				{
					RecordedWriteNodes.Add(Node);

					FTracedPropertyWrite Write;
					Write.WidgetName = WidgetVariableName;
					Write.PropertyName = VarSet->GetVarName();
					Write.NodeGuid = VarSet->NodeGuid;
					Write.bPreConstructReachable = PreConstructReachableNodes.Contains(Node);
					Write.bLifecycleReachable = Write.bPreConstructReachable || ConstructReachableNodes.Contains(Node);
					Write.bBranchGated = !UnconditionallyReachableNodes.Contains(Node);
					Write.ExportedValue = ExportWidgetPropertyValue(WidgetTree, WidgetVariableName, Write.PropertyName);
					OutWrites.Add(Write);
				}
			}
			else if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
			{
				// Only the self pin counts; this also excludes slot-accessor calls like SlotAsCanvasSlot(Widget).
				UEdGraphPin* SelfPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self);
				if (SelfPin && CurrentPin == SelfPin && !RecordedWriteNodes.Contains(Node))
				{
					RecordedWriteNodes.Add(Node);

					FTracedPropertyWrite Write;
					Write.WidgetName = WidgetVariableName;
					Write.FunctionName = CallNode->GetFunctionName();
					Write.NodeGuid = CallNode->NodeGuid;
					Write.bPreConstructReachable = PreConstructReachableNodes.Contains(Node);
					Write.bLifecycleReachable = Write.bPreConstructReachable || ConstructReachableNodes.Contains(Node);
					Write.bBranchGated = !UnconditionallyReachableNodes.Contains(Node);

					// Falls back to FunctionName alone when no property names this setter.
					if (const FProperty* const* FoundProperty = PropertySetterIndex.Find(Write.FunctionName))
					{
						Write.PropertyName = (*FoundProperty)->GetFName();
						Write.ExportedValue = ExportWidgetPropertyValue(WidgetTree, WidgetVariableName, Write.PropertyName);
					}

					OutWrites.Add(Write);
				}
			}
		}
	}
}

TArray<FTracedPropertyWrite> FWidgetPropertyTracer::TraceWrites(const UWidgetBlueprint* WidgetBlueprint)
{
	TArray<FTracedPropertyWrite> Writes;
	if (!WidgetBlueprint)
	{
		return Writes;
	}

	TArray<UEdGraph*> AllGraphs;
	FWidgetPropertyTracer::CollectAllGraphs(WidgetBlueprint, AllGraphs);
	const TSet<UEdGraphNode*> PreConstructReachableNodes = CollectReachableNodesFromEvent(AllGraphs, FName("PreConstruct"));
	const TSet<UEdGraphNode*> ConstructReachableNodes = CollectReachableNodesFromEvent(AllGraphs, FName("Construct"));
	const TSet<UEdGraphNode*> UnconditionallyReachableNodes = CollectUnconditionallyReachableNodes(AllGraphs);

	const UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
	const UClass* GeneratedClass = WidgetBlueprint->GeneratedClass;

	TSet<FName> WidgetVariableNames;
	for (UEdGraph* Graph : AllGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_VariableGet* VarGet = Cast<UK2Node_VariableGet>(Node))
			{
				const FName VariableName = VarGet->GetVarName();
				if (VarGet->VariableReference.IsSelfContext() && IsWidgetVariable(WidgetTree, GeneratedClass, VariableName))
				{
					WidgetVariableNames.Add(VariableName);
				}
			}
		}
	}

	// Setter index is built per call, once per class.
	TMap<const UClass*, TMap<FName, const FProperty*>> PropertySetterIndexByClass;
	TSet<const UClass*> ClassesWithBuiltSetterIndex;
	for (FName WidgetVariableName : WidgetVariableNames)
	{
		const UClass* WidgetClass = ResolveWidgetClassForVariable(WidgetTree, GeneratedClass, WidgetVariableName);
		TMap<FName, const FProperty*>& SetterIndex = PropertySetterIndexByClass.FindOrAdd(WidgetClass);

		bool bAlreadyBuilt = false;
		ClassesWithBuiltSetterIndex.Add(WidgetClass, &bAlreadyBuilt);
		if (!bAlreadyBuilt)
		{
			BuildPropertySetterIndexForClass(WidgetClass, SetterIndex);
		}

		TraceWritesForWidget(AllGraphs, WidgetTree, WidgetVariableName, PreConstructReachableNodes, ConstructReachableNodes, UnconditionallyReachableNodes, SetterIndex, Writes);
	}

	// Collapse duplicate compiled graph copies, OR-ing flags; NodeGuid keeps distinct writes apart.
	TArray<FTracedPropertyWrite> DeduplicatedWrites;
	for (const FTracedPropertyWrite& Write : Writes)
	{
		FTracedPropertyWrite* Existing = DeduplicatedWrites.FindByPredicate([&Write](const FTracedPropertyWrite& Candidate)
		{
			return Candidate.WidgetName == Write.WidgetName
				&& Candidate.PropertyName == Write.PropertyName
				&& Candidate.FunctionName == Write.FunctionName
				&& Candidate.NodeGuid == Write.NodeGuid;
		});
		if (Existing)
		{
			Existing->bLifecycleReachable |= Write.bLifecycleReachable;
			Existing->bPreConstructReachable |= Write.bPreConstructReachable;
			Existing->bBranchGated &= Write.bBranchGated;
		}
		else
		{
			DeduplicatedWrites.Add(Write);
		}
	}

	return DeduplicatedWrites;
}
