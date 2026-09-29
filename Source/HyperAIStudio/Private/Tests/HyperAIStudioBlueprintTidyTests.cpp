// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HyperAIStudioBlueprintTidy.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

namespace HyperAIStudio::BlueprintTidyTests
{
	FBox2D Bounds(const UEdGraphNode* Node)
	{
		const FVector2D Min(Node->NodePosX, Node->NodePosY);
		return FBox2D(Min, Min + FVector2D(UEdGraphSchema_K2::EstimateNodeWidth(Node), UEdGraphSchema_K2::EstimateNodeHeight(const_cast<UEdGraphNode*>(Node))));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintTidyArrangeTest,
	"HyperAIStudio.BlueprintTidy.ArrangesAgentNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintTidyArrangeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintTidyTests;
	const FString Name = TEXT("BP_Tidy_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	UPackage* Package = CreatePackage(*(TEXT("/Temp/HyperAITests/") + Name));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, *Name, BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("A test Actor Blueprint is made"), Blueprint))
	{
		return false;
	}
	// What HyperAI remembers when a Blueprint is loaded or made: its default event nodes.
	FHyperAIStudioBlueprintTidy::Remember(Blueprint);
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	UK2Node_Event* BeginPlay = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
		if (Event && Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
		{
			BeginPlay = Event;
		}
	}
	if (!TestNotNull(TEXT("with BeginPlay"), BeginPlay))
	{
		return false;
	}
	const FIntPoint EventAt(BeginPlay->NodePosX, BeginPlay->NodePosY);

	// Four Print String nodes chained from BeginPlay, dropped the way a graph DSL drops them: one row, a fixed stride
	// narrower than the nodes, so each lies on the next.
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UFunction* Print = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
	TArray<UK2Node_CallFunction*> Chain;
	UEdGraphPin* Then = BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		UK2Node_CallFunction* Node = Creator.CreateNode();
		Node->SetFromFunction(Print);
		Node->NodePosX = EventAt.X + 100 * (Index + 1);
		Node->NodePosY = EventAt.Y;
		Creator.Finalize();
		Schema->TryCreateConnection(Then, Node->GetExecPin());
		Then = Node->GetThenPin();
		Chain.Add(Node);
	}
	TestTrue(TEXT("The nodes start out lying on each other"), Bounds(Chain[0]).Intersect(Bounds(Chain[1])));

	AddInfo(FString::Printf(TEXT("Estimated sizes: BeginPlay %.0fx%.0f, Print %.0fx%.0f"), UEdGraphSchema_K2::EstimateNodeWidth(BeginPlay),
		UEdGraphSchema_K2::EstimateNodeHeight(BeginPlay), UEdGraphSchema_K2::EstimateNodeWidth(Chain[0]), UEdGraphSchema_K2::EstimateNodeHeight(Chain[0])));
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	FTSTicker::GetCoreTicker().Tick(0.01f);

	for (int32 Index = 0; Index < Chain.Num(); ++Index)
	{
		for (UEdGraphNode* Other : Graph->Nodes)
		{
			if (Other != Chain[Index] && Bounds(Chain[Index]).Intersect(Bounds(Other)))
			{
				AddError(FString::Printf(TEXT("Print %d still lies on %s."), Index, *Other->GetNodeTitle(ENodeTitleType::ListView).ToString()));
			}
		}
	}
	bool bLeftToRight = Chain[0]->NodePosX > EventAt.X;
	for (int32 Index = 1; Index < Chain.Num(); ++Index)
	{
		bLeftToRight &= Chain[Index]->NodePosX > Chain[Index - 1]->NodePosX;
	}
	TestTrue(TEXT("After the compile the chain reads left to right from BeginPlay"), bLeftToRight);
	TestEqual(TEXT("and BeginPlay, which was there before, did not move"), FIntPoint(BeginPlay->NodePosX, BeginPlay->NodePosY), EventAt);
	TestEqual(TEXT("A second tidy finds nothing new"), FHyperAIStudioBlueprintTidy::Tidy(Blueprint), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
