// Games by Hyper 2026.

#include "HyperAIStudioBlueprintStructureToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorLibrary.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeBinder.h"
#include "Components/ActorComponent.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "EdGraphUtilities.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
#include "Engine/UserDefinedEnum.h"
#include "GameFramework/Actor.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSourceControl.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/UObjectToken.h"
#include "ScopedTransaction.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Interface.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioBlueprintStructure, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioBlueprintStructure"

FString FHyperAIStudioBlueprintStructureContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioBlueprintStructure.HyperAIStudioBlueprintStructureToolset");
}

TArray<FString> FHyperAIStudioBlueprintStructureContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioBlueprintStructureContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

/**
 * The parts of Epic's Blueprint editor that fold nodes into a graph and open them back out. They are protected members, so they
 * are reached here the way its own Collapse and Expand commands reach them: the same code, the same results, no copy of it.
 */
struct FHyperAIBlueprintEditorAccess : public FBlueprintEditor
{
	static void CollapseInto(FBlueprintEditor& Editor, UEdGraphNode* Gateway, UK2Node_EditablePinBase* Entry, UK2Node_EditablePinBase* Result,
		UEdGraph* Source, UEdGraph* Destination, TSet<UEdGraphNode*>& Nodes, const bool bCanDiscardEmptyReturnNode, const bool bCanHaveWeakObjPtrParam)
	{
		(Editor.*(&FHyperAIBlueprintEditorAccess::CollapseNodesIntoGraph))(Gateway, Entry, Result, Source, Destination, Nodes,
			bCanDiscardEmptyReturnNode, bCanHaveWeakObjPtrParam);
	}

	static bool CanCollapseToFunction(const FBlueprintEditor& Editor, TSet<UEdGraphNode*>& Nodes)
	{
		using FCheck = bool (FBlueprintEditor::*)(TSet<UEdGraphNode*>&) const;
		const FCheck Check = static_cast<FCheck>(&FHyperAIBlueprintEditorAccess::CanCollapseSelectionToFunction);
		return (Editor.*Check)(Nodes);
	}

	static void Expand(UEdGraphNode* Node, UEdGraph* Source, TSet<UEdGraphNode*>& OutExpanded)
	{
		ExpandNode(Node, Source, OutExpanded);
	}
};

namespace HyperAIStudio::BlueprintStructure::Private
{
	using C = FHyperAIStudioBlueprintStructureContracts;

	// ------------------------------------------------------------------------------------------------------------- paths

	FString PackageOf(const FString& Path)
	{
		FString Package = Path.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Package.FindChar(TEXT('.'), Dot))
		{
			Package.LeftInline(Dot);
		}
		return Package;
	}

	FString ObjectPath(const FString& PackageOrObject)
	{
		const FString Trimmed = PackageOrObject.TrimStartAndEnd();
		return Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
	}

	bool IsProjectPath(const FString& Path, FString& OutError)
	{
		const FString Package = PackageOf(Path);
		FText Reason;
		if (!FPackageName::IsValidLongPackageName(Package, /*bIncludeReadOnlyRoots=*/false, &Reason)
			|| Package.StartsWith(TEXT("/Script/")) || Package.StartsWith(TEXT("/Engine/")) || Package.StartsWith(TEXT("/Temp/")))
		{
			OutError = FString::Printf(TEXT("'%s' is not a path in the project, such as /Game/Interfaces/BPI_Interactable. %s"), *Path, *Reason.ToString());
			return false;
		}
		return true;
	}

	UBlueprint* LoadBlueprint(const FString& Path)
	{
		return Path.TrimStartAndEnd().IsEmpty() ? nullptr : LoadObject<UBlueprint>(nullptr, *ObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	// ------------------------------------------------------------------------------------------------------------- types

	bool ParseType(FString Text, FEdGraphPinType& Out, FString& OutError)
	{
		Out = FEdGraphPinType();
		Text.TrimStartAndEndInline();
		if (Text.EndsWith(TEXT("[]")))
		{
			Out.ContainerType = EPinContainerType::Array;
			Text.LeftChopInline(2);
		}
		const FString Lower = Text.ToLower();
		auto Struct = [&Out](UScriptStruct* Type)
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Out.PinSubCategoryObject = Type;
		};
		if (Lower == TEXT("bool")) { Out.PinCategory = UEdGraphSchema_K2::PC_Boolean; }
		else if (Lower == TEXT("int")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int; }
		else if (Lower == TEXT("int64")) { Out.PinCategory = UEdGraphSchema_K2::PC_Int64; }
		else if (Lower == TEXT("float") || Lower == TEXT("double") || Lower == TEXT("real"))
		{
			Out.PinCategory = UEdGraphSchema_K2::PC_Real;
			Out.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (Lower == TEXT("byte")) { Out.PinCategory = UEdGraphSchema_K2::PC_Byte; }
		else if (Lower == TEXT("name")) { Out.PinCategory = UEdGraphSchema_K2::PC_Name; }
		else if (Lower == TEXT("string")) { Out.PinCategory = UEdGraphSchema_K2::PC_String; }
		else if (Lower == TEXT("text")) { Out.PinCategory = UEdGraphSchema_K2::PC_Text; }
		else if (Lower == TEXT("vector")) { Struct(TBaseStructure<FVector>::Get()); }
		else if (Lower == TEXT("rotator")) { Struct(TBaseStructure<FRotator>::Get()); }
		else if (Lower == TEXT("transform")) { Struct(TBaseStructure<FTransform>::Get()); }
		else if (Lower == TEXT("vector2d")) { Struct(TBaseStructure<FVector2D>::Get()); }
		else if (Lower == TEXT("linearcolor")) { Struct(TBaseStructure<FLinearColor>::Get()); }
		else
		{
			FString Prefix, Path;
			if (!Text.Split(TEXT(":"), &Prefix, &Path))
			{
				OutError = FString::Printf(TEXT("Unknown type '%s': bool, int, int64, float, byte, name, string, text, vector, rotator, transform, vector2d, ")
					TEXT("linearcolor, or object:, class:, struct:, enum: with a path; [] after any for an array."), *Text);
				return false;
			}
			Prefix = Prefix.TrimStartAndEnd().ToLower();
			Path.TrimStartAndEndInline();
			if (Prefix == TEXT("object") || Prefix == TEXT("class"))
			{
				UClass* Class = FindObject<UClass>(nullptr, *Path);
				Class = Class ? Class : LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
				if (!Class)
				{
					OutError = FString::Printf(TEXT("There is no class %s (a Blueprint's is its path with _C, such as /Game/BP_Door.BP_Door_C)."), *Path);
					return false;
				}
				Out.PinCategory = Prefix == TEXT("object") ? UEdGraphSchema_K2::PC_Object : UEdGraphSchema_K2::PC_Class;
				Out.PinSubCategoryObject = Class;
			}
			else if (Prefix == TEXT("struct"))
			{
				UScriptStruct* Type = FindObject<UScriptStruct>(nullptr, *Path);
				Type = Type ? Type : LoadObject<UScriptStruct>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
				if (!Type)
				{
					OutError = FString::Printf(TEXT("There is no struct %s."), *Path);
					return false;
				}
				Struct(Type);
			}
			else if (Prefix == TEXT("enum"))
			{
				UEnum* Enum = FindObject<UEnum>(nullptr, *Path);
				Enum = Enum ? Enum : LoadObject<UEnum>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
				if (!Enum)
				{
					OutError = FString::Printf(TEXT("There is no enum %s."), *Path);
					return false;
				}
				Out.PinCategory = UEdGraphSchema_K2::PC_Byte;
				Out.PinSubCategoryObject = Enum;
			}
			else
			{
				OutError = FString::Printf(TEXT("Unknown type prefix '%s:': object:, class:, struct: or enum:."), *Prefix);
				return false;
			}
		}
		return true;
	}

	FString TypeName(const FEdGraphPinType& Type)
	{
		const FName Category = Type.PinCategory;
		const UObject* Sub = Type.PinSubCategoryObject.Get();
		FString Name;
		if (Category == UEdGraphSchema_K2::PC_Boolean) { Name = TEXT("bool"); }
		else if (Category == UEdGraphSchema_K2::PC_Int) { Name = TEXT("int"); }
		else if (Category == UEdGraphSchema_K2::PC_Int64) { Name = TEXT("int64"); }
		else if (Category == UEdGraphSchema_K2::PC_Real || Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Double) { Name = TEXT("float"); }
		else if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum) { Name = Sub ? TEXT("enum:") + Sub->GetPathName() : TEXT("byte"); }
		else if (Category == UEdGraphSchema_K2::PC_Name) { Name = TEXT("name"); }
		else if (Category == UEdGraphSchema_K2::PC_String) { Name = TEXT("string"); }
		else if (Category == UEdGraphSchema_K2::PC_Text) { Name = TEXT("text"); }
		else if (Category == UEdGraphSchema_K2::PC_Exec) { Name = TEXT("exec"); }
		else if (Category == UEdGraphSchema_K2::PC_Struct)
		{
			Name = Sub == TBaseStructure<FVector>::Get() ? TEXT("vector") : Sub == TBaseStructure<FRotator>::Get() ? TEXT("rotator")
				: Sub == TBaseStructure<FTransform>::Get() ? TEXT("transform") : Sub == TBaseStructure<FVector2D>::Get() ? TEXT("vector2d")
				: Sub == TBaseStructure<FLinearColor>::Get() ? TEXT("linearcolor") : TEXT("struct:") + (Sub ? Sub->GetPathName() : FString());
		}
		else if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class)
		{
			Name = (Category == UEdGraphSchema_K2::PC_Object ? TEXT("object:") : TEXT("class:")) + (Sub ? Sub->GetPathName() : FString());
		}
		else { Name = Category.ToString(); }
		return Type.IsArray() ? Name + TEXT("[]") : Name;
	}

	// ------------------------------------------------------------------------------------------------------ replication

	bool ParseRpc(const FString& Text, uint32& OutFlags)
	{
		const FString Lower = Text.TrimStartAndEnd().ToLower();
		OutFlags = Lower == TEXT("server") ? static_cast<uint32>(FUNC_NetServer) : Lower == TEXT("client") ? static_cast<uint32>(FUNC_NetClient)
			: Lower == TEXT("multicast") ? static_cast<uint32>(FUNC_NetMulticast) : 0u;
		return OutFlags != 0 || Lower.IsEmpty() || Lower == TEXT("none");
	}

	FString RpcName(const uint32 Flags)
	{
		const FString Mode = (Flags & FUNC_NetServer) ? TEXT("server") : (Flags & FUNC_NetClient) ? TEXT("client") : (Flags & FUNC_NetMulticast) ? TEXT("multicast") : TEXT("none");
		return (Flags & FUNC_NetReliable) && Mode != TEXT("none") ? Mode + TEXT(", reliable") : Mode;
	}

	void ApplyRpc(UK2Node_CustomEvent& Event, const uint32 NetFlags, const bool bReliable)
	{
		// What the event's Details panel does: every net flag cleared, then the chosen ones set.
		Event.Modify();
		Event.FunctionFlags &= ~static_cast<uint32>(FUNC_Net | FUNC_NetMulticast | FUNC_NetServer | FUNC_NetClient | FUNC_NetReliable);
		if (NetFlags != 0)
		{
			Event.FunctionFlags |= static_cast<uint32>(FUNC_Net) | NetFlags | (bReliable ? static_cast<uint32>(FUNC_NetReliable) : 0u);
		}
	}

	const TMap<FString, ELifetimeCondition>& Conditions()
	{
		static const TMap<FString, ELifetimeCondition> Map = {
			{TEXT("none"), COND_None}, {TEXT("initial_only"), COND_InitialOnly}, {TEXT("owner_only"), COND_OwnerOnly},
			{TEXT("skip_owner"), COND_SkipOwner}, {TEXT("simulated_only"), COND_SimulatedOnly}, {TEXT("autonomous_only"), COND_AutonomousOnly},
			{TEXT("simulated_or_physics"), COND_SimulatedOrPhysics}, {TEXT("initial_or_owner"), COND_InitialOrOwner}, {TEXT("custom"), COND_Custom},
			{TEXT("replay_or_owner"), COND_ReplayOrOwner}, {TEXT("replay_only"), COND_ReplayOnly},
			{TEXT("simulated_only_no_replay"), COND_SimulatedOnlyNoReplay}, {TEXT("simulated_or_physics_no_replay"), COND_SimulatedOrPhysicsNoReplay},
			{TEXT("skip_replay"), COND_SkipReplay}};
		return Map;
	}

	FString ConditionName(const ELifetimeCondition Condition)
	{
		for (const TPair<FString, ELifetimeCondition>& Pair : Conditions())
		{
			if (Pair.Value == Condition)
			{
				return Pair.Key;
			}
		}
		return TEXT("none");
	}

	bool ParseReplication(const FString& Text, EBlueprintVariableReplication& Out)
	{
		const FString Lower = Text.TrimStartAndEnd().ToLower();
		if (Lower == TEXT("none")) { Out = EBlueprintVariableReplication::None; return true; }
		if (Lower == TEXT("replicated")) { Out = EBlueprintVariableReplication::Replicated; return true; }
		if (Lower == TEXT("rep_notify") || Lower == TEXT("repnotify")) { Out = EBlueprintVariableReplication::RepNotify; return true; }
		return false;
	}

	// ------------------------------------------------------------------------------------------------------ graphs, nodes

	UEdGraph* FindGraph(UBlueprint& Blueprint, const FString& Name)
	{
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			return FBlueprintEditorUtils::FindEventGraph(&Blueprint);
		}
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName().Equals(Name.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	bool IsEventGraph(const UEdGraph* Graph)
	{
		return Graph && GetDefault<UEdGraphSchema_K2>()->GetGraphType(Graph) == GT_Ubergraph;
	}

	FString Title(const UEdGraphNode& Node)
	{
		return Node.GetNodeTitle(ENodeTitleType::ListView).ToString();
	}

	/** A node by its name (K2Node_CallFunction_3), its GUID, or its title when only one node has it; InGraph narrows the search. */
	UEdGraphNode* FindNode(UBlueprint& Blueprint, const FString& Spec, UEdGraph* InGraph, FString& OutError)
	{
		const FString Wanted = Spec.TrimStartAndEnd();
		TArray<UEdGraph*> Graphs;
		if (InGraph)
		{
			Graphs.Add(InGraph);
		}
		else
		{
			Blueprint.GetAllGraphs(Graphs);
		}
		FGuid Guid;
		const bool bGuid = FGuid::Parse(Wanted, Guid);
		TArray<UEdGraphNode*> ByName, ByTitle;
		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (bGuid && Node->NodeGuid == Guid)
				{
					return Node;
				}
				if (Node->GetName() == Wanted)
				{
					ByName.Add(Node);
				}
				else if (Title(*Node).Equals(Wanted, ESearchCase::IgnoreCase))
				{
					ByTitle.Add(Node);
				}
			}
		}
		// A node's name is unique only in its own graph: the same name in two graphs, or a shared title, is refused, not guessed.
		const TArray<UEdGraphNode*>& Found = ByName.IsEmpty() ? ByTitle : ByName;
		if (Found.Num() == 1)
		{
			return Found[0];
		}
		OutError = Found.Num() > 1
			? FString::Printf(TEXT("'%s' fits %d nodes (%s in %s, %s in %s...): give Graph, or the node's GUID."), *Wanted, Found.Num(), *Found[0]->GetName(),
				*Found[0]->GetGraph()->GetName(), *Found[1]->GetName(), *Found[1]->GetGraph()->GetName())
			: FString::Printf(TEXT("No node is named or titled '%s'%s: hyper_blueprint_structure_inspect with bNodes lists them."), *Wanted,
				InGraph ? *FString::Printf(TEXT(" in %s"), *InGraph->GetName()) : TEXT(""));
		return nullptr;
	}

	UEdGraphPin* FindPin(UEdGraphNode& Node, const FString& Name, const EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && !Pin->bHidden && (Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase)
				|| Pin->GetDisplayName().ToString().Equals(Name, ESearchCase::IgnoreCase)))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	FString PinNames(const UEdGraphNode& Node, const EEdGraphPinDirection Direction)
	{
		TArray<FString> Names;
		for (const UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && !Pin->bHidden)
			{
				Names.Add(Pin->PinName.ToString());
			}
		}
		return Names.IsEmpty() ? TEXT("none") : FString::Join(Names, TEXT(", "));
	}

	FString DescribeNode(const UEdGraphNode& Node)
	{
		return FString::Printf(TEXT("node %s (title '%s', in %s); pins in: %s; out: %s"), *Node.GetName(), *Title(Node), *Node.GetGraph()->GetName(),
			*PinNames(Node, EGPD_Input), *PinNames(Node, EGPD_Output));
	}

	bool SplitNodePin(const FString& Text, FString& OutNode, FString& OutPin)
	{
		int32 Dot = INDEX_NONE;
		if (!Text.FindLastChar(TEXT('.'), Dot))
		{
			return false;
		}
		OutNode = Text.Left(Dot).TrimStartAndEnd();
		OutPin = Text.Mid(Dot + 1).TrimStartAndEnd();
		return !OutNode.IsEmpty() && !OutPin.IsEmpty();
	}

	bool IsNameFree(UBlueprint& Blueprint, const FString& Name, FString& OutError)
	{
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("Give it a Name.");
			return false;
		}
		const EValidatorResult Result = FKismetNameValidator(&Blueprint).IsValid(FName(*Name.TrimStartAndEnd()));
		if (Result != EValidatorResult::Ok)
		{
			OutError = FString::Printf(TEXT("'%s' can't be used in %s: %s"), *Name, *Blueprint.GetName(),
				*INameValidatorInterface::GetErrorText(Name, Result).ToString());
			return false;
		}
		return true;
	}

	UK2Node_CustomEvent* FindCustomEvent(UBlueprint& Blueprint, const FString& Name)
	{
		TArray<UK2Node_CustomEvent*> Events;
		FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, Events);
		for (UK2Node_CustomEvent* Event : Events)
		{
			if (Event->CustomFunctionName.ToString().Equals(Name.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				return Event;
			}
		}
		return nullptr;
	}

	// --------------------------------------------------------------------------------------------------------- interfaces

	FString DescribeFunctionGraph(const UEdGraph& Graph)
	{
		const UK2Node_FunctionEntry* Entry = nullptr;
		const UK2Node_FunctionResult* Result = nullptr;
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			Entry = Entry ? Entry : Cast<UK2Node_FunctionEntry>(Node);
			Result = Result ? Result : Cast<UK2Node_FunctionResult>(Node);
		}
		auto Params = [](const UK2Node_EditablePinBase* Node)
		{
			TArray<FString> Parts;
			if (Node)
			{
				for (const TSharedPtr<FUserPinInfo>& Pin : Node->UserDefinedPins)
				{
					Parts.Add(FString::Printf(TEXT("%s: %s"), *Pin->PinName.ToString(), *TypeName(Pin->PinType)));
				}
			}
			return FString::Join(Parts, TEXT(", "));
		};
		const FString Outputs = Params(Result);
		return FString::Printf(TEXT("%s(%s)%s"), *Graph.GetName(), *Params(Entry), Outputs.IsEmpty() ? TEXT(", an event") : *(TEXT(" -> ") + Outputs));
	}

	UBlueprint* CreateInterface(const FString& Path, FString& OutError)
	{
		if (!IsProjectPath(Path, OutError))
		{
			return nullptr;
		}
		const FString Package = PackageOf(Path);
		if (FindObject<UObject>(nullptr, *ObjectPath(Package)) || FPackageName::DoesPackageExist(Package))
		{
			OutError = FString::Printf(TEXT("%s exists already: interface.add_function adds to it."), *Package);
			return nullptr;
		}
		UPackage* Outer = CreatePackage(*Package);
		UBlueprint* Interface = FKismetEditorUtilities::CreateBlueprint(UInterface::StaticClass(), Outer, FName(*FPackageName::GetShortName(Package)),
			BPTYPE_Interface, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("HyperAIStudio"));
		if (!Interface)
		{
			OutError = FString::Printf(TEXT("Unreal did not make the interface %s."), *Package);
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Interface);
		Outer->MarkPackageDirty();
		return Interface;
	}

	UBlueprint* LoadInterface(const FString& Path, FString& OutError)
	{
		UBlueprint* Interface = LoadBlueprint(Path);
		if (!Interface || !FBlueprintEditorUtils::IsInterfaceBlueprint(Interface))
		{
			OutError = FString::Printf(TEXT("%s is not a Blueprint Interface: interface.create makes one."), *Path);
			return nullptr;
		}
		return Interface;
	}

	bool ParseParams(const TArray<FHyperAIBlueprintParam>& Params, TArray<TPair<FName, FEdGraphPinType>>& Out, FString& OutError)
	{
		for (const FHyperAIBlueprintParam& Param : Params)
		{
			FEdGraphPinType Type;
			if (Param.Name.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("Every parameter needs a Name.");
				return false;
			}
			if (!ParseType(Param.Type, Type, OutError))
			{
				return false;
			}
			Out.Emplace(FName(*Param.Name.TrimStartAndEnd()), Type);
		}
		return true;
	}

	bool AddInterfaceFunction(UBlueprint& Interface, const FHyperAIBlueprintStructureOp& Op, TArray<FString>& OutMade, FString& OutError)
	{
		if (!IsNameFree(Interface, Op.Name, OutError))
		{
			return false;
		}
		TArray<TPair<FName, FEdGraphPinType>> Inputs, Outputs;
		if (!ParseParams(Op.Inputs, Inputs, OutError) || !ParseParams(Op.Outputs, Outputs, OutError))
		{
			return false;
		}
		Interface.Modify();
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(&Interface, FName(*Op.Name.TrimStartAndEnd()), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(&Interface, Graph, /*bIsUserCreated=*/true, nullptr);
		TArray<UK2Node_FunctionEntry*> Entries;
		Graph->GetNodesOfClass(Entries);
		if (Entries.IsEmpty())
		{
			OutError = TEXT("The function graph came without its entry node.");
			return false;
		}
		for (const TPair<FName, FEdGraphPinType>& Input : Inputs)
		{
			Entries[0]->CreateUserDefinedPin(Input.Key, Input.Value, EGPD_Output);
		}
		if (!Outputs.IsEmpty())
		{
			UK2Node_FunctionResult* Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entries[0]);
			for (const TPair<FName, FEdGraphPinType>& Output : Outputs)
			{
				Result->CreateUserDefinedPin(Output.Key, Output.Value, EGPD_Input);
			}
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Interface);
		OutMade.Add(FString::Printf(TEXT("interface function %s in %s"), *DescribeFunctionGraph(*Graph), *Interface.GetName()));
		return true;
	}

	/** An interface made or changed earlier in the plan is compiled before a Blueprint implements it, so its class has the functions. */
	void CompileIfChanged(UBlueprint& Blueprint)
	{
		if (Blueprint.Status != BS_UpToDate && Blueprint.Status != BS_UpToDateWithWarnings)
		{
			FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		}
	}

	FString DescribeImplemented(UClass& Interface)
	{
		TArray<FString> Events, Functions;
		for (TFieldIterator<UFunction> It(&Interface, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			(UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(*It) ? Events : Functions).Add(It->GetName());
		}
		return FString::Printf(TEXT("%s: events %s (place each with Epic's BlueprintTools.add_event or an (event ...) in the graph DSL); functions %s (each has its own graph to fill)"),
			*Interface.GetPathName(), Events.IsEmpty() ? TEXT("none") : *FString::Join(Events, TEXT(", ")),
			Functions.IsEmpty() ? TEXT("none") : *FString::Join(Functions, TEXT(", ")));
	}

	// ---------------------------------------------------------------------------------------------------------- timelines

	ERichCurveInterpMode ParseInterp(const FString& Text)
	{
		const FString Lower = Text.TrimStartAndEnd().ToLower();
		return Lower == TEXT("linear") ? RCIM_Linear : Lower == TEXT("constant") ? RCIM_Constant : RCIM_Cubic;
	}

	int32 ValuesFor(const FString& TrackType)
	{
		const FString Lower = TrackType.TrimStartAndEnd().ToLower();
		return Lower == TEXT("float") ? 1 : Lower == TEXT("vector") ? 3 : Lower == TEXT("color") ? 4 : Lower == TEXT("event") ? 0 : INDEX_NONE;
	}

	void AddKeys(FRichCurve& Curve, const TArray<FHyperAIBlueprintTimelineKey>& Keys, const int32 Channel, const ERichCurveInterpMode Interp)
	{
		for (const FHyperAIBlueprintTimelineKey& Key : Keys)
		{
			const FKeyHandle Handle = Curve.AddKey(Key.Time, Channel < 0 ? 0.f : Key.Value[Channel]);
			Curve.SetKeyInterpMode(Handle, Interp);
		}
		Curve.AutoSetTangents();
	}

	FString DescribeTimeline(UBlueprint& Blueprint, UTimelineTemplate& Timeline)
	{
		TArray<FString> Tracks;
		for (const FTTFloatTrack& Track : Timeline.FloatTracks)
		{
			Tracks.Add(FString::Printf(TEXT("%s float (%d keys)"), *Track.GetTrackName().ToString(), Track.CurveFloat ? Track.CurveFloat->FloatCurve.GetNumKeys() : 0));
		}
		for (const FTTVectorTrack& Track : Timeline.VectorTracks)
		{
			Tracks.Add(FString::Printf(TEXT("%s vector (%d keys)"), *Track.GetTrackName().ToString(), Track.CurveVector ? Track.CurveVector->FloatCurves[0].GetNumKeys() : 0));
		}
		for (const FTTLinearColorTrack& Track : Timeline.LinearColorTracks)
		{
			Tracks.Add(FString::Printf(TEXT("%s color (%d keys)"), *Track.GetTrackName().ToString(), Track.CurveLinearColor ? Track.CurveLinearColor->FloatCurves[0].GetNumKeys() : 0));
		}
		for (const FTTEventTrack& Track : Timeline.EventTracks)
		{
			Tracks.Add(FString::Printf(TEXT("%s event (%d keys)"), *Track.GetTrackName().ToString(), Track.CurveKeys ? Track.CurveKeys->FloatCurve.GetNumKeys() : 0));
		}
		const UK2Node_Timeline* Node = FBlueprintEditorUtils::FindNodeForTimeline(&Blueprint, &Timeline);
		return FString::Printf(TEXT("%s: %.2f s%s%s%s%s; tracks %s; %s"), *Timeline.GetVariableName().ToString(), Timeline.TimelineLength,
			Timeline.LengthMode == TL_LastKeyFrame ? TEXT(" (to its last key)") : TEXT(""), Timeline.bAutoPlay ? TEXT(", autoplay") : TEXT(""),
			Timeline.bLoop ? TEXT(", loops") : TEXT(""), Timeline.bReplicated ? TEXT(", replicated") : TEXT(""),
			Tracks.IsEmpty() ? TEXT("none") : *FString::Join(Tracks, TEXT(", ")), Node ? *DescribeNode(*Node) : TEXT("no node"));
	}

	bool AddTimeline(UBlueprint& Blueprint, const FHyperAIBlueprintStructureOp& Op, TArray<FString>& OutMade, FString& OutError)
	{
		UEdGraph* Graph = FindGraph(Blueprint, Op.Graph);
		if (!FBlueprintEditorUtils::DoesSupportTimelines(&Blueprint) || !IsEventGraph(Graph))
		{
			OutError = Graph ? FString::Printf(TEXT("A timeline goes in an event graph of an Actor Blueprint; %s is not one."), *Graph->GetName())
				: FString::Printf(TEXT("%s has no graph '%s'."), *Blueprint.GetName(), *Op.Graph);
			return false;
		}
		if (!IsNameFree(Blueprint, Op.Name, OutError))
		{
			return false;
		}
		const FName Name(*Op.Name.TrimStartAndEnd());
		Blueprint.Modify();
		UTimelineTemplate* Timeline = FBlueprintEditorUtils::AddNewTimeline(&Blueprint, Name);
		if (!Timeline)
		{
			OutError = FString::Printf(TEXT("Unreal did not add the timeline %s."), *Op.Name);
			return false;
		}
		Timeline->Modify();
		// As the Timeline editor makes a track: the curve belongs to the generated class, and is public so level instances reach it.
		UClass* OwnerClass = Blueprint.GeneratedClass;
		float LastKey = 0.f;
		for (const FHyperAIBlueprintTimelineTrack& Track : Op.Tracks)
		{
			const FName TrackName(*Track.Name.TrimStartAndEnd());
			const FString Type = Track.Type.TrimStartAndEnd().ToLower();
			const ERichCurveInterpMode Interp = ParseInterp(Track.Interp);
			for (const FHyperAIBlueprintTimelineKey& Key : Track.Keys)
			{
				LastKey = FMath::Max(LastKey, Key.Time);
			}
			FTTTrackId Id;
			if (Type == TEXT("event"))
			{
				FTTEventTrack New;
				New.SetTrackName(TrackName, Timeline);
				New.CurveKeys = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
				New.CurveKeys->bIsEventCurve = true;
				AddKeys(New.CurveKeys->FloatCurve, Track.Keys, INDEX_NONE, RCIM_Constant);
				Id = FTTTrackId(FTTTrackBase::TT_Event, Timeline->EventTracks.Num());
				Timeline->EventTracks.Add(New);
			}
			else if (Type == TEXT("float"))
			{
				FTTFloatTrack New;
				New.SetTrackName(TrackName, Timeline);
				New.CurveFloat = NewObject<UCurveFloat>(OwnerClass, NAME_None, RF_Public);
				AddKeys(New.CurveFloat->FloatCurve, Track.Keys, 0, Interp);
				Id = FTTTrackId(FTTTrackBase::TT_FloatInterp, Timeline->FloatTracks.Num());
				Timeline->FloatTracks.Add(New);
			}
			else if (Type == TEXT("vector"))
			{
				FTTVectorTrack New;
				New.SetTrackName(TrackName, Timeline);
				New.CurveVector = NewObject<UCurveVector>(OwnerClass, NAME_None, RF_Public);
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					AddKeys(New.CurveVector->FloatCurves[Channel], Track.Keys, Channel, Interp);
				}
				Id = FTTTrackId(FTTTrackBase::TT_VectorInterp, Timeline->VectorTracks.Num());
				Timeline->VectorTracks.Add(New);
			}
			else
			{
				FTTLinearColorTrack New;
				New.SetTrackName(TrackName, Timeline);
				New.CurveLinearColor = NewObject<UCurveLinearColor>(OwnerClass, NAME_None, RF_Public);
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					AddKeys(New.CurveLinearColor->FloatCurves[Channel], Track.Keys, Channel, Interp);
				}
				Id = FTTTrackId(FTTTrackBase::TT_LinearColorInterp, Timeline->LinearColorTracks.Num());
				Timeline->LinearColorTracks.Add(New);
			}
			Timeline->AddDisplayTrack(Id);
		}
		Timeline->LengthMode = Op.Length > 0.f ? TL_TimelineLength : TL_LastKeyFrame;
		Timeline->TimelineLength = Op.Length > 0.f ? Op.Length : FMath::Max(LastKey, 0.01f);
		Timeline->bAutoPlay = Op.bAutoPlay;
		Timeline->bLoop = Op.bLoop;
		Timeline->bReplicated = Op.bReplicated;
		Timeline->bIgnoreTimeDilation = Op.bIgnoreTimeDilation;
		UK2Node_Timeline* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_Timeline>(Graph, FVector2D(Op.X, Op.Y), EK2NewNodeFlags::None,
			[Name](UK2Node_Timeline* New) { New->TimelineName = Name; });
		if (!Node)
		{
			OutError = TEXT("Unreal did not place the timeline's node.");
			return false;
		}
		Node->bAutoPlay = Timeline->bAutoPlay;
		Node->bLoop = Timeline->bLoop;
		Node->bReplicated = Timeline->bReplicated;
		Node->bIgnoreTimeDilation = Timeline->bIgnoreTimeDilation;
		Node->ReconstructNode();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		OutMade.Add(TEXT("timeline ") + DescribeTimeline(Blueprint, *Timeline));
		if (!Op.Drive.TrimStartAndEnd().IsEmpty())
		{
			// The Update pin runs the function; each track's pin feeds the input named like it.
			const UFunction* Function = FindUField<UFunction>(Blueprint.SkeletonGeneratedClass.Get(), FName(*Op.Drive.TrimStartAndEnd()));
			IBlueprintNodeBinder::FBindingSet Bindings;
			UEdGraphNode* Call = Function ? UBlueprintFunctionNodeSpawner::Create(Function)->Invoke(Graph, Bindings, FVector2D(Op.X + 400, Op.Y)) : nullptr;
			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			UEdGraphPin* Execute = Call ? Schema->FindExecutionPin(*Call, EGPD_Input) : nullptr;
			if (!Execute || !Schema->TryCreateConnection(Node->GetUpdatePin(), Execute))
			{
				OutError = FString::Printf(TEXT("%s has no function %s for the timeline to drive (write it, and compile, first)."), *Blueprint.GetName(), *Op.Drive);
				return false;
			}
			TArray<FString> Fed;
			for (const FHyperAIBlueprintTimelineTrack& Track : Op.Tracks)
			{
				UEdGraphPin* Out = Node->FindPin(FName(*Track.Name.TrimStartAndEnd()), EGPD_Output);
				UEdGraphPin* In = Call->FindPin(FName(*Track.Name.TrimStartAndEnd()), EGPD_Input);
				if (Out && In && Out->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && Schema->TryCreateConnection(Out, In))
				{
					Fed.Add(Track.Name);
				}
			}
			OutMade.Add(FString::Printf(TEXT("%s.Update runs %s%s; %s"), *Node->GetName(), *Op.Drive,
				Fed.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", fed %s"), *FString::Join(Fed, TEXT(", "))), *DescribeNode(*Call)));
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------------- collapse, expand

	FBlueprintEditor* OpenBlueprintEditor(UBlueprint& Blueprint, bool& bOutOpened)
	{
		bOutOpened = false;
		UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!Editors)
		{
			return nullptr;
		}
		IAssetEditorInstance* Instance = Editors->FindEditorForAsset(&Blueprint, /*bFocusIfOpen=*/false);
		if (!Instance)
		{
			bOutOpened = Editors->OpenEditorForAsset(&Blueprint);
			Instance = Editors->FindEditorForAsset(&Blueprint, /*bFocusIfOpen=*/false);
		}
		static const TSet<FName> BlueprintEditors = {TEXT("BlueprintEditor"), TEXT("AnimationBlueprintEditor"), TEXT("WidgetBlueprintEditor")};
		return Instance && BlueprintEditors.Contains(Instance->GetEditorName()) ? static_cast<FBlueprintEditor*>(Instance) : nullptr;
	}

	void PlaceAround(TSet<UEdGraphNode*>& Nodes, const FVector2D& At, const bool bNewGuids)
	{
		if (Nodes.IsEmpty())
		{
			return;
		}
		FVector2D Sum = FVector2D::ZeroVector;
		for (const UEdGraphNode* Node : Nodes)
		{
			Sum += FVector2D(Node->NodePosX, Node->NodePosY);
		}
		const FVector2D Average = Sum / Nodes.Num();
		for (UEdGraphNode* Node : Nodes)
		{
			Node->NodePosX = static_cast<int32>(Node->NodePosX - Average.X + At.X);
			Node->NodePosY = static_cast<int32>(Node->NodePosY - Average.Y + At.Y);
			Node->SnapToGrid(16);
			if (bNewGuids)
			{
				Node->CreateNewGuid();
			}
		}
	}

	bool GatherNodes(UBlueprint& Blueprint, const FHyperAIBlueprintStructureOp& Op, TSet<UEdGraphNode*>& OutNodes, UEdGraph*& OutGraph, FString& OutError)
	{
		OutGraph = Op.Graph.TrimStartAndEnd().IsEmpty() ? nullptr : FindGraph(Blueprint, Op.Graph);
		if (!Op.Graph.TrimStartAndEnd().IsEmpty() && !OutGraph)
		{
			OutError = FString::Printf(TEXT("%s has no graph '%s'."), *Blueprint.GetName(), *Op.Graph);
			return false;
		}
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		for (const FString& Spec : Op.Nodes)
		{
			UEdGraphNode* Node = FindNode(Blueprint, Spec, OutGraph, OutError);
			if (!Node)
			{
				return false;
			}
			OutGraph = OutGraph ? OutGraph : Node->GetGraph();
			if (!Schema->CanEncapuslateNode(*Node))
			{
				OutError = FString::Printf(TEXT("%s ('%s') can't be folded into another graph: it is a graph's own entry, result or event."), *Node->GetName(), *Title(*Node));
				return false;
			}
			OutNodes.Add(Node);
		}
		if (OutNodes.IsEmpty() || !OutGraph || OutGraph->GetSchema()->GetClass() != UEdGraphSchema_K2::StaticClass())
		{
			OutError = OutNodes.IsEmpty() ? TEXT("Name the Nodes to collapse.") : TEXT("Only Blueprint script graphs collapse (not animation or other graphs).");
			return false;
		}
		return true;
	}

	bool Collapse(UBlueprint& Blueprint, const FHyperAIBlueprintStructureOp& Op, TArray<FString>& OutMade, FString& OutError)
	{
		TSet<UEdGraphNode*> Nodes;
		UEdGraph* Source = nullptr;
		if (!GatherNodes(Blueprint, Op, Nodes, Source, OutError))
		{
			return false;
		}
		const FString Into = Op.Into.TrimStartAndEnd().IsEmpty() ? TEXT("collapsed") : Op.Into.TrimStartAndEnd().ToLower();
		if (Into != TEXT("collapsed") && Into != TEXT("function") && Into != TEXT("macro"))
		{
			OutError = FString::Printf(TEXT("Into is collapsed, function or macro, not '%s'."), *Op.Into);
			return false;
		}
		const FString Base = Into == TEXT("function") ? TEXT("NewFunction") : Into == TEXT("macro") ? TEXT("NewMacro") : TEXT("CollapseGraph");
		const FName Name = Op.Name.TrimStartAndEnd().IsEmpty() ? FBlueprintEditorUtils::FindUniqueKismetName(&Blueprint, Base) : FName(*Op.Name.TrimStartAndEnd());
		if (!Op.Name.TrimStartAndEnd().IsEmpty() && !IsNameFree(Blueprint, Op.Name, OutError))
		{
			return false;
		}
		bool bOpened = false;
		FBlueprintEditor* Editor = OpenBlueprintEditor(Blueprint, bOpened);
		ON_SCOPE_EXIT
		{
			if (bOpened && GEditor)
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(&Blueprint);
			}
		};
		if (!Editor)
		{
			OutError = TEXT("Epic's Blueprint editor could not be opened for it, and collapsing is its work.");
			return false;
		}
		if (Into == TEXT("function") && !FHyperAIBlueprintEditorAccess::CanCollapseToFunction(*Editor, Nodes))
		{
			OutError = TEXT("These nodes can't become a function: a latent node (Delay, a timeline) or an event other than one custom event, more than one way in, ")
				TEXT("or links out to different places. The Message Log's Blueprint page says which.");
			return false;
		}
		if (Into == TEXT("macro"))
		{
			// Epic's own check reads the editor's selection, so it is made here the way it makes it: can each node live in a macro?
			UEdGraph* Trial = FBlueprintEditorUtils::CreateNewGraph(&Blueprint, FBlueprintEditorUtils::FindUniqueKismetName(&Blueprint, TEXT("TempGraph")),
				UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddMacroGraph(&Blueprint, Trial, /*bIsUserCreated=*/true, nullptr);
			const UEdGraphNode* Refused = nullptr;
			for (const UEdGraphNode* Node : Nodes)
			{
				Refused = Refused ? Refused : (Node->CanPasteHere(Trial) ? nullptr : Node);
			}
			FBlueprintEditorUtils::RemoveGraph(&Blueprint, Trial);
			Trial->MarkAsGarbage();
			if (Refused)
			{
				OutError = FString::Printf(TEXT("%s ('%s') can't be placed in a macro."), *Refused->GetName(), *Title(*Refused));
				return false;
			}
		}
		Blueprint.Modify();
		Source->Modify();
		UEdGraphNode* Gateway = nullptr;
		UEdGraph* Destination = nullptr;
		if (Into == TEXT("collapsed"))
		{
			UK2Node_Composite* Composite = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_Composite>(Source, FVector2D::ZeroVector, EK2NewNodeFlags::None);
			Composite->bCanRenameNode = true;
			Destination = Composite->BoundGraph;
			FBlueprintEditorUtils::RenameGraphWithSuggestion(Destination, MakeShareable(new FKismetNameValidator(&Blueprint, Name)), Name.ToString());
			FHyperAIBlueprintEditorAccess::CollapseInto(*Editor, Composite, Composite->GetInputSink(), Composite->GetOutputSource(), Source, Destination, Nodes,
				/*bCanDiscardEmptyReturnNode=*/false, /*bCanHaveWeakObjPtrParam=*/true);
			Gateway = Composite;
		}
		else if (Into == TEXT("function"))
		{
			Destination = FBlueprintEditorUtils::CreateNewGraph(&Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(&Blueprint, Destination, /*bIsUserCreated=*/true, nullptr);
			TArray<UK2Node_FunctionEntry*> Entries;
			Destination->GetNodesOfClass(Entries);
			FGraphNodeCreator<UK2Node_FunctionResult> ResultCreator(*Destination);
			UK2Node_FunctionResult* Result = ResultCreator.CreateNode();
			Result->NodePosX = Entries[0]->NodePosX + Entries[0]->NodeWidth + 256;
			Result->NodePosY = Entries[0]->NodePosY;
			ResultCreator.Finalize();
			IBlueprintNodeBinder::FBindingSet Bindings;
			Gateway = UBlueprintFunctionNodeSpawner::Create(FindUField<UFunction>(Blueprint.SkeletonGeneratedClass.Get(), Name))->Invoke(Source, Bindings, FVector2D::ZeroVector);
			if (!Gateway)
			{
				OutError = TEXT("Unreal did not place the call to the new function.");
				return false;
			}
			FHyperAIBlueprintEditorAccess::CollapseInto(*Editor, Gateway, Entries[0], Result, Source, Destination, Nodes,
				/*bCanDiscardEmptyReturnNode=*/true, /*bCanHaveWeakObjPtrParam=*/false);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
			Gateway->ReconstructNode();
		}
		else
		{
			Destination = FBlueprintEditorUtils::CreateNewGraph(&Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddMacroGraph(&Blueprint, Destination, /*bIsUserCreated=*/true, nullptr);
			UK2Node_MacroInstance* Macro = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_MacroInstance>(Source, FVector2D::ZeroVector, EK2NewNodeFlags::None,
				[Destination](UK2Node_MacroInstance* New) { New->SetMacroGraph(Destination); });
			UK2Node_EditablePinBase* InputSink = nullptr;
			UK2Node_EditablePinBase* OutputSink = nullptr;
			TArray<UK2Node_Tunnel*> Tunnels;
			Destination->GetNodesOfClass(Tunnels);
			for (UK2Node_Tunnel* Tunnel : Tunnels)
			{
				if (Tunnel->IsEditable())
				{
					InputSink = Tunnel->bCanHaveOutputs ? Tunnel : InputSink;
					OutputSink = !Tunnel->bCanHaveOutputs && Tunnel->bCanHaveInputs ? Tunnel : OutputSink;
				}
			}
			FHyperAIBlueprintEditorAccess::CollapseInto(*Editor, Macro, InputSink, OutputSink, Source, Destination, Nodes,
				/*bCanDiscardEmptyReturnNode=*/false, /*bCanHaveWeakObjPtrParam=*/false);
			Macro->ReconstructNode();
			Gateway = Macro;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		OutMade.Add(FString::Printf(TEXT("%d node(s) collapsed into the %s %s; %s"), Nodes.Num(), *Into, *Destination->GetName(), *DescribeNode(*Gateway)));
		return true;
	}

	/** One pin of the node being expanded: what it was linked to outside, and what its twin on the entry or result node reached inside. */
	struct FBoundaryLink
	{
		bool bInput = true;
		bool bExec = false;
		TArray<UEdGraphPin*> Outside;
		TArray<UEdGraphPin*> Inside;
		FString DefaultValue;
		UObject* DefaultObject = nullptr;
		FText DefaultText;
	};

	/**
	 * Epic's Expand moves the copied graph's nodes out, but first drops any it can't paste into the destination, and a function's
	 * entry and result (and a macro's tunnels) can't be pasted into an event graph: the links through them go with them. They are
	 * written down here, before, so RestoreBoundary can put them back after, as Expand meant to.
	 */
	TArray<FBoundaryLink> CaptureBoundary(UEdGraphNode& Gateway, UEdGraph& Copy)
	{
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		const bool bFunction = Gateway.IsA<UK2Node_CallFunction>();
		UEdGraphNode* Entry = nullptr;
		UEdGraphNode* Result = nullptr;
		for (UEdGraphNode* Node : Copy.Nodes)
		{
			const UK2Node_Tunnel* Tunnel = Node && Node->GetClass() == UK2Node_Tunnel::StaticClass() ? CastChecked<UK2Node_Tunnel>(Node) : nullptr;
			if (Node && (Node->GetClass() == UK2Node_FunctionEntry::StaticClass() || (Tunnel && Tunnel->bCanHaveOutputs)))
			{
				Entry = Node;
			}
			else if (Node && (Node->GetClass() == UK2Node_FunctionResult::StaticClass() || (Tunnel && Tunnel->bCanHaveInputs)))
			{
				Result = Node;
			}
		}
		TArray<FBoundaryLink> Links;
		for (UEdGraphPin* Pin : Gateway.Pins)
		{
			if (!Pin || Pin->bHidden || Pin->PinName == UEdGraphSchema_K2::PN_Self)
			{
				continue;
			}
			FBoundaryLink Link;
			Link.bInput = Pin->Direction == EGPD_Input;
			Link.bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			if (bFunction && Link.bExec && !Link.bInput)
			{
				continue; // A function's way out is reconnected by Expand itself.
			}
			UEdGraphNode* Boundary = Link.bInput ? Entry : Result;
			UEdGraphPin* Twin = !Boundary ? nullptr : bFunction && Link.bExec ? Schema->FindExecutionPin(*Boundary, Link.bInput ? EGPD_Output : EGPD_Input)
				: Boundary->FindPin(Pin->PinName, Link.bInput ? EGPD_Output : EGPD_Input);
			if (!Twin)
			{
				continue;
			}
			Link.Outside = Pin->LinkedTo;
			Link.Inside = Twin->LinkedTo;
			Link.DefaultValue = Pin->DefaultValue;
			Link.DefaultObject = Pin->DefaultObject;
			Link.DefaultText = Pin->DefaultTextValue;
			Links.Add(MoveTemp(Link));
		}
		return Links;
	}

	void RestoreBoundary(const TArray<FBoundaryLink>& Links, const TSet<UEdGraphNode*>& Expanded)
	{
		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		for (const FBoundaryLink& Link : Links)
		{
			for (UEdGraphPin* Inner : Link.Inside)
			{
				if (!Inner || !Expanded.Contains(Inner->GetOwningNode()))
				{
					continue;
				}
				if (!Link.bInput)
				{
					for (UEdGraphPin* Consumer : Link.Outside)
					{
						if (Consumer && !Inner->LinkedTo.Contains(Consumer))
						{
							Schema->TryCreateConnection(Inner, Consumer);
						}
					}
					continue;
				}
				if (Link.Outside.IsEmpty() && !Link.bExec && Inner->LinkedTo.IsEmpty())
				{
					Inner->DefaultValue = Link.DefaultValue;
					Inner->DefaultObject = Link.DefaultObject;
					Inner->DefaultTextValue = Link.DefaultText;
				}
				for (UEdGraphPin* Source : Link.Outside)
				{
					if (Source && !Source->LinkedTo.Contains(Inner))
					{
						Schema->TryCreateConnection(Source, Inner);
					}
				}
			}
		}
	}

	bool Expand(UBlueprint& Blueprint, const FHyperAIBlueprintStructureOp& Op, TArray<FString>& OutMade, FString& OutError)
	{
		if (Op.Nodes.Num() != 1)
		{
			OutError = TEXT("graph.expand opens one node: name it in Nodes.");
			return false;
		}
		UEdGraph* InGraph = Op.Graph.TrimStartAndEnd().IsEmpty() ? nullptr : FindGraph(Blueprint, Op.Graph);
		UEdGraphNode* Node = FindNode(Blueprint, Op.Nodes[0], InGraph, OutError);
		if (!Node)
		{
			return false;
		}
		const FVector2D At(Node->NodePosX, Node->NodePosY);
		const FString Name = Node->GetName();
		TSet<UEdGraphNode*> Expanded;
		Blueprint.Modify();
		Node->GetGraph()->Modify();
		// As Epic's Expand Node does it: a macro or a function is copied out, a collapsed graph moves out and is removed.
		if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			UEdGraph* MacroGraph = Macro->GetMacroGraph();
			if (!MacroGraph)
			{
				OutError = FString::Printf(TEXT("%s's macro graph is missing."), *Name);
				return false;
			}
			UEdGraph* Copy = FEdGraphUtilities::CloneGraph(MacroGraph, nullptr);
			const TArray<FBoundaryLink> Boundary = CaptureBoundary(*Macro, *Copy);
			FHyperAIBlueprintEditorAccess::Expand(Macro, Copy, Expanded);
			RestoreBoundary(Boundary, Expanded);
			Copy->MarkAsGarbage();
			PlaceAround(Expanded, At, /*bNewGuids=*/true);
		}
		else if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
		{
			UEdGraph* Bound = Composite->BoundGraph;
			FHyperAIBlueprintEditorAccess::Expand(Composite, Bound, Expanded);
			FBlueprintEditorUtils::RemoveGraph(&Blueprint, Bound, EGraphRemoveFlags::Recompile);
			PlaceAround(Expanded, At, /*bNewGuids=*/false);
		}
		else if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			const UEdGraphNode* EventNode = nullptr;
			UEdGraph* FunctionGraph = Call->GetFunctionGraph(EventNode);
			if (!FunctionGraph || EventNode)
			{
				OutError = FString::Printf(TEXT("%s calls %s, which is not a function graph of this Blueprint to open out."), *Name, *Title(*Node));
				return false;
			}
			UEdGraph* Copy = FEdGraphUtilities::CloneGraph(FunctionGraph, nullptr);
			const TArray<FBoundaryLink> Boundary = CaptureBoundary(*Call, *Copy);
			FHyperAIBlueprintEditorAccess::Expand(Call, Copy, Expanded);
			RestoreBoundary(Boundary, Expanded);
			Copy->MarkAsGarbage();
			PlaceAround(Expanded, At, /*bNewGuids=*/true);
		}
		else
		{
			OutError = FString::Printf(TEXT("%s ('%s') is not a collapsed graph, a macro or a function call."), *Name, *Title(*Node));
			return false;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		TArray<FString> Names;
		for (const UEdGraphNode* Out : Expanded)
		{
			Names.Add(Out->GetName());
		}
		OutMade.Add(FString::Printf(TEXT("%s expanded into %d node(s): %s"), *Name, Expanded.Num(), *FString::Join(Names, TEXT(", "))));
		return true;
	}

	// --------------------------------------------------------------------------------------------------------------- ops

	bool Connect(UBlueprint& Blueprint, const FHyperAIBlueprintStructureOp& Op, TArray<FString>& OutMade, FString& OutError)
	{
		FString FromNode, FromPin, ToNode, ToPin;
		if (!SplitNodePin(Op.From, FromNode, FromPin) || !SplitNodePin(Op.To, ToNode, ToPin))
		{
			OutError = TEXT("From and To are each Node.Pin, such as \"Event BeginPlay.then\" and \"Open.Play\".");
			return false;
		}
		UEdGraph* InGraph = Op.Graph.TrimStartAndEnd().IsEmpty() ? nullptr : FindGraph(Blueprint, Op.Graph);
		UEdGraphNode* A = FindNode(Blueprint, FromNode, InGraph, OutError);
		UEdGraphNode* B = A ? FindNode(Blueprint, ToNode, A->GetGraph(), OutError) : nullptr;
		if (!A || !B)
		{
			return false;
		}
		UEdGraphPin* Out = FindPin(*A, FromPin, EGPD_Output);
		UEdGraphPin* In = FindPin(*B, ToPin, EGPD_Input);
		if (!Out || !In)
		{
			OutError = !Out ? FString::Printf(TEXT("%s has no output pin '%s'; its outputs: %s."), *A->GetName(), *FromPin, *PinNames(*A, EGPD_Output))
				: FString::Printf(TEXT("%s has no input pin '%s'; its inputs: %s."), *B->GetName(), *ToPin, *PinNames(*B, EGPD_Input));
			return false;
		}
		const UEdGraphSchema* Schema = A->GetGraph()->GetSchema();
		const FPinConnectionResponse Response = Schema->CanCreateConnection(Out, In);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW || !Schema->TryCreateConnection(Out, In))
		{
			OutError = FString::Printf(TEXT("%s.%s can't drive %s.%s: %s"), *A->GetName(), *Out->PinName.ToString(), *B->GetName(), *In->PinName.ToString(),
				*Response.Message.ToString());
			return false;
		}
		FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
		OutMade.Add(FString::Printf(TEXT("linked %s.%s -> %s.%s%s"), *A->GetName(), *Out->PinName.ToString(), *B->GetName(), *In->PinName.ToString(),
			Response.Response == CONNECT_RESPONSE_MAKE ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Response.Message.ToString())));
		return true;
	}

	/** Applies one op; Touched collects the Blueprints to compile and save, interfaces first. */
	// ------------------------------------------------------------------------------------------------ structs and enums

	UUserDefinedStruct* LoadStruct(const FString& Path)
	{
		return Path.TrimStartAndEnd().IsEmpty() ? nullptr : LoadObject<UUserDefinedStruct>(nullptr, *ObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	UUserDefinedEnum* LoadEnum(const FString& Path)
	{
		return Path.TrimStartAndEnd().IsEmpty() ? nullptr : LoadObject<UUserDefinedEnum>(nullptr, *ObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** The package for a new struct or enum; refused when an asset is there already. */
	UPackage* NewAssetPackage(const FString& Path, FString& OutError)
	{
		if (!IsProjectPath(Path, OutError))
		{
			return nullptr;
		}
		const FString Package = PackageOf(Path);
		if (FindObject<UObject>(nullptr, *ObjectPath(Package)) || FPackageName::DoesPackageExist(Package))
		{
			OutError = FString::Printf(TEXT("%s exists already."), *Package);
			return nullptr;
		}
		return CreatePackage(*Package);
	}

	const FStructVariableDescription* FindField(const UUserDefinedStruct& Struct, const FString& Name)
	{
		return FStructureEditorUtils::GetVarDesc(&Struct).FindByPredicate([Wanted = Name.TrimStartAndEnd()](const FStructVariableDescription& Field)
		{
			return Field.FriendlyName.Equals(Wanted, ESearchCase::IgnoreCase);
		});
	}

	FString FieldNames(const UUserDefinedStruct& Struct)
	{
		TArray<FString> Names;
		for (const FStructVariableDescription& Field : FStructureEditorUtils::GetVarDesc(&Struct))
		{
			Names.Add(Field.FriendlyName);
		}
		return FString::Join(Names, TEXT(", "));
	}

	FString DescribeField(const FStructVariableDescription& Field)
	{
		FString Line = FString::Printf(TEXT("%s: %s"), *Field.FriendlyName, *TypeName(Field.ToPinType()));
		if (!Field.DefaultValue.IsEmpty())
		{
			Line += TEXT(" = ") + Field.DefaultValue;
		}
		if (!Field.ToolTip.IsEmpty())
		{
			Line += TEXT(" (") + Field.ToolTip + TEXT(")");
		}
		return Line;
	}

	FString DescribeStruct(const UUserDefinedStruct& Struct)
	{
		TArray<FString> Fields;
		for (const FStructVariableDescription& Field : FStructureEditorUtils::GetVarDesc(&Struct))
		{
			Fields.Add(DescribeField(Field));
		}
		return FString::Printf(TEXT("struct %s: %s"), *Struct.GetPathName(), *FString::Join(Fields, TEXT(", ")));
	}

	/** An enum field's default may name a value as it reads (Idle); the struct keeps the value's own name (NewEnumerator0). */
	FString EnumDefault(const FEdGraphPinType& Type, const FString& Default)
	{
		if (const UEnum* Enum = Cast<UEnum>(Type.PinSubCategoryObject.Get()))
		{
			for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
			{
				if (Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(Default, ESearchCase::IgnoreCase))
				{
					return Enum->GetNameStringByIndex(Index);
				}
			}
		}
		return Default;
	}

	bool SetFieldDetails(UUserDefinedStruct& Struct, const FGuid Guid, const FHyperAIBlueprintStructField& Field, FString& OutError)
	{
		const FStructVariableDescription* Current = FStructureEditorUtils::GetVarDescByGuid(&Struct, Guid);
		if (!Current)
		{
			OutError = TEXT("The field went missing while it was being set.");
			return false;
		}
		const FString Default = EnumDefault(Current->ToPinType(), Field.Default.TrimStartAndEnd());
		if (!Default.IsEmpty() && Default != Current->DefaultValue && !FStructureEditorUtils::ChangeVariableDefaultValue(&Struct, Guid, Default))
		{
			OutError = FString::Printf(TEXT("'%s' is not a default a %s field takes: numbers as 100, bools as true, vectors 1,2,3, rotators 0,90,0, ")
				TEXT("colors (R=1,G=0,B=0,A=1), an enum's value by name, an asset by its path."), *Field.Default, *TypeName(Current->ToPinType()));
			return false;
		}
		if (!Field.Tooltip.IsEmpty())
		{
			FStructureEditorUtils::ChangeVariableTooltip(&Struct, Guid, Field.Tooltip);
		}
		return true;
	}

	bool AddFields(UUserDefinedStruct& Struct, const TArray<FHyperAIBlueprintStructField>& Fields, FString& OutError)
	{
		for (const FHyperAIBlueprintStructField& Field : Fields)
		{
			FEdGraphPinType Type;
			FString Why;
			if (!ParseType(Field.Type, Type, OutError))
			{
				return false;
			}
			if (!FStructureEditorUtils::CanHaveAMemberVariableOfType(&Struct, Type, &Why) || !FStructureEditorUtils::AddVariable(&Struct, Type))
			{
				OutError = FString::Printf(TEXT("%s can't have a %s field %s. %s"), *Struct.GetName(), *Field.Type, *Field.Name, *Why);
				return false;
			}
			const FGuid Guid = FStructureEditorUtils::GetVarDesc(&Struct).Last().VarGuid;
			if (!FStructureEditorUtils::RenameVariable(&Struct, Guid, Field.Name.TrimStartAndEnd()))
			{
				OutError = FString::Printf(TEXT("%s can't name a field '%s': each field's name is its own (it has %s)."), *Struct.GetName(), *Field.Name, *FieldNames(Struct));
				return false;
			}
			if (!SetFieldDetails(Struct, Guid, Field, OutError))
			{
				return false;
			}
		}
		return true;
	}

	UUserDefinedStruct* CreateStruct(const FHyperAIBlueprintStructureOp& Op, FString& OutError)
	{
		UPackage* Package = NewAssetPackage(Op.Asset, OutError);
		if (!Package)
		{
			return nullptr;
		}
		UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*FPackageName::GetShortName(Package)),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Struct)
		{
			OutError = TEXT("Unreal did not make the struct.");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Struct);
		Package->MarkPackageDirty();
		// A new struct starts with one bool field, MemberVar_0; the plan's fields take its place.
		const FGuid Placeholder = FStructureEditorUtils::GetVarDesc(Struct).IsEmpty() ? FGuid() : FStructureEditorUtils::GetVarDesc(Struct)[0].VarGuid;
		if (!AddFields(*Struct, Op.Fields, OutError))
		{
			return nullptr;
		}
		if (Placeholder.IsValid())
		{
			FStructureEditorUtils::RemoveVariable(Struct, Placeholder);
		}
		return Struct;
	}

	/** An enum's value by the name it reads as (Idle), or by its own (NewEnumerator0). */
	int32 FindValue(const UUserDefinedEnum& Enum, const FString& Name)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		for (int32 Index = 0; Index < Enum.NumEnums() - 1; ++Index)
		{
			if (Enum.GetDisplayNameTextByIndex(Index).ToString().Equals(Wanted, ESearchCase::IgnoreCase)
				|| Enum.GetNameStringByIndex(Index).Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	TArray<FString> DescribeValues(const UUserDefinedEnum& Enum)
	{
		TArray<FString> Values;
		for (int32 Index = 0; Index < Enum.NumEnums() - 1; ++Index)
		{
			Values.Add(FString::Printf(TEXT("%d %s (%s)"), Index, *Enum.GetDisplayNameTextByIndex(Index).ToString(), *Enum.GetNameStringByIndex(Index)));
		}
		return Values;
	}

	bool AddValues(UUserDefinedEnum& Enum, const TArray<FString>& Values, FString& OutError)
	{
		for (const FString& Value : Values)
		{
			FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(&Enum);
			// The new value sits last, before the hidden _MAX entry.
			if (!FEnumEditorUtils::SetEnumeratorDisplayName(&Enum, Enum.NumEnums() - 2, FText::FromString(Value.TrimStartAndEnd())))
			{
				OutError = FString::Printf(TEXT("%s can't have a value named '%s': each value's name is its own."), *Enum.GetName(), *Value);
				return false;
			}
		}
		return true;
	}

	UUserDefinedEnum* CreateEnum(const FHyperAIBlueprintStructureOp& Op, FString& OutError)
	{
		UPackage* Package = NewAssetPackage(Op.Asset, OutError);
		if (!Package)
		{
			return nullptr;
		}
		UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*FPackageName::GetShortName(Package)),
			RF_Public | RF_Standalone | RF_Transactional));
		if (!Enum)
		{
			OutError = TEXT("Unreal did not make the enum.");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Enum);
		Package->MarkPackageDirty();
		return AddValues(*Enum, Op.Values, OutError) ? Enum : nullptr;
	}

	/** struct.* and enum.*, through the struct and enum editors' own code. */
	bool ApplyTypeOp(const FString& Kind, const FHyperAIBlueprintStructureOp& Op, TArray<UObject*>& TouchedAssets, TArray<FString>& OutMade, FString& OutError)
	{
		if (Kind.StartsWith(TEXT("struct.")))
		{
			UUserDefinedStruct* Struct = Kind == TEXT("struct.create") ? CreateStruct(Op, OutError) : LoadStruct(Op.Asset);
			if (!Struct)
			{
				OutError = OutError.IsEmpty() ? FString::Printf(TEXT("%s is not a struct: struct.create makes one."), *Op.Asset) : OutError;
				return false;
			}
			TouchedAssets.AddUnique(Struct);
			if (Kind == TEXT("struct.add_field"))
			{
				if (!AddFields(*Struct, Op.Fields, OutError))
				{
					return false;
				}
			}
			else if (Kind != TEXT("struct.create"))
			{
				const FStructVariableDescription* Field = FindField(*Struct, Op.Name);
				if (!Field)
				{
					OutError = FString::Printf(TEXT("%s has no field %s; it has %s."), *Struct->GetName(), *Op.Name, *FieldNames(*Struct));
					return false;
				}
				const FGuid Guid = Field->VarGuid;
				if (Kind == TEXT("struct.remove_field"))
				{
					if (!FStructureEditorUtils::RemoveVariable(Struct, Guid))
					{
						OutError = FString::Printf(TEXT("%s keeps at least one field."), *Struct->GetName());
						return false;
					}
				}
				else if (Kind == TEXT("struct.rename_field"))
				{
					if (!FStructureEditorUtils::RenameVariable(Struct, Guid, Op.To.TrimStartAndEnd()))
					{
						OutError = FString::Printf(TEXT("%s can't rename %s to '%s': each field's name is its own (it has %s)."), *Struct->GetName(), *Op.Name, *Op.To, *FieldNames(*Struct));
						return false;
					}
				}
				else if (Kind == TEXT("struct.set_field") && Op.Fields.Num() == 1)
				{
					const FHyperAIBlueprintStructField& Change = Op.Fields[0];
					if (!Change.Type.TrimStartAndEnd().IsEmpty())
					{
						FEdGraphPinType Type;
						if (!ParseType(Change.Type, Type, OutError))
						{
							return false;
						}
						if (Type != Field->ToPinType() && !FStructureEditorUtils::ChangeVariableType(Struct, Guid, Type))
						{
							FString Why;
							FStructureEditorUtils::CanHaveAMemberVariableOfType(Struct, Type, &Why);
							OutError = FString::Printf(TEXT("%s's field %s can't become a %s. %s"), *Struct->GetName(), *Op.Name, *Change.Type, *Why);
							return false;
						}
					}
					if (!SetFieldDetails(*Struct, Guid, Change, OutError))
					{
						return false;
					}
				}
				else
				{
					OutError = FString::Printf(TEXT("Unknown struct op '%s', or struct.set_field without exactly one entry in Fields."), *Op.Kind);
					return false;
				}
			}
			if (Struct->Status == UDSS_Error)
			{
				OutError = FString::Printf(TEXT("%s does not compile: %s"), *Struct->GetName(), *Struct->ErrorMessage);
				return false;
			}
			OutMade.Add(DescribeStruct(*Struct));
			return true;
		}
		UUserDefinedEnum* Enum = Kind == TEXT("enum.create") ? CreateEnum(Op, OutError) : LoadEnum(Op.Asset);
		if (!Enum)
		{
			OutError = OutError.IsEmpty() ? FString::Printf(TEXT("%s is not an enum: enum.create makes one."), *Op.Asset) : OutError;
			return false;
		}
		TouchedAssets.AddUnique(Enum);
		if (Kind == TEXT("enum.add_value"))
		{
			if (!AddValues(*Enum, Op.Values, OutError))
			{
				return false;
			}
		}
		else if (Kind != TEXT("enum.create"))
		{
			const int32 Index = FindValue(*Enum, Op.Name);
			if (Index == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("%s has no value %s; it has %s."), *Enum->GetName(), *Op.Name, *FString::Join(DescribeValues(*Enum), TEXT(", ")));
				return false;
			}
			if (Kind == TEXT("enum.remove_value"))
			{
				FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, Index);
			}
			else if (Kind == TEXT("enum.rename_value"))
			{
				if (!FEnumEditorUtils::SetEnumeratorDisplayName(Enum, Index, FText::FromString(Op.To.TrimStartAndEnd())))
				{
					OutError = FString::Printf(TEXT("%s can't rename %s to '%s': each value's name is its own."), *Enum->GetName(), *Op.Name, *Op.To);
					return false;
				}
			}
			else
			{
				OutError = FString::Printf(TEXT("Unknown enum op '%s'."), *Op.Kind);
				return false;
			}
		}
		OutMade.Add(FString::Printf(TEXT("enum %s: %s"), *Enum->GetPathName(), *FString::Join(DescribeValues(*Enum), TEXT(", "))));
		return true;
	}

	/** A struct or enum op, checked against the asset as it is and against what the plan's earlier ops make of it. */
	bool CheckTypeOp(const FString& Kind, const FHyperAIBlueprintStructureOp& Op, TMap<FString, TSet<FString>>& Members, FString& OutError)
	{
		static const TSet<FString> Kinds = {TEXT("struct.create"), TEXT("struct.add_field"), TEXT("struct.set_field"), TEXT("struct.rename_field"),
			TEXT("struct.remove_field"), TEXT("enum.create"), TEXT("enum.add_value"), TEXT("enum.rename_value"), TEXT("enum.remove_value")};
		if (!Kinds.Contains(Kind))
		{
			OutError = FString::Printf(TEXT("Unknown op '%s'."), *Op.Kind);
			return false;
		}
		const bool bStruct = Kind.StartsWith(TEXT("struct."));
		const FString Key = (bStruct ? TEXT("struct:") : TEXT("enum:")) + PackageOf(Op.Asset).ToLower();
		TSet<FString>* Known = Members.Find(Key);
		if (Kind.EndsWith(TEXT(".create")))
		{
			if (!IsProjectPath(Op.Asset, OutError))
			{
				return false;
			}
			if (Known || FPackageName::DoesPackageExist(PackageOf(Op.Asset)) || FindObject<UObject>(nullptr, *ObjectPath(PackageOf(Op.Asset))))
			{
				OutError = FString::Printf(TEXT("%s exists already."), *Op.Asset);
				return false;
			}
			Known = &Members.Add(Key);
		}
		else if (!Known)
		{
			TSet<FString> Names;
			if (bStruct)
			{
				const UUserDefinedStruct* Struct = LoadStruct(Op.Asset);
				if (!Struct)
				{
					OutError = FString::Printf(TEXT("%s is not a struct, and the plan does not make it."), *Op.Asset);
					return false;
				}
				for (const FStructVariableDescription& Field : FStructureEditorUtils::GetVarDesc(Struct))
				{
					Names.Add(Field.FriendlyName.ToLower());
				}
			}
			else
			{
				const UUserDefinedEnum* Enum = LoadEnum(Op.Asset);
				if (!Enum)
				{
					OutError = FString::Printf(TEXT("%s is not an enum, and the plan does not make it."), *Op.Asset);
					return false;
				}
				for (int32 Index = 0; Index < Enum->NumEnums() - 1; ++Index)
				{
					Names.Add(Enum->GetDisplayNameTextByIndex(Index).ToString().ToLower());
				}
			}
			Known = &Members.Add(Key, MoveTemp(Names));
		}
		auto AddName = [&](const FString& Name)
		{
			const FString Trimmed = Name.TrimStartAndEnd();
			if (Trimmed.IsEmpty() || Known->Contains(Trimmed.ToLower()))
			{
				OutError = Trimmed.IsEmpty() ? FString(TEXT("Every field and value needs a name.")) : FString::Printf(TEXT("%s has %s already."), *Op.Asset, *Trimmed);
				return false;
			}
			Known->Add(Trimmed.ToLower());
			return true;
		};
		auto Has = [&](const FString& Name)
		{
			if (!Known->Contains(Name.TrimStartAndEnd().ToLower()))
			{
				// Names are matched without case, so they are listed that way.
				TArray<FString> Names = Known->Array();
				Names.Sort();
				OutError = FString::Printf(TEXT("%s has no %s %s; it has %s."), *Op.Asset, bStruct ? TEXT("field") : TEXT("value"), *Name,
					*FString::Join(Names, TEXT(", ")));
				return false;
			}
			return true;
		};
		// A field may be a struct or enum an earlier op of the plan makes, which exists only once the plan runs.
		auto CheckType = [&](const FString& TypeText)
		{
			FString Text = TypeText.TrimStartAndEnd();
			Text.RemoveFromEnd(TEXT("[]"));
			FString Prefix, Path;
			if (Text.Split(TEXT(":"), &Prefix, &Path))
			{
				Prefix = Prefix.TrimStartAndEnd().ToLower();
				if ((Prefix == TEXT("struct") || Prefix == TEXT("enum")) && Members.Contains(Prefix + TEXT(":") + PackageOf(Path).ToLower()))
				{
					return true;
				}
			}
			FEdGraphPinType Type;
			return ParseType(TypeText, Type, OutError);
		};
		if (Kind == TEXT("struct.create") || Kind == TEXT("struct.add_field"))
		{
			if (Op.Fields.IsEmpty())
			{
				OutError = TEXT("Give the struct its Fields, each a Name and a Type.");
				return false;
			}
			for (const FHyperAIBlueprintStructField& Field : Op.Fields)
			{
				if (!AddName(Field.Name) || !CheckType(Field.Type))
				{
					return false;
				}
			}
			return true;
		}
		if (Kind == TEXT("enum.create") || Kind == TEXT("enum.add_value"))
		{
			if (Op.Values.IsEmpty())
			{
				OutError = TEXT("Give the enum its Values.");
				return false;
			}
			for (const FString& Value : Op.Values)
			{
				if (!AddName(Value))
				{
					return false;
				}
			}
			return true;
		}
		if (!Has(Op.Name))
		{
			return false;
		}
		if (Kind == TEXT("struct.set_field"))
		{
			if (Op.Fields.Num() != 1)
			{
				OutError = TEXT("struct.set_field takes one entry in Fields: the field's new Type, Default or Tooltip.");
				return false;
			}
			return Op.Fields[0].Type.TrimStartAndEnd().IsEmpty() || CheckType(Op.Fields[0].Type);
		}
		// What is left is a rename, which takes its new name, or a removal.
		Known->Remove(Op.Name.TrimStartAndEnd().ToLower());
		return Kind == TEXT("struct.rename_field") || Kind == TEXT("enum.rename_value") ? AddName(Op.To) : true;
	}

	bool ApplyOp(UBlueprint* Target, const FHyperAIBlueprintStructureOp& Op, TArray<UBlueprint*>& Touched, TArray<UObject*>& TouchedAssets,
		TArray<FString>& OutMade, TArray<FString>& OutNotes, FString& OutError)
	{
		const FString Kind = Op.Kind.TrimStartAndEnd().ToLower();
		auto Touch = [&Touched](UBlueprint* Blueprint)
		{
			Touched.AddUnique(Blueprint);
		};
		if (Kind.StartsWith(TEXT("struct.")) || Kind.StartsWith(TEXT("enum.")))
		{
			return ApplyTypeOp(Kind, Op, TouchedAssets, OutMade, OutError);
		}
		if (Kind == TEXT("interface.create"))
		{
			UBlueprint* Interface = CreateInterface(Op.Interface, OutError);
			if (!Interface)
			{
				return false;
			}
			Touch(Interface);
			OutMade.Add(FString::Printf(TEXT("interface %s"), *Interface->GetPathName()));
			return true;
		}
		if (Kind == TEXT("interface.add_function"))
		{
			UBlueprint* Interface = LoadInterface(Op.Interface, OutError);
			if (!Interface || !AddInterfaceFunction(*Interface, Op, OutMade, OutError))
			{
				return false;
			}
			Touch(Interface);
			return true;
		}
		if (!Target)
		{
			OutError = TEXT("This op edits a Blueprint: give BlueprintPath.");
			return false;
		}
		Touch(Target);
		if (Kind == TEXT("interface.implement") || Kind == TEXT("interface.remove"))
		{
			UBlueprint* Interface = LoadInterface(Op.Interface, OutError);
			if (!Interface)
			{
				return false;
			}
			CompileIfChanged(*Interface);
			UClass* InterfaceClass = Interface->GeneratedClass;
			const bool bHas = Target->ImplementedInterfaces.ContainsByPredicate([InterfaceClass](const FBPInterfaceDescription& Each) { return Each.Interface.Get() == InterfaceClass; });
			if (Kind == TEXT("interface.remove"))
			{
				if (!bHas)
				{
					OutError = FString::Printf(TEXT("%s does not implement %s."), *Target->GetName(), *Interface->GetName());
					return false;
				}
				FBlueprintEditorUtils::RemoveInterface(Target, InterfaceClass->GetClassPathName(), Op.bKeepFunctions);
				OutMade.Add(FString::Printf(TEXT("removed interface %s%s"), *Interface->GetName(), Op.bKeepFunctions ? TEXT(", its functions kept") : TEXT("")));
				return true;
			}
			if (bHas || !FBlueprintEditorUtils::ImplementNewInterface(Target, InterfaceClass->GetClassPathName()))
			{
				OutError = bHas ? FString::Printf(TEXT("%s implements %s already."), *Target->GetName(), *Interface->GetName())
					: FString::Printf(TEXT("Unreal did not let %s implement %s (a parent may implement it, or it may not suit this class)."), *Target->GetName(), *Interface->GetName());
				return false;
			}
			OutMade.Add(TEXT("implements ") + DescribeImplemented(*InterfaceClass));
			return true;
		}
		if (Kind == TEXT("timeline.add"))
		{
			return AddTimeline(*Target, Op, OutMade, OutError);
		}
		if (Kind == TEXT("timeline.remove"))
		{
			UTimelineTemplate* Timeline = Target->FindTimelineTemplateByVariableName(FName(*Op.Name.TrimStartAndEnd()));
			if (!Timeline)
			{
				OutError = FString::Printf(TEXT("%s has no timeline %s."), *Target->GetName(), *Op.Name);
				return false;
			}
			Target->Modify();
			if (UK2Node_Timeline* Node = FBlueprintEditorUtils::FindNodeForTimeline(Target, Timeline))
			{
				FBlueprintEditorUtils::RemoveNode(Target, Node, /*bDontRecompile=*/true);
			}
			if (Target->FindTimelineTemplateByVariableName(FName(*Op.Name.TrimStartAndEnd())))
			{
				FBlueprintEditorUtils::RemoveTimeline(Target, Timeline, /*bDontRecompile=*/true);
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
			OutMade.Add(FString::Printf(TEXT("removed timeline %s"), *Op.Name));
			return true;
		}
		if (Kind == TEXT("event.add"))
		{
			UEdGraph* Graph = FindGraph(*Target, Op.Graph);
			uint32 NetFlags = 0;
			if (!IsEventGraph(Graph) || !ParseRpc(Op.Rpc, NetFlags) || !IsNameFree(*Target, Op.Name, OutError))
			{
				OutError = !IsEventGraph(Graph) ? FString::Printf(TEXT("A custom event goes in an event graph; '%s' is not one."), *Op.Graph)
					: !ParseRpc(Op.Rpc, NetFlags) ? FString::Printf(TEXT("Rpc is none, server, client or multicast, not '%s'."), *Op.Rpc) : OutError;
				return false;
			}
			TArray<TPair<FName, FEdGraphPinType>> Inputs;
			if (!ParseParams(Op.Inputs, Inputs, OutError))
			{
				return false;
			}
			const FName Name(*Op.Name.TrimStartAndEnd());
			UK2Node_CustomEvent* Event = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CustomEvent>(Graph, FVector2D(Op.X, Op.Y), EK2NewNodeFlags::None,
				[Name](UK2Node_CustomEvent* New) { New->CustomFunctionName = Name; New->bIsEditable = true; });
			for (const TPair<FName, FEdGraphPinType>& Input : Inputs)
			{
				Event->CreateUserDefinedPin(Input.Key, Input.Value, EGPD_Output);
			}
			ApplyRpc(*Event, NetFlags, Op.bReliable);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
			OutMade.Add(FString::Printf(TEXT("event %s (%s); %s"), *Name.ToString(), *RpcName(Event->FunctionFlags), *DescribeNode(*Event)));
			return true;
		}
		if (Kind == TEXT("event.set_rpc"))
		{
			UK2Node_CustomEvent* Event = FindCustomEvent(*Target, Op.Name);
			uint32 NetFlags = 0;
			if (!Event || !ParseRpc(Op.Rpc, NetFlags))
			{
				OutError = !Event ? FString::Printf(TEXT("%s has no custom event %s (inherited and interface events keep their own replication)."), *Target->GetName(), *Op.Name)
					: FString::Printf(TEXT("Rpc is none, server, client or multicast, not '%s'."), *Op.Rpc);
				return false;
			}
			ApplyRpc(*Event, NetFlags, Op.bReliable);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
			OutMade.Add(FString::Printf(TEXT("event %s is %s"), *Op.Name, *RpcName(Event->FunctionFlags)));
			return true;
		}
		if (Kind == TEXT("variable.set_replication"))
		{
			const FName Variable(*Op.Name.TrimStartAndEnd());
			const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Target, Variable);
			EBlueprintVariableReplication Mode = EBlueprintVariableReplication::None;
			const ELifetimeCondition* Condition = Conditions().Find(Op.Condition.TrimStartAndEnd().ToLower());
			if (Index == INDEX_NONE || (!Op.Replication.IsEmpty() && !ParseReplication(Op.Replication, Mode)) || (!Op.Condition.IsEmpty() && !Condition)
				|| (Op.Replication.IsEmpty() && Op.Condition.IsEmpty()))
			{
				OutError = Index == INDEX_NONE ? FString::Printf(TEXT("%s has no variable %s of its own."), *Target->GetName(), *Op.Name)
					: Op.Replication.IsEmpty() && Op.Condition.IsEmpty() ? TEXT("Set Replication, Condition or both.")
					: !Op.Condition.IsEmpty() && !Condition ? FString::Printf(TEXT("Unknown Condition '%s'."), *Op.Condition)
					: FString::Printf(TEXT("Replication is none, replicated or rep_notify, not '%s'."), *Op.Replication);
				return false;
			}
			Target->Modify();
			if (!Op.Replication.IsEmpty())
			{
				UBlueprintEditorLibrary::SetBlueprintVariableReplication(Target, Variable, Mode);
			}
			if (Condition)
			{
				Target->NewVariables[Index].ReplicationCondition = *Condition;
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
			const FBPVariableDescription& Now = Target->NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(Target, Variable)];
			OutMade.Add(FString::Printf(TEXT("variable %s: %s%s, condition %s"), *Op.Name, (Now.PropertyFlags & CPF_Net) == 0 ? TEXT("not replicated")
				: Now.RepNotifyFunc.IsNone() ? TEXT("replicated") : TEXT("rep_notify "), (Now.PropertyFlags & CPF_Net) && !Now.RepNotifyFunc.IsNone() ? *Now.RepNotifyFunc.ToString() : TEXT(""),
				*ConditionName(Now.ReplicationCondition)));
			const AActor* Defaults = Target->GeneratedClass ? Cast<AActor>(Target->GeneratedClass->GetDefaultObject()) : nullptr;
			if (Defaults && !Defaults->GetIsReplicated() && (Now.PropertyFlags & CPF_Net))
			{
				OutNotes.Add(FString::Printf(TEXT("%s itself does not replicate yet (Class Defaults, Replicates): its replicated variables stay local until it does."), *Target->GetName()));
			}
			return true;
		}
		if (Kind == TEXT("component.set_replicated"))
		{
			USCS_Node* Node = Target->SimpleConstructionScript ? Target->SimpleConstructionScript->FindSCSNode(FName(*Op.Name.TrimStartAndEnd())) : nullptr;
			UActorComponent* Template = Node ? Node->ComponentTemplate.Get() : nullptr;
			FBoolProperty* Replicates = CastField<FBoolProperty>(UActorComponent::StaticClass()->FindPropertyByName(TEXT("bReplicates")));
			if (!Template || !Replicates || !Template->GetComponentClassCanReplicate())
			{
				OutError = !Template ? FString::Printf(TEXT("%s adds no component %s (inherited ones are set in their own Blueprint or class)."), *Target->GetName(), *Op.Name)
					: FString::Printf(TEXT("A %s can't replicate."), *Template->GetClass()->GetName());
				return false;
			}
			Template->Modify();
			Template->PreEditChange(Replicates);
			Replicates->SetPropertyValue_InContainer(Template, Op.bReplicated);
			FPropertyChangedEvent Changed(Replicates);
			Template->PostEditChangeProperty(Changed);
			FBlueprintEditorUtils::MarkBlueprintAsModified(Target);
			OutMade.Add(FString::Printf(TEXT("component %s %s"), *Op.Name, Op.bReplicated ? TEXT("replicates") : TEXT("does not replicate")));
			return true;
		}
		if (Kind == TEXT("graph.collapse"))
		{
			return Collapse(*Target, Op, OutMade, OutError);
		}
		if (Kind == TEXT("graph.expand"))
		{
			return Expand(*Target, Op, OutMade, OutError);
		}
		if (Kind == TEXT("pins.connect"))
		{
			return Connect(*Target, Op, OutMade, OutError);
		}
		OutError = FString::Printf(TEXT("Unknown op '%s': interface.create, interface.add_function, interface.implement, interface.remove, timeline.add, ")
			TEXT("timeline.remove, event.add, event.set_rpc, variable.set_replication, component.set_replicated, graph.collapse, graph.expand, pins.connect, ")
			TEXT("struct.* or enum.*."), *Op.Kind);
		return false;
	}

	/** What can be known before anything changes: the shape of each op, its names and types, and what it points at. */
	bool Check(const FHyperAIBlueprintStructurePlanRequest& Request, UBlueprint* Target, FString& OutError)
	{
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations; this one has %d."), C::MaxOperations, Request.Operations.Num());
			return false;
		}
		if (!Request.BlueprintPath.TrimStartAndEnd().IsEmpty() && !Target)
		{
			OutError = LoadStruct(Request.BlueprintPath) || LoadEnum(Request.BlueprintPath)
				? FString::Printf(TEXT("%s is a struct or enum: leave BlueprintPath empty and give it as each struct or enum op's Asset."), *Request.BlueprintPath)
				: FString::Printf(TEXT("%s is not a Blueprint."), *Request.BlueprintPath);
			return false;
		}
		TSet<FString> NewInterfaces, NewNames;
		TMap<FString, TSet<FString>> TypeMembers;
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIBlueprintStructureOp& Op = Request.Operations[Index];
			const FString Kind = Op.Kind.TrimStartAndEnd().ToLower();
			FString Error;
			auto Fail = [&](const FString& Why) { OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Kind, *Why); return false; };
			auto CheckTypes = [&](const TArray<FHyperAIBlueprintParam>& Params)
			{
				for (const FHyperAIBlueprintParam& Param : Params)
				{
					FEdGraphPinType Type;
					if (Param.Name.TrimStartAndEnd().IsEmpty() || !ParseType(Param.Type, Type, Error))
					{
						Error = Param.Name.TrimStartAndEnd().IsEmpty() ? TEXT("Every parameter needs a Name.") : Error;
						return false;
					}
				}
				return true;
			};
			if (Kind.StartsWith(TEXT("struct.")) || Kind.StartsWith(TEXT("enum.")))
			{
				if (!CheckTypeOp(Kind, Op, TypeMembers, Error))
				{
					return Fail(Error);
				}
				continue;
			}
			const bool bInterfaceKnown = NewInterfaces.Contains(PackageOf(Op.Interface)) || (LoadBlueprint(Op.Interface) && FBlueprintEditorUtils::IsInterfaceBlueprint(LoadBlueprint(Op.Interface)));
			if (Kind == TEXT("interface.create"))
			{
				if (!IsProjectPath(Op.Interface, Error) || FPackageName::DoesPackageExist(PackageOf(Op.Interface)) || NewInterfaces.Contains(PackageOf(Op.Interface)))
				{
					return Fail(Error.IsEmpty() ? FString::Printf(TEXT("%s exists already."), *Op.Interface) : Error);
				}
				NewInterfaces.Add(PackageOf(Op.Interface));
				continue;
			}
			if (Kind.StartsWith(TEXT("interface.")) && !bInterfaceKnown)
			{
				return Fail(FString::Printf(TEXT("%s is not a Blueprint Interface, and the plan does not make it."), *Op.Interface));
			}
			if (Kind == TEXT("interface.add_function"))
			{
				if (Op.Name.TrimStartAndEnd().IsEmpty() || !CheckTypes(Op.Inputs) || !CheckTypes(Op.Outputs))
				{
					return Fail(Error.IsEmpty() ? TEXT("Give the function a Name.") : Error);
				}
				continue;
			}
			if (!Target)
			{
				return Fail(TEXT("This op edits a Blueprint: give BlueprintPath."));
			}
			if (Kind == TEXT("timeline.add"))
			{
				if (!FBlueprintEditorUtils::DoesSupportTimelines(Target) || !IsEventGraph(FindGraph(*Target, Op.Graph)))
				{
					return Fail(TEXT("A timeline goes in an event graph of an Actor Blueprint."));
				}
				if (NewNames.Contains(Op.Name.ToLower()) || !IsNameFree(*Target, Op.Name, Error))
				{
					return Fail(Error.IsEmpty() ? FString::Printf(TEXT("The plan makes %s twice."), *Op.Name) : Error);
				}
				TSet<FString> Tracks;
				for (const FHyperAIBlueprintTimelineTrack& Track : Op.Tracks)
				{
					const int32 Values = ValuesFor(Track.Type);
					if (Values == INDEX_NONE || Track.Name.TrimStartAndEnd().IsEmpty() || Tracks.Contains(Track.Name.ToLower()) || Track.Keys.Num() > C::MaxTrackKeys)
					{
						return Fail(Values == INDEX_NONE ? FString::Printf(TEXT("Track type is float, vector, color or event, not '%s'."), *Track.Type)
							: FString::Printf(TEXT("Each track needs its own name and at most %d keys."), C::MaxTrackKeys));
					}
					for (const FHyperAIBlueprintTimelineKey& Key : Track.Keys)
					{
						if (Key.Time < 0.f || Key.Value.Num() < Values)
						{
							return Fail(FString::Printf(TEXT("Track %s: each key has a time from 0 and %d value(s)."), *Track.Name, Values));
						}
					}
					Tracks.Add(Track.Name.ToLower());
				}
				NewNames.Add(Op.Name.ToLower());
			}
			else if (Kind == TEXT("event.add"))
			{
				uint32 Flags = 0;
				if (!ParseRpc(Op.Rpc, Flags) || !IsEventGraph(FindGraph(*Target, Op.Graph)) || NewNames.Contains(Op.Name.ToLower())
					|| !IsNameFree(*Target, Op.Name, Error) || !CheckTypes(Op.Inputs))
				{
					return Fail(!Error.IsEmpty() ? Error : !ParseRpc(Op.Rpc, Flags) ? FString::Printf(TEXT("Rpc is none, server, client or multicast, not '%s'."), *Op.Rpc)
						: NewNames.Contains(Op.Name.ToLower()) ? FString::Printf(TEXT("The plan makes %s twice."), *Op.Name) : TEXT("A custom event goes in an event graph."));
				}
				NewNames.Add(Op.Name.ToLower());
			}
			else if (Kind == TEXT("event.set_rpc") || Kind == TEXT("timeline.remove"))
			{
				const bool bExists = Kind == TEXT("event.set_rpc") ? FindCustomEvent(*Target, Op.Name) != nullptr
					: Target->FindTimelineTemplateByVariableName(FName(*Op.Name.TrimStartAndEnd())) != nullptr;
				uint32 Flags = 0;
				if ((!bExists && !NewNames.Contains(Op.Name.ToLower())) || (Kind == TEXT("event.set_rpc") && !ParseRpc(Op.Rpc, Flags)))
				{
					return Fail(!bExists && !NewNames.Contains(Op.Name.ToLower()) ? FString::Printf(TEXT("%s has no %s %s."), *Target->GetName(),
						Kind == TEXT("event.set_rpc") ? TEXT("custom event") : TEXT("timeline"), *Op.Name) : FString::Printf(TEXT("Rpc is none, server, client or multicast, not '%s'."), *Op.Rpc));
				}
			}
			else if (Kind == TEXT("variable.set_replication"))
			{
				EBlueprintVariableReplication Mode;
				if (FBlueprintEditorUtils::FindNewVariableIndex(Target, FName(*Op.Name.TrimStartAndEnd())) == INDEX_NONE
					|| (!Op.Replication.IsEmpty() && !ParseReplication(Op.Replication, Mode)) || (!Op.Condition.IsEmpty() && !Conditions().Contains(Op.Condition.TrimStartAndEnd().ToLower()))
					|| (Op.Replication.IsEmpty() && Op.Condition.IsEmpty()))
				{
					return Fail(FString::Printf(TEXT("%s needs a variable of %s's own, and Replication (none, replicated, rep_notify) and/or a known Condition."), *Op.Name, *Target->GetName()));
				}
			}
			else if (Kind == TEXT("component.set_replicated"))
			{
				if (!Target->SimpleConstructionScript || !Target->SimpleConstructionScript->FindSCSNode(FName(*Op.Name.TrimStartAndEnd())))
				{
					return Fail(FString::Printf(TEXT("%s adds no component %s."), *Target->GetName(), *Op.Name));
				}
			}
			else if (Kind == TEXT("graph.collapse"))
			{
				const FString Into = Op.Into.TrimStartAndEnd().ToLower();
				if (Op.Nodes.IsEmpty() || (!Into.IsEmpty() && Into != TEXT("collapsed") && Into != TEXT("function") && Into != TEXT("macro"))
					|| (!Op.Name.TrimStartAndEnd().IsEmpty() && (NewNames.Contains(Op.Name.ToLower()) || !IsNameFree(*Target, Op.Name, Error))))
				{
					return Fail(!Error.IsEmpty() ? Error : Op.Nodes.IsEmpty() ? TEXT("Name the Nodes to collapse.") : TEXT("Into is collapsed, function or macro."));
				}
				NewNames.Add(Op.Name.ToLower());
			}
			else if (Kind == TEXT("graph.expand"))
			{
				if (Op.Nodes.Num() != 1)
				{
					return Fail(TEXT("graph.expand opens one node: name it in Nodes."));
				}
			}
			else if (Kind == TEXT("pins.connect"))
			{
				FString Node, Pin;
				if (!SplitNodePin(Op.From, Node, Pin) || !SplitNodePin(Op.To, Node, Pin))
				{
					return Fail(TEXT("From and To are each Node.Pin, such as \"Event BeginPlay.then\" and \"Open.Play\"."));
				}
			}
			else if (Kind != TEXT("interface.implement") && Kind != TEXT("interface.remove"))
			{
				return Fail(FString::Printf(TEXT("Unknown op '%s'."), *Op.Kind));
			}
		}
		return true;
	}

	// -------------------------------------------------------------------------------------------------- compile, save

	FString Compile(UBlueprint& Blueprint, TArray<FHyperAIBlueprintCompileMessage>& OutMessages)
	{
		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(&Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning && Severity != EMessageSeverity::PerformanceWarning)
			{
				continue;
			}
			FHyperAIBlueprintCompileMessage Entry;
			Entry.Blueprint = Blueprint.GetPathName();
			Entry.Severity = Severity == EMessageSeverity::Error ? TEXT("error") : TEXT("warning");
			Entry.Message = Message->ToText().ToString();
			for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
			{
				if (Token->GetType() == EMessageToken::Object)
				{
					if (const UEdGraphNode* Node = Cast<UEdGraphNode>(StaticCastSharedRef<FUObjectToken>(Token)->GetObject().Get()))
					{
						Entry.Node = Node->GetName();
						break;
					}
				}
			}
			OutMessages.Add(MoveTemp(Entry));
		}
		return Blueprint.Status == BS_Error ? TEXT("error") : Blueprint.Status == BS_UpToDateWithWarnings ? TEXT("up_to_date_with_warnings") : TEXT("up_to_date");
	}

	void RecordOutcome(const TCHAR* Tool, const FString& OperationId, const FString& Target, const bool bOk, const FString& Detail)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = Tool;
		Entry.Target = Target;
		Entry.OperationId = OperationId;
		Entry.StatusCode = bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Detail.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	FString NewOperationId(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s-%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower());
	}

	/** The plan for real: one undo step, then every Blueprint it touched compiled (interfaces first) and, when all compile, saved. */
	FHyperAIBlueprintStructurePlanReport Execute(const FHyperAIBlueprintStructurePlanRequest& Request, const FString& OperationId)
	{
		FHyperAIBlueprintStructurePlanReport Report;
		Report.OperationId = OperationId;
		UBlueprint* Target = LoadBlueprint(Request.BlueprintPath);
		Report.BlueprintPath = Target ? Target->GetPathName() : FString();
		TArray<UBlueprint*> Touched;
		TArray<UObject*> TouchedAssets;
		{
			const FScopedTransaction Transaction(LOCTEXT("StructurePlan", "HyperAI: Edit Blueprint Structure"));
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				FString Error;
				if (!ApplyOp(Target, Request.Operations[Index], Touched, TouchedAssets, Report.Made, Report.Notes, Error))
				{
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Request.Operations[Index].Kind, *Error,
						Index > 0 ? TEXT(" The operations before it stay applied; Edit > Undo takes the whole plan back.") : TEXT(""));
					return Report;
				}
				++Report.Applied;
			}
		}
		Touched.StableSort([](const UBlueprint& A, const UBlueprint& B) { return FBlueprintEditorUtils::IsInterfaceBlueprint(&A) && !FBlueprintEditorUtils::IsInterfaceBlueprint(&B); });
		Report.CompileStatus = TEXT("up_to_date");
		for (UBlueprint* Blueprint : Touched)
		{
			const FString Status = Compile(*Blueprint, Report.CompileMessages);
			Report.CompileStatus = Status == TEXT("error") || Report.CompileStatus == TEXT("error") ? TEXT("error")
				: Status == TEXT("up_to_date_with_warnings") ? Status : Report.CompileStatus;
		}
		if (Report.CompileStatus == TEXT("error"))
		{
			Report.Status = TEXT("compile_failed");
			Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied, but the result does not compile (CompileMessages say where); nothing was saved. ")
				TEXT("Fix it with another plan or Epic's Blueprint tools, or Edit > Undo."), Report.Applied);
			return Report;
		}
		if (Request.bSave)
		{
			TArray<UObject*> ToSave(TouchedAssets);
			ToSave.Append(Touched);
			for (UObject* Asset : ToSave)
			{
				FString Error;
				if (!FHyperAIStudioSourceControl::SaveAsset(*Asset, Error))
				{
					Report.Status = TEXT("save_failed");
					Report.Diagnostic = Error;
					return Report;
				}
				Report.Saved.Add(Asset->GetPathName());
			}
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied, compiled%s%s."), Report.Applied,
			Report.CompileStatus == TEXT("up_to_date_with_warnings") ? TEXT(" with warnings") : TEXT(""), Request.bSave ? TEXT(" and saved") : TEXT(", not saved"));
		return Report;
	}

	FString GraphKind(const UBlueprint& Blueprint, const UEdGraph* Graph)
	{
		if (Blueprint.UbergraphPages.Contains(Graph)) return TEXT("event");
		if (Blueprint.FunctionGraphs.Contains(Graph)) return FBlueprintEditorUtils::IsInterfaceBlueprint(&Blueprint) ? TEXT("interface function") : TEXT("function");
		if (Blueprint.MacroGraphs.Contains(Graph)) return TEXT("macro");
		if (Blueprint.DelegateSignatureGraphs.Contains(Graph)) return TEXT("event dispatcher");
		for (const FBPInterfaceDescription& Interface : Blueprint.ImplementedInterfaces)
		{
			if (Interface.Graphs.Contains(Graph)) return TEXT("interface implementation");
		}
		return TEXT("collapsed");
	}
}

FHyperAIBlueprintStructureInspectReport FHyperAIStudioBlueprintStructureContracts::Inspect(const FHyperAIBlueprintStructureInspectRequest& Request)
{
	using namespace HyperAIStudio::BlueprintStructure::Private;
	FHyperAIBlueprintStructureInspectReport Report;
	UBlueprint* Blueprint = LoadBlueprint(Request.BlueprintPath);
	if (!Blueprint)
	{
		if (const UUserDefinedStruct* Struct = LoadStruct(Request.BlueprintPath))
		{
			Report.BlueprintPath = Struct->GetPathName();
			Report.AssetType = TEXT("struct");
			for (const FStructVariableDescription& Field : FStructureEditorUtils::GetVarDesc(Struct))
			{
				Report.Fields.Add(DescribeField(Field));
			}
			Report.CompileStatus = Struct->Status == UDSS_Error ? TEXT("error: ") + Struct->ErrorMessage : TEXT("up_to_date");
			Report.bOk = true;
			return Report;
		}
		if (const UUserDefinedEnum* Enum = LoadEnum(Request.BlueprintPath))
		{
			Report.BlueprintPath = Enum->GetPathName();
			Report.AssetType = TEXT("enum");
			Report.EnumValues = DescribeValues(*Enum);
			Report.bOk = true;
			return Report;
		}
		Report.Diagnostic = FString::Printf(TEXT("%s is not a Blueprint, a struct or an enum."), *Request.BlueprintPath);
		return Report;
	}
	Report.BlueprintPath = Blueprint->GetPathName();
	Report.ParentClass = Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : FString();
	Report.bIsInterface = FBlueprintEditorUtils::IsInterfaceBlueprint(Blueprint);
	Report.AssetType = Report.bIsInterface ? TEXT("interface") : TEXT("blueprint");
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (const UEdGraph* Graph : Graphs)
	{
		if (Graph)
		{
			Report.Graphs.Add(FString::Printf(TEXT("%s (%s, %d nodes)"), *Graph->GetName(), *GraphKind(*Blueprint, Graph), Graph->Nodes.Num()));
		}
	}
	if (Report.bIsInterface)
	{
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			Report.Interfaces.Add(DescribeFunctionGraph(*Graph));
		}
	}
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		if (Interface.Interface)
		{
			Report.Interfaces.Add(DescribeImplemented(*Interface.Interface.Get()));
		}
	}
	for (UTimelineTemplate* Timeline : Blueprint->Timelines)
	{
		if (Timeline)
		{
			Report.Timelines.Add(DescribeTimeline(*Blueprint, *Timeline));
		}
	}
	TArray<UK2Node_CustomEvent*> Events;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
	for (const UK2Node_CustomEvent* Event : Events)
	{
		TArray<FString> Inputs;
		for (const TSharedPtr<FUserPinInfo>& Pin : Event->UserDefinedPins)
		{
			Inputs.Add(FString::Printf(TEXT("%s: %s"), *Pin->PinName.ToString(), *TypeName(Pin->PinType)));
		}
		Report.Events.Add(FString::Printf(TEXT("%s(%s): %s; node %s"), *Event->CustomFunctionName.ToString(), *FString::Join(Inputs, TEXT(", ")),
			*RpcName(Event->FunctionFlags), *Event->GetName()));
	}
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		if (Variable.PropertyFlags & CPF_Net)
		{
			Report.ReplicatedVariables.Add(FString::Printf(TEXT("%s: %s%s, condition %s"), *Variable.VarName.ToString(),
				Variable.RepNotifyFunc.IsNone() ? TEXT("replicated") : TEXT("rep_notify "), Variable.RepNotifyFunc.IsNone() ? TEXT("") : *Variable.RepNotifyFunc.ToString(),
				*ConditionName(Variable.ReplicationCondition)));
		}
	}
	if (Blueprint->SimpleConstructionScript)
	{
		for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentTemplate)
			{
				Report.Components.Add(FString::Printf(TEXT("%s (%s)%s"), *Node->GetVariableName().ToString(), *Node->ComponentTemplate->GetClass()->GetName(),
					Node->ComponentTemplate->GetIsReplicated() ? TEXT(", replicated") : TEXT("")));
			}
		}
	}
	Report.CompileStatus = Blueprint->Status == BS_Error ? TEXT("error") : Blueprint->Status == BS_UpToDateWithWarnings ? TEXT("up_to_date_with_warnings")
		: Blueprint->Status == BS_UpToDate ? TEXT("up_to_date") : Blueprint->Status == BS_Dirty ? TEXT("dirty") : TEXT("not_compiled");
	if (Request.bNodes)
	{
		TArray<UEdGraph*> NodeGraphs;
		if (Request.Graph.TrimStartAndEnd().IsEmpty())
		{
			NodeGraphs = Graphs;
		}
		else if (UEdGraph* Graph = FindGraph(*Blueprint, Request.Graph))
		{
			NodeGraphs.Add(Graph);
		}
		else
		{
			Report.Diagnostic = FString::Printf(TEXT("%s has no graph '%s'."), *Blueprint->GetName(), *Request.Graph);
			return Report;
		}
		// Find keeps the nodes whose name, title or class contains it, so one call finds a node in a large graph.
		const FString Find = Request.Find.TrimStartAndEnd();
		TArray<UEdGraphNode*> Nodes;
		for (const UEdGraph* Graph : NodeGraphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && (Find.IsEmpty() || Node->GetName().Contains(Find) || Title(*Node).Contains(Find)
					|| Node->GetClass()->GetName().Contains(Find)))
				{
					Nodes.Add(Node);
				}
			}
		}
		Report.TotalNodes = Nodes.Num();
		const int32 Start = FMath::Clamp(Request.Offset, 0, Nodes.Num());
		const int32 End = FMath::Min(Nodes.Num(), Start + FMath::Clamp(Request.Limit, 1, 1000));
		for (int32 Index = Start; Index < End; ++Index)
		{
			const UEdGraphNode* Node = Nodes[Index];
			FHyperAIBlueprintNodeInfo& Info = Report.Nodes.AddDefaulted_GetRef();
			Info.Name = Node->GetName();
			Info.Guid = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			Info.Title = Title(*Node);
			Info.Class = Node->GetClass()->GetName();
			Info.Graph = Node->GetGraph()->GetName();
			Info.X = Node->NodePosX;
			Info.Y = Node->NodePosY;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output)
				{
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						Info.Links.Add(FString::Printf(TEXT("%s -> %s.%s"), *Pin->PinName.ToString(), *Linked->GetOwningNode()->GetName(), *Linked->PinName.ToString()));
					}
				}
			}
		}
		Report.NextOffset = End < Nodes.Num() ? End : -1;
	}
	Report.bOk = true;
	return Report;
}

FHyperAIBlueprintStructurePlanReport FHyperAIStudioBlueprintStructureContracts::ApplyPlan(const FHyperAIBlueprintStructurePlanRequest& Request)
{
	using namespace HyperAIStudio::BlueprintStructure::Private;
	FHyperAIBlueprintStructurePlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty() ? NewOperationId(TEXT("bp-structure")) : Request.OperationId;
	UBlueprint* Target = LoadBlueprint(Request.BlueprintPath);
	Report.BlueprintPath = Target ? Target->GetPathName() : Request.BlueprintPath;
	FString Error;
	if (!Check(Request, Target, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked; nothing was changed."), Request.Operations.Num());
		return Report;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("blueprint.structure.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Report.BlueprintPath;
		Summary.Effects.Add(FString::Printf(TEXT("Edit the structure of %s: %d operation(s), one undo step, compiled%s"),
			Report.BlueprintPath.IsEmpty() ? TEXT("Blueprint types (interfaces, structs, enums)") : *Report.BlueprintPath, Request.Operations.Num(),
			Request.bSave ? TEXT(" and saved") : TEXT("")));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAIBlueprintStructureOp& Op = Request.Operations[Index];
			const FString& Asset = Op.Interface.IsEmpty() ? Op.Asset : Op.Interface;
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s%s"), *Op.Kind, *Op.Name, Asset.IsEmpty() ? TEXT("") : *(TEXT(" ") + Asset)));
		}
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				const FHyperAIBlueprintStructurePlanReport Done = Execute(Request, OperationId);
				RecordOutcome(PlanToolName, OperationId, Done.BlueprintPath, Done.bOk, Done.Diagnostic);
			}, Error))
		{
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		return Report;
	}
	return Execute(Request, Report.OperationId);
}

FHyperAIBlueprintStructureInspectReport UHyperAIStudioBlueprintStructureToolset::hyper_blueprint_structure_inspect(const FHyperAIBlueprintStructureInspectRequest& Request)
{
	return FHyperAIStudioBlueprintStructureContracts::Inspect(Request);
}

FHyperAIBlueprintStructurePlanReport UHyperAIStudioBlueprintStructureToolset::hyper_blueprint_structure_apply_plan(const FHyperAIBlueprintStructurePlanRequest& Request)
{
	return FHyperAIStudioBlueprintStructureContracts::ApplyPlan(Request);
}

void FHyperAIStudioBlueprintStructureRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioBlueprintStructureRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioBlueprintStructureRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioBlueprintStructureToolset::StaticClass(),
			FHyperAIStudioBlueprintStructureContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioBlueprintStructure, Warning, TEXT("Could not unregister the Blueprint structure toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioBlueprintStructureRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioBlueprintStructureContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioBlueprintStructure, Verbose, TEXT("The Blueprint structure source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioBlueprintStructureToolset::StaticClass(),
		FHyperAIStudioBlueprintStructureContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioBlueprintStructure, Error, TEXT("Blueprint structure cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
