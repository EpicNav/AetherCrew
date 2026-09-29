// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioBlueprintStructureToolset.generated.h"

/** A parameter of an interface function or a custom event. */
USTRUCT(BlueprintType)
struct FHyperAIBlueprintParam
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/**
	 * bool, int, int64, float, byte, name, string, text, vector, rotator, transform, vector2d, linearcolor; or object:, class:,
	 * struct: or enum: with a path, such as object:/Script/Engine.Actor or struct:/Script/CoreUObject.Vector; [] after any
	 * makes it an array.
	 */
	UPROPERTY() FString Type;
};

/** One key of a timeline track. */
USTRUCT(BlueprintType)
struct FHyperAIBlueprintTimelineKey
{
	GENERATED_BODY()

	/** Seconds from the timeline's start. */
	UPROPERTY() float Time = 0.f;
	/** One number for a float track (none for an event), three for a vector (X, Y, Z), four for a color (R, G, B, A). */
	UPROPERTY() TArray<float> Value;
};

/** One timeline track: its output pin carries the curve's value (or fires, for an event track) as the timeline plays. */
USTRUCT(BlueprintType)
struct FHyperAIBlueprintTimelineTrack
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** float, vector, color or event. */
	UPROPERTY() FString Type;
	UPROPERTY() TArray<FHyperAIBlueprintTimelineKey> Keys;
	/** How the curve runs between keys: auto (smooth, the default), linear or constant. */
	UPROPERTY() FString Interp;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintStructureOp
{
	GENERATED_BODY()

	/**
	 * interface.create, interface.add_function, interface.implement, interface.remove; timeline.add, timeline.remove;
	 * event.add, event.set_rpc; variable.set_replication; component.set_replicated; graph.collapse, graph.expand;
	 * pins.connect.
	 */
	UPROPERTY() FString Kind;
	/**
	 * pins.connect: an output pin and the input pin it drives, each as Node.Pin: the node by name (K2Node_Timeline_0) or by
	 * its title when only one node has it (Event BeginPlay, a timeline's name), such as "Event BeginPlay.then" and "Open.Play".
	 */
	UPROPERTY() FString From;
	UPROPERTY() FString To;
	/**
	 * The member the op is about: the interface function, the timeline, the custom event, the variable, the component (its
	 * variable name), or the graph graph.collapse makes.
	 */
	UPROPERTY() FString Name;
	/** interface.*: the Blueprint Interface asset, such as /Game/Interfaces/BPI_Interactable. */
	UPROPERTY() FString Interface;
	/** interface.add_function, event.add: the inputs; interface.add_function: its outputs too (a function with none is an event). */
	UPROPERTY() TArray<FHyperAIBlueprintParam> Inputs;
	UPROPERTY() TArray<FHyperAIBlueprintParam> Outputs;
	/** event.add, event.set_rpc: none, server, client or multicast, and whether it is reliable. */
	UPROPERTY() FString Rpc;
	UPROPERTY() bool bReliable = false;
	/** variable.set_replication: none, replicated or rep_notify (which makes its OnRep_ function). */
	UPROPERTY() FString Replication;
	/**
	 * variable.set_replication: who receives it: none (everyone), initial_only, owner_only, skip_owner, simulated_only,
	 * autonomous_only, simulated_or_physics, initial_or_owner, custom, replay_or_owner, replay_only, skip_replay.
	 */
	UPROPERTY() FString Condition;
	/** component.set_replicated: whether the component replicates. timeline.add: whether the timeline does. */
	UPROPERTY() bool bReplicated = false;
	/** timeline.add: length in seconds; 0 ends at its last key. */
	UPROPERTY() float Length = 0.f;
	UPROPERTY() bool bAutoPlay = false;
	UPROPERTY() bool bLoop = false;
	UPROPERTY() bool bIgnoreTimeDilation = false;
	UPROPERTY() TArray<FHyperAIBlueprintTimelineTrack> Tracks;
	/**
	 * timeline.add: a function of this Blueprint for the timeline to run on every Update, each track feeding the function's
	 * input of the same name, such as ApplyOpen(Alpha) written with Epic's graph DSL. Then only what starts it needs wiring.
	 */
	UPROPERTY() FString Drive;
	/** timeline.add, event.add: the graph for the node, the event graph when empty; and where the node goes in it. */
	UPROPERTY() FString Graph;
	UPROPERTY() int32 X = 0;
	UPROPERTY() int32 Y = 0;
	/**
	 * graph.collapse: the nodes to fold into one, all in one graph, by name as hyper_blueprint_structure_inspect lists them
	 * (or by GUID). graph.expand: the one node to open back out: a collapsed graph, a macro or a function call.
	 */
	UPROPERTY() TArray<FString> Nodes;
	/** graph.collapse: collapsed (a collapsed graph in place), function (a new function the nodes call) or macro. */
	UPROPERTY() FString Into;
	/** interface.remove: keep the Blueprint's implementations as its own functions. */
	UPROPERTY() bool bKeepFunctions = false;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintStructurePlanRequest
{
	GENERATED_BODY()

	/** The Blueprint to edit, such as /Game/Blueprints/BP_Door. Not needed when every op makes or edits an interface. */
	UPROPERTY() FString BlueprintPath;
	UPROPERTY() TArray<FHyperAIBlueprintStructureOp> Operations;
	/** Save what the plan changed, once it compiles (the default). Off leaves it changed in the editor, unsaved. */
	UPROPERTY() bool bSave = true;
	/** Only check the plan: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

/** A compiler message: error, warning or note, with the node it is about when it names one. */
USTRUCT(BlueprintType)
struct FHyperAIBlueprintCompileMessage
{
	GENERATED_BODY()

	UPROPERTY() FString Blueprint;
	UPROPERTY() FString Severity;
	UPROPERTY() FString Message;
	UPROPERTY() FString Node;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintStructurePlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, checked, awaiting_user_approval, invalid_request, operation_failed, compile_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString BlueprintPath;
	UPROPERTY() int32 Applied = 0;
	/** What each op made, by name, so the next step can wire it: a timeline node and its pins, a collapsed node, a graph. */
	UPROPERTY() TArray<FString> Made;
	UPROPERTY() TArray<FString> Notes;
	/** up_to_date, up_to_date_with_warnings or error, for the Blueprint (and any interface the plan touched). */
	UPROPERTY() FString CompileStatus;
	UPROPERTY() TArray<FHyperAIBlueprintCompileMessage> CompileMessages;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintStructureInspectRequest
{
	GENERATED_BODY()

	UPROPERTY() FString BlueprintPath;
	/** List the nodes too (with their links), in this graph when set; a large graph pages with Offset and Limit. */
	UPROPERTY() bool bNodes = false;
	UPROPERTY() FString Graph;
	UPROPERTY() int32 Offset = 0;
	UPROPERTY() int32 Limit = 200;
};

/** One node: its name (what graph.collapse and Epic's tools take), title, class, place, and where its outputs lead. */
USTRUCT(BlueprintType)
struct FHyperAIBlueprintNodeInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Guid;
	UPROPERTY() FString Title;
	UPROPERTY() FString Class;
	UPROPERTY() FString Graph;
	UPROPERTY() int32 X = 0;
	UPROPERTY() int32 Y = 0;
	/** Output pin -> node.pin, one entry per link. */
	UPROPERTY() TArray<FString> Links;
};

USTRUCT(BlueprintType)
struct FHyperAIBlueprintStructureInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString BlueprintPath;
	UPROPERTY() FString ParentClass;
	UPROPERTY() bool bIsInterface = false;
	/** Each graph: its name, kind (event, function, macro, collapsed, interface) and node count. */
	UPROPERTY() TArray<FString> Graphs;
	/** For an interface asset: its functions with inputs and outputs. For a Blueprint: the interfaces it implements. */
	UPROPERTY() TArray<FString> Interfaces;
	UPROPERTY() TArray<FString> Timelines;
	/** Custom events, with their replication (server, client, multicast; reliable). */
	UPROPERTY() TArray<FString> Events;
	/** Member variables that replicate, with their condition and notify. */
	UPROPERTY() TArray<FString> ReplicatedVariables;
	/** Components the Blueprint adds, and whether each replicates. */
	UPROPERTY() TArray<FString> Components;
	UPROPERTY() FString CompileStatus;
	UPROPERTY() TArray<FHyperAIBlueprintNodeInfo> Nodes;
	UPROPERTY() int32 TotalNodes = 0;
	UPROPERTY() int32 NextOffset = -1;
};

/**
 * What Epic's Blueprint tools leave out: timelines, Blueprint interfaces, RPC events and replication conditions, and folding
 * nodes into functions, macros and collapsed graphs (and back). Each plan is one undo step, compiled, reported and saved.
 */
UCLASS()
class HYPERAISTUDIOBLUEPRINTSTRUCTURE_API UHyperAIStudioBlueprintStructureToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * A Blueprint's structure: graphs, interfaces (implemented, or an interface asset's functions), timelines and their
	 * tracks, custom events and their replication, replicated variables, components, compile status; with bNodes, its
	 * nodes by name with their links, paged, for large graphs and for graph.collapse.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintStructureInspectReport hyper_blueprint_structure_inspect(const FHyperAIBlueprintStructureInspectRequest& Request);

	/**
	 * Edits a Blueprint's structure as one undo step, compiles it and saves it: makes Blueprint Interfaces and their
	 * functions and implements them; adds timelines with float, vector, color and event tracks and their keys; adds custom
	 * events as RPCs (server, client, multicast, reliable) or sets an event's replication; sets a variable's replication
	 * and condition and a component's; collapses nodes into a function, macro or collapsed graph, or expands one. The report
	 * names what each op made, and the compiler's messages. Wire the new nodes with Epic's Blueprint tools.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Blueprint")
	static FHyperAIBlueprintStructurePlanReport hyper_blueprint_structure_apply_plan(const FHyperAIBlueprintStructurePlanRequest& Request);
};

class FHyperAIStudioBlueprintStructureContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("blueprint_structure");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioblueprintstructuretoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_blueprint_structure_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_blueprint_structure_apply_plan");
	static constexpr int32 MaxOperations = 64;
	static constexpr int32 MaxTrackKeys = 1024;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIBlueprintStructureInspectReport Inspect(const FHyperAIBlueprintStructureInspectRequest& Request);
	static FHyperAIBlueprintStructurePlanReport ApplyPlan(const FHyperAIBlueprintStructurePlanRequest& Request);
};

class FHyperAIStudioBlueprintStructureRegistration final
{
public:
	void Startup();
	void Shutdown();

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
