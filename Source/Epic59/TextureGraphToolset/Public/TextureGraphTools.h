// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "TextureGraph.h"
#include "TG_Node.h"
#include "Expressions/TG_Expression.h"
#include "Model/Mix/MixSettings.h"
#include "Templates/SubclassOf.h"
#include "AsyncFunctionResult/AsyncFunctionResultVoid.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions

#include "TextureGraphTools.generated.h"

/** A pin on a node inside a TextureGraph. */
USTRUCT(BlueprintType)
struct FTextureGraphPinInfo
{
	GENERATED_BODY()

	/** Display/alias name of the pin. May be uniquified for param pins (e.g. a second Color node's pin is "Color_1"). */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	FName Name;

	/** Schema/argument name on the expression, stable per class (e.g. "Color"). Use this to address the pin. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	FName ArgumentName;

	/** CPP type name of the pin's argument (e.g. "float", "FLinearColor", "FTG_Variant"). */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	FString CppType;

	/** Current value of an input pin. Empty for output pins. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	FString Value;

	/** For a connected input pin, the node whose output feeds it; null for output pins and unconnected inputs. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	TObjectPtr<UTG_Node> SourceNode;

	/** Name of the output pin on SourceNode feeding this pin; None when there is no source. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	FName SourcePin;
};

/**
 * A node inside a TextureGraph: its reference, type, pins (split by direction), and editor position.
 * The node's kind (regular, input parameter, or output) is identified by ExpressionClass: input
 * parameters derive from UTG_Expression_InputParam, outputs are UTG_Expression_Output.
 */
USTRUCT(BlueprintType)
struct FTextureGraphNodeInfo
{
	GENERATED_BODY()

	/** The node itself. Pass it to the editing tools to connect, set pin values, or remove. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	TObjectPtr<UTG_Node> Node;

	/** Current display name. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	FString NodeName;

	/** The node's expression type; identifies whether the node is a regular, input-parameter, or output node. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	TSubclassOf<UTG_Expression> ExpressionClass;

	/** The node's input pins (values it reads and connections it receives). */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	TArray<FTextureGraphPinInfo> InputPins;

	/** The node's output pins (values it emits to downstream nodes). */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	TArray<FTextureGraphPinInfo> OutputPins;

	/** Editor-graph position x. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	int32 PosX = 0;

	/** Editor-graph position y. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	int32 PosY = 0;
};

/**
 * A single issue found in a TextureGraph by validation: one error or warning, together with the node
 * that produced it.
 */
USTRUCT(BlueprintType)
struct FTextureGraphIssue
{
	GENERATED_BODY()

	/** Severity of the issue: "Error", "Warning", or "Info". */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	FString Severity;

	/** The kind of issue (for example a missing required input, an unsupported type, or a node warning). */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	FString Category;

	/** Human-readable description of the issue. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	FString Message;

	/** The node that produced the issue; null for a graph-level issue with no specific node. */
	UPROPERTY(BlueprintReadOnly, Category=TextureGraphToolset)
	TObjectPtr<UTG_Node> Node;
};

/**
 * Tools for creating, inspecting, and editing TextureGraph assets.
 * TextureGraphs are procedural texture authoring graphs.
 */
UCLASS(BlueprintType, MinimalAPI)
class UTextureGraphTools : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	// -------------------------------------------------------------------------
	// Asset creation
	// -------------------------------------------------------------------------

	/**
	 * Creates a new empty TextureGraph asset.
	 * This should ONLY be called after getting explicit direction or permission from the user.
	 *
	 * @param FolderPath  Content-browser folder for the new asset (e.g. "/Game/Textures").
	 * @param AssetName   Name of the new asset.
	 * @return The newly created TextureGraph asset, or null on failure.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Create")
	static TEXTUREGRAPHTOOLSET_API UTextureGraph* CreateTextureGraph(const FString& FolderPath, const FString& AssetName);

	// -------------------------------------------------------------------------
	// Node type discovery
	// -------------------------------------------------------------------------

	/**
	 * Returns every available TextureGraph expression node type, sorted. A returned class identifies
	 * the node type to add. Input-parameter and Output types are included but are rejected by the
	 * generic add; create those through the dedicated input-parameter and output tools.
	 * @return Every non-abstract expression node type.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API TArray<TSubclassOf<UTG_Expression>> ListNodeTypes();

	// -------------------------------------------------------------------------
	// Node editing
	// -------------------------------------------------------------------------

	/**
	 * Adds a node of the given expression type to the graph and returns the new node. Input-parameter
	 * and Output expression types are not accepted here; they are created with the dedicated
	 * input-parameter and output tools.
	 *
	 * @param Graph            The TextureGraph to modify.
	 * @param ExpressionClass  The node type to add.
	 * @param PosX             Editor-graph X position for the new node (defaults to 0).
	 * @param PosY             Editor-graph Y position for the new node (defaults to 0).
	 * @return The newly created node. Raises a script error and returns null on a null graph, a null
	 *         or abstract ExpressionClass, an input-parameter or Output ExpressionClass, or a graph
	 *         with no internal UTG_Graph.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API UTG_Node* AddNode(UTextureGraph* Graph, TSubclassOf<UTG_Expression> ExpressionClass, int32 PosX = 0, int32 PosY = 0);

	/**
	 * Adds an input-parameter node (a graph-level parameter such as Scalar, Color, or Vector) and
	 * returns it. The parameter is exposed on the graph automatically. If ParamName is set it becomes
	 * the parameter's name, uniquified against the graph's existing parameters.
	 *
	 * @param Graph            The TextureGraph to modify.
	 * @param ExpressionClass  An input-parameter expression type (a UTG_Expression_InputParam subclass).
	 * @param ParamName        Desired parameter name; None leaves the type's default name.
	 * @param PosX             Editor-graph X position for the new node (defaults to 0).
	 * @param PosY             Editor-graph Y position for the new node (defaults to 0).
	 * @return The newly created node. Raises a script error and returns null on a null graph, a null,
	 *         abstract, or non-input-parameter ExpressionClass, or a graph with no internal UTG_Graph.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API UTG_Node* AddInputParam(UTextureGraph* Graph, TSubclassOf<UTG_Expression> ExpressionClass, FName ParamName, int32 PosX = 0, int32 PosY = 0);

	/**
	 * Removes the input-parameter node exposing the named parameter.
	 *
	 * @param Graph      The TextureGraph to modify.
	 * @param ParamName  Name of the input parameter to remove.
	 * @return True on success. Raises a script error and returns false on a null graph, a None
	 *         ParamName, a graph with no internal UTG_Graph, or when no input parameter of that name exists.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API bool RemoveInputParam(UTextureGraph* Graph, FName ParamName);

	/**
	 * Adds an Output node and returns it. The output is exposed on the graph automatically. If
	 * OutputName is set it becomes the output's name (and its export filename), uniquified against the
	 * graph's existing outputs. Configure the output further with the output-settings tool.
	 *
	 * @param Graph       The TextureGraph to modify.
	 * @param OutputName  Desired output name; None leaves the default name.
	 * @param PosX        Editor-graph X position for the new node (defaults to 0).
	 * @param PosY        Editor-graph Y position for the new node (defaults to 0).
	 * @return The newly created node. Raises a script error and returns null on a null graph or a graph
	 *         with no internal UTG_Graph.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API UTG_Node* AddOutput(UTextureGraph* Graph, FName OutputName, int32 PosX = 0, int32 PosY = 0);

	/**
	 * Removes the Output node exposing the named output.
	 *
	 * @param Graph       The TextureGraph to modify.
	 * @param OutputName  Name of the output to remove.
	 * @return True on success. Raises a script error and returns false on a null graph, a None
	 *         OutputName, a graph with no internal UTG_Graph, or when no output of that name exists.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API bool RemoveOutput(UTextureGraph* Graph, FName OutputName);

	/**
	 * Removes a node and all its connected edges from the graph.
	 * @param Graph  The TextureGraph to modify.
	 * @param Node   The node to remove.
	 * @return True on success. Raises a script error and returns false on a null graph or node, a graph
	 *         with no internal UTG_Graph, or a node that does not belong to the graph.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API bool RemoveNode(UTextureGraph* Graph, UTG_Node* Node);

	/**
	 * Renames a node by setting its primary pin's alias name. For Output nodes this additionally
	 * syncs OutputSettings.OutputName so the bake's export filename matches the new node name.
	 * For other node types, sets the alias on the node's first output pin (or first input pin
	 * for input-only nodes like Scalar/Color/Texture).
	 *
	 * @param Node      The node to rename.
	 * @param NewAlias  The new display name.
	 * @return True if the alias was applied. Raises a script error and returns false on a null node, a
	 *         None alias (an empty FName resolves to NAME_None), or a node with no settable pin.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API bool SetNodeAlias(UTG_Node* Node, FName NewAlias);

	/**
	 * Typed shortcut for the most common Output-node configuration. Sets BaseName, FolderPath, and
	 * TexturePresetType (cascading preset defaults for Compression/LODGroup/sRGB), then syncs the
	 * node's display name and export name to BaseName.
	 *
	 * @param OutputNode  Node whose expression is UTG_Expression_Output.
	 * @param BaseName    Exported asset filename (e.g. "BaseColor").
	 * @param FolderPath  Content folder for the exported asset (e.g. "/Game/MyTextures").
	 * @param PresetType  Which preset's defaults to cascade onto Compression/LODGroup/sRGB.
	 *                    ETG_TexturePresetType::None applies no cascade (leaves those settings untouched).
	 * @return True on success. Raises a script error and returns false if the node is null or not an Output.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API bool SetOutputSettings(UTG_Node* OutputNode, FName BaseName, FName FolderPath, const ETG_TexturePresetType PresetType);

	/**
	 * Sets one input pin's value on a node, resolving the pin by argument name or alias. For a
	 * polymorphic Variant pin (which takes its value from a connected node, not a string) this creates
	 * an inline constant node matching the value (a scalar, color, or vector inferred from the value
	 * string) and connects it to the pin, replacing any existing source. Re-setting an ordinary pin to
	 * the value it already holds is an idempotent success.
	 *
	 * @param Node     The node to modify.
	 * @param PinName  Argument name or alias of the input pin.
	 * @param Value    New value in the node's pin-value string format (e.g. "4.0", "(R=1,G=0,B=0,A=1)").
	 * @return True on success. Raises a script error and returns false on a null node, a NAME_None
	 *         PinName, an unresolved pin, a node with no internal UTG_Graph, a Variant value that
	 *         cannot be interpreted as a scalar/color/vector, or a Variant constant that could not be
	 *         created or connected. Ordinary pins report success once the value is handed to the pin;
	 *         UTG_Pin::FromString returns void, so a rejected parse cannot be detected here.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API bool SetPinValue(UTG_Node* Node, FName PinName, const FString& Value);

	/**
	 * Connects an output pin on FromNode to an input pin on ToNode.
	 *
	 * @param Graph     The TextureGraph containing both nodes.
	 * @param FromNode  Source node (output side).
	 * @param FromPin   Name of the output pin on FromNode.
	 * @param ToNode    Destination node (input side).
	 * @param ToPin     Name of the input pin on ToNode.
	 * @return True if the edge was created. Raises a script error and returns false on a null argument,
	 *         a graph with no internal UTG_Graph, an unresolved pin, incompatible pins, or a connection
	 *         that would form a cycle.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API bool ConnectPins(UTextureGraph* Graph, UTG_Node* FromNode, FName FromPin, UTG_Node* ToNode, FName ToPin);

	/**
	 * Removes the edge between the specified pins.
	 * @param Graph     The TextureGraph containing both nodes.
	 * @param FromNode  Source node (output side).
	 * @param FromPin   Name of the output pin on FromNode.
	 * @param ToNode    Destination node (input side).
	 * @param ToPin     Name of the input pin on ToNode.
	 * @return True if an edge was removed; false (without error) when the pins resolve but share no
	 *         edge. Raises a script error on a null argument, a graph with no internal UTG_Graph, or an
	 *         unresolved pin.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Nodes")
	static TEXTUREGRAPHTOOLSET_API bool DisconnectPins(UTextureGraph* Graph, UTG_Node* FromNode, FName FromPin, UTG_Node* ToNode, FName ToPin);

	// -------------------------------------------------------------------------
	// Evaluation and export
	// -------------------------------------------------------------------------

	/**
	 * Exports every texture output node to a Texture2D asset in the given content folder.
	 * Triggers a full graph evaluation internally before writing assets.
	 *
	 * @param Graph         The TextureGraph to export.
	 * @param OutputFolder  Content-browser folder for the baked textures (e.g. "/Game/Textures/Baked").
	 * @return A non-null async result, never null. Already resolved with an error on a null graph, an
	 *         empty OutputFolder, or a graph with no internal UTG_Graph. Otherwise resolved when the
	 *         export finishes: completed when every output was written, failed when the export raised
	 *         an error or wrote fewer maps than the graph has outputs.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Export")
	static TEXTUREGRAPHTOOLSET_API UAsyncFunctionResultVoid* ExportTextureGraph(UTextureGraph* Graph, const FString& OutputFolder);

	// -------------------------------------------------------------------------
	// Whole-graph read
	// -------------------------------------------------------------------------

	/**
	 * Returns every node in the graph, each with its input and output pins and, for a connected input
	 * pin, the node feeding it. Input parameters and outputs are ordinary nodes here, identified by
	 * their ExpressionClass.
	 *
	 * @param Graph  The TextureGraph to read.
	 * @return The graph's nodes. Raises a script error and returns an empty array when Graph is null or
	 *         has no internal UTG_Graph.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Read")
	static TEXTUREGRAPHTOOLSET_API TArray<FTextureGraphNodeInfo> GetGraph(UTextureGraph* Graph);

	/**
	 * Returns the graph's input-parameter nodes: the graph-level parameters such as Scalar, Color, or
	 * Vector.
	 *
	 * @param Graph  The TextureGraph to read.
	 * @return The input-parameter nodes. Raises a script error and returns an empty array when Graph is
	 *         null or has no internal UTG_Graph.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Read")
	static TEXTUREGRAPHTOOLSET_API TArray<FTextureGraphNodeInfo> ListInputParams(UTextureGraph* Graph);

	/**
	 * Returns the graph's Output nodes.
	 *
	 * @param Graph  The TextureGraph to read.
	 * @return The Output nodes. Raises a script error and returns an empty array when Graph is null or
	 *         has no internal UTG_Graph.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Read")
	static TEXTUREGRAPHTOOLSET_API TArray<FTextureGraphNodeInfo> ListOutputs(UTextureGraph* Graph);

	/**
	 * Validates the graph and returns every issue it reports: the errors and warnings the engine's
	 * validation surfaces (for example missing required inputs, unsupported types, material or subgraph
	 * errors, and node warnings). Use it to check a graph's health after editing.
	 *
	 * @param Graph  The TextureGraph to validate.
	 * @return The issues found; empty when the graph validates cleanly. Raises a script error and
	 *         returns an empty array when Graph is null or has no internal UTG_Graph.
	 */
	UFUNCTION(meta = (AICallable), Category = "TextureGraph|Validate")
	static TEXTUREGRAPHTOOLSET_API TArray<FTextureGraphIssue> GetGraphIssues(UTextureGraph* Graph);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface
};
