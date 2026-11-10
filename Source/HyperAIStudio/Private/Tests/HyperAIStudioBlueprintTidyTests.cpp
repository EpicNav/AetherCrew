// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HyperAIStudioBlueprintTidy.h"
#include "HyperAIStudioSourceControl.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace HyperAIStudio::BlueprintTidyTests
{
	FBox2D Bounds(const UEdGraphNode* Node)
	{
		const FVector2D Min(Node->NodePosX, Node->NodePosY);
		return FBox2D(Min, Min + FVector2D(UEdGraphSchema_K2::EstimateNodeWidth(Node), UEdGraphSchema_K2::EstimateNodeHeight(const_cast<UEdGraphNode*>(Node))));
	}

	UBlueprint* MakeBlueprint(const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		return FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, *FPackageName::GetShortName(PackageName), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	/** Print String nodes chained from BeginPlay, dropped as a graph DSL drops them: one row, each lying on the next. */
	TArray<UK2Node_CallFunction*> DropChain(UBlueprint& Blueprint, const int32 Count)
	{
		TArray<UK2Node_CallFunction*> Chain;
		UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(&Blueprint);
		if (!Graph)
		{
			return Chain;
		}
		UK2Node_Event* BeginPlay = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			if (Event && Event->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"))
			{
				BeginPlay = Event;
			}
		}
		if (!BeginPlay)
		{
			return Chain;
		}
		UFunction* Print = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
		UEdGraphPin* Then = BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode();
			Node->SetFromFunction(Print);
			Node->NodePosX = BeginPlay->NodePosX + 100 * (Index + 1);
			Node->NodePosY = BeginPlay->NodePosY;
			Creator.Finalize();
			GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(Then, Node->GetExecPin());
			Then = Node->GetThenPin();
			Chain.Add(Node);
		}
		return Chain;
	}

	bool AnyOverlap(const TArray<UK2Node_CallFunction*>& Chain)
	{
		for (const UK2Node_CallFunction* Node : Chain)
		{
			for (const UEdGraphNode* Other : Node->GetGraph()->Nodes)
			{
				if (Other != Node && Bounds(Node).Intersect(Bounds(Other)))
				{
					return true;
				}
			}
		}
		return false;
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
	TestTrue(TEXT("Moving nodes left the compiled Blueprint up to date (only the package is dirty)"),
		Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	TestEqual(TEXT("A second tidy finds nothing new"), FHyperAIStudioBlueprintTidy::Tidy(Blueprint), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintTidyUneditedTest,
	"HyperAIStudio.BlueprintTidy.LeavesUneditedBlueprints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintTidyUneditedTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintTidyTests;
	// A Blueprint HyperAI never saw, its nodes lying on each other, compiled with nothing unsaved: what a recompile, or a
	// Blueprint loaded only in part for another asset, looks like. Saving the layout of one such crashed the editor.
	UBlueprint* Blueprint = MakeBlueprint(TEXT("/Temp/HyperAITests/BP_TidyUnedited_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const TArray<UK2Node_CallFunction*> Chain = Blueprint ? DropChain(*Blueprint, 3) : TArray<UK2Node_CallFunction*>();
	if (!TestEqual(TEXT("A test Blueprint is made with three nodes lying on each other"), Chain.Num(), 3) || !TestTrue(TEXT("overlapping"), AnyOverlap(Chain)))
	{
		return false;
	}
	TArray<FIntPoint> Before;
	for (const UK2Node_CallFunction* Node : Chain)
	{
		Before.Add(FIntPoint(Node->NodePosX, Node->NodePosY));
	}
	Blueprint->GetOutermost()->SetDirtyFlag(false);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	FTSTicker::GetCoreTicker().Tick(0.01f);
	bool bStill = true;
	for (int32 Index = 0; Index < Chain.Num(); ++Index)
	{
		bStill &= FIntPoint(Chain[Index]->NodePosX, Chain[Index]->NodePosY) == Before[Index];
	}
	TestTrue(TEXT("A compile that followed no edit moves none of them"), bStill);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBlueprintTidySavesLayoutTest,
	"HyperAIStudio.BlueprintTidy.SavesLayoutWithAgentSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBlueprintTidySavesLayoutTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BlueprintTidyTests;
	UBlueprint* Blueprint = MakeBlueprint(TEXT("/Game/HyperAITests/Tidy/BP_TidySave_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	if (!TestNotNull(TEXT("A test Blueprint is made"), Blueprint))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Blueprint);
	ON_SCOPE_EXIT
	{
		ObjectTools::ForceDeleteObjects(TArray<UObject*>{Blueprint}, /*ShowConfirmation=*/false);
	};
	FString Error;
	const bool bSaved = FHyperAIStudioSourceControl::SaveAsset(*Blueprint, Error);
	TestTrue(TEXT("and saved, as an agent's earlier work would be: ") + Error, bSaved);
	FHyperAIStudioBlueprintTidy::Remember(Blueprint);
	// An agent's edit: nodes dropped on each other, the package dirty, the Blueprint compiled, then saved straight away.
	const TArray<UK2Node_CallFunction*> Chain = DropChain(*Blueprint, 3);
	Blueprint->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	Error.Reset();
	const bool bAgentSaved = FHyperAIStudioSourceControl::SaveAsset(*Blueprint, Error);
	TestTrue(TEXT("The agent's save succeeds: ") + Error, bAgentSaved);
	FTSTicker::GetCoreTicker().Tick(0.01f);
	TestTrue(TEXT("On the next tick the new nodes are arranged"), Chain.Num() == 3 && !AnyOverlap(Chain));
	TestFalse(TEXT("and the layout is saved along with the agent's save"), Blueprint->GetOutermost()->IsDirty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
