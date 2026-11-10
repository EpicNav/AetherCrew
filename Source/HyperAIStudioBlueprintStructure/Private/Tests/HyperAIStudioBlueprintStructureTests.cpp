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
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
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
#include "Materials/Material.h"
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
	FHyperAIBlueprintStructureInspectRequest Search = Look;
	Search.Find = TEXT("timeline");
	const FHyperAIBlueprintStructureInspectReport Found = C::Inspect(Search);
	TestTrue(*FString::Printf(TEXT("Find keeps the timeline node and only matching ones (%d of %d)"), Found.TotalNodes, Inspected.TotalNodes),
		Found.bOk && Found.TotalNodes < Inspected.TotalNodes
		&& Found.Nodes.ContainsByPredicate([](const FHyperAIBlueprintNodeInfo& Node) { return Node.Class == TEXT("K2Node_Timeline"); })
		&& !Found.Nodes.ContainsByPredicate([](const FHyperAIBlueprintNodeInfo& Node)
		{
			return !Node.Name.Contains(TEXT("timeline")) && !Node.Title.Contains(TEXT("timeline")) && !Node.Class.Contains(TEXT("timeline"));
		}));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintStructureTypesTest,
	"HyperAIStudio.BlueprintStructure.StructsAndEnums",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintStructureTypesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintStructureTests;
	using C = FHyperAIStudioBlueprintStructureContracts;
	const FNoApproval NoApproval;
	const FString StatePath = NewPath(TEXT("E_State"));
	const FString WeaponPath = NewPath(TEXT("S_Weapon"));
	auto Find = [](const FString& Path) { return FindObject<UObject>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path))); };
	ON_SCOPE_EXIT { Clean({Find(WeaponPath), Find(StatePath)}); };
	auto Field = [](const TCHAR* Name, const TCHAR* Type, const TCHAR* Default = TEXT(""), const TCHAR* Tooltip = TEXT(""))
	{
		FHyperAIBlueprintStructField Out;
		Out.Name = Name;
		Out.Type = Type;
		Out.Default = Default;
		Out.Tooltip = Tooltip;
		return Out;
	};
	auto Run = [](TArray<FHyperAIBlueprintStructureOp> Ops, const bool bDryRun = false)
	{
		FHyperAIBlueprintStructurePlanRequest Plan;
		Plan.Operations = MoveTemp(Ops);
		Plan.bDryRun = bDryRun;
		return C::ApplyPlan(Plan);
	};
	auto Inspect = [](const FString& Path)
	{
		FHyperAIBlueprintStructureInspectRequest Look;
		Look.BlueprintPath = Path;
		return C::Inspect(Look);
	};

	// One plan: an enum, then a struct with a field of that enum, by the value's name as it reads.
	FHyperAIBlueprintStructureOp State = Op(TEXT("enum.create"));
	State.Asset = StatePath;
	State.Values = {TEXT("Idle"), TEXT("Chase"), TEXT("Attack")};
	FHyperAIBlueprintStructureOp Weapon = Op(TEXT("struct.create"));
	Weapon.Asset = WeaponPath;
	Weapon.Fields = {Field(TEXT("Name"), TEXT("name"), TEXT("Sword")), Field(TEXT("Damage"), TEXT("float"), TEXT("12.5"), TEXT("Damage per hit")),
		Field(TEXT("Mood"), *(TEXT("enum:") + StatePath + TEXT(".") + FPackageName::GetShortName(StatePath)), TEXT("Chase")),
		Field(TEXT("Offset"), TEXT("vector"), TEXT("1,2,3")), Field(TEXT("Tags"), TEXT("name[]"))};
	const FHyperAIBlueprintStructurePlanReport Made = Run({State, Weapon});
	if (!TestTrue(TEXT("An enum and a struct that uses it, in one plan: ") + Made.Diagnostic, Made.bOk && Made.Saved.Num() == 2))
	{
		return false;
	}
	const FHyperAIBlueprintStructureInspectReport Enum = Inspect(StatePath);
	TestTrue(TEXT("The enum reads its three values in order: ") + FString::Join(Enum.EnumValues, TEXT(", ")), Enum.bOk && Enum.AssetType == TEXT("enum")
		&& Enum.EnumValues.Num() == 3 && Enum.EnumValues[0].StartsWith(TEXT("0 Idle")) && Enum.EnumValues[2].StartsWith(TEXT("2 Attack")));
	const FHyperAIBlueprintStructureInspectReport Struct = Inspect(WeaponPath);
	const FString Fields = FString::Join(Struct.Fields, TEXT(" | "));
	TestTrue(TEXT("The struct has exactly its five fields, the starting bool gone: ") + Fields, Struct.bOk && Struct.AssetType == TEXT("struct")
		&& Struct.Fields.Num() == 5 && Struct.CompileStatus == TEXT("up_to_date"));
	TestTrue(TEXT("with their types, defaults and tooltip: ") + Fields, Struct.Fields.Num() == 5 && Struct.Fields[0] == TEXT("Name: name = Sword")
		&& Struct.Fields[1].StartsWith(TEXT("Damage: float = 12.5")) && Struct.Fields[1].EndsWith(TEXT("(Damage per hit)"))
		&& Struct.Fields[2].Contains(TEXT("= NewEnumerator1")) && Struct.Fields[3].StartsWith(TEXT("Offset: vector = "))
		&& Struct.Fields[4] == TEXT("Tags: name[]"));

	// A second plan edits both: a new field, a changed default, a rename and a removal; a value added, renamed, removed.
	FHyperAIBlueprintStructureOp AddSpeed = Op(TEXT("struct.add_field"));
	AddSpeed.Asset = WeaponPath;
	AddSpeed.Fields = {Field(TEXT("Speed"), TEXT("float"), TEXT("1.5"))};
	FHyperAIBlueprintStructureOp Harder = Op(TEXT("struct.set_field"), TEXT("Damage"));
	Harder.Asset = WeaponPath;
	Harder.Fields = {Field(TEXT(""), TEXT(""), TEXT("20"))};
	FHyperAIBlueprintStructureOp Rename = Op(TEXT("struct.rename_field"), TEXT("Damage"));
	Rename.Asset = WeaponPath;
	Rename.To = TEXT("BaseDamage");
	FHyperAIBlueprintStructureOp Drop = Op(TEXT("struct.remove_field"), TEXT("Offset"));
	Drop.Asset = WeaponPath;
	FHyperAIBlueprintStructureOp Dead = Op(TEXT("enum.add_value"));
	Dead.Asset = StatePath;
	Dead.Values = {TEXT("Dead")};
	FHyperAIBlueprintStructureOp Pursue = Op(TEXT("enum.rename_value"), TEXT("Chase"));
	Pursue.Asset = StatePath;
	Pursue.To = TEXT("Pursue");
	FHyperAIBlueprintStructureOp NoAttack = Op(TEXT("enum.remove_value"), TEXT("Attack"));
	NoAttack.Asset = StatePath;

	// Refused before anything changes: a field that is not there, and a name the struct has.
	FHyperAIBlueprintStructureOp Missing = Op(TEXT("struct.remove_field"), TEXT("Nope"));
	Missing.Asset = WeaponPath;
	const FHyperAIBlueprintStructurePlanReport Refused = Run({AddSpeed, Missing});
	TestTrue(TEXT("A missing field is refused by name: ") + Refused.Diagnostic, !Refused.bOk && Refused.Status == TEXT("invalid_request")
		&& Refused.Diagnostic.Contains(TEXT("Nope")) && Inspect(WeaponPath).Fields.Num() == 5);
	FHyperAIBlueprintStructureOp Twice = AddSpeed;
	Twice.Fields = {Field(TEXT("Name"), TEXT("string"))};
	TestFalse(TEXT("and so is a second field called Name"), Run({Twice}, /*bDryRun=*/true).bOk);

	const FHyperAIBlueprintStructurePlanReport Edited = Run({AddSpeed, Harder, Rename, Drop, Dead, Pursue, NoAttack});
	TestTrue(TEXT("The edit plan applies: ") + Edited.Diagnostic, Edited.bOk);
	const TArray<FString> AfterFields = Inspect(WeaponPath).Fields;
	const FString After = FString::Join(AfterFields, TEXT(" | "));
	TestTrue(TEXT("Speed added, Damage renamed BaseDamage at 20, Offset gone: ") + After, After.Contains(TEXT("Speed: float = 1.5"))
		&& After.Contains(TEXT("BaseDamage: float = 20")) && !After.Contains(TEXT("Offset"))
		&& !AfterFields.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Damage:")); }));
	const FHyperAIBlueprintStructureInspectReport Values = Inspect(StatePath);
	TestTrue(TEXT("Idle, Pursue, Dead: ") + FString::Join(Values.EnumValues, TEXT(", ")), Values.EnumValues.Num() == 3
		&& Values.EnumValues[0].StartsWith(TEXT("0 Idle")) && Values.EnumValues[1].StartsWith(TEXT("1 Pursue")) && Values.EnumValues[2].StartsWith(TEXT("2 Dead")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintStructureApprovalTest,
	"HyperAIStudio.BlueprintStructure.CheckedAgainOnApproval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintStructureApprovalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintStructureTests;
	// A held plan runs only once approved, and the editor can move on meanwhile: here an asset of another class takes the
	// new interface's path. The plan is checked again before anything changes, and refused.
	const FString DoorPath = NewPath(TEXT("BP_LateDoor"));
	const FString UsePath = NewPath(TEXT("BPI_Late"));
	const FString ObjectPath = UsePath + TEXT(".") + FPackageName::GetShortName(UsePath);
	UBlueprint* Door = NewActorBlueprint(DoorPath);
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	UObject* Squatter = nullptr;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		Clean({Squatter, FindObject<UBlueprint>(nullptr, *ObjectPath), Door});
	};
	Settings->bRequireApprovalForAgentEdits = true;
	FHyperAIBlueprintStructureOp Create = Op(TEXT("interface.create"));
	Create.Interface = UsePath;
	FHyperAIBlueprintStructurePlanRequest Plan;
	Plan.BlueprintPath = DoorPath;
	Plan.Operations = {Create};
	Plan.bDryRun = false;
	const FHyperAIBlueprintStructurePlanReport Held = FHyperAIStudioBlueprintStructureContracts::ApplyPlan(Plan);
	if (!TestEqual(TEXT("the new interface waits for approval: ") + Held.Diagnostic, Held.Status, FString(TEXT("awaiting_user_approval")))) return false;
	Squatter = NewObject<UMaterial>(CreatePackage(*UsePath), FName(*FPackageName::GetShortName(UsePath)), RF_Public | RF_Standalone);
	Plan.bDryRun = true;
	const FHyperAIBlueprintStructurePlanReport Checked = FHyperAIStudioBlueprintStructureContracts::ApplyPlan(Plan);
	TestTrue(TEXT("a plan checked now refuses the path, unsaved as the asset there is: ") + Checked.Diagnostic,
		!Checked.bOk && Checked.Diagnostic.Contains(TEXT("exists already")));
	FString Error;
	TestTrue(TEXT("the approval runs: ") + Error, FHyperAIStudioApprovalGate::Approve(Held.OperationId, Error));
	FString Said;
	for (const FHyperAIStudioActivityEntry& Entry : FHyperAIStudioAgentActivityLog::GetSnapshot())
	{
		if (Entry.OperationId == Held.OperationId && !Entry.Detail.IsEmpty())
		{
			Said = Entry.Detail;
			break;
		}
	}
	TestTrue(TEXT("and is checked again, refused for the path taken meanwhile: ") + Said,
		Said.Contains(TEXT("changed while the plan waited")) && Said.Contains(TEXT("exists already")));
	TestTrue(TEXT("the asset there is left alone"), FindObject<UMaterial>(nullptr, *ObjectPath) == Squatter);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
