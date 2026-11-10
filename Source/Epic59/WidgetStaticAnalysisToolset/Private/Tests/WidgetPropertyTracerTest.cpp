// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "WidgetPropertyTracer.h"
#include "WidgetStaticAnalysisToolset.h"
#include "WidgetConnectionMapperTestFixtures.h"
#include "WidgetStaticAnalysisSchemaTestFixtures.h"
#include "WidgetStaticAnalysisSchemaTestFlags.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/SlateWrapperTypes.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMPropertyPath.h"
#include "MVVMViewModelBase.h"
#include "Types/MVVMFieldVariant.h"
#include "UObject/SoftObjectPath.h"
#include "WidgetBlueprint.h"

BEGIN_DEFINE_SPEC(
	FWidgetPropertyTracerTest,
	"AIAssistant.Toolsets.WidgetStaticAnalysisToolset.PropertyTracer",
	WidgetStaticAnalysisSchemaTest::Flags)

UWidgetBlueprint* TestBP = nullptr;
int32 TestCounter = 0;

/** Adds a self-context UK2Node_VariableGet for VariableName (a widget variable, or any other
 *  self-context member, e.g. a plain bool gate variable feeding a branch condition). */
UK2Node_VariableGet* AddSelfVariableGetNode(UEdGraph* Graph, FName VariableName);

/** Adds a UK2Node_VariableSet targeting Property; caller links a widget Get's value pin to its
 *  PN_Self pin to make it a traced write. */
UK2Node_VariableSet* AddVariableSetNode(UEdGraph* Graph, const FProperty* Property);

/** Adds a UK2Node_VariableSet targeting a self-context member variable named VariableName (e.g. a
 *  widget-typed Blueprint variable "Solo") rather than an arbitrary FProperty. Used to build a
 *  type-correct "Solo = Other" style assignment, where the widget being tested is fed into the
 *  *value* pin, not the self/target pin. */
UK2Node_VariableSet* AddSelfVariableSetNode(UEdGraph* Graph, FName VariableName);

/** Finds a UK2Node_VariableSet's non-exec, non-self input pin: the pin carrying the value being
 *  assigned. */
UEdGraphPin* FindVariableSetValuePin(UK2Node_VariableSet* VariableSetNode);

/** Adds a UK2Node_CallFunction for Function; caller links a widget Get's value pin to its PN_Self
 *  pin (when the target function has one) to make it a traced write. */
UK2Node_CallFunction* AddCallFunctionNode(UEdGraph* Graph, UFunction* Function);

/** Adds a UK2Node_Event overriding EventFunctionName on OwnerClass (e.g. "PreConstruct",
 *  "Construct", "OnMouseButtonDown", all real UUserWidget override points), exec-wired by the
 *  caller via its PN_Then pin. */
UK2Node_Event* AddOverrideEventNode(UEdGraph* Graph, FName EventFunctionName, UClass* OwnerClass);

/** Adds a UK2Node_IfThenElse; caller wires its PN_Condition, exec-in, and PN_Then/PN_Else pins. */
UK2Node_IfThenElse* AddBranchNode(UEdGraph* Graph);

/** Declares a fresh bool member variable on TestBP (e.g. a branch-gating condition this tracer
 *  cannot constant-fold), returning true on success. */
bool AddBoolMemberVariable(FName VariableName);

/** Builds TestBP with WidgetA and WidgetB, each with one graph write to Visibility. */
void BuildTwoWidgetsWithVisibilityWrites();

END_DEFINE_SPEC(FWidgetPropertyTracerTest)

UK2Node_VariableGet* FWidgetPropertyTracerTest::AddSelfVariableGetNode(UEdGraph* Graph, FName VariableName)
{
	UK2Node_VariableGet* VarGet = NewObject<UK2Node_VariableGet>(Graph);
	VarGet->VariableReference.SetSelfMember(VariableName);
	VarGet->CreateNewGuid();
	Graph->AddNode(VarGet, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	VarGet->AllocateDefaultPins();
	return VarGet;
}

UK2Node_VariableSet* FWidgetPropertyTracerTest::AddVariableSetNode(UEdGraph* Graph, const FProperty* Property)
{
	UK2Node_VariableSet* VarSet = NewObject<UK2Node_VariableSet>(Graph);
	VarSet->VariableReference.SetFromField<FProperty>(Property, false);
	VarSet->CreateNewGuid();
	Graph->AddNode(VarSet, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	VarSet->AllocateDefaultPins();
	return VarSet;
}

UK2Node_VariableSet* FWidgetPropertyTracerTest::AddSelfVariableSetNode(UEdGraph* Graph, FName VariableName)
{
	UK2Node_VariableSet* VarSet = NewObject<UK2Node_VariableSet>(Graph);
	VarSet->VariableReference.SetSelfMember(VariableName);
	VarSet->CreateNewGuid();
	Graph->AddNode(VarSet, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	VarSet->AllocateDefaultPins();
	return VarSet;
}

UEdGraphPin* FWidgetPropertyTracerTest::FindVariableSetValuePin(UK2Node_VariableSet* VariableSetNode)
{
	for (UEdGraphPin* Pin : VariableSetNode->Pins)
	{
		if (Pin->Direction == EGPD_Input
			&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
			&& Pin->PinName != UEdGraphSchema_K2::PN_Self)
		{
			return Pin;
		}
	}
	return nullptr;
}

UK2Node_CallFunction* FWidgetPropertyTracerTest::AddCallFunctionNode(UEdGraph* Graph, UFunction* Function)
{
	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
	CallNode->SetFromFunction(Function);
	CallNode->CreateNewGuid();
	Graph->AddNode(CallNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	CallNode->AllocateDefaultPins();
	return CallNode;
}

UK2Node_Event* FWidgetPropertyTracerTest::AddOverrideEventNode(UEdGraph* Graph, FName EventFunctionName, UClass* OwnerClass)
{
	UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
	EventNode->EventReference.SetExternalMember(EventFunctionName, OwnerClass);
	EventNode->bOverrideFunction = true;
	EventNode->CreateNewGuid();
	Graph->AddNode(EventNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	EventNode->AllocateDefaultPins();
	return EventNode;
}

UK2Node_IfThenElse* FWidgetPropertyTracerTest::AddBranchNode(UEdGraph* Graph)
{
	UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Graph);
	Branch->CreateNewGuid();
	Graph->AddNode(Branch, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	Branch->AllocateDefaultPins();
	return Branch;
}

bool FWidgetPropertyTracerTest::AddBoolMemberVariable(FName VariableName)
{
	FEdGraphPinType BoolPinType;
	BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	return FBlueprintEditorUtils::AddMemberVariable(TestBP, VariableName, BoolPinType);
}

void FWidgetPropertyTracerTest::BuildTwoWidgetsWithVisibilityWrites()
{
	TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
	UVerticalBox* Root = TestBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
	UTextBlock* WidgetA = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("WidgetA"));
	UTextBlock* WidgetB = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("WidgetB"));
	// Both widgets must be in the tree; the widget compiler skips orphans.
	Root->AddChildToVerticalBox(WidgetA);
	Root->AddChildToVerticalBox(WidgetB);
	TestBP->WidgetTree->RootWidget = Root;
	WidgetA->bIsVariable = true;
	WidgetB->bIsVariable = true;
	FKismetEditorUtilities::CompileBlueprint(TestBP);

	UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
	if (!EventGraph)
	{
		return;
	}

	UK2Node_VariableGet* GetA = AddSelfVariableGetNode(EventGraph, TEXT("WidgetA"));
	UK2Node_VariableSet* VarSetA = AddVariableSetNode(EventGraph, WidgetA->GetClass()->FindPropertyByName(TEXT("Visibility")));
	VarSetA->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(GetA->GetValuePin());
	UK2Node_Event* MouseEvent = AddOverrideEventNode(EventGraph, TEXT("OnMouseButtonDown"), UUserWidget::StaticClass());
	MouseEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(VarSetA->FindPin(UEdGraphSchema_K2::PN_Execute));

	UK2Node_VariableGet* GetB = AddSelfVariableGetNode(EventGraph, TEXT("WidgetB"));
	UK2Node_CallFunction* CallNodeB = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
	CallNodeB->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(GetB->GetValuePin());
	UK2Node_Event* ConstructEvent = AddOverrideEventNode(EventGraph, TEXT("Construct"), UUserWidget::StaticClass());
	ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNodeB->FindPin(UEdGraphSchema_K2::PN_Execute));
}

void FWidgetPropertyTracerTest::Define()
{
	// TraceWrites
	Describe("TraceWrites: BFS write-location engine", [this]()
	{
		BeforeEach([this]()
		{
			WidgetStaticAnalysisSchemaTest::RegisterTestMountPoint();
		});
		AfterEach([this]()
		{
			TestBP = nullptr;
			WidgetStaticAnalysisSchemaTest::UnregisterTestMountPoint();
		});

		It("returns no writes for a widget blueprint with no graphs referencing any widget variable", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerEngine_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;

			const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(TestBP);
			TestEqual(TEXT("no writes found"), Writes.Num(), 0);
		});

		It("does not misattribute a write when the widget only feeds the assigned value, not the write's target", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerEngine_"), TestCounter);
			UVerticalBox* Root = TestBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			UTextBlock* Other = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Other"));
			Root->AddChildToVerticalBox(Text);
			Root->AddChildToVerticalBox(Other);
			TestBP->WidgetTree->RootWidget = Root;
			Text->bIsVariable = true;
			Other->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			// Other is the assigned value, not a write target.
			UK2Node_VariableGet* OtherGet = AddSelfVariableGetNode(EventGraph, TEXT("Other"));
			UK2Node_VariableSet* VarSet = AddSelfVariableSetNode(EventGraph, TEXT("Solo"));

			UEdGraphPin* ValuePin = FindVariableSetValuePin(VarSet);
			TestNotNull(TEXT("VariableSet has a value pin"), ValuePin);
			if (ValuePin)
			{
				ValuePin->MakeLinkTo(OtherGet->GetValuePin());
			}

			const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(TestBP);
			const bool bFoundWriteToOther = Writes.ContainsByPredicate([](const FTracedPropertyWrite& Write) { return Write.WidgetName == FName(TEXT("Other")); });
			TestFalse(TEXT("no write attributed to the assigned-in widget"), bFoundWriteToOther);
		});

		It("does not report writes reached through a self-context variable that is not a widget", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerEngine_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;

			FEdGraphPinType ObjectPinType;
			ObjectPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			ObjectPinType.PinSubCategoryObject = UObject::StaticClass();
			TestTrue(TEXT("non-widget member variable added"),
				FBlueprintEditorUtils::AddMemberVariable(TestBP, TEXT("CachedHelper"), ObjectPinType));

			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* HelperGet = AddSelfVariableGetNode(EventGraph, TEXT("CachedHelper"));
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			if (UEdGraphPin* SelfPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self))
			{
				SelfPin->MakeLinkTo(HelperGet->GetValuePin());
			}

			const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(TestBP);
			const bool bFoundWriteToHelper = Writes.ContainsByPredicate([](const FTracedPropertyWrite& Write) { return Write.WidgetName == FName(TEXT("CachedHelper")); });
			TestFalse(TEXT("no write attributed to the non-widget variable"), bFoundWriteToHelper);
		});

		It("does not report writes through a widget that is in the tree but not marked as a variable", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerEngine_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = false;

			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* SoloGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			if (UEdGraphPin* SelfPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self))
			{
				SelfPin->MakeLinkTo(SoloGet->GetValuePin());
			}

			const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(TestBP);
			const bool bFoundWriteToSolo = Writes.ContainsByPredicate([](const FTracedPropertyWrite& Write) { return Write.WidgetName == FName(TEXT("Solo")); });
			TestFalse(TEXT("no write attributed to the non-variable widget"), bFoundWriteToSolo);
		});

		It("detects a VariableSet write on a property with no BlueprintSetter metadata at all", [this]()
		{
			// No setter metadata, so VariableSet detection can't rely on the setter index.
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerEngine_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			const FProperty* VolatileProperty = UWidget::StaticClass()->FindPropertyByName(TEXT("bIsVolatile"));
			TestNotNull(TEXT("bIsVolatile property found"), VolatileProperty);
			TestFalse(TEXT("bIsVolatile has no BlueprintSetter metadata"),
				VolatileProperty && VolatileProperty->HasMetaData(FBlueprintMetadata::MD_PropertySetFunction));

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_VariableSet* VarSet = AddVariableSetNode(EventGraph, VolatileProperty);
			if (UEdGraphPin* SelfPin = VarSet->FindPin(UEdGraphSchema_K2::PN_Self))
			{
				SelfPin->MakeLinkTo(WidgetGet->GetValuePin());
			}

			const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(TestBP);
			TestEqual(TEXT("one write found"), Writes.Num(), 1);
			if (Writes.Num() == 1)
			{
				TestEqual(TEXT("property"), Writes[0].PropertyName, FName(TEXT("bIsVolatile")));
				TestEqual(TEXT("no function name"), Writes[0].FunctionName, NAME_None);
			}
		});

		It("resolves a setter-function-call write against the BlueprintSetter reverse index (SetVisibility)", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerEngine_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			Text->SetVisibility(ESlateVisibility::Hidden);
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UFunction* SetVisibilityFunction = UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility"));
			TestNotNull(TEXT("UWidget::SetVisibility exists"), SetVisibilityFunction);
			if (!SetVisibilityFunction)
			{
				return;
			}
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, SetVisibilityFunction);
			if (UEdGraphPin* SelfPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self))
			{
				SelfPin->MakeLinkTo(WidgetGet->GetValuePin());
			}

			const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(TestBP);
			TestEqual(TEXT("one write found"), Writes.Num(), 1);
			if (Writes.Num() == 1)
			{
				TestEqual(TEXT("function"), Writes[0].FunctionName, FName(TEXT("SetVisibility")));
				TestEqual(TEXT("property resolved via reverse index"), Writes[0].PropertyName, FName(TEXT("Visibility")));
				TestTrue(TEXT("exported value reflects the widget's archetype value"), Writes[0].ExportedValue.Contains(TEXT("Hidden")));
			}
		});

		It("leaves PropertyName unresolved for a function call with no matching BlueprintSetter metadata (ForceVolatile)", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerEngine_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UFunction* ForceVolatileFunction = UWidget::StaticClass()->FindFunctionByName(TEXT("ForceVolatile"));
			TestNotNull(TEXT("UWidget::ForceVolatile exists"), ForceVolatileFunction);
			if (!ForceVolatileFunction)
			{
				return;
			}
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, ForceVolatileFunction);
			if (UEdGraphPin* SelfPin = CallNode->FindPin(UEdGraphSchema_K2::PN_Self))
			{
				SelfPin->MakeLinkTo(WidgetGet->GetValuePin());
			}

			const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(TestBP);
			TestEqual(TEXT("one write found"), Writes.Num(), 1);
			if (Writes.Num() == 1)
			{
				TestEqual(TEXT("function name reported"), Writes[0].FunctionName, FName(TEXT("ForceVolatile")));
				TestEqual(TEXT("no property resolved, since ForceVolatile is not any property's BlueprintSetter"), Writes[0].PropertyName, NAME_None);
			}
		});

		It("produces no write at all for a SlotAsCanvasSlot-style call, since its widget argument is an ordinary input pin, not a self pin", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerEngine_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UFunction* SlotAsCanvasSlotFunction = UWidgetLayoutLibrary::StaticClass()->FindFunctionByName(TEXT("SlotAsCanvasSlot"));
			TestNotNull(TEXT("UWidgetLayoutLibrary::SlotAsCanvasSlot exists"), SlotAsCanvasSlotFunction);
			if (!SlotAsCanvasSlotFunction)
			{
				return;
			}
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, SlotAsCanvasSlotFunction);

			// Widget feeds the ordinary parameter pin, not the hidden self pin.
			UEdGraphPin* WidgetArgPin = CallNode->FindPin(TEXT("Widget"));
			TestNotNull(TEXT("Widget argument pin"), WidgetArgPin);
			if (WidgetArgPin)
			{
				WidgetArgPin->MakeLinkTo(WidgetGet->GetValuePin());
			}

			const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(TestBP);
			TestEqual(TEXT("no writes found, since a self-pin match structurally never fires"), Writes.Num(), 0);
		});

		It("records a CallFunction self-pin write even when the traced widget also feeds an ordinary argument pin of the same call, regardless of which pin the BFS reaches first", [this]()
		{
			// Same widget on self and argument pins; two nodes cover both arrival orders.
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerEngine_"), TestCounter);
			UWidgetStaticAnalysisTestNativeUserWidget* Solo = TestBP->WidgetTree->ConstructWidget<UWidgetStaticAnalysisTestNativeUserWidget>(UWidgetStaticAnalysisTestNativeUserWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Solo;
			Solo->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UFunction* RegisterSelfReferenceFunction = UWidgetStaticAnalysisTestNativeUserWidget::StaticClass()->FindFunctionByName(TEXT("RegisterSelfReference"));
			TestNotNull(TEXT("RegisterSelfReference exists"), RegisterSelfReferenceFunction);
			if (!RegisterSelfReferenceFunction)
			{
				return;
			}

			UK2Node_Event* MouseEvent = AddOverrideEventNode(EventGraph, TEXT("OnMouseButtonDown"), UUserWidget::StaticClass());
			UEdGraphPin* MouseEventExecPin = MouseEvent->FindPin(UEdGraphSchema_K2::PN_Then);

			UK2Node_VariableGet* ArgFirstGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* ArgFirstCall = AddCallFunctionNode(EventGraph, RegisterSelfReferenceFunction);
			UEdGraphPin* ArgFirstArgPin = ArgFirstCall->FindPin(TEXT("SelfReference"));
			UEdGraphPin* ArgFirstSelfPin = ArgFirstCall->FindPin(UEdGraphSchema_K2::PN_Self);
			TestNotNull(TEXT("Case 1 argument pin"), ArgFirstArgPin);
			TestNotNull(TEXT("Case 1 self pin"), ArgFirstSelfPin);
			if (ArgFirstArgPin && ArgFirstSelfPin)
			{
				ArgFirstArgPin->MakeLinkTo(ArgFirstGet->GetValuePin());
				ArgFirstSelfPin->MakeLinkTo(ArgFirstGet->GetValuePin());
			}
			MouseEventExecPin->MakeLinkTo(ArgFirstCall->FindPin(UEdGraphSchema_K2::PN_Execute));

			UK2Node_VariableGet* SelfFirstGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* SelfFirstCall = AddCallFunctionNode(EventGraph, RegisterSelfReferenceFunction);
			UEdGraphPin* SelfFirstSelfPin = SelfFirstCall->FindPin(UEdGraphSchema_K2::PN_Self);
			UEdGraphPin* SelfFirstArgPin = SelfFirstCall->FindPin(TEXT("SelfReference"));
			TestNotNull(TEXT("Case 2 self pin"), SelfFirstSelfPin);
			TestNotNull(TEXT("Case 2 argument pin"), SelfFirstArgPin);
			if (SelfFirstSelfPin && SelfFirstArgPin)
			{
				SelfFirstSelfPin->MakeLinkTo(SelfFirstGet->GetValuePin());
				SelfFirstArgPin->MakeLinkTo(SelfFirstGet->GetValuePin());
			}
			MouseEventExecPin->MakeLinkTo(SelfFirstCall->FindPin(UEdGraphSchema_K2::PN_Execute));

			const TArray<FTracedPropertyWrite> Writes = FWidgetPropertyTracer::TraceWrites(TestBP);
			const int32 ArgFirstWriteCount = Writes.FilterByPredicate([&ArgFirstCall](const FTracedPropertyWrite& Write) { return Write.NodeGuid == ArgFirstCall->NodeGuid; }).Num();
			const int32 SelfFirstWriteCount = Writes.FilterByPredicate([&SelfFirstCall](const FTracedPropertyWrite& Write) { return Write.NodeGuid == SelfFirstCall->NodeGuid; }).Num();
			TestEqual(TEXT("self-pin write recorded once even though the argument pin's worklist entry is reached first"), ArgFirstWriteCount, 1);
			TestEqual(TEXT("self-pin write still recorded once when the self pin's worklist entry is reached first"), SelfFirstWriteCount, 1);
		});
	});

	// TraceWidgetPropertyOverrides
	Describe("TraceWidgetPropertyOverrides: Chains/Winners/Flags", [this]()
	{
		BeforeEach([this]()
		{
			WidgetStaticAnalysisSchemaTest::RegisterTestMountPoint();
		});
		AfterEach([this]()
		{
			TestBP = nullptr;
			WidgetStaticAnalysisSchemaTest::UnregisterTestMountPoint();
		});

		It("a PreConstruct-reachable write reports Source=PreConstructSetter and ApplicationStatus=Applied", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			Text->SetVisibility(ESlateVisibility::Hidden);
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			CallNode->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(WidgetGet->GetValuePin());

			UK2Node_Event* PreConstructEvent = AddOverrideEventNode(EventGraph, TEXT("PreConstruct"), UUserWidget::StaticClass());
			PreConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP);
			TestEqual(TEXT("one Chains row"), Result.Chains.Num(), 1);
			if (Result.Chains.Num() == 1)
			{
				TestEqual(TEXT("Source=PreConstructSetter"), Result.Chains[0].Source, EWidgetPropertyAssignmentSource::PreConstructSetter);
				TestEqual(TEXT("ApplicationStatus=Applied"), Result.Chains[0].ApplicationStatus, EWidgetPropertyApplicationStatus::Applied);
			}
			TestEqual(TEXT("one Winner"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestTrue(TEXT("bCertain"), Result.Winners[0].bCertain);
				TestTrue(TEXT("effective value reflects the archetype's current value"), Result.Winners[0].EffectiveValue.Contains(TEXT("Hidden")));
			}
		});

		It("a Construct-reachable write reports Source=ConstructSetter, distinct from a PreConstruct-reachable write's PreConstructSetter above", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			CallNode->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(WidgetGet->GetValuePin());

			UK2Node_Event* ConstructEvent = AddOverrideEventNode(EventGraph, TEXT("Construct"), UUserWidget::StaticClass());
			ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP);
			TestEqual(TEXT("one Chains row"), Result.Chains.Num(), 1);
			if (Result.Chains.Num() == 1)
			{
				TestEqual(TEXT("Source=ConstructSetter"), Result.Chains[0].Source, EWidgetPropertyAssignmentSource::ConstructSetter);
			}
		});

		It("a write outside any lifecycle chain, reached unconditionally from a runtime event, reports Source=RuntimeHandler and ApplicationStatus=Applied", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			CallNode->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(WidgetGet->GetValuePin());

			UK2Node_Event* MouseEvent = AddOverrideEventNode(EventGraph, TEXT("OnMouseButtonDown"), UUserWidget::StaticClass());
			MouseEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP);
			TestEqual(TEXT("one Chains row"), Result.Chains.Num(), 1);
			if (Result.Chains.Num() == 1)
			{
				TestEqual(TEXT("Source=RuntimeHandler"), Result.Chains[0].Source, EWidgetPropertyAssignmentSource::RuntimeHandler);
				TestEqual(TEXT("ApplicationStatus=Applied"), Result.Chains[0].ApplicationStatus, EWidgetPropertyApplicationStatus::Applied);
			}
		});

		It("a write gated behind an unevaluated branch condition reports ApplicationStatus=DeclaredButUnconfirmed", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			TestTrue(TEXT("gate condition variable added"), AddBoolMemberVariable(TEXT("GateCondition")));
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			CallNode->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(WidgetGet->GetValuePin());

			UK2Node_VariableGet* ConditionGet = AddSelfVariableGetNode(EventGraph, TEXT("GateCondition"));
			UK2Node_IfThenElse* Branch = AddBranchNode(EventGraph);
			Branch->GetConditionPin()->MakeLinkTo(ConditionGet->GetValuePin());

			UK2Node_Event* ConstructEvent = AddOverrideEventNode(EventGraph, TEXT("Construct"), UUserWidget::StaticClass());
			ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(Branch->FindPin(UEdGraphSchema_K2::PN_Execute));
			Branch->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP);
			TestEqual(TEXT("one Chains row"), Result.Chains.Num(), 1);
			if (Result.Chains.Num() == 1)
			{
				TestEqual(TEXT("ApplicationStatus=DeclaredButUnconfirmed"), Result.Chains[0].ApplicationStatus, EWidgetPropertyApplicationStatus::DeclaredButUnconfirmed);
			}
			TestEqual(TEXT("one Winner, uncertain"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestFalse(TEXT("bCertain is false, since no Applied entry exists"), Result.Winners[0].bCertain);
			}
		});

		It("a write gated behind an unconnected, literal-false branch condition on its Then edge reports ApplicationStatus=DeclaredButUnconfirmed", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			CallNode->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(WidgetGet->GetValuePin());

			UK2Node_IfThenElse* Branch = AddBranchNode(EventGraph);
			Branch->GetConditionPin()->DefaultValue = TEXT("false");

			UK2Node_Event* ConstructEvent = AddOverrideEventNode(EventGraph, TEXT("Construct"), UUserWidget::StaticClass());
			ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(Branch->FindPin(UEdGraphSchema_K2::PN_Execute));
			Branch->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP);
			TestEqual(TEXT("one Chains row"), Result.Chains.Num(), 1);
			if (Result.Chains.Num() == 1)
			{
				TestEqual(TEXT("ApplicationStatus=DeclaredButUnconfirmed"), Result.Chains[0].ApplicationStatus, EWidgetPropertyApplicationStatus::DeclaredButUnconfirmed);
			}
			TestEqual(TEXT("one Winner, uncertain"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestFalse(TEXT("bCertain is false, since no Applied entry exists"), Result.Winners[0].bCertain);
			}
		});

		It("picks the highest-Order Applied write as the certain Winner, flags the earlier write Redundant and the later branch-gated write Conflict", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			Text->SetVisibility(ESlateVisibility::Hidden);
			TestTrue(TEXT("gate condition variable added"), AddBoolMemberVariable(TEXT("GateCondition")));
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			const FProperty* VisibilityProperty = TestBP->WidgetTree->FindWidget(TEXT("Solo"))->GetClass()->FindPropertyByName(TEXT("Visibility"));
			UFunction* SetVisibilityFunction = UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility"));

			UK2Node_VariableGet* Get0 = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_VariableSet* VarSet0 = AddVariableSetNode(EventGraph, VisibilityProperty);
			VarSet0->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(Get0->GetValuePin());

			UK2Node_VariableGet* Get1 = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* CallNode1 = AddCallFunctionNode(EventGraph, SetVisibilityFunction);
			CallNode1->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(Get1->GetValuePin());

			UK2Node_VariableGet* Get2 = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* CallNode2 = AddCallFunctionNode(EventGraph, SetVisibilityFunction);
			CallNode2->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(Get2->GetValuePin());
			UK2Node_VariableGet* ConditionGet = AddSelfVariableGetNode(EventGraph, TEXT("GateCondition"));
			UK2Node_IfThenElse* Branch = AddBranchNode(EventGraph);
			Branch->GetConditionPin()->MakeLinkTo(ConditionGet->GetValuePin());
			Branch->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode2->FindPin(UEdGraphSchema_K2::PN_Execute));

			UK2Node_Event* MouseEvent = AddOverrideEventNode(EventGraph, TEXT("OnMouseButtonDown"), UUserWidget::StaticClass());
			MouseEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(VarSet0->FindPin(UEdGraphSchema_K2::PN_Execute));

			UK2Node_Event* ConstructEvent = AddOverrideEventNode(EventGraph, TEXT("Construct"), UUserWidget::StaticClass());
			ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode1->FindPin(UEdGraphSchema_K2::PN_Execute));
			ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(Branch->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"));
			TestEqual(TEXT("three Chains rows"), Result.Chains.Num(), 3);
			if (Result.Chains.Num() != 3)
			{
				return;
			}

			TestEqual(TEXT("Order 0"), Result.Chains[0].Order, 0);
			TestEqual(TEXT("Order 0 Source"), Result.Chains[0].Source, EWidgetPropertyAssignmentSource::RuntimeHandler);
			TestEqual(TEXT("Order 0 Applied"), Result.Chains[0].ApplicationStatus, EWidgetPropertyApplicationStatus::Applied);

			TestEqual(TEXT("Order 1"), Result.Chains[1].Order, 1);
			TestEqual(TEXT("Order 1 Source"), Result.Chains[1].Source, EWidgetPropertyAssignmentSource::ConstructSetter);
			TestEqual(TEXT("Order 1 Applied"), Result.Chains[1].ApplicationStatus, EWidgetPropertyApplicationStatus::Applied);

			TestEqual(TEXT("Order 2"), Result.Chains[2].Order, 2);
			TestEqual(TEXT("Order 2 DeclaredButUnconfirmed"), Result.Chains[2].ApplicationStatus, EWidgetPropertyApplicationStatus::DeclaredButUnconfirmed);

			TestEqual(TEXT("one Winner"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestTrue(TEXT("bCertain"), Result.Winners[0].bCertain);
				TestEqual(TEXT("winning source is Order 1's"), Result.Winners[0].WinningSource, EWidgetPropertyAssignmentSource::ConstructSetter);
				TestEqual(TEXT("winning source detail is Order 1's"), Result.Winners[0].WinningSourceDetail, Result.Chains[1].SourceDetail);
			}

			TestEqual(TEXT("two Flags"), Result.Flags.Num(), 2);
			if (Result.Flags.Num() == 2)
			{
				const FWidgetPropertyOverrideFlag* RedundantFlag = Result.Flags.FindByPredicate(
					[](const FWidgetPropertyOverrideFlag& Flag) { return Flag.Kind == EWidgetPropertyOverrideFlagKind::Redundant; });
				const FWidgetPropertyOverrideFlag* ConflictFlag = Result.Flags.FindByPredicate(
					[](const FWidgetPropertyOverrideFlag& Flag) { return Flag.Kind == EWidgetPropertyOverrideFlagKind::Conflict; });
				TestNotNull(TEXT("Redundant flag present"), RedundantFlag);
				TestNotNull(TEXT("Conflict flag present"), ConflictFlag);
				if (RedundantFlag)
				{
					TestEqual(TEXT("Redundant names Order 0's location"), RedundantFlag->Location, Result.Chains[0].SourceDetail);
					TestFalse(TEXT("Redundant names a value"), RedundantFlag->ValueSet.IsEmpty());
					TestFalse(TEXT("Redundant carries a recommendation"), RedundantFlag->Recommendation.IsEmpty());
				}
				if (ConflictFlag)
				{
					TestEqual(TEXT("Conflict names Order 2's location"), ConflictFlag->Location, Result.Chains[2].SourceDetail);
					TestFalse(TEXT("Conflict names a value"), ConflictFlag->ValueSet.IsEmpty());
					TestFalse(TEXT("Conflict carries a recommendation"), ConflictFlag->Recommendation.IsEmpty());
				}
			}
		});

		It("reports an uncertain best-guess Winner and flags every other entry Conflict when no write is unconditionally Applied", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Uncertain"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			TestTrue(TEXT("gate condition variable added"), AddBoolMemberVariable(TEXT("GateCondition")));
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UFunction* SetVisibilityFunction = UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility"));
			UK2Node_VariableGet* Get0 = AddSelfVariableGetNode(EventGraph, TEXT("Uncertain"));
			UK2Node_CallFunction* CallNode0 = AddCallFunctionNode(EventGraph, SetVisibilityFunction);
			CallNode0->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(Get0->GetValuePin());

			UK2Node_VariableGet* Get1 = AddSelfVariableGetNode(EventGraph, TEXT("Uncertain"));
			UK2Node_CallFunction* CallNode1 = AddCallFunctionNode(EventGraph, SetVisibilityFunction);
			CallNode1->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(Get1->GetValuePin());

			UK2Node_VariableGet* ConditionGet = AddSelfVariableGetNode(EventGraph, TEXT("GateCondition"));
			UK2Node_IfThenElse* Branch = AddBranchNode(EventGraph);
			Branch->GetConditionPin()->MakeLinkTo(ConditionGet->GetValuePin());
			Branch->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode0->FindPin(UEdGraphSchema_K2::PN_Execute));
			Branch->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode1->FindPin(UEdGraphSchema_K2::PN_Execute));

			UK2Node_Event* ConstructEvent = AddOverrideEventNode(EventGraph, TEXT("Construct"), UUserWidget::StaticClass());
			ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(Branch->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"));
			TestEqual(TEXT("two Chains rows, both DeclaredButUnconfirmed"), Result.Chains.Num(), 2);
			for (const FWidgetPropertyOverrideEntry& Entry : Result.Chains)
			{
				TestEqual(TEXT("DeclaredButUnconfirmed"), Entry.ApplicationStatus, EWidgetPropertyApplicationStatus::DeclaredButUnconfirmed);
			}

			TestEqual(TEXT("one Winner"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestFalse(TEXT("bCertain is false"), Result.Winners[0].bCertain);
				TestTrue(TEXT("Rationale discloses the uncertainty"), Result.Winners[0].Rationale.Contains(TEXT("uncertain")));
			}

			// Uncertain winner isn't flagged against itself.
			TestEqual(TEXT("only the non-winner entry flagged Conflict"), Result.Flags.Num(), 1);
			if (Result.Flags.Num() == 1)
			{
				TestEqual(TEXT("Conflict"), Result.Flags[0].Kind, EWidgetPropertyOverrideFlagKind::Conflict);
				TestEqual(TEXT("Conflict names the non-winner entry's location"), Result.Flags[0].Location, Result.Chains[0].SourceDetail);
				TestTrue(TEXT("recommendation discloses the winner is itself only an uncertain guess"),
					Result.Flags[0].Recommendation.Contains(TEXT("uncertain best-guess winner")));
			}
		});

		It("a property with exactly one Chains entry gets a Winner but no Flags", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solitary"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solitary"));
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			CallNode->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(WidgetGet->GetValuePin());
			UK2Node_Event* MouseEvent = AddOverrideEventNode(EventGraph, TEXT("OnMouseButtonDown"), UUserWidget::StaticClass());
			MouseEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"));
			TestEqual(TEXT("one Chains row"), Result.Chains.Num(), 1);
			TestEqual(TEXT("one Winner"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestTrue(TEXT("bCertain"), Result.Winners[0].bCertain);
			}
			TestEqual(TEXT("no Flags for a single-entry property"), Result.Flags.Num(), 0);
		});

		It("keeps the same property name on two different widgets in one tree as two independent Winners groups, never flagging one widget's write Redundant/Conflict against another's", [this]()
		{
			BuildTwoWidgetsWithVisibilityWrites();

			// The panel root holding both widgets contributes its own NativeClassDefault group.
			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"));
			TestEqual(TEXT("three independent Winners groups, one per widget (Root, WidgetA, WidgetB)"), Result.Winners.Num(), 3);
			TestEqual(TEXT("three (widget, property) pairs traced"), Result.AvailablePropertyCount, 3);
			TestEqual(TEXT("three Chains rows, one per widget, each its own single-entry chain"), Result.Chains.Num(), 3);

			const auto ChainCount = [&Result](const TCHAR* WidgetName)
			{
				return Result.Chains.FilterByPredicate([WidgetName](const FWidgetPropertyOverrideEntry& Entry) { return Entry.WidgetName == FName(WidgetName); }).Num();
			};
			TestEqual(TEXT("WidgetA has its own single-entry chain"), ChainCount(TEXT("WidgetA")), 1);
			TestEqual(TEXT("WidgetB has its own single-entry chain"), ChainCount(TEXT("WidgetB")), 1);

			const FWidgetPropertyWinner* WinnerA = Result.Winners.FindByPredicate([](const FWidgetPropertyWinner& Winner) { return Winner.WidgetName == FName(TEXT("WidgetA")); });
			const FWidgetPropertyWinner* WinnerB = Result.Winners.FindByPredicate([](const FWidgetPropertyWinner& Winner) { return Winner.WidgetName == FName(TEXT("WidgetB")); });
			TestNotNull(TEXT("WidgetA has its own Winner"), WinnerA);
			TestNotNull(TEXT("WidgetB has its own Winner"), WinnerB);
			if (WinnerA && WinnerB)
			{
				TestTrue(TEXT("both winners are certain (each is its widget's sole, unconditional write)"), WinnerA->bCertain && WinnerB->bCertain);
				TestTrue(TEXT("WidgetA's winner names WidgetA"), WinnerA->WinningSourceDetail.Contains(TEXT("WidgetA")));
				TestTrue(TEXT("WidgetB's winner names WidgetB"), WinnerB->WinningSourceDetail.Contains(TEXT("WidgetB")));
			}
			TestEqual(TEXT("no Flags at all, since no widget's single-entry chain has anything to flag against another's"), Result.Flags.Num(), 0);
		});

		It("limits the trace to the widget named by WidgetNameFilter", [this]()
		{
			BuildTwoWidgetsWithVisibilityWrites();

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"), TEXT("WidgetB"));
			TestEqual(TEXT("one (widget, property) pair traced"), Result.AvailablePropertyCount, 1);
			TestEqual(TEXT("one Winner"), Result.Winners.Num(), 1);
			TestTrue(TEXT("every Chains row is WidgetB's"), Result.Chains.Num() > 0 && !Result.Chains.ContainsByPredicate(
				[](const FWidgetPropertyOverrideEntry& Entry) { return Entry.WidgetName != FName(TEXT("WidgetB")); }));
			if (Result.Winners.Num() == 1)
			{
				TestEqual(TEXT("the Winner is WidgetB's"), Result.Winners[0].WidgetName, FName(TEXT("WidgetB")));
			}
		});

		It("returns an empty result for a WidgetNameFilter that matches no widget", [this]()
		{
			BuildTwoWidgetsWithVisibilityWrites();

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"), TEXT("NoSuchWidget"));
			TestEqual(TEXT("nothing traced"), Result.AvailablePropertyCount, 0);
			TestEqual(TEXT("no Winners"), Result.Winners.Num(), 0);
			TestEqual(TEXT("no Chains"), Result.Chains.Num(), 0);
		});

		It("reports NativeClassDefault alone for an explicitly-filtered property that is never touched anywhere else (closes the former 8-bucket blind spot)", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"));
			TestEqual(TEXT("one Chains row"), Result.Chains.Num(), 1);
			if (Result.Chains.Num() == 1)
			{
				TestEqual(TEXT("Source=NativeClassDefault"), Result.Chains[0].Source, EWidgetPropertyAssignmentSource::NativeClassDefault);
				TestEqual(TEXT("ApplicationStatus=Applied"), Result.Chains[0].ApplicationStatus, EWidgetPropertyApplicationStatus::Applied);
				TestTrue(TEXT("native default is Visible"), Result.Chains[0].ExportedValue.Contains(TEXT("Visible")));
			}
			TestEqual(TEXT("one Winner"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestTrue(TEXT("bCertain"), Result.Winners[0].bCertain);
				TestEqual(TEXT("winning source is NativeClassDefault"), Result.Winners[0].WinningSource, EWidgetPropertyAssignmentSource::NativeClassDefault);
			}
			TestEqual(TEXT("no Flags for a single-entry property"), Result.Flags.Num(), 0);
		});

		It("reports NativeClassDefault and WidgetTreeArchetype (default sweep, no explicit filter) for a property overridden directly on the archetype with no Blueprint graph at all", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->SetVisibility(ESlateVisibility::Collapsed);

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP);
			const FWidgetPropertyWinner* VisibilityWinner = Result.Winners.FindByPredicate(
				[](const FWidgetPropertyWinner& Winner) { return Winner.PropertyName == TEXT("Visibility"); });
			TestNotNull(TEXT("Visibility traced (default sweep, no filter needed)"), VisibilityWinner);
			if (!VisibilityWinner)
			{
				return;
			}
			TestTrue(TEXT("bCertain"), VisibilityWinner->bCertain);
			TestEqual(TEXT("winning source is WidgetTreeArchetype"), VisibilityWinner->WinningSource, EWidgetPropertyAssignmentSource::WidgetTreeArchetype);
			TestTrue(TEXT("effective value is Collapsed"), VisibilityWinner->EffectiveValue.Contains(TEXT("Collapsed")));

			const FWidgetPropertyOverrideEntry* NativeEntry = Result.Chains.FindByPredicate([](const FWidgetPropertyOverrideEntry& Entry)
			{
				return Entry.PropertyName == TEXT("Visibility") && Entry.Source == EWidgetPropertyAssignmentSource::NativeClassDefault;
			});
			TestNotNull(TEXT("NativeClassDefault row present as the chain's baseline"), NativeEntry);
			if (NativeEntry)
			{
				TestTrue(TEXT("native default is Visible"), NativeEntry->ExportedValue.Contains(TEXT("Visible")));
			}
		});

		It("reports NamedSlotArchetype (not WidgetTreeArchetype) for a property overridden on a widget placed directly into a named slot", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Root = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Root"));
			TestBP->WidgetTree->RootWidget = Root;
			UTextBlock* SlotContent = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SlotContent"));
			SlotContent->SetVisibility(ESlateVisibility::Collapsed);
			TestBP->WidgetTree->SetContentForSlot(TEXT("MySlot"), SlotContent);

			// Whole-tree filter also yields a Root NativeClassDefault row.
			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"));
			const FWidgetPropertyOverrideEntry* NamedSlotEntry = Result.Chains.FindByPredicate([](const FWidgetPropertyOverrideEntry& Entry)
			{
				return Entry.Source == EWidgetPropertyAssignmentSource::NamedSlotArchetype;
			});
			TestNotNull(TEXT("NamedSlotArchetype row present"), NamedSlotEntry);
			if (NamedSlotEntry)
			{
				TestTrue(TEXT("names SlotContent"), NamedSlotEntry->SourceDetail.Contains(TEXT("SlotContent")));
				TestTrue(TEXT("value is Collapsed"), NamedSlotEntry->ExportedValue.Contains(TEXT("Collapsed")));
			}
			const bool bAnyWidgetTreeArchetypeRow = Result.Chains.ContainsByPredicate([](const FWidgetPropertyOverrideEntry& Entry)
			{
				return Entry.Source == EWidgetPropertyAssignmentSource::WidgetTreeArchetype;
			});
			TestFalse(TEXT("SlotContent's override classifies as NamedSlotArchetype, not WidgetTreeArchetype"), bAnyWidgetTreeArchetypeRow);
		});

		It("reports NativeClassDefault and GeneratedClassDefault for a widget whose own class is Blueprint-generated with an overridden class default", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UWidgetBlueprint* NestedBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4Nested_"), TestCounter);
			FKismetEditorUtilities::CompileBlueprint(NestedBP);
			TestNotNull(TEXT("NestedBP has a GeneratedClass"), NestedBP->GeneratedClass.Get());
			if (!NestedBP->GeneratedClass)
			{
				return;
			}

			CastChecked<UWidget>(NestedBP->GeneratedClass->GetDefaultObject())->SetVisibility(ESlateVisibility::Collapsed);

			UWidget* Nested = TestBP->WidgetTree->ConstructWidget<UUserWidget>(TSubclassOf<UUserWidget>(NestedBP->GeneratedClass), TEXT("Nested"));
			TestBP->WidgetTree->RootWidget = Nested;

			// Sync the instance, else a spurious WidgetTreeArchetype row appears.
			Nested->SetVisibility(ESlateVisibility::Collapsed);

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"));

			const FWidgetPropertyOverrideEntry* NativeEntry = Result.Chains.FindByPredicate([](const FWidgetPropertyOverrideEntry& Entry)
			{
				return Entry.Source == EWidgetPropertyAssignmentSource::NativeClassDefault;
			});
			TestNotNull(TEXT("NativeClassDefault row present"), NativeEntry);
			if (NativeEntry)
			{
				TestTrue(TEXT("native default is Visible"), NativeEntry->ExportedValue.Contains(TEXT("Visible")));
			}

			const FWidgetPropertyOverrideEntry* GeneratedEntry = Result.Chains.FindByPredicate([](const FWidgetPropertyOverrideEntry& Entry)
			{
				return Entry.Source == EWidgetPropertyAssignmentSource::GeneratedClassDefault;
			});
			TestNotNull(TEXT("GeneratedClassDefault row present"), GeneratedEntry);
			if (GeneratedEntry)
			{
				TestTrue(TEXT("generated default is Collapsed"), GeneratedEntry->ExportedValue.Contains(TEXT("Collapsed")));
			}
			TestEqual(TEXT("one Winner"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestTrue(TEXT("bCertain"), Result.Winners[0].bCertain);
				TestEqual(TEXT("winner is GeneratedClassDefault"), Result.Winners[0].WinningSource, EWidgetPropertyAssignmentSource::GeneratedClassDefault);
			}
		});

		It("does not crash and reports GeneratedClassDefault (no NativeClassDefault) for a Blueprint-only member variable on a composed child with no native storage for it", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UWidgetBlueprint* NestedBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4NestedVar_"), TestCounter);

			FEdGraphPinType TextPinType;
			TextPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			TestTrue(TEXT("Blueprint-only FText member variable added to NestedBP"),
				FBlueprintEditorUtils::AddMemberVariable(NestedBP, TEXT("BlueprintOnlyCaption"), TextPinType));

			FKismetEditorUtilities::CompileBlueprint(NestedBP);
			TestNotNull(TEXT("NestedBP has a GeneratedClass"), NestedBP->GeneratedClass.Get());
			if (!NestedBP->GeneratedClass)
			{
				return;
			}

			UWidget* Nested = TestBP->WidgetTree->ConstructWidget<UUserWidget>(TSubclassOf<UUserWidget>(NestedBP->GeneratedClass), TEXT("Nested"));
			TestBP->WidgetTree->RootWidget = Nested;

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("BlueprintOnlyCaption"));

			const bool bAnyNativeClassDefaultRow = Result.Chains.ContainsByPredicate([](const FWidgetPropertyOverrideEntry& Entry)
			{
				return Entry.Source == EWidgetPropertyAssignmentSource::NativeClassDefault;
			});
			TestFalse(TEXT("no NativeClassDefault row for a property with no storage on any native class"), bAnyNativeClassDefaultRow);

			const FWidgetPropertyOverrideEntry* GeneratedEntry = Result.Chains.FindByPredicate([](const FWidgetPropertyOverrideEntry& Entry)
			{
				return Entry.Source == EWidgetPropertyAssignmentSource::GeneratedClassDefault;
			});
			TestNotNull(TEXT("GeneratedClassDefault row present instead"), GeneratedEntry);
		});

		It("reports LegacyBinding for a property with a legacy Blueprint property binding and no Blueprint-graph write", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;

			FDelegateEditorBinding LegacyBinding;
			LegacyBinding.ObjectName = TEXT("Solo");
			LegacyBinding.PropertyName = TEXT("Text");
			LegacyBinding.FunctionName = TEXT("GetSoloText");
			LegacyBinding.Kind = EBindingKind::Function;
			TestBP->Bindings.Add(LegacyBinding);

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Text"));
			const FWidgetPropertyOverrideEntry* LegacyEntry = Result.Chains.FindByPredicate([](const FWidgetPropertyOverrideEntry& Entry)
			{
				return Entry.Source == EWidgetPropertyAssignmentSource::LegacyBinding;
			});
			TestNotNull(TEXT("LegacyBinding row present"), LegacyEntry);
			if (LegacyEntry)
			{
				TestEqual(TEXT("Applied, since it is unconditionally active whenever declared"), LegacyEntry->ApplicationStatus, EWidgetPropertyApplicationStatus::Applied);
				TestTrue(TEXT("names the bound function"), LegacyEntry->SourceDetail.Contains(TEXT("GetSoloText")));
				TestTrue(TEXT("no static value, since it is computed at runtime"), LegacyEntry->ExportedValue.IsEmpty());
			}
		});

		It("reports MVVMBinding with ApplicationStatus=DeclaredButUnconfirmed for an enabled MVVM field binding and no Blueprint-graph write", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;

			UMVVMEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* IntDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("IntDestination"));
			TestNotNull(TEXT("TestField property exists"), TestFieldProperty);
			TestNotNull(TEXT("IntDestination property exists"), IntDestinationProperty);
			if (!TestFieldProperty || !IntDestinationProperty)
			{
				return;
			}

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(IntDestinationProperty));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("IntDestination"));
			TestEqual(TEXT("one Chains row"), Result.Chains.Num(), 1);
			if (Result.Chains.Num() == 1)
			{
				TestEqual(TEXT("Source=MVVMBinding"), Result.Chains[0].Source, EWidgetPropertyAssignmentSource::MVVMBinding);
				TestEqual(TEXT("ApplicationStatus=DeclaredButUnconfirmed"), Result.Chains[0].ApplicationStatus, EWidgetPropertyApplicationStatus::DeclaredButUnconfirmed);
				TestTrue(TEXT("names the source ViewModel field"), Result.Chains[0].SourceDetail.Contains(TEXT("TestField")));
			}
		});

		It("still reports an enabled MVVMBinding row alongside a coexisting ConstructSetter write, ordered and flagged Conflict rather than Redundant", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;
			Widget->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UMVVMEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* IntDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("IntDestination"));
			TestNotNull(TEXT("TestField property exists"), TestFieldProperty);
			TestNotNull(TEXT("IntDestination property exists"), IntDestinationProperty);
			if (!TestFieldProperty || !IntDestinationProperty)
			{
				return;
			}

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(IntDestinationProperty));

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_VariableSet* VarSet = AddVariableSetNode(EventGraph, IntDestinationProperty);
			VarSet->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(WidgetGet->GetValuePin());
			UK2Node_Event* ConstructEvent = AddOverrideEventNode(EventGraph, TEXT("Construct"), UUserWidget::StaticClass());
			ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(VarSet->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("IntDestination"));
			TestEqual(TEXT("two Chains rows: the graph write and the MVVM binding, neither dropping the other"), Result.Chains.Num(), 2);
			if (Result.Chains.Num() == 2)
			{
				TestEqual(TEXT("Order 0 is the ConstructSetter write"), Result.Chains[0].Source, EWidgetPropertyAssignmentSource::ConstructSetter);
				TestEqual(TEXT("Order 0 Applied"), Result.Chains[0].ApplicationStatus, EWidgetPropertyApplicationStatus::Applied);
				TestEqual(TEXT("Order 1 is the MVVM binding"), Result.Chains[1].Source, EWidgetPropertyAssignmentSource::MVVMBinding);
				TestEqual(TEXT("Order 1 DeclaredButUnconfirmed"), Result.Chains[1].ApplicationStatus, EWidgetPropertyApplicationStatus::DeclaredButUnconfirmed);
			}

			TestEqual(TEXT("one Winner"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestTrue(TEXT("bCertain, since the ConstructSetter write is the certain Winner"), Result.Winners[0].bCertain);
				TestEqual(TEXT("winning source is the ConstructSetter write"), Result.Winners[0].WinningSource, EWidgetPropertyAssignmentSource::ConstructSetter);
			}

			TestEqual(TEXT("one Flags row: the MVVM binding, since it comes after the winner"), Result.Flags.Num(), 1);
			if (Result.Flags.Num() == 1)
			{
				TestEqual(TEXT("Conflict, not Redundant, since the MVVM binding could still silently override the winner"), Result.Flags[0].Kind, EWidgetPropertyOverrideFlagKind::Conflict);
			}
		});

		It("flags a disabled MVVM binding's Conflict row as statically inert rather than claiming genuine runtime uncertainty", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;
			Widget->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UMVVMEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* IntDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("IntDestination"));
			TestNotNull(TEXT("TestField property exists"), TestFieldProperty);
			TestNotNull(TEXT("IntDestination property exists"), IntDestinationProperty);
			if (!TestFieldProperty || !IntDestinationProperty)
			{
				return;
			}

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(IntDestinationProperty));
			Binding.bEnabled = false;

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_VariableSet* VarSet = AddVariableSetNode(EventGraph, IntDestinationProperty);
			VarSet->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(WidgetGet->GetValuePin());
			UK2Node_Event* ConstructEvent = AddOverrideEventNode(EventGraph, TEXT("Construct"), UUserWidget::StaticClass());
			ConstructEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(VarSet->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("IntDestination"));
			TestEqual(TEXT("two Chains rows"), Result.Chains.Num(), 2);
			if (Result.Chains.Num() == 2)
			{
				TestEqual(TEXT("Order 1 is the disabled MVVM binding"), Result.Chains[1].Source, EWidgetPropertyAssignmentSource::MVVMBinding);
				TestEqual(TEXT("Order 1 KnownNotApplied"), Result.Chains[1].ApplicationStatus, EWidgetPropertyApplicationStatus::KnownNotApplied);
			}

			TestEqual(TEXT("one Winner, certain"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestTrue(TEXT("bCertain, since the ConstructSetter write is the certain Winner"), Result.Winners[0].bCertain);
			}

			TestEqual(TEXT("one Flags row for the disabled binding"), Result.Flags.Num(), 1);
			if (Result.Flags.Num() == 1)
			{
				TestTrue(TEXT("recommendation says no action needed, not that it might silently override the winner"),
					Result.Flags[0].Recommendation.Contains(TEXT("No action needed")));
				TestFalse(TEXT("recommendation does not ask the caller to confirm a runtime push that can't happen"),
					Result.Flags[0].Recommendation.Contains(TEXT("actually populated and pushes a value at runtime")));
			}
		});

		It("does not add a static-origin row for a (Widget, Property) pair already covered by a Blueprint-graph write (disclosed narrowing)", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			// Also overridden on the archetype.
			Text->SetVisibility(ESlateVisibility::Collapsed);
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* WidgetGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* CallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			CallNode->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(WidgetGet->GetValuePin());
			UK2Node_Event* MouseEvent = AddOverrideEventNode(EventGraph, TEXT("OnMouseButtonDown"), UUserWidget::StaticClass());
			MouseEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(CallNode->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"));
			TestEqual(TEXT("exactly one Chains row: the Blueprint-graph write only"), Result.Chains.Num(), 1);
			if (Result.Chains.Num() == 1)
			{
				TestEqual(TEXT("Source=RuntimeHandler"), Result.Chains[0].Source, EWidgetPropertyAssignmentSource::RuntimeHandler);
			}
		});

		It("PropertyNameFilter, when non-empty, traces exactly the named property", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_TracerFR4_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;
			Text->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* VisibilityGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_CallFunction* VisibilityCallNode = AddCallFunctionNode(EventGraph, UWidget::StaticClass()->FindFunctionByName(TEXT("SetVisibility")));
			VisibilityCallNode->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(VisibilityGet->GetValuePin());
			UK2Node_Event* MouseEvent = AddOverrideEventNode(EventGraph, TEXT("OnMouseButtonDown"), UUserWidget::StaticClass());
			MouseEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(VisibilityCallNode->FindPin(UEdGraphSchema_K2::PN_Execute));

			UK2Node_VariableGet* VolatileGet = AddSelfVariableGetNode(EventGraph, TEXT("Solo"));
			UK2Node_VariableSet* VolatileSet = AddVariableSetNode(EventGraph, UWidget::StaticClass()->FindPropertyByName(TEXT("bIsVolatile")));
			VolatileSet->FindPin(UEdGraphSchema_K2::PN_Self)->MakeLinkTo(VolatileGet->GetValuePin());
			MouseEvent->FindPin(UEdGraphSchema_K2::PN_Then)->MakeLinkTo(VolatileSet->FindPin(UEdGraphSchema_K2::PN_Execute));

			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Visibility"));
			TestEqual(TEXT("one Winner"), Result.Winners.Num(), 1);
			if (Result.Winners.Num() == 1)
			{
				TestEqual(TEXT("filtered to Visibility"), Result.Winners[0].PropertyName, FString(TEXT("Visibility")));
			}
			for (const FWidgetPropertyOverrideEntry& Entry : Result.Chains)
			{
				TestEqual(TEXT("every Chains row is Visibility"), Entry.PropertyName, FString(TEXT("Visibility")));
			}
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
