// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "TextureGraphTools.h"
#include "Expressions/TG_Expression.h"
#include "Expressions/Input/TG_Expression_InputParam.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "TG_Graph.h"
#include "TG_Node.h"
#include "TG_OutputSettings.h"
#include "TG_Pin.h"
#include "ToolsetRegistry/Tests/TestAssetSandbox.h"
#include "Tests/TextureGraphToolsetTestFixture.h"

#if WITH_DEV_AUTOMATION_TESTS

// Invalid-input coverage note: these tools report invalid input by raising a script error
// (UKismetSystemLibrary::RaiseScriptError), which only throws when the call runs through the
// Blueprint / tool-call VM (e.g. via MCP). Called directly from C++ here there is no VM frame, so
// the raise is a no-op and the tool returns its documented default (null/false/empty). These specs
// assert that returned default; the caller-facing error is exercised through the MCP/tool-call layer.

namespace
{
	// Finds the node in a GetGraph result whose Node ref matches, or nullptr.
	const FTextureGraphNodeInfo* FindNode(const TArray<FTextureGraphNodeInfo>& Graph, const UTG_Node* Node)
	{
		for (const FTextureGraphNodeInfo& Info : Graph)
		{
			if (Info.Node.Get() == Node)
			{
				return &Info;
			}
		}
		return nullptr;
	}

	// The node feeding TargetNode's named input pin, per a GetGraph snapshot (null if unconnected).
	// Connectivity lives on the input pins, so an edge reads as "input pin X sourced by node Y".
	const UTG_Node* SourceOf(const TArray<FTextureGraphNodeInfo>& Graph, const UTG_Node* TargetNode, FName InputArg)
	{
		if (const FTextureGraphNodeInfo* Info = FindNode(Graph, TargetNode))
		{
			for (const FTextureGraphPinInfo& P : Info->InputPins)
			{
				if (P.ArgumentName == InputArg)
				{
					return P.SourceNode;
				}
			}
		}
		return nullptr;
	}
}

// -------------------------------------------------------------------------
// CreateTextureGraph
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FCreateTextureGraphSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.CreateTextureGraph",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FCreateTextureGraphSpec)

void FCreateTextureGraphSpec::Define()
{
	Describe(TEXT("success"), [this]()
	{
		It(TEXT("creates a TextureGraph asset at the requested path"), [this]()
		{
			FTestAssetSandbox Sandbox(TEXT("TextureGraphToolsetTest"));
			UTextureGraph* Graph = UTextureGraphTools::CreateTextureGraph(Sandbox.Folder(), TEXT("TG_CreateTest"));
			TestNotNull(TEXT("Graph created"), Graph);
			if (Graph)
			{
				TestEqual(TEXT("Asset name"), Graph->GetName(), TEXT("TG_CreateTest"));
			}
		});
	});

	Describe(TEXT("validation"), [this]()
	{
		It(TEXT("returns null for empty FolderPath or AssetName"), [this]()
		{
			TestNull(TEXT("empty FolderPath"), UTextureGraphTools::CreateTextureGraph(TEXT(""), TEXT("AnyName")));

			FTestAssetSandbox Sandbox(TEXT("TextureGraphToolsetTest"));
			TestNull(TEXT("empty AssetName"), UTextureGraphTools::CreateTextureGraph(Sandbox.Folder(), TEXT("")));
		});
	});
}

// -------------------------------------------------------------------------
// ListNodeTypes
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FListNodeTypesSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.ListNodeTypes",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FListNodeTypesSpec)

void FListNodeTypesSpec::Define()
{
	It(TEXT("returns a non-empty sorted list of node types"), [this]()
	{
		TArray<TSubclassOf<UTG_Expression>> Types = UTextureGraphTools::ListNodeTypes();
		TestTrue(TEXT("Non-empty"), Types.Num() > 0);
		for (int32 i = 1; i < Types.Num(); ++i)
		{
			const UClass* Prev = Types[i - 1].Get();
			const UClass* Curr = Types[i].Get();
			if (!TestNotNull(TEXT("Previous class valid"), Prev) || !TestNotNull(TEXT("Current class valid"), Curr))
			{
				break;
			}
			TestTrue(TEXT("Sorted by path"), Prev->GetPathName() <= Curr->GetPathName());
		}
	});

	It(TEXT("all returned types are UTG_Expression subclasses"), [this]()
	{
		TArray<TSubclassOf<UTG_Expression>> Types = UTextureGraphTools::ListNodeTypes();
		const int32 CheckCount = FMath::Min(Types.Num(), 5);
		for (int32 i = 0; i < CheckCount; ++i)
		{
			UClass* Class = Types[i].Get();
			TestNotNull(TEXT("Class valid"), Class);
			if (Class)
			{
				TestTrue(*FString::Printf(TEXT("'%s' is UTG_Expression subclass"), *Class->GetPathName()),
					Class->IsChildOf(UTG_Expression::StaticClass()));
			}
		}
	});
}

// -------------------------------------------------------------------------
// AddNode / RemoveNode
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FNodeEditingSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.NodeEditing",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FNodeEditingSpec)

void FNodeEditingSpec::Define()
{
	Describe(TEXT("AddNode"), [this]()
	{
		It(TEXT("adds a node of the requested type and returns it"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddNodeGraph"));
			UTG_Node* Node = UTextureGraphTools::AddNode(Graph, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath));
			TestNotNull(TEXT("Node added"), Node);
		});

		It(TEXT("returns null for invalid inputs (null graph or null class)"), [this]()
		{
			const TSubclassOf<UTG_Expression> NoiseClass = TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath);
			TestNull(TEXT("null graph"), UTextureGraphTools::AddNode(nullptr, NoiseClass));

			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph();
			TestNull(TEXT("null class"), UTextureGraphTools::AddNode(Graph, TSubclassOf<UTG_Expression>()));
		});

		It(TEXT("returns null for an abstract ExpressionClass"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddNodeAbstractGraph"));
			// UTG_Expression is declared UCLASS(abstract), so it can never be instantiated as a node.
			TestNull(TEXT("abstract class rejected"),
				UTextureGraphTools::AddNode(Graph, UTG_Expression::StaticClass()));
		});

		It(TEXT("returns null for an input-parameter class (added with the input-parameter tool)"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddNodeRejectParamGraph"));
			TestNull(TEXT("input-parameter class rejected"),
				UTextureGraphTools::AddNode(Graph, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::ScalarClassPath)));
		});

		It(TEXT("returns null for an Output class (added with the output tool)"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddNodeRejectOutputGraph"));
			TestNull(TEXT("Output class rejected"),
				UTextureGraphTools::AddNode(Graph, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::OutputClassPath)));
		});
	});

	Describe(TEXT("RemoveNode"), [this]()
	{
		It(TEXT("removes a previously added node"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("RemoveNodeGraph"));
			UTG_Node* Node = UTextureGraphTools::AddNode(Graph, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath));
			if (!TestNotNull(TEXT("Node added for setup"), Node))
			{
				return;
			}
			TestTrue(TEXT("RemoveNode returned true"), UTextureGraphTools::RemoveNode(Graph, Node));
		});

		It(TEXT("returns false for null arguments"), [this]()
		{
			TestFalse(TEXT("Null graph"), UTextureGraphTools::RemoveNode(nullptr, nullptr));
		});

		It(TEXT("returns false when the node does not belong to the graph"), [this]()
		{
			UTextureGraph* GraphA = TextureGraphToolsetTest::MakeTransientGraph(TEXT("RemoveNodeGraphA"));
			UTextureGraph* GraphB = TextureGraphToolsetTest::MakeTransientGraph(TEXT("RemoveNodeGraphB"));
			UTG_Node* NodeInB = UTextureGraphTools::AddNode(GraphB, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath));
			if (!TestNotNull(TEXT("Node added to GraphB"), NodeInB))
			{
				return;
			}
			// Node belongs to GraphB; removing it via GraphA must be rejected, not silently no-op.
			TestFalse(TEXT("Cross-graph removal rejected"), UTextureGraphTools::RemoveNode(GraphA, NodeInB));
		});
	});
}

// -------------------------------------------------------------------------
// AddInputParam / RemoveInputParam
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FInputParamSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.InputParam",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FInputParamSpec)

void FInputParamSpec::Define()
{
	Describe(TEXT("AddInputParam"), [this]()
	{
		It(TEXT("adds an input-parameter node of the requested type"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddInputParamGraph"));
			UTG_Node* Node = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath, FName(TEXT("Roughness")));
			if (!TestNotNull(TEXT("Input param added"), Node))
			{
				return;
			}
			const UTG_Expression* Expr = Node->GetExpression();
			if (TestNotNull(TEXT("Expression present"), Expr))
			{
				TestTrue(TEXT("Is an input-parameter expression"),
					Expr->GetClass()->IsChildOf(UTG_Expression_InputParam::StaticClass()));
			}
		});

		It(TEXT("names the parameter and exposes it as a pin in GetGraph"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddInputParamNameGraph"));
			UTG_Node* Node = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath, FName(TEXT("Roughness")));
			if (!TestNotNull(TEXT("Input param added"), Node))
			{
				return;
			}
			TArray<FTextureGraphNodeInfo> GraphNodes = UTextureGraphTools::GetGraph(Graph);
			const FTextureGraphNodeInfo* Info = FindNode(GraphNodes, Node);
			if (!TestNotNull(TEXT("Node present in GetGraph"), Info))
			{
				return;
			}
			bool bFound = false;
			for (const FTextureGraphPinInfo& P : Info->InputPins)
			{
				if (P.Name == FName(TEXT("Roughness")))
				{
					bFound = true;
					break;
				}
			}
			TestTrue(TEXT("Parameter pin named 'Roughness' present"), bFound);
		});

		It(TEXT("uniquifies a duplicate parameter name"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddInputParamDupGraph"));
			UTG_Node* First  = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath, FName(TEXT("Dup")));
			UTG_Node* Second = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath, FName(TEXT("Dup")));
			if (!TestNotNull(TEXT("First added"), First) || !TestNotNull(TEXT("Second added"), Second))
			{
				return;
			}
			// Each Scalar's value pin carries the (possibly uniquified) parameter name as its alias.
			auto ParamNameOf = [](const UTG_Node* Node) -> FName
			{
				TArray<const UTG_Pin*> Pins;
				Node->GetInputPins(Pins);
				return Pins.Num() > 0 && Pins[0] ? Pins[0]->GetAliasName() : NAME_None;
			};
			const FName FirstName = ParamNameOf(First);
			const FName SecondName = ParamNameOf(Second);
			TestFalse(TEXT("First name set"), FirstName.IsNone());
			TestFalse(TEXT("Second name set"), SecondName.IsNone());
			TestNotEqual(TEXT("Duplicate name was uniquified"), FirstName, SecondName);
		});

		It(TEXT("returns null for a null graph or a non-input-parameter class"), [this]()
		{
			TestNull(TEXT("null graph"),
				UTextureGraphTools::AddInputParam(nullptr, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::ScalarClassPath), NAME_None));

			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddInputParamBadClassGraph"));
			TestNull(TEXT("non-input-parameter class rejected"),
				UTextureGraphTools::AddInputParam(Graph, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath), NAME_None));
		});
	});

	Describe(TEXT("RemoveInputParam"), [this]()
	{
		It(TEXT("removes an input parameter by name"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("RemoveInputParamGraph"));
			UTG_Node* Node = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath, FName(TEXT("Amount")));
			if (!TestNotNull(TEXT("Input param added"), Node))
			{
				return;
			}
			TestTrue(TEXT("RemoveInputParam returned true"), UTextureGraphTools::RemoveInputParam(Graph, FName(TEXT("Amount"))));

			TArray<FTextureGraphNodeInfo> GraphNodes = UTextureGraphTools::GetGraph(Graph);
			TestNull(TEXT("Node gone from graph"), FindNode(GraphNodes, Node));
		});

		It(TEXT("returns false for an unknown name"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("RemoveInputParamUnknownGraph"));
			TestFalse(TEXT("Unknown name rejected"), UTextureGraphTools::RemoveInputParam(Graph, FName(TEXT("NoSuchParam"))));
		});

		It(TEXT("returns false for a non-input-parameter name (an Output)"), [this]()
		{
			// A fresh graph has a default Output named "Output"; it is not an input parameter.
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("RemoveInputParamWrongKindGraph"));
			TestFalse(TEXT("Output name rejected by RemoveInputParam"),
				UTextureGraphTools::RemoveInputParam(Graph, FName(TEXT("Output"))));
		});
	});
}

// -------------------------------------------------------------------------
// AddOutput / RemoveOutput
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FOutputNodeSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.OutputNode",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FOutputNodeSpec)

void FOutputNodeSpec::Define()
{
	Describe(TEXT("AddOutput"), [this]()
	{
		It(TEXT("adds an Output node and names it"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddOutputGraph"));
			UTG_Node* Node = TextureGraphToolsetTest::AddOutputNode(Graph, FName(TEXT("Albedo")));
			if (!TestNotNull(TEXT("Output added"), Node))
			{
				return;
			}
			const UTG_Expression_Output* OutExpr = Cast<UTG_Expression_Output>(Node->GetExpression());
			if (TestNotNull(TEXT("Is an Output expression"), OutExpr))
			{
				TestEqual(TEXT("OutputSettings.OutputName set"), OutExpr->OutputSettings.OutputName, FName(TEXT("Albedo")));
			}
		});

		It(TEXT("supports multiple outputs on one graph"), [this]()
		{
			// A fresh graph already has a default Output; add a second and confirm both are present.
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("AddOutputTwoGraph"));
			UTG_Node* Second = TextureGraphToolsetTest::AddOutputNode(Graph, FName(TEXT("Normal")));
			if (!TestNotNull(TEXT("Second Output added"), Second))
			{
				return;
			}
			TArray<FTextureGraphNodeInfo> GraphNodes = UTextureGraphTools::GetGraph(Graph);
			TArray<FName> OutputNames;
			for (const FTextureGraphNodeInfo& Info : GraphNodes)
			{
				if (const UTG_Expression_Output* OutExpr = Cast<UTG_Expression_Output>(Info.Node ? Info.Node->GetExpression() : nullptr))
				{
					OutputNames.Add(OutExpr->GetTitleName());
				}
			}
			TestEqual(TEXT("Two Output nodes present"), OutputNames.Num(), 2);
			TestTrue(TEXT("Default Output present"), OutputNames.Contains(FName(TEXT("Output"))));
			TestTrue(TEXT("Named second Output present"), OutputNames.Contains(FName(TEXT("Normal"))));
		});

		It(TEXT("returns null for a null graph"), [this]()
		{
			TestNull(TEXT("null graph"), UTextureGraphTools::AddOutput(nullptr, FName(TEXT("Albedo"))));
		});
	});

	Describe(TEXT("RemoveOutput"), [this]()
	{
		It(TEXT("removes an output by name"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("RemoveOutputGraph"));
			UTG_Node* Node = TextureGraphToolsetTest::AddOutputNode(Graph, FName(TEXT("Albedo")));
			if (!TestNotNull(TEXT("Output added"), Node))
			{
				return;
			}
			TestTrue(TEXT("RemoveOutput returned true"), UTextureGraphTools::RemoveOutput(Graph, FName(TEXT("Albedo"))));

			TArray<FTextureGraphNodeInfo> GraphNodes = UTextureGraphTools::GetGraph(Graph);
			TestNull(TEXT("Node gone from graph"), FindNode(GraphNodes, Node));
		});

		It(TEXT("returns false for an unknown name"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("RemoveOutputUnknownGraph"));
			TestFalse(TEXT("Unknown name rejected"), UTextureGraphTools::RemoveOutput(Graph, FName(TEXT("NoSuchOutput"))));
		});

		It(TEXT("returns false for a non-output name (an input parameter)"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("RemoveOutputWrongKindGraph"));
			UTG_Node* Param = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath, FName(TEXT("Rough")));
			if (!TestNotNull(TEXT("Input param added"), Param))
			{
				return;
			}
			TestFalse(TEXT("Input-parameter name rejected by RemoveOutput"),
				UTextureGraphTools::RemoveOutput(Graph, FName(TEXT("Rough"))));
		});
	});
}

// -------------------------------------------------------------------------
// ConnectPins / DisconnectPins
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FPinConnectionSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.PinConnection",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FPinConnectionSpec)

void FPinConnectionSpec::Define()
{
	Describe(TEXT("ConnectPins"), [this]()
	{
		It(TEXT("returns false for null arguments"), [this]()
		{
			TestFalse(TEXT("All null"),
				UTextureGraphTools::ConnectPins(nullptr, nullptr, NAME_None, nullptr, NAME_None));
		});

		It(TEXT("connects compatible output-to-input pins between two nodes"), [this]()
		{
			// A known-compatible texture pair (Noise output -> Blur input); an incompatible pairing now
			// raises rather than returning false, so use a deterministically valid connection here.
			const TSubclassOf<UTG_Expression> NoiseClass = TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath);
			const TSubclassOf<UTG_Expression> BlurClass  = TextureGraphToolsetTest::ExprClass(TEXT("/Script/TextureGraph.TG_Expression_Blur"));

			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("ConnectGraph"));
			UTG_Node* Noise = UTextureGraphTools::AddNode(Graph, NoiseClass);
			UTG_Node* Blur  = UTextureGraphTools::AddNode(Graph, BlurClass);
			if (!TestNotNull(TEXT("Noise"), Noise) || !TestNotNull(TEXT("Blur"), Blur))
			{
				return;
			}
			TestTrue(TEXT("Noise.Output -> Blur.Input connected"),
				UTextureGraphTools::ConnectPins(Graph, Noise, TEXT("Output"), Blur, TEXT("Input")));
		});

		It(TEXT("returns false when a pin name does not resolve"), [this]()
		{
			const TSubclassOf<UTG_Expression> NoiseClass = TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath);
			const TSubclassOf<UTG_Expression> BlurClass  = TextureGraphToolsetTest::ExprClass(TEXT("/Script/TextureGraph.TG_Expression_Blur"));

			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("ConnectBadPinGraph"));
			UTG_Node* Noise = UTextureGraphTools::AddNode(Graph, NoiseClass);
			UTG_Node* Blur  = UTextureGraphTools::AddNode(Graph, BlurClass);
			if (!TestNotNull(TEXT("Noise"), Noise) || !TestNotNull(TEXT("Blur"), Blur))
			{
				return;
			}
			TestFalse(TEXT("Unknown output pin rejected"),
				UTextureGraphTools::ConnectPins(Graph, Noise, TEXT("ZZZ_NoSuchPin"), Blur, TEXT("Input")));
		});
	});

	Describe(TEXT("DisconnectPins"), [this]()
	{
		It(TEXT("returns false for null arguments"), [this]()
		{
			TestFalse(TEXT("All null"),
				UTextureGraphTools::DisconnectPins(nullptr, nullptr, NAME_None, nullptr, NAME_None));
		});

		// Connect/DisconnectPins must resolve pins by editable alias too, so the alias names that
		// GetGraph reports round-trip back in; argument-name-only lookup would miss them.
		It(TEXT("resolves pins by editable alias, matching how GetGraph reports them"), [this]()
		{
			const TSubclassOf<UTG_Expression> NoiseClassA = TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath);

			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("ConnectByAliasGraph"));
			UTG_Node* Noise  = UTextureGraphTools::AddNode(Graph, NoiseClassA);
			UTG_Node* Output = TextureGraphToolsetTest::AddOutputNode(Graph);
			if (!TestNotNull(TEXT("Noise added"), Noise) || !TestNotNull(TEXT("Output added"), Output))
			{
				return;
			}

			// Force the Noise output pin's alias to diverge from its argument name ("Output").
			FName OutputArg(TEXT("Output"));
			UTG_Pin* NoiseOut = Noise->GetOutputPin(OutputArg);
			if (!TestNotNull(TEXT("Noise output pin"), NoiseOut)) return;
			NoiseOut->SetAliasName(FName(TEXT("MyNoiseOut")));

			// Both calls address the output pin by its alias; argument-name-only resolution would miss it.
			TestTrue(TEXT("ConnectPins resolves the output pin by alias"),
				UTextureGraphTools::ConnectPins(Graph, Noise, FName(TEXT("MyNoiseOut")), Output, FName(TEXT("Source"))));
			TestTrue(TEXT("DisconnectPins resolves the output pin by alias"),
				UTextureGraphTools::DisconnectPins(Graph, Noise, FName(TEXT("MyNoiseOut")), Output, FName(TEXT("Source"))));
		});

		// Regression for the "DisconnectPins cascade" claim: disconnecting a mid-chain edge was reported
		// to silently drop the unrelated terminal edge into Output.Source. Build Noise -> Blur -> Output,
		// disconnect Noise->Blur, and assert Blur->Output.Source survives.
		It(TEXT("disconnecting a mid-chain edge leaves the terminal Output edge intact"), [this]()
		{
			const TSubclassOf<UTG_Expression> NoiseClass = TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath);
			const TSubclassOf<UTG_Expression> BlurClass  = TextureGraphToolsetTest::ExprClass(TEXT("/Script/TextureGraph.TG_Expression_Blur"));

			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("DisconnectCascadeGraph"));
			UTG_Node* Noise  = UTextureGraphTools::AddNode(Graph, NoiseClass);
			UTG_Node* Blur   = UTextureGraphTools::AddNode(Graph, BlurClass);
			UTG_Node* Output = TextureGraphToolsetTest::AddOutputNode(Graph);
			if (!TestNotNull(TEXT("Noise added"), Noise) ||
				!TestNotNull(TEXT("Blur added"), Blur) ||
				!TestNotNull(TEXT("Output added"), Output))
			{
				return;
			}

			// Wire the linear chain. Bail loudly if a connect fails: the repro premise depends on it.
			if (!TestTrue(TEXT("Noise.Output -> Blur.Input connected"),
					UTextureGraphTools::ConnectPins(Graph, Noise, TEXT("Output"), Blur, TEXT("Input"))) ||
				!TestTrue(TEXT("Blur.Output -> Output.Source connected"),
					UTextureGraphTools::ConnectPins(Graph, Blur, TEXT("Output"), Output, TEXT("Source"))))
			{
				return;
			}

			// Pre-state: Blur.Input fed by Noise, Output.Source fed by Blur.
			const TArray<FTextureGraphNodeInfo> Before = UTextureGraphTools::GetGraph(Graph);
			if (!TestTrue(TEXT("Blur.Input fed by Noise before disconnect"), SourceOf(Before, Blur, TEXT("Input")) == Noise) ||
				!TestTrue(TEXT("Output.Source fed by Blur before disconnect"), SourceOf(Before, Output, TEXT("Source")) == Blur))
			{
				return;
			}

			// Disconnect ONLY the mid-chain edge.
			TestTrue(TEXT("DisconnectPins returned true"),
				UTextureGraphTools::DisconnectPins(Graph, Noise, TEXT("Output"), Blur, TEXT("Input")));

			// Post-state: Blur.Input source cleared, Output.Source untouched.
			const TArray<FTextureGraphNodeInfo> After = UTextureGraphTools::GetGraph(Graph);
			TestTrue(TEXT("Blur.Input source cleared"), SourceOf(After, Blur, TEXT("Input")) == nullptr);
			TestTrue(TEXT("Output.Source SURVIVES the disconnect (cascade bug would clear it)"),
				SourceOf(After, Output, TEXT("Source")) == Blur);

			// Re-disconnecting the now-absent edge must report false (no-op), not a bogus success.
			TestFalse(TEXT("DisconnectPins returns false when the pins share no edge"),
				UTextureGraphTools::DisconnectPins(Graph, Noise, TEXT("Output"), Blur, TEXT("Input")));
		});

		// Reconnecting a new source into an occupied input runs through UTG_Graph::Connect's
		// "kill the existing edge at the destination" path, the likelier cascade suspect than RemoveEdge.
		// Assert the terminal Output edge survives disconnect+reconnect.
		It(TEXT("disconnect followed by reconnecting a new source leaves the terminal Output edge intact"), [this]()
		{
			const TSubclassOf<UTG_Expression> NoiseClass = TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath);
			const TSubclassOf<UTG_Expression> BlurClass  = TextureGraphToolsetTest::ExprClass(TEXT("/Script/TextureGraph.TG_Expression_Blur"));

			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("DisconnectReconnectGraph"));
			UTG_Node* Noise1 = UTextureGraphTools::AddNode(Graph, NoiseClass);
			UTG_Node* Noise2 = UTextureGraphTools::AddNode(Graph, NoiseClass);
			UTG_Node* Blur   = UTextureGraphTools::AddNode(Graph, BlurClass);
			UTG_Node* Output = TextureGraphToolsetTest::AddOutputNode(Graph);
			if (!TestNotNull(TEXT("Noise1 added"), Noise1) ||
				!TestNotNull(TEXT("Noise2 added"), Noise2) ||
				!TestNotNull(TEXT("Blur added"), Blur) ||
				!TestNotNull(TEXT("Output added"), Output))
			{
				return;
			}

			// Initial chain: Noise1 -> Blur -> Output.
			if (!TestTrue(TEXT("Noise1.Output -> Blur.Input connected"),
					UTextureGraphTools::ConnectPins(Graph, Noise1, TEXT("Output"), Blur, TEXT("Input"))) ||
				!TestTrue(TEXT("Blur.Output -> Output.Source connected"),
					UTextureGraphTools::ConnectPins(Graph, Blur, TEXT("Output"), Output, TEXT("Source"))))
			{
				return;
			}

			// Disconnect the mid-chain edge, then reconnect a DIFFERENT source into the same input.
			TestTrue(TEXT("DisconnectPins returned true"),
				UTextureGraphTools::DisconnectPins(Graph, Noise1, TEXT("Output"), Blur, TEXT("Input")));
			TestTrue(TEXT("Noise2.Output -> Blur.Input reconnected"),
				UTextureGraphTools::ConnectPins(Graph, Noise2, TEXT("Output"), Blur, TEXT("Input")));

			const TArray<FTextureGraphNodeInfo> After = UTextureGraphTools::GetGraph(Graph);
			TestTrue(TEXT("Blur.Input now fed by Noise2"), SourceOf(After, Blur, TEXT("Input")) == Noise2);
			TestTrue(TEXT("Output.Source SURVIVES disconnect+reconnect (the original repro symptom)"),
				SourceOf(After, Output, TEXT("Source")) == Blur);
		});
	});
}

// -------------------------------------------------------------------------
// SetNodeAlias
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FSetNodeAliasSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.SetNodeAlias",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FSetNodeAliasSpec)

void FSetNodeAliasSpec::Define()
{
	Describe(TEXT("success"), [this]()
	{
		It(TEXT("renames an Output node and keeps OutputSettings.OutputName in sync"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SetAliasOutGraph"));
			UTG_Node* Out = TextureGraphToolsetTest::AddOutputNode(Graph);
			if (!TestNotNull(TEXT("Output added"), Out))
			{
				return;
			}
			TestTrue(TEXT("Rename succeeded"), UTextureGraphTools::SetNodeAlias(Out, TEXT("Albedo")));
			const UTG_Expression* Expr = Out->GetExpression();
			if (!TestNotNull(TEXT("Output expression"), Expr))
			{
				return;
			}
			TestEqual(TEXT("TitleName updated"), Expr->GetTitleName(), FName(TEXT("Albedo")));
			const UTG_Expression_Output* OutExpr = Cast<UTG_Expression_Output>(Expr);
			if (TestNotNull(TEXT("Output expression is Output type"), OutExpr))
			{
				TestEqual(TEXT("OutputSettings.OutputName synced"), OutExpr->OutputSettings.OutputName, FName(TEXT("Albedo")));
			}
		});

		It(TEXT("renames a Scalar input-only node via its primary pin alias"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SetAliasScalarGraph"));
			UTG_Node* Scalar = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath);
			if (!TestNotNull(TEXT("Scalar added"), Scalar))
			{
				return;
			}
			TestTrue(TEXT("Rename succeeded"), UTextureGraphTools::SetNodeAlias(Scalar, TEXT("MyValue")));
			// Either the primary output pin or the input pin (input-only nodes expose value as input)
			// should now report the new alias.
			TArray<UTG_Pin*> Pins;
			Scalar->GetOutputPins(Pins);
			Scalar->GetInputPins(Pins);
			bool bFoundAlias = false;
			for (const UTG_Pin* Pin : Pins)
			{
				if (Pin && Pin->GetAliasName() == FName(TEXT("MyValue")))
				{
					bFoundAlias = true;
					break;
				}
			}
			TestTrue(TEXT("Primary pin alias matches"), bFoundAlias);
		});
	});

	Describe(TEXT("validation"), [this]()
	{
		It(TEXT("returns false for null Node or NAME_None alias"), [this]()
		{
			TestFalse(TEXT("Null node"), UTextureGraphTools::SetNodeAlias(nullptr, TEXT("Anything")));

			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SetAliasNoneGraph"));
			UTG_Node* Node = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath);
			if (!TestNotNull(TEXT("Node added"), Node))
			{
				return;
			}
			TestFalse(TEXT("None alias rejected"), UTextureGraphTools::SetNodeAlias(Node, NAME_None));
		});
	});
}

// -------------------------------------------------------------------------
// SetOutputSettings
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FSetOutputSettingsSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.SetOutputSettings",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FSetOutputSettingsSpec)

void FSetOutputSettingsSpec::Define()
{
	Describe(TEXT("success"), [this]()
	{
		It(TEXT("sets BaseName, FolderPath, and cascades MaskComp preset defaults"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SetOutCfgMaskGraph"));
			UTG_Node* Out = TextureGraphToolsetTest::AddOutputNode(Graph);
			if (!TestNotNull(TEXT("Output added"), Out))
			{
				return;
			}
			TestTrue(TEXT("Configure succeeded"),
				UTextureGraphTools::SetOutputSettings(Out, TEXT("Roughness"), TEXT("/Game/Test"), ETG_TexturePresetType::MaskComp));
			const UTG_Expression_Output* OutExpr = Cast<UTG_Expression_Output>(Out->GetExpression());
			if (!TestNotNull(TEXT("Output expression"), OutExpr))
			{
				return;
			}
			TestEqual(TEXT("BaseName"), OutExpr->OutputSettings.BaseName, FName(TEXT("Roughness")));
			TestEqual(TEXT("FolderPath"), OutExpr->OutputSettings.FolderPath, FName(TEXT("/Game/Test")));
			TestTrue(TEXT("PresetType"), OutExpr->OutputSettings.TexturePresetType == ETG_TexturePresetType::MaskComp);
			TestEqual(TEXT("Display name syncs"), OutExpr->GetTitleName(), FName(TEXT("Roughness")));
		});

		It(TEXT("Normal preset cascades TC_Normalmap and CharacterNormalMap LOD group, bSRGB=false"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SetOutCfgNormalGraph"));
			UTG_Node* Out = TextureGraphToolsetTest::AddOutputNode(Graph);
			if (!TestNotNull(TEXT("Output added"), Out))
			{
				return;
			}
			TestTrue(TEXT("Configure succeeded"),
				UTextureGraphTools::SetOutputSettings(Out, TEXT("Normal"), TEXT("/Game/Test"), ETG_TexturePresetType::Normal));
			const UTG_Expression_Output* OutExpr = Cast<UTG_Expression_Output>(Out->GetExpression());
			if (!TestNotNull(TEXT("Output expression"), OutExpr))
			{
				return;
			}
			// See FTG_OutputSettings::OnSetTexturePresetType: Normal cascades CharacterNormalMap, not World*.
			TestTrue(TEXT("Compression cascaded"), OutExpr->OutputSettings.Compression.GetValue() == TC_Normalmap);
			TestTrue(TEXT("LODGroup cascaded"), OutExpr->OutputSettings.LODGroup.GetValue() == TEXTUREGROUP_CharacterNormalMap);
			TestFalse(TEXT("bSRGB cleared"), OutExpr->OutputSettings.bSRGB);
		});

		It(TEXT("syncs the OutputSettings pin, not just the struct member"), [this]()
		{
			// A graph evaluation reconstructs OutputSettings from the pin, so a member-only write
			// gets reverted on the next eval/bake. Guard that the pin carries the values.
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SetOutCfgPinSyncGraph"));
			UTG_Node* Out = TextureGraphToolsetTest::AddOutputNode(Graph);
			if (!TestNotNull(TEXT("Output added"), Out))
			{
				return;
			}
			TestTrue(TEXT("Configure succeeded"),
				UTextureGraphTools::SetOutputSettings(Out, TEXT("Height"), TEXT("/Game/Test"), ETG_TexturePresetType::None));

			UTG_Pin* SettingsPin = Out->GetPin(GET_MEMBER_NAME_CHECKED(UTG_Expression_Output, OutputSettings));
			if (!TestNotNull(TEXT("OutputSettings pin"), SettingsPin))
			{
				return;
			}
			const FString PinValue = SettingsPin->GetEvaluatedVarValue();
			TestTrue(TEXT("pin carries BaseName"), PinValue.Contains(TEXT("BaseName=\"Height\"")));
			TestTrue(TEXT("pin carries FolderPath"), PinValue.Contains(TEXT("FolderPath=\"/Game/Test\"")));
		});
	});

	Describe(TEXT("validation"), [this]()
	{
		It(TEXT("returns false for null Node or a non-Output node"), [this]()
		{
			TestFalse(TEXT("Null node"),
				UTextureGraphTools::SetOutputSettings(nullptr, TEXT("X"), TEXT("/Game"), ETG_TexturePresetType::Diffuse));

			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SetOutCfgWrongNodeGraph"));
			UTG_Node* Scalar = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath);
			if (!TestNotNull(TEXT("Scalar added"), Scalar))
			{
				return;
			}
			TestFalse(TEXT("Non-Output rejected"),
				UTextureGraphTools::SetOutputSettings(Scalar, TEXT("X"), TEXT("/Game"), ETG_TexturePresetType::Diffuse));
		});
	});
}

// -------------------------------------------------------------------------
// SetPinValue
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FSetPinValueSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.SetPinValue",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FSetPinValueSpec)

void FSetPinValueSpec::Define()
{
	Describe(TEXT("success"), [this]()
	{
		It(TEXT("sets a Color pin and reads back the value"), [this]()
		{
			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SPV1_Color"));
			UTG_Node* Color = TextureGraphToolsetTest::AddInputParamNode(G, TextureGraphToolsetTest::ColorClassPath);
			if (!TestNotNull(TEXT("Color node added"), Color)) return;
			TestTrue(TEXT("Set succeeded"),
				UTextureGraphTools::SetPinValue(Color, FName(TEXT("Color")), TEXT("(R=0.5,G=0.25,B=0.125,A=1.0)")));

			UTG_Pin* Pin = Color->GetInputPin(FName(TEXT("Color")));
			if (TestNotNull(TEXT("Color pin"), Pin))
			{
				TestTrue(TEXT("value contains R=0.5"), Pin->GetEvaluatedVarValue().Contains(TEXT("R=0.5")));
			}
		});

		It(TEXT("resolves a pin by its alias when it diverges from the argument name"), [this]()
		{
			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SPV1_Alias"));
			UTG_Node* Color = TextureGraphToolsetTest::AddInputParamNode(G, TextureGraphToolsetTest::ColorClassPath);
			if (!TestNotNull(TEXT("Color added"), Color)) return;

			// Force the pin alias to diverge from the argument name, mimicking param-pin uniquification.
			UTG_Pin* ColorPin = Color->GetInputPin(FName(TEXT("Color")));
			if (!TestNotNull(TEXT("Color input pin"), ColorPin)) return;
			ColorPin->SetAliasName(FName(TEXT("MyColor")));

			TestTrue(TEXT("set via alias"),
				UTextureGraphTools::SetPinValue(Color, FName(TEXT("MyColor")), TEXT("(R=0.5,G=0.25,B=0.125,A=1.0)")));
			TestTrue(TEXT("value set via alias"), ColorPin->GetEvaluatedVarValue().Contains(TEXT("R=0.5")));
		});

		It(TEXT("returns true when re-setting a settable pin to the value it already holds (idempotent, not a no-op failure)"), [this]()
		{
			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SPV1_Idempotent"));
			UTG_Node* Color = TextureGraphToolsetTest::AddInputParamNode(G, TextureGraphToolsetTest::ColorClassPath);
			if (!TestNotNull(TEXT("Color node added"), Color)) return;

			const FString Val = TEXT("(R=0.5,G=0.25,B=0.125,A=1.0)");
			TestTrue(TEXT("first set succeeds"),
				UTextureGraphTools::SetPinValue(Color, FName(TEXT("Color")), Val));
			// A settable pin already holding the requested value is an idempotent success.
			TestTrue(TEXT("idempotent re-set still succeeds"),
				UTextureGraphTools::SetPinValue(Color, FName(TEXT("Color")), Val));
		});

		It(TEXT("creates and connects an inline constant node for a Variant pin"), [this]()
		{
			// MAD's Input2 is FTG_Variant (connection-driven). SetPinValue now auto-creates a constant
			// source of the inferred type and connects it, so the call succeeds and the pin is sourced.
			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SPV1_VariantScalar"));
			UTG_Node* MAD = UTextureGraphTools::AddNode(G, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::MADClassPath));
			if (!TestNotNull(TEXT("MAD added"), MAD)) return;

			TestTrue(TEXT("scalar-string variant set succeeds"),
				UTextureGraphTools::SetPinValue(MAD, FName(TEXT("Input2")), TEXT("0.5")));

			const TArray<FTextureGraphNodeInfo> GraphNodes = UTextureGraphTools::GetGraph(G);
			const UTG_Node* Source = SourceOf(GraphNodes, MAD, FName(TEXT("Input2")));
			if (TestNotNull(TEXT("Input2 now has a source node"), Source))
			{
				const UTG_Expression* SrcExpr = Source->GetExpression();
				TestTrue(TEXT("source is a Scalar constant"),
					SrcExpr && SrcExpr->GetClass() == TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::ScalarClassPath).Get());
			}
		});

		It(TEXT("infers a Color constant from a color-literal Variant value"), [this]()
		{
			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SPV1_VariantColor"));
			UTG_Node* MAD = UTextureGraphTools::AddNode(G, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::MADClassPath));
			if (!TestNotNull(TEXT("MAD added"), MAD)) return;

			TestTrue(TEXT("color-string variant set succeeds"),
				UTextureGraphTools::SetPinValue(MAD, FName(TEXT("Input2")), TEXT("(R=1,G=0,B=0,A=1)")));

			const TArray<FTextureGraphNodeInfo> GraphNodes = UTextureGraphTools::GetGraph(G);
			const UTG_Node* Source = SourceOf(GraphNodes, MAD, FName(TEXT("Input2")));
			if (TestNotNull(TEXT("Input2 now has a source node"), Source))
			{
				const UTG_Expression* SrcExpr = Source->GetExpression();
				TestTrue(TEXT("source is a Color constant"),
					SrcExpr && SrcExpr->GetClass() == TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::ColorClassPath).Get());
			}
		});

		It(TEXT("reuses the existing constant when re-setting a Variant pin to the same type"), [this]()
		{
			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SPV1_VariantReuse"));
			UTG_Node* MAD = UTextureGraphTools::AddNode(G, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::MADClassPath));
			if (!TestNotNull(TEXT("MAD added"), MAD)) return;

			TestTrue(TEXT("first scalar set succeeds"),
				UTextureGraphTools::SetPinValue(MAD, FName(TEXT("Input2")), TEXT("0.5")));
			const TArray<FTextureGraphNodeInfo> AfterFirst = UTextureGraphTools::GetGraph(G);
			const UTG_Node* FirstSource = SourceOf(AfterFirst, MAD, FName(TEXT("Input2")));
			const int32 CountAfterFirst = AfterFirst.Num();
			if (!TestNotNull(TEXT("Input2 sourced after first set"), FirstSource)) return;

			// Re-set to another scalar: the same constant node is updated in place, not replaced, so no
			// node churn and no downstream retype.
			TestTrue(TEXT("same-type re-set succeeds"),
				UTextureGraphTools::SetPinValue(MAD, FName(TEXT("Input2")), TEXT("0.75")));
			const TArray<FTextureGraphNodeInfo> AfterSecond = UTextureGraphTools::GetGraph(G);
			const UTG_Node* SecondSource = SourceOf(AfterSecond, MAD, FName(TEXT("Input2")));

			TestEqual(TEXT("node count unchanged (constant reused, not replaced)"), AfterSecond.Num(), CountAfterFirst);
			TestTrue(TEXT("same source node reused"), SecondSource == FirstSource);
			if (const FTextureGraphNodeInfo* SrcInfo = FindNode(AfterSecond, SecondSource))
			{
				bool bValueUpdated = false;
				for (const FTextureGraphPinInfo& P : SrcInfo->InputPins)
				{
					if (P.ArgumentName == FName(TEXT("Scalar")) && P.Value.Contains(TEXT("0.75")))
					{
						bValueUpdated = true;
					}
				}
				TestTrue(TEXT("constant value updated to 0.75"), bValueUpdated);
			}
		});
	});

	Describe(TEXT("validation"), [this]()
	{
		It(TEXT("returns false for null Node or NAME_None PinName"), [this]()
		{
			TestFalse(TEXT("null node"), UTextureGraphTools::SetPinValue(nullptr, FName(TEXT("Color")), TEXT("x")));

			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SPV1_Nulls"));
			UTG_Node* Color = TextureGraphToolsetTest::AddInputParamNode(G, TextureGraphToolsetTest::ColorClassPath);
			if (!TestNotNull(TEXT("Color added"), Color)) return;
			TestFalse(TEXT("NAME_None pin"), UTextureGraphTools::SetPinValue(Color, NAME_None, TEXT("x")));
		});

		It(TEXT("returns false for an unknown pin name"), [this]()
		{
			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SPV1_BadPin"));
			UTG_Node* Color = TextureGraphToolsetTest::AddInputParamNode(G, TextureGraphToolsetTest::ColorClassPath);
			if (!TestNotNull(TEXT("Color added"), Color)) return;
			TestFalse(TEXT("unknown pin"),
				UTextureGraphTools::SetPinValue(Color, FName(TEXT("ZZZ_NoSuchPin")), TEXT("1.0")));
		});

		It(TEXT("returns false when a Variant value cannot be interpreted"), [this]()
		{
			// A non-numeric, non-color, non-vector string yields no constant type to create.
			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("SPV1_VariantBad"));
			UTG_Node* MAD = UTextureGraphTools::AddNode(G, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::MADClassPath));
			if (!TestNotNull(TEXT("MAD added"), MAD)) return;
			TestFalse(TEXT("uninterpretable variant value rejected"),
				UTextureGraphTools::SetPinValue(MAD, FName(TEXT("Input2")), TEXT("not_a_value")));
		});
	});
}

// -------------------------------------------------------------------------
// GetGraph
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FGetGraphSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.GetGraph",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FGetGraphSpec)

void FGetGraphSpec::Define()
{
	Describe(TEXT("success"), [this]()
	{
		It(TEXT("returns nodes including the default Output for a fresh graph"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("GetGraphFreshGraph"));
			TArray<FTextureGraphNodeInfo> Nodes = UTextureGraphTools::GetGraph(Graph);
			TestTrue(TEXT("Has at least one node"), Nodes.Num() >= 1);

			bool bFoundOutput = false;
			for (const FTextureGraphNodeInfo& Info : Nodes)
			{
				if (Info.ExpressionClass.Get() == UTG_Expression_Output::StaticClass())
				{
					bFoundOutput = true;
					break;
				}
			}
			TestTrue(TEXT("Default Output node present"), bFoundOutput);
		});

		It(TEXT("populates the node reference and expression class"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("GetGraphPopGraph"));
			const TSubclassOf<UTG_Expression> NoiseClass = TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath);
			UTG_Node* Noise = UTextureGraphTools::AddNode(Graph, NoiseClass);
			if (!TestNotNull(TEXT("Noise added"), Noise))
			{
				return;
			}
			TArray<FTextureGraphNodeInfo> Nodes = UTextureGraphTools::GetGraph(Graph);
			const FTextureGraphNodeInfo* Info = FindNode(Nodes, Noise);
			if (!TestNotNull(TEXT("Noise node present in graph"), Info))
			{
				return;
			}
			TestTrue(TEXT("ExpressionClass matches"), Info->ExpressionClass.Get() == NoiseClass.Get());
			TestTrue(TEXT("Has an output pin"), Info->OutputPins.Num() > 0);
		});

		It(TEXT("reports both the alias Name and the stable ArgumentName for a pin"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("GetGraphArgNameGraph"));
			UTG_Node* Color = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ColorClassPath);
			if (!TestNotNull(TEXT("Color added"), Color))
			{
				return;
			}
			// A param pin's alias uniquifies (e.g. "Color_1") while its argument name stays "Color";
			// force that divergence so the assertion is deterministic.
			UTG_Pin* ColorPin = Color->GetInputPin(FName(TEXT("Color")));
			if (!TestNotNull(TEXT("Color input pin"), ColorPin))
			{
				return;
			}
			ColorPin->SetAliasName(FName(TEXT("MyColor")));

			TArray<FTextureGraphNodeInfo> Nodes = UTextureGraphTools::GetGraph(Graph);
			const FTextureGraphNodeInfo* Info = FindNode(Nodes, Color);
			if (!TestNotNull(TEXT("Color node present"), Info))
			{
				return;
			}
			bool bFound = false;
			for (const FTextureGraphPinInfo& P : Info->InputPins)
			{
				if (P.ArgumentName == FName(TEXT("Color")))
				{
					bFound = true;
					TestEqual(TEXT("Alias Name reflects the rename"), P.Name, FName(TEXT("MyColor")));
				}
			}
			TestTrue(TEXT("Input pin with ArgumentName 'Color' present"), bFound);
		});
	});

	Describe(TEXT("validation"), [this]()
	{
		It(TEXT("returns an empty array for null Graph"), [this]()
		{
			TArray<FTextureGraphNodeInfo> Nodes = UTextureGraphTools::GetGraph(nullptr);
			TestEqual(TEXT("Nodes empty"), Nodes.Num(), 0);
		});
	});
}

// -------------------------------------------------------------------------
// ExportTextureGraph
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FExportTextureGraphSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.ExportTextureGraph",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FExportTextureGraphSpec)

void FExportTextureGraphSpec::Define()
{
	// NOTE: the success path (actual bake cycle) requires full rendering infrastructure not
	// available in transient-graph tests. Success coverage lives in MCP demos. These tests
	// cover the synchronous early-return paths only.

	Describe(TEXT("validation"), [this]()
	{
		It(TEXT("returns an error for null Graph"), [this]()
		{
			UAsyncFunctionResultVoid* R = UTextureGraphTools::ExportTextureGraph(nullptr, TEXT("/Automation/TG"));
			if (TestNotNull(TEXT("Result not null"), R))
			{
				TestFalse(TEXT("Error is set"), R->Error.IsEmpty());
			}
		});

		It(TEXT("returns an error for empty OutputFolder"), [this]()
		{
			UTextureGraph* G = TextureGraphToolsetTest::MakeTransientGraph(TEXT("Export_EmptyFolder"));
			UAsyncFunctionResultVoid* R = UTextureGraphTools::ExportTextureGraph(G, TEXT(""));
			if (TestNotNull(TEXT("Result not null"), R))
			{
				TestFalse(TEXT("Error is set"), R->Error.IsEmpty());
			}
		});
	});
}

// -------------------------------------------------------------------------
// GetGraphIssues
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FGetGraphIssuesSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.GetGraphIssues",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FGetGraphIssuesSpec)

void FGetGraphIssuesSpec::Define()
{
	// NOTE: the two success Its below run a validate-only update cycle (InitTargets + Update without
	// scheduling), which needs the TextureGraph engine to be initialized. That holds in-editor (where
	// these run). If a headless harness lacks that infrastructure, keep the null-Graph validation It
	// and rely on the MCP smoke checks for the success paths (as ExportTextureGraph does for its bake).

	Describe(TEXT("validation"), [this]()
	{
		It(TEXT("returns an empty array for null Graph"), [this]()
		{
			TestEqual(TEXT("Null graph"), UTextureGraphTools::GetGraphIssues(nullptr).Num(), 0);
		});
	});

	Describe(TEXT("success"), [this]()
	{
		It(TEXT("returns no issues for a well-formed graph"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("IssuesHealthy"));

			// Repurpose the default Output with valid settings, then feed it a scalar input param.
			UTG_Node* OutNode = nullptr;
			for (const FTextureGraphNodeInfo& Info : UTextureGraphTools::GetGraph(Graph))
			{
				if (Info.ExpressionClass.Get() == UTG_Expression_Output::StaticClass())
				{
					OutNode = Info.Node.Get();
					break;
				}
			}
			if (!TestNotNull(TEXT("default Output present"), OutNode))
			{
				return;
			}
			UTextureGraphTools::SetOutputSettings(OutNode, FName(TEXT("Albedo")), FName(TEXT("/Game/Test")), ETG_TexturePresetType::Diffuse);

			UTG_Node* Rough = TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath, FName(TEXT("Roughness")));
			if (!TestNotNull(TEXT("Scalar input param added"), Rough))
			{
				return;
			}
			TestTrue(TEXT("Scalar -> Output.Source connected"),
				UTextureGraphTools::ConnectPins(Graph, Rough, TEXT("ValueOut"), OutNode, TEXT("Source")));

			TArray<FTextureGraphIssue> Issues = UTextureGraphTools::GetGraphIssues(Graph);
			TestEqual(TEXT("A well-formed graph reports no issues"), Issues.Num(), 0);
		});

		It(TEXT("reports a warning for an input path that is not a valid local path"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("IssuesBadPath"));
			UTG_Node* TexPath = TextureGraphToolsetTest::AddInputParamNode(
				Graph, TEXT("/Script/TextureGraph.TG_Expression_TexturePath"), FName(TEXT("BadPath")));
			if (!TestNotNull(TEXT("TexturePath added"), TexPath))
			{
				return;
			}
			TestTrue(TEXT("Path set to an invalid local path"),
				UTextureGraphTools::SetPinValue(TexPath, FName(TEXT("Path")), TEXT("Z:/definitely_not_real_12345.png")));

			TArray<FTextureGraphIssue> Issues = UTextureGraphTools::GetGraphIssues(Graph);
			bool bFound = false;
			for (const FTextureGraphIssue& Issue : Issues)
			{
				if (Issue.Severity == TEXT("Warning") && Issue.Message.Contains(TEXT("not a valid local path")))
				{
					bFound = true;
					TestTrue(TEXT("Issue is attributed to the TexturePath node"), Issue.Node.Get() == TexPath);
				}
			}
			TestTrue(TEXT("Invalid-path warning is reported"), bFound);
		});
	});
}

// -------------------------------------------------------------------------
// ListInputParams / ListOutputs
// -------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FListParamsOutputsSpec,
	"AIAssistant.Toolsets.TextureGraphToolset.ListParamsOutputs",
	TextureGraphToolsetTest::Flags)
END_DEFINE_SPEC(FListParamsOutputsSpec)

void FListParamsOutputsSpec::Define()
{
	Describe(TEXT("ListInputParams"), [this]()
	{
		It(TEXT("returns only the input-parameter nodes"), [this]()
		{
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("ListInputParamsGraph"));
			TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath, FName(TEXT("Roughness")));
			TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ColorClassPath, FName(TEXT("Tint")));
			// A non-param node that must be excluded.
			UTextureGraphTools::AddNode(Graph, TextureGraphToolsetTest::ExprClass(TextureGraphToolsetTest::NoiseClassPath));

			TArray<FTextureGraphNodeInfo> Params = UTextureGraphTools::ListInputParams(Graph);
			TestEqual(TEXT("Two input parameters"), Params.Num(), 2);
			for (const FTextureGraphNodeInfo& Info : Params)
			{
				const UClass* Class = Info.ExpressionClass.Get();
				TestTrue(TEXT("Each entry is an input-parameter expression"),
					Class && Class->IsChildOf(UTG_Expression_InputParam::StaticClass()));
			}
		});

		It(TEXT("returns an empty array for null Graph"), [this]()
		{
			TestEqual(TEXT("Null graph"), UTextureGraphTools::ListInputParams(nullptr).Num(), 0);
		});
	});

	Describe(TEXT("ListOutputs"), [this]()
	{
		It(TEXT("returns only the Output nodes"), [this]()
		{
			// A fresh graph already has one default Output; add a second.
			UTextureGraph* Graph = TextureGraphToolsetTest::MakeTransientGraph(TEXT("ListOutputsGraph"));
			if (!TestNotNull(TEXT("Second Output added"), TextureGraphToolsetTest::AddOutputNode(Graph, FName(TEXT("Normal")))))
			{
				return;
			}
			TextureGraphToolsetTest::AddInputParamNode(Graph, TextureGraphToolsetTest::ScalarClassPath, FName(TEXT("Roughness")));

			TArray<FTextureGraphNodeInfo> Outputs = UTextureGraphTools::ListOutputs(Graph);
			TestEqual(TEXT("Two Output nodes"), Outputs.Num(), 2);
			for (const FTextureGraphNodeInfo& Info : Outputs)
			{
				TestTrue(TEXT("Each entry is an Output expression"),
					Info.ExpressionClass.Get() == UTG_Expression_Output::StaticClass());
			}
		});

		It(TEXT("returns an empty array for null Graph"), [this]()
		{
			TestEqual(TEXT("Null graph"), UTextureGraphTools::ListOutputs(nullptr).Num(), 0);
		});
	});
}

#endif  // WITH_DEV_AUTOMATION_TESTS
