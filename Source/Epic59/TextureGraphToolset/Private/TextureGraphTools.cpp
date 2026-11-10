// Copyright Epic Games, Inc. All Rights Reserved.

#include "TextureGraphTools.h"

#include "TG_Graph.h"
#include "TG_Pin.h"
#include "TG_OutputSettings.h"
#include "TG_Var.h"
#include "TextureGraphErrorReporter.h"
#include "Expressions/TG_Expression.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "Expressions/Input/TG_Expression_InputParam.h"
#include "Expressions/Input/TG_Expression_Scalar.h"
#include "Expressions/Input/TG_Expression_Color.h"
#include "Expressions/Input/TG_Expression_Vector.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ScopedTransaction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Export/TextureExporter.h"
#include "TG_HelperFunctions.h"
#include "TextureGraphEngine.h"
#include "Job/JobBatch.h"
#include "Model/ModelObject.h"
#include "Model/Mix/MixUpdateCycle.h"
#include "AsyncFunctionResult/AsyncFunctionResultUtils.h"
#include "AsyncFunctionResult/AsyncFunctionResultVoid.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "ToolsetRegistry/ToolsetLibrary59.h" // UE 5.8: the UToolsetLibrary functions 5.9 added
#include "UObject/StrongObjectPtr.h"

#include <exception>

namespace
{
	IAssetTools& GetAssetTools()
	{
		return FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	}

	/**
	 * Resolves an input pin by argument name first (UTG_Node::GetInputPin matches the signature
	 * argument), then falls back to the editable alias (what GetGraph reports) so callers can pass
	 * either form. Aliases are node-unique, so the first alias match is unambiguous.
	 */
	UTG_Pin* FindInputPinByArgOrAlias(const UTG_Node* Node, FName Name)
	{
		if (!Node)
		{
			return nullptr;
		}
		if (UTG_Pin* Pin = Node->GetInputPin(Name))
		{
			return Pin;
		}
		TArray<UTG_Pin*> InputPins;
		Node->GetInputPins(InputPins);
		for (UTG_Pin* Pin : InputPins)
		{
			if (Pin && Pin->GetAliasName() == Name)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** Output-pin counterpart of FindInputPinByArgOrAlias (argument name first, then alias). */
	UTG_Pin* FindOutputPinByArgOrAlias(const UTG_Node* Node, FName Name)
	{
		if (!Node)
		{
			return nullptr;
		}
		if (UTG_Pin* Pin = Node->GetOutputPin(Name))
		{
			return Pin;
		}
		TArray<UTG_Pin*> OutputPins;
		Node->GetOutputPins(OutputPins);
		for (UTG_Pin* Pin : OutputPins)
		{
			if (Pin && Pin->GetAliasName() == Name)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/**
	 * Picks the node's "primary pin" for alias-rename purposes:
	 *   - First non-private output pin if any output pins exist,
	 *   - Otherwise the first non-private input pin (for input-only generators like Scalar/Color).
	 * Returns nullptr if neither is found.
	 */
	UTG_Pin* GetPrimaryPinForAlias(UTG_Node* Node)
	{
		if (!Node)
		{
			return nullptr;
		}
		TArray<UTG_Pin*> Pins;
		Node->GetOutputPins(Pins);
		for (UTG_Pin* Pin : Pins)
		{
			if (Pin && !Pin->IsPrivate())
			{
				return Pin;
			}
		}
		Pins.Reset();
		Node->GetInputPins(Pins);
		for (UTG_Pin* Pin : Pins)
		{
			if (Pin && !Pin->IsPrivate())
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** Returns the title-names of every UTG_Expression_Output in the graph (skipping NAME_None). */
	TArray<FName> CollectOutputTitleNames(UTG_Graph* TGGraph)
	{
		TArray<FName> Names;
		if (!TGGraph)
		{
			return Names;
		}
		TGGraph->ForEachNodes([&](const UTG_Node* Node, uint32 /*Index*/)
			{
				if (!Node || !Node->GetExpression())
				{
					return;
				}
				const UTG_Expression_Output* Out = Cast<UTG_Expression_Output>(Node->GetExpression());
				if (!Out)
				{
					return;
				}
				const FName Title = Out->GetTitleName();
				if (!Title.IsNone())
				{
					Names.Add(Title);
				}
			});
		return Names;
	}

	/**
	 * Appends one pin's info into OutPins. bResolveSource is set for input pins: they carry the current
	 * value and, when connected, their single upstream source (SourceNode + SourcePin) resolved via the
	 * owning graph, so callers never need a separate edge list. Direction is conveyed by which array the
	 * pin lands in (InputPins vs OutputPins). Graph may be null (source resolution is then skipped).
	 */
	void AppendPinInfo(const UTG_Pin* Pin, bool bResolveSource, const UTG_Graph* Graph, TArray<FTextureGraphPinInfo>& OutPins)
	{
		if (!Pin)
		{
			return;
		}
		FTextureGraphPinInfo& Info = OutPins.AddDefaulted_GetRef();
		Info.Name = Pin->GetAliasName();
		Info.ArgumentName = Pin->GetArgumentName();
		Info.CppType = Pin->GetArgumentCPPTypeName().ToString();
		if (bResolveSource)
		{
			Info.Value = Pin->GetEvaluatedVarValue();

			// A TG input pin has at most one incoming edge (its single source). Resolve it so the
			// pin self-describes its connection; output-pin fan-out is recoverable by scanning inputs.
			const TArray<FTG_Id>& Edges = Pin->GetEdges();
			if (Graph && Edges.Num() > 0)
			{
				if (const UTG_Pin* SourcePin = Graph->GetPin(Edges[0]))
				{
					Info.SourceNode = SourcePin->GetNodePtr();
					Info.SourcePin = SourcePin->GetAliasName();
				}
			}
		}
	}

	/**
	 * Builds one node's FTextureGraphNodeInfo (reference, name, expression class, editor position, and
	 * pins split by direction) and appends it to Out. Shared by GetGraph and the List* convenience
	 * filters so they report nodes identically.
	 */
	void AppendNodeInfo(UTG_Graph* Graph, const UTG_Node* Node, TArray<FTextureGraphNodeInfo>& Out)
	{
		if (!Node)
		{
			return;
		}
		FTextureGraphNodeInfo& NodeInfo = Out.AddDefaulted_GetRef();
		NodeInfo.Node = Graph ? Graph->GetNode(Node->GetId()) : nullptr;
		NodeInfo.NodeName = Node->GetNodeName().ToString();
		if (const UTG_Expression* Expr = Node->GetExpression())
		{
			NodeInfo.ExpressionClass = Expr->GetClass();
		}
#if WITH_EDITORONLY_DATA
		NodeInfo.PosX = Node->EditorData.PosX;
		NodeInfo.PosY = Node->EditorData.PosY;
#endif

		TArray<const UTG_Pin*> InputPins;
		TArray<const UTG_Pin*> OutputPins;
		Node->GetInputPins(InputPins);
		Node->GetOutputPins(OutputPins);
		for (const UTG_Pin* Pin : InputPins)  { AppendPinInfo(Pin, /*bResolveSource=*/true,  Graph, NodeInfo.InputPins); }
		for (const UTG_Pin* Pin : OutputPins) { AppendPinInfo(Pin, /*bResolveSource=*/false, Graph, NodeInfo.OutputPins); }
	}

	/**
	 * Chooses the inline-constant expression class for a Variant pin value, inferred from the value's
	 * string form: a color literal ("(R=..,G=..,B=..)") -> Color, a vector literal ("(X=..,Y=..,Z=..)")
	 * -> Vector, an otherwise-numeric string -> Scalar. OutValueArg receives the class's editable value
	 * pin argument name. Returns false when the value matches none of these.
	 */
	bool InferConstantClass(const FString& Value, UClass*& OutClass, FName& OutValueArg)
	{
		const bool bLooksLikeColor = Value.Contains(TEXT("R="), ESearchCase::IgnoreCase)
			&& Value.Contains(TEXT("G="), ESearchCase::IgnoreCase)
			&& Value.Contains(TEXT("B="), ESearchCase::IgnoreCase);
		if (bLooksLikeColor)
		{
			OutClass = UTG_Expression_Color::StaticClass();
			OutValueArg = FName(TEXT("Color"));
			return true;
		}
		const bool bLooksLikeVector = Value.Contains(TEXT("X="), ESearchCase::IgnoreCase)
			&& Value.Contains(TEXT("Y="), ESearchCase::IgnoreCase)
			&& Value.Contains(TEXT("Z="), ESearchCase::IgnoreCase);
		if (bLooksLikeVector)
		{
			OutClass = UTG_Expression_Vector::StaticClass();
			OutValueArg = FName(TEXT("Vector"));
			return true;
		}
		if (Value.IsNumeric())
		{
			OutClass = UTG_Expression_Scalar::StaticClass();
			OutValueArg = FName(TEXT("Scalar"));
			return true;
		}
		return false;
	}

	/**
	 * Shared body of RemoveInputParam/RemoveOutput: resolves the graph param named Name to its owning
	 * node, verifies that node's expression is RequiredExprClass, and removes it. ToolName drives the
	 * error text. Returns false (raising) on any failure.
	 */
	bool RemoveParamNodeOfType(UTextureGraph* Graph, FName Name, UClass* RequiredExprClass, const TCHAR* ToolName)
	{
		if (!Graph || Name.IsNone())
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s: Graph must not be null and the name must not be NAME_None."), ToolName));
			return false;
		}
		UTG_Graph* TGGraph = Graph->TextureGraph;
		if (!TGGraph)
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s: Graph has no internal UTG_Graph."), ToolName));
			return false;
		}

		const FTG_Id PinId = TGGraph->FindParamPinId(Name);
		UTG_Pin* Pin = PinId.IsValid() ? TGGraph->GetPin(PinId) : nullptr;
		UTG_Node* Owner = Pin ? Pin->GetNodePtr() : nullptr;
		if (!Owner || !Owner->GetExpression() || !Owner->GetExpression()->IsA(RequiredExprClass))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s: no matching entry named '%s'."), ToolName, *Name.ToString()));
			return false;
		}

		FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "RemoveParamNode", "Remove TextureGraph Node"));
		TGGraph->Modify();
		TGGraph->RemoveNode(Owner);
		return true;
	}

	class FTextureGraphIssueReporter : public FTextureGraphErrorReporter
	{
	public:
		virtual FTextureGraphErrorReport ReportLog(int32 ErrorId, const FString& ErrorMsg, UObject* ReferenceObj = nullptr) override;
		virtual FTextureGraphErrorReport ReportWarning(int32 ErrorId, const FString& ErrorMsg, UObject* ReferenceObj = nullptr) override;
		virtual FTextureGraphErrorReport ReportError(int32 ErrorId, const FString& ErrorMsg, UObject* ReferenceObj = nullptr) override;

		/** Issues collected across all Report* calls, in report order. */
		const TArray<FTextureGraphIssue>& GetIssues() const { return Issues; }
	private:
		FTextureGraphErrorReport Record(int32 ErrorId, const FString& ErrorMsg, UObject* ReferenceObj, const TCHAR* Severity);

		TArray<FTextureGraphIssue> Issues;
	};

	FTextureGraphErrorReport FTextureGraphIssueReporter::Record(int32 ErrorId, const FString& ErrorMsg, UObject* ReferenceObj, const TCHAR* Severity)
	{
		FTextureGraphErrorReport Report{ ErrorId, ErrorMsg, ReferenceObj };
		FTextureGraphIssue Issue;

		Issue.Severity = Severity;

		switch (static_cast<ETextureGraphErrorType>(ErrorId))
		{
			case ETextureGraphErrorType::UNSUPPORTED_TYPE:
				Issue.Category = TEXT("Unsupported Type");
				break;

			case ETextureGraphErrorType::MISSING_REQUIRED_INPUT:
				Issue.Category = TEXT("Missing Required Input");
				break;

			case ETextureGraphErrorType::RECURSIVE_CALL:
				Issue.Category = TEXT("Recursive Call");
				break;

			case ETextureGraphErrorType::UNSUPPORTED_MATERIAL:
				Issue.Category = TEXT("Unsupported Material");
				break;

			case ETextureGraphErrorType::SUBGRAPH_INTERNAL_ERROR:
				Issue.Category = TEXT("Subgraph Internal Error");
				break;

			case ETextureGraphErrorType::NODE_WARNING:
				Issue.Category = TEXT("Node Warning");
				break;

			case ETextureGraphErrorType::INPUT_ARRAY_WARNING:
				Issue.Category = TEXT("Input Array Warning");
				break;

			default:
				Issue.Category = TEXT("Unknown");
				break;
		}

		Issue.Message = ErrorMsg;

		if (UTG_Node* NodeObj = Cast<UTG_Node>(ReferenceObj))
		{
			Issue.Node = NodeObj;
		}

		Issues.Add(Issue);

		return Report;
	}
	FTextureGraphErrorReport FTextureGraphIssueReporter::ReportLog(int32 ErrorId, const FString& ErrorMsg, UObject* ReferenceObj /*= nullptr*/)
	{
		return Record(ErrorId, ErrorMsg, ReferenceObj, TEXT("Info"));
	}
	FTextureGraphErrorReport FTextureGraphIssueReporter::ReportWarning(int32 ErrorId, const FString& ErrorMsg, UObject* ReferenceObj /*= nullptr*/)
	{
		return Record(ErrorId, ErrorMsg, ReferenceObj, TEXT("Warning"));
	}
	FTextureGraphErrorReport FTextureGraphIssueReporter::ReportError(int32 ErrorId, const FString& ErrorMsg, UObject* ReferenceObj /*= nullptr*/)
	{
		return Record(ErrorId, ErrorMsg, ReferenceObj, TEXT("Error"));
	}
}

// -------------------------------------------------------------------------
// Asset creation
// -------------------------------------------------------------------------

UTextureGraph* UTextureGraphTools::CreateTextureGraph(const FString& FolderPath, const FString& AssetName)
{
	if (FolderPath.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CreateTextureGraph: FolderPath must not be empty."));
		return nullptr;
	}
	if (AssetName.IsEmpty())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CreateTextureGraph: AssetName must not be empty."));
		return nullptr;
	}

	if (!FToolsetLibrary59::ValidateAssetPath(FolderPath)) // UE 5.8: 5.9 UToolsetLibrary addition
	{
		return nullptr;
	}

	FString PackageName;
	FString UniqueAssetName;
	GetAssetTools().CreateUniqueAssetName(FolderPath / AssetName, TEXT(""), PackageName, UniqueAssetName);

	UPackage* Package = CreatePackage(*PackageName);
	UTextureGraph* TextureGraph = NewObject<UTextureGraph>(Package, UTextureGraph::StaticClass(), *UniqueAssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!TextureGraph)
	{
		return nullptr;
	}
	TextureGraph->Construct(UniqueAssetName);
	FAssetRegistryModule::AssetCreated(TextureGraph);
	Package->MarkPackageDirty();
	return TextureGraph;
}

// -------------------------------------------------------------------------
// Node type discovery
// -------------------------------------------------------------------------

TArray<TSubclassOf<UTG_Expression>> UTextureGraphTools::ListNodeTypes()
{
	TArray<TSubclassOf<UTG_Expression>> Results;
	UClass* ExprBaseClass = UTG_Expression::StaticClass();
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->IsChildOf(ExprBaseClass) && !Class->HasAnyClassFlags(CLASS_Abstract))
		{
			Results.Add(Class);
		}
	}
	// Sort by class path for stable, deterministic output.
	Results.Sort([](const TSubclassOf<UTG_Expression>& A, const TSubclassOf<UTG_Expression>& B)
		{
			return A.Get()->GetPathName() < B.Get()->GetPathName();
		});
	return Results;
}

// -------------------------------------------------------------------------
// Node editing
// -------------------------------------------------------------------------

UTG_Node* UTextureGraphTools::AddNode(UTextureGraph* Graph, TSubclassOf<UTG_Expression> ExpressionClass, int32 PosX, int32 PosY)
{
	if (!Graph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddNode: Graph must not be null."));
		return nullptr;
	}
	if (!ExpressionClass)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddNode: ExpressionClass must not be null."));
		return nullptr;
	}
	if (ExpressionClass->HasAnyClassFlags(CLASS_Abstract))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("AddNode: ExpressionClass '%s' is abstract and cannot be instantiated."), *ExpressionClass->GetName()));
		return nullptr;
	}
	// Input parameters and outputs are exposed on the graph as named parameters and have their own
	// uniqueness rules, so they are created through the dedicated input-parameter and output tools
	// rather than this generic add.
	if (ExpressionClass->IsChildOf(UTG_Expression_InputParam::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("AddNode: adding input-parameter expressions ('%s') is unsupported by this function; use the dedicated tool for adding input parameters."), *ExpressionClass->GetName()));
		return nullptr;
	}
	if (ExpressionClass->IsChildOf(UTG_Expression_Output::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("AddNode: adding Output expressions ('%s') is unsupported by this function; use the dedicated tool for adding outputs."), *ExpressionClass->GetName()));
		return nullptr;
	}

	UTG_Graph* TGGraph = Graph->TextureGraph;
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddNode: Graph has no internal UTG_Graph (was it constructed?)."));
		return nullptr;
	}

	FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "AddNode", "Add TextureGraph Node"));
	TGGraph->Modify();
	UTG_Node* NewNode = TGGraph->CreateExpressionNode(ExpressionClass);
#if WITH_EDITORONLY_DATA
	if (NewNode)
	{
		NewNode->Modify();
		NewNode->EditorData.PosX = PosX;
		NewNode->EditorData.PosY = PosY;
	}
#endif
	return NewNode;
}

UTG_Node* UTextureGraphTools::AddInputParam(UTextureGraph* Graph, TSubclassOf<UTG_Expression> ExpressionClass, FName ParamName, int32 PosX, int32 PosY)
{
	if (!Graph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddInputParam: Graph must not be null."));
		return nullptr;
	}
	if (!ExpressionClass)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddInputParam: ExpressionClass must not be null."));
		return nullptr;
	}
	if (ExpressionClass->HasAnyClassFlags(CLASS_Abstract))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("AddInputParam: ExpressionClass '%s' is abstract and cannot be instantiated."), *ExpressionClass->GetName()));
		return nullptr;
	}
	if (!ExpressionClass->IsChildOf(UTG_Expression_InputParam::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("AddInputParam: ExpressionClass '%s' is not an input-parameter expression."), *ExpressionClass->GetName()));
		return nullptr;
	}

	UTG_Graph* TGGraph = Graph->TextureGraph;
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddInputParam: Graph has no internal UTG_Graph (was it constructed?)."));
		return nullptr;
	}

	FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "AddInputParam", "Add TextureGraph Input Parameter"));
	TGGraph->Modify();
	UTG_Node* NewNode = TGGraph->CreateExpressionNode(ExpressionClass);
	if (NewNode)
	{
		NewNode->Modify();
#if WITH_EDITORONLY_DATA
		NewNode->EditorData.PosX = PosX;
		NewNode->EditorData.PosY = PosY;
#endif
		// SetTitleName renames the parameter's value pin; the base graph auto-uniquifies the name
		// against existing parameters (ValidateGeneratePinAliasName + RenameParam).
		if (!ParamName.IsNone())
		{
			if (UTG_Expression_InputParam* Expr = Cast<UTG_Expression_InputParam>(NewNode->GetExpression()))
			{
				Expr->Modify();
				Expr->SetTitleName(ParamName);
			}
		}
	}
	return NewNode;
}

bool UTextureGraphTools::RemoveInputParam(UTextureGraph* Graph, FName ParamName)
{
	return RemoveParamNodeOfType(Graph, ParamName, UTG_Expression_InputParam::StaticClass(), TEXT("RemoveInputParam"));
}

UTG_Node* UTextureGraphTools::AddOutput(UTextureGraph* Graph, FName OutputName, int32 PosX, int32 PosY)
{
	if (!Graph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddOutput: Graph must not be null."));
		return nullptr;
	}
	UTG_Graph* TGGraph = Graph->TextureGraph;
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddOutput: Graph has no internal UTG_Graph (was it constructed?)."));
		return nullptr;
	}

	FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "AddOutput", "Add TextureGraph Output"));
	TGGraph->Modify();
	UTG_Node* NewNode = TGGraph->CreateExpressionNode(UTG_Expression_Output::StaticClass());
	if (NewNode)
	{
		NewNode->Modify();
#if WITH_EDITORONLY_DATA
		NewNode->EditorData.PosX = PosX;
		NewNode->EditorData.PosY = PosY;
#endif
		// SetTitleName renames the Output pin and keeps OutputSettings.OutputName (the export
		// filename) in sync; the base graph auto-uniquifies the name against existing outputs.
		if (!OutputName.IsNone())
		{
			if (UTG_Expression_Output* Out = Cast<UTG_Expression_Output>(NewNode->GetExpression()))
			{
				Out->Modify();
				Out->SetTitleName(OutputName);
			}
		}
	}
	return NewNode;
}

bool UTextureGraphTools::RemoveOutput(UTextureGraph* Graph, FName OutputName)
{
	return RemoveParamNodeOfType(Graph, OutputName, UTG_Expression_Output::StaticClass(), TEXT("RemoveOutput"));
}

bool UTextureGraphTools::RemoveNode(UTextureGraph* Graph, UTG_Node* Node)
{
	if (!Graph || !Node)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveNode: Graph and Node must not be null."));
		return false;
	}
	UTG_Graph* TGGraph = Graph->TextureGraph;
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveNode: Graph has no internal UTG_Graph."));
		return false;
	}
	if (Node->GetGraph() != TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveNode: Node does not belong to the given graph."));
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "RemoveNode", "Remove TextureGraph Node"));
	TGGraph->Modify();
	TGGraph->RemoveNode(Node);
	return true;
}

bool UTextureGraphTools::SetPinValue(UTG_Node* Node, FName PinName, const FString& Value)
{
	if (!Node || PinName.IsNone())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetPinValue: Node must not be null and PinName must not be NAME_None."));
		return false;
	}

	UTG_Pin* Pin = FindInputPinByArgOrAlias(Node, PinName);
	if (!Pin)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("SetPinValue: input pin '%s' not found on node '%s'."), *PinName.ToString(), *Node->GetNodeName().ToString()));
		return false;
	}

	// Ordinary pins take a string value directly. Re-setting a pin to the value it already holds is an
	// idempotent success (UTG_Pin::FromString returns void, so there is nothing to check).
	const bool bIsVariant = Pin->GetArgumentCPPTypeName() == FName(TEXT("FTG_Variant"));
	if (!bIsVariant)
	{
		FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "SetPinValue", "Set TextureGraph Pin Value"));
		Node->Modify();
		Pin->Modify();
		Pin->FromString(Value, /*bIsTweaking=*/false);
		return true;
	}

	// A polymorphic FTG_Variant pin is connection-driven: it has no working string setter (the base
	// TG_Var_SetValueFromString<FTG_Variant> specialization is unimplemented), so it takes its value
	// from a connected node. Create an inline constant node matching the value and connect it.
	UTG_Graph* TGGraph = Node->GetGraph();
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetPinValue: node has no internal UTG_Graph."));
		return false;
	}

	UClass* ConstClass = nullptr;
	FName ValueArg;
	if (!InferConstantClass(Value, ConstClass, ValueArg))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("SetPinValue: could not interpret '%s' as a scalar, color, or vector for Variant pin '%s'."),
			*Value, *PinName.ToString()));
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "SetPinValueVariant", "Set TextureGraph Variant Pin Value"));
	TGGraph->Modify();
	Node->Modify();
	Pin->Modify();

	// Capture any current source so an inline constant this call replaces can be cleaned up below.
	UTG_Node* OldSource = nullptr;
	{
		const TArray<FTG_Id>& Edges = Pin->GetEdges();
		if (Edges.Num() > 0)
		{
			if (const UTG_Pin* SourcePin = TGGraph->GetPin(Edges[0]))
			{
				OldSource = SourcePin->GetNodePtr();
			}
		}
	}

	// Fast path: if the pin is already fed by an inline constant of the same type, update that
	// constant's value in place rather than creating a replacement. This avoids node churn and, because
	// the source type is unchanged, avoids retyping the downstream variant chain (a type change would
	// regenerate pins and drop their connections).
	if (OldSource && OldSource->GetExpression() && OldSource->GetExpression()->GetClass() == ConstClass)
	{
		if (const UTG_Expression_InputParam* OldExpr = Cast<UTG_Expression_InputParam>(OldSource->GetExpression()))
		{
			if (OldExpr->bIsConstant)
			{
				if (UTG_Pin* ValuePin = OldSource->GetInputPin(ValueArg))
				{
					OldSource->Modify();
					ValuePin->Modify();
					ValuePin->FromString(Value, /*bIsTweaking=*/false);
					return true;
				}
			}
		}
	}

	UTG_Node* ConstNode = TGGraph->CreateExpressionNode(ConstClass);
	if (!ConstNode)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetPinValue: failed to create a constant node for the Variant value."));
		return false;
	}
	ConstNode->Modify();

	// Make it an inline constant: un-params and disconnects the value pin so it holds the value
	// directly rather than being exposed as a second graph parameter. The ValueOut output is
	// unaffected, so it can still feed the target pin. The signature is rebuilt here, so the value pin
	// must be re-fetched afterwards.
	if (UTG_Expression_InputParam* ConstExpr = Cast<UTG_Expression_InputParam>(ConstNode->GetExpression()))
	{
		ConstExpr->Modify();
		ConstExpr->SetbIsConstant(true);
	}
	if (UTG_Pin* ValuePin = ConstNode->GetInputPin(ValueArg))
	{
		ValuePin->Modify();
		ValuePin->FromString(Value, /*bIsTweaking=*/false);
	}

#if WITH_EDITORONLY_DATA
	// Place the constant just left of the node it feeds.
	ConstNode->EditorData.PosX = Node->EditorData.PosX - 250;
	ConstNode->EditorData.PosY = Node->EditorData.PosY;
#endif

	FTG_Name FromArg(TEXT("ValueOut"));
	FTG_Name ToArg = Pin->GetArgumentName();
	if (!TGGraph->Connect(*ConstNode, FromArg, *Node, ToArg))
	{
		TGGraph->RemoveNode(ConstNode);
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("SetPinValue: could not connect the constant to Variant pin '%s'."), *PinName.ToString()));
		return false;
	}

	// Garbage-collect the replaced source only if it was itself an auto-created inline constant now
	// feeding nothing. User-authored sources (and constants still feeding other pins) are left in place.
	if (OldSource && OldSource != ConstNode)
	{
		const UTG_Expression_InputParam* OldExpr = Cast<UTG_Expression_InputParam>(OldSource->GetExpression());
		if (OldExpr && OldExpr->bIsConstant)
		{
			bool bStillFeeds = false;
			TArray<UTG_Pin*> OldOutputs;
			OldSource->GetOutputPins(OldOutputs);
			for (const UTG_Pin* OutPin : OldOutputs)
			{
				if (OutPin && OutPin->GetEdges().Num() > 0)
				{
					bStillFeeds = true;
					break;
				}
			}
			if (!bStillFeeds)
			{
				TGGraph->RemoveNode(OldSource);
			}
		}
	}
	return true;
}

// -------------------------------------------------------------------------
// Pin connections
// -------------------------------------------------------------------------

bool UTextureGraphTools::ConnectPins(UTextureGraph* Graph, UTG_Node* FromNode, FName FromPin, UTG_Node* ToNode, FName ToPin)
{
	if (!Graph || !FromNode || !ToNode)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("ConnectPins: Graph, FromNode, and ToNode must not be null."));
		return false;
	}
	UTG_Graph* TGGraph = Graph->TextureGraph;
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("ConnectPins: Graph has no internal UTG_Graph."));
		return false;
	}

	// Resolve both pins by argument name or editable alias so aliases reported by the graph read
	// round-trip back in.
	const UTG_Pin* PinFrom = FindOutputPinByArgOrAlias(FromNode, FromPin);
	if (!PinFrom)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("ConnectPins: output pin '%s' not found on node '%s'."), *FromPin.ToString(), *FromNode->GetNodeName().ToString()));
		return false;
	}
	const UTG_Pin* PinTo = FindInputPinByArgOrAlias(ToNode, ToPin);
	if (!PinTo)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("ConnectPins: input pin '%s' not found on node '%s'."), *ToPin.ToString(), *ToNode->GetNodeName().ToString()));
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "ConnectPins", "Connect TextureGraph Pins"));
	TGGraph->Modify();
	// UTG_Graph::Connect takes FName& (non-const ref) and resolves by argument name.
	FTG_Name FromArg = PinFrom->GetArgumentName();
	FTG_Name ToArg = PinTo->GetArgumentName();
	if (!TGGraph->Connect(*FromNode, FromArg, *ToNode, ToArg))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("ConnectPins: could not connect '%s' -> '%s'; the pins are incompatible or the connection would form a cycle."), *FromPin.ToString(), *ToPin.ToString()));
		return false;
	}
	return true;
}

bool UTextureGraphTools::DisconnectPins(UTextureGraph* Graph, UTG_Node* FromNode, FName FromPin, UTG_Node* ToNode, FName ToPin)
{
	if (!Graph || !FromNode || !ToNode)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("DisconnectPins: Graph, FromNode, and ToNode must not be null."));
		return false;
	}
	UTG_Graph* TGGraph = Graph->TextureGraph;
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("DisconnectPins: Graph has no internal UTG_Graph."));
		return false;
	}

	// Resolve by argument name or editable alias so aliases reported by the graph read round-trip
	// back in, matching ConnectPins/SetPinValue.
	const UTG_Pin* PinFrom = FindOutputPinByArgOrAlias(FromNode, FromPin);
	if (!PinFrom)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("DisconnectPins: output pin '%s' not found on node '%s'."), *FromPin.ToString(), *FromNode->GetNodeName().ToString()));
		return false;
	}
	const UTG_Pin* PinTo = FindInputPinByArgOrAlias(ToNode, ToPin);
	if (!PinTo)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("DisconnectPins: input pin '%s' not found on node '%s'."), *ToPin.ToString(), *ToNode->GetNodeName().ToString()));
		return false;
	}

	// Both pins resolved. If they share no edge there is nothing to remove: a non-error no-op (the
	// desired end state, an absent edge, already holds), so return false without raising. RemoveEdge
	// itself returns void and silently no-ops, so the edge check both guards the return contract and
	// drives RemoveEdge only when there is really an edge to drop.
	if (!PinTo->GetEdges().Contains(PinFrom->GetId()))
	{
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "DisconnectPins", "Disconnect TextureGraph Pins"));
	TGGraph->Modify();
	FTG_Name FromArg = PinFrom->GetArgumentName();
	FTG_Name ToArg = PinTo->GetArgumentName();
	TGGraph->RemoveEdge(*FromNode, FromArg, *ToNode, ToArg);
	return true;
}

// -------------------------------------------------------------------------
// Node alias and output-settings convenience setters
// -------------------------------------------------------------------------

bool UTextureGraphTools::SetNodeAlias(UTG_Node* Node, FName NewAlias)
{
	if (!Node)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetNodeAlias: Node must not be null."));
		return false;
	}
	if (NewAlias.IsNone())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetNodeAlias: NewAlias must not be NAME_None."));
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "SetNodeAlias", "Rename TextureGraph Node"));
	Node->Modify();

	// Output nodes have their own SetTitleName that renames the pin AND keeps
	// OutputSettings.OutputName in sync (so the bake's export filename matches
	// the new node display name). Always use it for Output nodes.
	if (UTG_Expression_Output* Out = Cast<UTG_Expression_Output>(Node->GetExpression()))
	{
		Out->Modify();
		Out->SetTitleName(NewAlias);
		return true;
	}

	UTG_Pin* PrimaryPin = GetPrimaryPinForAlias(Node);
	if (!PrimaryPin)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("SetNodeAlias: node '%s' has no settable primary pin."), *Node->GetNodeName().ToString()));
		return false;
	}
	PrimaryPin->Modify();
	PrimaryPin->SetAliasName(NewAlias);
	return true;
}

bool UTextureGraphTools::SetOutputSettings(UTG_Node* OutputNode, FName BaseName, FName FolderPath, const ETG_TexturePresetType PresetType)
{
	if (!OutputNode)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetOutputSettings: OutputNode must not be null."));
		return false;
	}
	UTG_Expression_Output* Expr = Cast<UTG_Expression_Output>(OutputNode->GetExpression());
	if (!Expr)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("SetOutputSettings: node '%s' is not an Output expression."), *OutputNode->GetNodeName().ToString()));
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("TextureGraphToolset", "SetOutputSettings", "Configure TextureGraph Output"));
	OutputNode->Modify();
	Expr->Modify();

	Expr->OutputSettings.BaseName = BaseName;
	Expr->OutputSettings.FolderPath = FolderPath;
	// OnSetTexturePresetType cascades LODGroup/Compression/bSRGB but doesn't update the
	// TexturePresetType field itself; set it explicitly so reads reflect the chosen preset.
	Expr->OutputSettings.TexturePresetType = PresetType;
	Expr->OutputSettings.OnSetTexturePresetType(PresetType);

	// Keep node display name + OutputName aligned with BaseName. SetTitleName writes
	// OutputSettings.OutputName from the (possibly auto-uniquified) output-pin alias, so it must run
	// before the pin sync below or OutputName is captured stale.
	if (!BaseName.IsNone())
	{
		Expr->SetTitleName(BaseName);
	}

	// Push the fully-updated struct back into the OutputSettings pin. The base plugin treats the pin
	// as the authoritative store (InitializeOutputSettings and SetShouldExport both sync it the same
	// way); a graph evaluation reconstructs OutputSettings from the pin, so without this a subsequent
	// eval/bake clobbers our writes back to the pin's stale defaults. This bites added Output nodes in
	// particular, which never ran InitializeOutputSettings.
	if (UTG_Pin* SettingsPin = OutputNode->GetPin(GET_MEMBER_NAME_CHECKED(UTG_Expression_Output, OutputSettings)))
	{
		SettingsPin->FromString(Expr->OutputSettings.ToString());
	}
	return true;
}

// -------------------------------------------------------------------------
// Evaluation and export
// -------------------------------------------------------------------------

UAsyncFunctionResultVoid* UTextureGraphTools::ExportTextureGraph(UTextureGraph* Graph, const FString& OutputFolder)
{
	using namespace UE::AsyncFunctionResult;

	if (!Graph)
	{
		return MakeErrorResult<UAsyncFunctionResultVoid>(TEXT("Graph must not be null."));
	}
	if (OutputFolder.IsEmpty())
	{
		return MakeErrorResult<UAsyncFunctionResultVoid>(TEXT("OutputFolder must not be empty."));
	}
	if (!Graph->TextureGraph)
	{
		return MakeErrorResult<UAsyncFunctionResultVoid>(TEXT("Graph has no internal UTG_Graph (was it constructed?)."));
	}

	// The TG editor calls InitTargets on asset open; without it the mix has no targets registered
	// and no error reporter, so the first tick trips assertions inside MixManager.
	FTG_HelperFunctions::InitTargets(Graph);

	const int32 ExpectedCount = CollectOutputTitleNames(Graph->TextureGraph).Num();

	UAsyncFunctionResultVoid* Result = NewObject<UAsyncFunctionResultVoid>();
	TStrongObjectPtr<UAsyncFunctionResultVoid> ResultPin(Result);
	TStrongObjectPtr<UTextureGraph> GraphPin(Graph);

	FExportSettings Settings;
	Settings.PresetName = Graph->GetName();
	FTG_HelperFunctions::ExportAsync(Graph, OutputFolder, FString(), Settings,
		/*OverrideExportPath=*/true,
		/*OverwriteTextures=*/true,
		/*ExportAllOutputs=*/true,
		/*bSave=*/true)
		.then([ResultPin, GraphPin, ExpectedCount](int32 MapsExported) mutable
			{
				if (MapsExported < ExpectedCount)
				{
					ResultPin->SetError(FString::Printf(TEXT("Export incomplete: %d of %d maps exported."), MapsExported, ExpectedCount));
				}
				else
				{
					ResultPin->SetCompleted();
				}
				ResultPin.Reset();
				GraphPin.Reset();
			})
		// then() only observes the value channel. Without this the result would never resolve on a
		// rejected export (TextureGraphEngine rejects via set_exception in several places), leaving
		// the async call pending forever and the captured pins holding both objects alive.
		.fail([ResultPin, GraphPin](std::exception_ptr Error) mutable
			{
				FString Message = TEXT("Export failed.");
				try
				{
					if (Error)
					{
						std::rethrow_exception(Error);
					}
				}
				catch (const std::exception& Ex)
				{
					Message = FString::Printf(TEXT("Export failed: %s"), UTF8_TO_TCHAR(Ex.what()));
				}
				catch (...)
				{
				}

				ResultPin->SetError(Message);
				ResultPin.Reset();
				GraphPin.Reset();
			});

	return Result;
}

// -------------------------------------------------------------------------
// Whole-graph read
// -------------------------------------------------------------------------

TArray<FTextureGraphNodeInfo> UTextureGraphTools::GetGraph(UTextureGraph* Graph)
{
	TArray<FTextureGraphNodeInfo> Result;
	if (!Graph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetGraph: Graph must not be null."));
		return Result;
	}
	UTG_Graph* TGGraph = Graph->TextureGraph;
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetGraph: Graph has no internal UTG_Graph."));
		return Result;
	}

	// Every node is reported the same way: input parameters and outputs are ordinary nodes here,
	// identified by their expression class (a UTG_Expression_InputParam subclass, or
	// UTG_Expression_Output). Connectivity lives on each input pin's resolved source.
	TGGraph->ForEachNodes([&Result, TGGraph](const UTG_Node* Node, uint32 /*Index*/)
		{
			AppendNodeInfo(TGGraph, Node, Result);
		});

	return Result;
}

TArray<FTextureGraphNodeInfo> UTextureGraphTools::ListInputParams(UTextureGraph* Graph)
{
	TArray<FTextureGraphNodeInfo> Result;
	if (!Graph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("ListInputParams: Graph must not be null."));
		return Result;
	}
	UTG_Graph* TGGraph = Graph->TextureGraph;
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("ListInputParams: Graph has no internal UTG_Graph."));
		return Result;
	}

	TGGraph->ForEachNodes([&Result, TGGraph](const UTG_Node* Node, uint32 /*Index*/)
		{
			if (Node && Node->GetExpression() && Node->GetExpression()->IsA(UTG_Expression_InputParam::StaticClass()))
			{
				AppendNodeInfo(TGGraph, Node, Result);
			}
		});

	return Result;
}

TArray<FTextureGraphNodeInfo> UTextureGraphTools::ListOutputs(UTextureGraph* Graph)
{
	TArray<FTextureGraphNodeInfo> Result;
	if (!Graph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("ListOutputs: Graph must not be null."));
		return Result;
	}
	UTG_Graph* TGGraph = Graph->TextureGraph;
	if (!TGGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("ListOutputs: Graph has no internal UTG_Graph."));
		return Result;
	}

	TGGraph->ForEachNodes([&Result, TGGraph](const UTG_Node* Node, uint32 /*Index*/)
		{
			if (Node && Node->GetExpression() && Node->GetExpression()->IsA(UTG_Expression_Output::StaticClass()))
			{
				AppendNodeInfo(TGGraph, Node, Result);
			}
		});

	return Result;
}

TArray<FTextureGraphIssue> UTextureGraphTools::GetGraphIssues(UTextureGraph* Graph)
{
	TArray<FTextureGraphIssue> Result;
	if (!Graph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetGraphIssues: Graph must not be null."));
		return Result;
	}
	if (!Graph->TextureGraph)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetGraphIssues: Graph has no internal UTG_Graph."));
		return Result;
	}

	// InitTargets makes the graph eval-ready and registers a reporter slot for this mix, which also
	// guarantees GetErrorReporter cannot crash on a missing key.
	FTG_HelperFunctions::InitTargets(Graph);

	// Install our collecting reporter so validation reports land where we can read them back.
	std::shared_ptr<FTextureGraphIssueReporter> Reporter = std::make_shared<FTextureGraphIssueReporter>();
	TextureGraphEngine::RegisterErrorReporter(Graph, Reporter);

	// Build an "invalidate everything" update cycle but never submit its batch to the scheduler:
	// UTextureGraphBase::Update runs Graph->Validate(Cycle) (reporting into our reporter) before any
	// Evaluate, and without AddBatch no GPU/bake work is scheduled.
	FInvalidationDetails Details;
	Details.All();
	Details.Mix = Graph;
	JobBatchPtr Batch = JobBatch::Create(Details);
	MixUpdateCyclePtr Cycle = Batch->GetCycle();
#if UE59_COMPAT // validate only (UTextureGraphBase::Update would also evaluate, which can assert on a null texture)
	Cycle->AddTarget(std::make_shared<MixTargetUpdate>(Cycle->GetMix(), 0));
	Cycle->PushMix(Graph);
	Graph->TextureGraph->Validate(Cycle);
	Cycle->PopMix();
#else
	Graph->Update(Cycle);
#endif

	Result = Reporter->GetIssues();

	// Leave a plain reporter installed so our collector is not left referenced by the mix.
	TextureGraphEngine::RegisterErrorReporter(Graph, std::make_shared<FTextureGraphErrorReporter>());

	return Result;
}

bool UTextureGraphTools::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
