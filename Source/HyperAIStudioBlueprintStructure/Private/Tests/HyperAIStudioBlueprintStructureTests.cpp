// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "BlueprintEditorLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
#include "GameFramework/Actor.h"
#include "HyperAIStudioBlueprintStructureToolset.h"
#include "HyperAIStudioSettings.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace HyperAIStudio::BlueprintStructureTests
{
	FString NewPath(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("/Game/HyperAITests/Blueprint/%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	UBlueprint* NewActorBlueprint(const FString& Path)
	{
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*Path), FName(*FPackageName::GetShortName(Path)),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		return Blueprint;
	}

	void Clean(const TArray<UObject*>& Objects)
	{
		TArray<UObject*> Live;
		for (UObject* Object : Objects)
		{
			if (IsValid(Object))
			{
				Live.Add(Object);
			}
		}
		if (!Live.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Live, /*ShowConfirmation=*/false);
		}
	}

	struct FNoApproval
	{
		bool bWas = GetDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits;
		FNoApproval() { GetMutableDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits = false; }
		~FNoApproval() { GetMutableDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits = bWas; }
	};

	FHyperAIBlueprintStructureOp Op(const TCHAR* Kind, const TCHAR* Name = TEXT(""))
	{
		FHyperAIBlueprintStructureOp Out;
		Out.Kind = Kind;
		Out.Name = Name;
		return Out;
	}

	FHyperAIBlueprintParam Param(const TCHAR* Name, const TCHAR* Type)
	{
		FHyperAIBlueprintParam Out;
		Out.Name = Name;
		Out.Type = Type;
		return Out;
	}

	FHyperAIBlueprintTimelineKey Key(const float Time, TArray<float> Value)
	{
		FHyperAIBlueprintTimelineKey Out;
		Out.Time = Time;
		Out.Value = MoveTemp(Value);
		return Out;
	}

	int32 CountCalls(const UEdGraph* Graph, const FName Function)
	{
		int32 Count = 0;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
			Count += Call && Call->FunctionReference.GetMemberName() == Function ? 1 : 0;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintStructureMembersTest,
	"HyperAIStudio.BlueprintStructure.InterfaceTimelineReplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintStructureMembersTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintStructureTests;
	using C = FHyperAIStudioBlueprintStructureContracts;
	const FNoApproval NoApproval;

	// A door: an Actor Blueprint with a Health variable and a mesh component.
	const FString DoorPath = NewPath(TEXT("BP_Door"));
	const FString UsePath = NewPath(TEXT("BPI_Use"));
	UBlueprint* Door = NewActorBlueprint(DoorPath);
	FEdGraphPinType Float;
	Float.PinCategory = UEdGraphSchema_K2::PC_Real;
	Float.PinSubCategory = UEdGraphSchema_K2::PC_Double;
	FBlueprintEditorUtils::AddMemberVariable(Door, TEXT("Health"), Float);
	// ApplyOpen(Alpha: float), for the timeline to drive.
	UEdGraph* ApplyOpen = FBlueprintEditorUtils::CreateNewGraph(Door, TEXT("ApplyOpen"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Door, ApplyOpen, /*bIsUserCreated=*/true, nullptr);
	TArray<UK2Node_FunctionEntry*> Entries;
	ApplyOpen->GetNodesOfClass(Entries);
	Entries[0]->CreateUserDefinedPin(TEXT("Alpha"), Float, EGPD_Output);
	USCS_Node* Mesh = Door->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), TEXT("Mesh"));
	Door->SimpleConstructionScript->AddNode(Mesh);
	FKismetEditorUtilities::CompileBlueprint(Door);
	TArray<UObject*> Made = {Door};
	ON_SCOPE_EXIT
	{
		if (UBlueprint* Use = LoadObject<UBlueprint>(nullptr, *(UsePath + TEXT(".") + FPackageName::GetShortName(UsePath)), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Made.Add(Use);
		}
		Clean(Made);
	};

	FHyperAIBlueprintStructurePlanRequest Plan;
	Plan.BlueprintPath = DoorPath;
	FHyperAIBlueprintStructureOp Create = Op(TEXT("interface.create"));
	Create.Interface = UsePath;
	FHyperAIBlueprintStructureOp Interact = Op(TEXT("interface.add_function"), TEXT("Interact"));
	Interact.Interface = UsePath;
	Interact.Inputs = {Param(TEXT("Instigator"), TEXT("object:/Script/Engine.Actor"))};
	FHyperAIBlueprintStructureOp Score = Op(TEXT("interface.add_function"), TEXT("GetScore"));
	Score.Interface = UsePath;
	Score.Outputs = {Param(TEXT("Score"), TEXT("int"))};
	FHyperAIBlueprintStructureOp Implement = Op(TEXT("interface.implement"));
	Implement.Interface = UsePath;
	FHyperAIBlueprintStructureOp Timeline = Op(TEXT("timeline.add"), TEXT("Open"));
	Timeline.Length = 1.5f;
	Timeline.X = 400;
	FHyperAIBlueprintTimelineTrack& Alpha = Timeline.Tracks.AddDefaulted_GetRef();
	Alpha.Name = TEXT("Alpha");
	Alpha.Type = TEXT("float");
	Alpha.Interp = TEXT("linear");
	Alpha.Keys = {Key(0.f, {0.f}), Key(1.5f, {1.f})};
	FHyperAIBlueprintTimelineTrack& Glow = Timeline.Tracks.AddDefaulted_GetRef();
	Glow.Name = TEXT("Glow");
	Glow.Type = TEXT("color");
	Glow.Keys = {Key(0.f, {0.f, 0.f, 0.f, 1.f}), Key(1.5f, {1.f, 0.5f, 0.f, 1.f})};
	FHyperAIBlueprintTimelineTrack& Halfway = Timeline.Tracks.AddDefaulted_GetRef();
	Halfway.Name = TEXT("Halfway");
	Halfway.Type = TEXT("event");
	Halfway.Keys = {Key(0.75f, {})};
	Timeline.Drive = TEXT("ApplyOpen");
	FHyperAIBlueprintStructureOp Server = Op(TEXT("event.add"), TEXT("ServerOpen"));
	Server.Rpc = TEXT("server");
	Server.bReliable = true;
	Server.Inputs = {Param(TEXT("Speed"), TEXT("float"))};
	FHyperAIBlueprintStructureOp Health = Op(TEXT("variable.set_replication"), TEXT("Health"));
	Health.Replication = TEXT("rep_notify");
	Health.Condition = TEXT("owner_only");
	FHyperAIBlueprintStructureOp MeshReplicates = Op(TEXT("component.set_replicated"), TEXT("Mesh"));
	MeshReplicates.bReplicated = true;
	FHyperAIBlueprintStructureOp Wire = Op(TEXT("pins.connect"));
	Wire.From = TEXT("ServerOpen.then");
	Wire.To = TEXT("Open.Play");
	Plan.Operations = {Create, Interact, Score, Implement, Timeline, Server, Health, MeshReplicates, Wire};

	// A plan with a bad track changes nothing.
	FHyperAIBlueprintStructurePlanRequest Bad = Plan;
	Bad.Operations[4].Tracks[0].Type = TEXT("quaternion");
	const FHyperAIBlueprintStructurePlanReport Refused = C::ApplyPlan(Bad);
	TestTrue(TEXT("A plan with an unknown track type is refused before anything changes: ") + Refused.Diagnostic,
		!Refused.bOk && Refused.Status == TEXT("invalid_request") && Refused.Diagnostic.Contains(TEXT("Operation 4")));
	TestTrue(TEXT("and it made nothing"), !FPackageName::DoesPackageExist(UsePath) && !FindObject<UObject>(nullptr, *(UsePath + TEXT(".") + FPackageName::GetShortName(UsePath)))
		&& Door->Timelines.IsEmpty());

	const FHyperAIBlueprintStructurePlanReport Report = C::ApplyPlan(Plan);
	for (const FHyperAIBlueprintCompileMessage& Message : Report.CompileMessages)
	{
		AddInfo(FString::Printf(TEXT("%s %s: %s (%s)"), *Message.Severity, *Message.Blueprint, *Message.Message, *Message.Node));
	}
	for (const FString& Line : Report.Made)
	{
		AddInfo(Line);
	}
	if (!TestTrue(TEXT("The whole plan applies, compiles and saves: ") + Report.Diagnostic, Report.bOk && Report.Status == TEXT("completed") && Report.Applied == 9))
	{
		return false;
	}
	UBlueprint* Use = LoadObject<UBlueprint>(nullptr, *(UsePath + TEXT(".") + FPackageName::GetShortName(UsePath)));
	TestTrue(TEXT("The interface and the door were saved"), Use && Report.Saved.Contains(Use->GetPathName()) && Report.Saved.Contains(Door->GetPathName()));
	TestTrue(TEXT("The door's class implements the interface"), Use && Door->GeneratedClass->ImplementsInterface(Use->GeneratedClass));

	UTimelineTemplate* Open = Door->FindTimelineTemplateByVariableName(TEXT("Open"));
	TestTrue(TEXT("The timeline has its float, color and event tracks, keys and length"), Open && Open->FloatTracks.Num() == 1 && Open->LinearColorTracks.Num() == 1
		&& Open->EventTracks.Num() == 1 && Open->FloatTracks[0].CurveFloat->FloatCurve.GetNumKeys() == 2
		&& FMath::IsNearlyEqual(Open->FloatTracks[0].CurveFloat->FloatCurve.Eval(0.75f), 0.5f, 0.01f) && FMath::IsNearlyEqual(Open->TimelineLength, 1.5f));
	const UK2Node_Timeline* OpenNode = Open ? FBlueprintEditorUtils::FindNodeForTimeline(Door, Open) : nullptr;
	TestTrue(TEXT("and its node has a pin for each track"), OpenNode && OpenNode->FindPin(TEXT("Alpha")) && OpenNode->FindPin(TEXT("Glow")) && OpenNode->FindPin(TEXT("Halfway")));
	const UEdGraphPin* Update = OpenNode ? OpenNode->FindPin(TEXT("Update")) : nullptr;
	const UK2Node_CallFunction* Driven = Update && Update->LinkedTo.Num() == 1 ? Cast<UK2Node_CallFunction>(Update->LinkedTo[0]->GetOwningNode()) : nullptr;
	const UEdGraphPin* AlphaIn = Driven ? Driven->FindPin(TEXT("Alpha")) : nullptr;
	TestTrue(TEXT("Its Update runs ApplyOpen, fed the Alpha track"), Driven && Driven->FunctionReference.GetMemberName() == TEXT("ApplyOpen")
		&& AlphaIn && AlphaIn->LinkedTo.Num() == 1 && AlphaIn->LinkedTo[0]->GetOwningNode() == OpenNode);

	const UFunction* ServerOpen = Door->GeneratedClass->FindFunctionByName(TEXT("ServerOpen"));
	TestTrue(TEXT("ServerOpen compiles as a reliable server RPC"), ServerOpen && ServerOpen->HasAllFunctionFlags(FUNC_Net | FUNC_NetServer | FUNC_NetReliable));
	const UEdGraphPin* Play = OpenNode ? OpenNode->FindPin(TEXT("Play")) : nullptr;
	TestTrue(TEXT("and drives the timeline's Play"), Play && Play->LinkedTo.Num() == 1 && Cast<UK2Node_CustomEvent>(Play->LinkedTo[0]->GetOwningNode()));

	const FProperty* HealthProperty = Door->GeneratedClass->FindPropertyByName(TEXT("Health"));
	const int32 HealthIndex = FBlueprintEditorUtils::FindNewVariableIndex(Door, TEXT("Health"));
	TestTrue(TEXT("Health replicates to the owner only, with OnRep_Health"), HealthProperty && HealthProperty->HasAnyPropertyFlags(CPF_Net)
		&& HealthProperty->RepNotifyFunc == TEXT("OnRep_Health") && HealthIndex != INDEX_NONE && Door->NewVariables[HealthIndex].ReplicationCondition == COND_OwnerOnly);
	TestTrue(TEXT("The mesh component replicates"), Mesh->ComponentTemplate && Mesh->ComponentTemplate->GetIsReplicated());

	FHyperAIBlueprintStructureInspectRequest Look;
	Look.BlueprintPath = DoorPath;
	Look.bNodes = true;
	const FHyperAIBlueprintStructureInspectReport Inspected = C::Inspect(Look);
	TestTrue(TEXT("Inspect reads it all back"), Inspected.bOk && Inspected.Interfaces.Num() == 1 && Inspected.Interfaces[0].Contains(TEXT("Interact"))
		&& Inspected.Timelines.Num() == 1 && Inspected.Timelines[0].Contains(TEXT("Glow color"))
		&& Inspected.Events.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("ServerOpen(Speed: float): server, reliable")); })
		&& Inspected.ReplicatedVariables.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("rep_notify OnRep_Health, condition owner_only")); })
		&& Inspected.Components.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("Mesh")) && Line.Contains(TEXT("replicated")); })
		&& Inspected.TotalNodes > 0 && Inspected.CompileStatus != TEXT("error"));
	FHyperAIBlueprintStructureInspectRequest LookInterface;
	LookInterface.BlueprintPath = UsePath;
	const FHyperAIBlueprintStructureInspectReport Interface = C::Inspect(LookInterface);
	TestTrue(TEXT("and the interface's own functions, an event and a function"), Interface.bIsInterface
		&& Interface.Interfaces.Contains(TEXT("Interact(Instigator: object:/Script/Engine.Actor), an event")) && Interface.Interfaces.Contains(TEXT("GetScore() -> Score: int")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintStructureCollapseTest,
	"HyperAIStudio.BlueprintStructure.CollapseAndExpand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintStructureCollapseTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintStructureTests;
	using C = FHyperAIStudioBlueprintStructureContracts;
	const FNoApproval NoApproval;

	// BeginPlay -> Print String "Hello" -> Print String "World".
	const FString Path = NewPath(TEXT("BP_Greeter"));
	UBlueprint* Greeter = NewActorBlueprint(Path);
	TArray<UObject*> Made = {Greeter};
	ON_SCOPE_EXIT { Clean(Made); };
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Greeter);
	UK2Node_Event* BeginPlay = UBlueprintEditorLibrary::AddEventOverride(Greeter, TEXT("ReceiveBeginPlay"), FIntPoint(0, 0));
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	const FName PrintString = GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString);
	auto Print = [&](const int32 X, const TCHAR* Text)
	{
		UK2Node_CallFunction* Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(EventGraph, FVector2D(X, 0), EK2NewNodeFlags::None,
			[PrintString](UK2Node_CallFunction* New) { New->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(PrintString)); });
		Schema->TrySetDefaultValue(*Node->FindPin(TEXT("InString")), Text);
		return Node;
	};
	UK2Node_CallFunction* Hello = Print(300, TEXT("Hello"));
	UK2Node_CallFunction* World = Print(600, TEXT("World"));
	Schema->TryCreateConnection(BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then), Hello->GetExecPin());
	Schema->TryCreateConnection(Hello->GetThenPin(), World->GetExecPin());
	FKismetEditorUtilities::CompileBlueprint(Greeter);
	if (!TestTrue(TEXT("The greeter compiles to begin with"), Greeter->Status != BS_Error && CountCalls(EventGraph, PrintString) == 2))
	{
		return false;
	}

	auto Run = [&](FHyperAIBlueprintStructureOp Step)
	{
		FHyperAIBlueprintStructurePlanRequest Plan;
		Plan.BlueprintPath = Path;
		Plan.bSave = false;
		Plan.Operations = {MoveTemp(Step)};
		const FHyperAIBlueprintStructurePlanReport Report = C::ApplyPlan(Plan);
		for (const FString& Line : Report.Made)
		{
			AddInfo(Line);
		}
		for (const FHyperAIBlueprintCompileMessage& Message : Report.CompileMessages)
		{
			AddInfo(FString::Printf(TEXT("%s: %s (%s)"), *Message.Severity, *Message.Message, *Message.Node));
		}
		return Report;
	};

	// Into a function, and back.
	FHyperAIBlueprintStructureOp ToFunction = Op(TEXT("graph.collapse"), TEXT("SayHello"));
	ToFunction.Into = TEXT("function");
	ToFunction.Nodes = {Hello->GetName(), World->GetName()};
	const FHyperAIBlueprintStructurePlanReport Folded = Run(ToFunction);
	UEdGraph* SayHello = FindObject<UEdGraph>(Greeter, TEXT("SayHello"));
	TestTrue(TEXT("Two prints collapse into the function SayHello, which compiles: ") + Folded.Diagnostic, Folded.bOk && SayHello
		&& Greeter->FunctionGraphs.Contains(SayHello) && CountCalls(SayHello, PrintString) == 2 && CountCalls(EventGraph, PrintString) == 0
		&& CountCalls(EventGraph, TEXT("SayHello")) == 1);
	const UEdGraphPin* Then = BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then);
	TestTrue(TEXT("and BeginPlay now calls it"), Then && Then->LinkedTo.Num() == 1 && Cast<UK2Node_CallFunction>(Then->LinkedTo[0]->GetOwningNode())
		&& Cast<UK2Node_CallFunction>(Then->LinkedTo[0]->GetOwningNode())->FunctionReference.GetMemberName() == TEXT("SayHello"));
	if (!Then || Then->LinkedTo.IsEmpty())
	{
		return false;
	}

	FHyperAIBlueprintStructureOp OpenOut = Op(TEXT("graph.expand"));
	OpenOut.Nodes = {Then->LinkedTo[0]->GetOwningNode()->GetName()};
	const FHyperAIBlueprintStructurePlanReport Unfolded = Run(OpenOut);
	TestTrue(TEXT("Expanding the call puts both prints back in the event graph: ") + Unfolded.Diagnostic, Unfolded.bOk
		&& CountCalls(EventGraph, PrintString) == 2 && CountCalls(EventGraph, TEXT("SayHello")) == 0);
	auto BeginPlayRunsAPrint = [&]()
	{
		const UEdGraphPin* Out = BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then);
		const UK2Node_CallFunction* Next = Out && Out->LinkedTo.Num() == 1 ? Cast<UK2Node_CallFunction>(Out->LinkedTo[0]->GetOwningNode()) : nullptr;
		return Next && Next->FunctionReference.GetMemberName() == PrintString;
	};
	TestTrue(TEXT("and BeginPlay still runs them"), BeginPlayRunsAPrint());

	// Into a collapsed graph, and back.
	TArray<FString> Prints;
	for (const UEdGraphNode* Node : EventGraph->Nodes)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		if (Call && Call->FunctionReference.GetMemberName() == PrintString)
		{
			Prints.Add(Call->GetName());
		}
	}
	FHyperAIBlueprintStructureOp Ambiguous = Op(TEXT("graph.collapse"), TEXT("Greeting"));
	Ambiguous.Nodes = Prints;
	const FHyperAIBlueprintStructurePlanReport Refused = Run(Ambiguous);
	TestTrue(TEXT("A node name found in two graphs (the event graph and SayHello's) is refused, not guessed: ") + Refused.Diagnostic,
		!Refused.bOk && Refused.Diagnostic.Contains(TEXT("give Graph")));
	FHyperAIBlueprintStructureOp ToCollapsed = Ambiguous;
	ToCollapsed.Graph = TEXT("EventGraph");
	const FHyperAIBlueprintStructurePlanReport Collapsed = Run(ToCollapsed);
	UK2Node_Composite* Composite = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		Composite = Composite ? Composite : Cast<UK2Node_Composite>(Node);
	}
	TestTrue(TEXT("Into a collapsed graph named Greeting: ") + Collapsed.Diagnostic, Collapsed.bOk && Composite && Composite->BoundGraph
		&& Composite->BoundGraph->GetName() == TEXT("Greeting") && CountCalls(Composite->BoundGraph, PrintString) == 2 && CountCalls(EventGraph, PrintString) == 0);
	if (!Composite)
	{
		return false;
	}
	FHyperAIBlueprintStructureOp Uncollapse = Op(TEXT("graph.expand"));
	Uncollapse.Nodes = {Composite->GetName()};
	const FHyperAIBlueprintStructurePlanReport Uncollapsed = Run(Uncollapse);
	TestTrue(TEXT("and back out: ") + Uncollapsed.Diagnostic, Uncollapsed.bOk && CountCalls(EventGraph, PrintString) == 2 && BeginPlayRunsAPrint());

	// Into a macro.
	Prints.Reset();
	for (const UEdGraphNode* Node : EventGraph->Nodes)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		if (Call && Call->FunctionReference.GetMemberName() == PrintString)
		{
			Prints.Add(Call->GetName());
		}
	}
	FHyperAIBlueprintStructureOp ToMacro = Op(TEXT("graph.collapse"), TEXT("HelloMacro"));
	ToMacro.Into = TEXT("macro");
	ToMacro.Graph = TEXT("EventGraph");
	ToMacro.Nodes = Prints;
	const FHyperAIBlueprintStructurePlanReport Macro = Run(ToMacro);
	const UEdGraphNode* Instance = nullptr;
	for (const UEdGraphNode* Node : EventGraph->Nodes)
	{
		Instance = Instance ? Instance : (Node->IsA<UK2Node_MacroInstance>() ? Node : nullptr);
	}
	TestTrue(TEXT("Into a macro named HelloMacro, placed as an instance: ") + Macro.Diagnostic, Macro.bOk && Instance
		&& Greeter->MacroGraphs.ContainsByPredicate([](const UEdGraph* Graph) { return Graph->GetName() == TEXT("HelloMacro"); }));
	if (!Instance)
	{
		return false;
	}
	FHyperAIBlueprintStructureOp Unmacro = Op(TEXT("graph.expand"));
	Unmacro.Graph = TEXT("EventGraph");
	Unmacro.Nodes = {Instance->GetName()};
	const FHyperAIBlueprintStructurePlanReport Unmacroed = Run(Unmacro);
	TestTrue(TEXT("and the macro instance opens back out, still run by BeginPlay: ") + Unmacroed.Diagnostic, Unmacroed.bOk
		&& CountCalls(EventGraph, PrintString) == 2 && BeginPlayRunsAPrint());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
