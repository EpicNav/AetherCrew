// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "WidgetStaticAnalysisToolset.h"
#include "DescribeWidgetTestFixtures.h"
#include "WidgetDescriptionRenderer.h"
#include "WidgetPropertyReflectionUtils.h"
#include "WidgetStaticAnalysisSchemaTestFlags.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/ComboBoxString.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Misc/AutomationTest.h"
#include "Styling/SlateColor.h"
#include "Templates/UniquePtr.h"
#include "ToolsetRegistry/Tests/TestAssetSandbox.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintOperationUtils.h" // UE 5.8: 5.9 moved these helpers to UWidgetBlueprintEditorSubsystem

BEGIN_DEFINE_SPEC(
	FDescribeWidgetTest,
	"AIAssistant.Toolsets.WidgetStaticAnalysisToolset.Description",
	WidgetStaticAnalysisSchemaTest::Flags)

UWidgetBlueprint* TestBP = nullptr;
int32 TestCounter = 0;
TUniquePtr<FTestAssetSandbox> Sandbox;

/** Root(Border) -> Stack(VerticalBox) -> [First(TextBlock), Second(TextBlock)]. */
void BuildFixture();

/** Root(VerticalBox) -> Host(slot host) -> named slot "Content" holds Slotted(Border) -> Inner(TextBlock). */
void BuildNamedSlotFixture();

/** Builds a WidgetBlueprint whose ParentClass is UDescribeWidgetTestParentWidget (declares an
 *  "InheritedText" UPROPERTY), so a child-tree widget of that same name exercises bInherited=true. */
UWidgetBlueprint* MakeWidgetBlueprintWithTestParentClass(const TCHAR* NamePrefix, int32& InOutCounter);

END_DEFINE_SPEC(FDescribeWidgetTest)

void FDescribeWidgetTest::BuildFixture()
{
	TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_DescribeWidget_"), TestCounter, Sandbox->Folder());

	UBorder* Root = TestBP->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Root"));
	UVerticalBox* Stack = TestBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Stack"));
	UTextBlock* First = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("First"));
	UTextBlock* Second = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Second"));

	Root->SetContent(Stack);
	Stack->AddChildToVerticalBox(First);
	Stack->AddChildToVerticalBox(Second);

	TestBP->WidgetTree->RootWidget = Root;
}

void FDescribeWidgetTest::BuildNamedSlotFixture()
{
	TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_NamedSlot_"), TestCounter, Sandbox->Folder());

	UVerticalBox* Root = TestBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
	UDescribeWidgetTestSlotHostWidget* Host = TestBP->WidgetTree->ConstructWidget<UDescribeWidgetTestSlotHostWidget>(
		UDescribeWidgetTestSlotHostWidget::StaticClass(), TEXT("Host"));
	UBorder* Slotted = TestBP->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Slotted"));
	UTextBlock* Inner = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Inner"));

	Root->AddChildToVerticalBox(Host);
	Host->SlotContent = Slotted;
	Slotted->SetContent(Inner);

	TestBP->WidgetTree->RootWidget = Root;
}

UWidgetBlueprint* FDescribeWidgetTest::MakeWidgetBlueprintWithTestParentClass(const TCHAR* NamePrefix, int32& InOutCounter)
{
	const FString Name = FString::Printf(TEXT("%s%d"), NamePrefix, InOutCounter++);
	const FString PackagePath = Sandbox->Folder() / Name;
	UPackage* Package = CreatePackage(*PackagePath);

	return FWidgetBlueprintOperationUtils::CreateWidgetBlueprint( // UE 5.8: 5.8 name of the 5.9 helper
		Package, FName(*Name), BPTYPE_Normal,
		UDescribeWidgetTestParentWidget::StaticClass(), nullptr, NAME_None, /*bRegisterAndCompile=*/false);
}

void FDescribeWidgetTest::Define()
{
	// One sandbox for the whole spec; every blueprint gets a unique name from TestCounter.
	BeforeEach([this]() // UE 5.8: 5.8 specs have no BeforeAll; one sandbox per test
	{
		Sandbox = MakeUnique<FTestAssetSandbox>(WidgetStaticAnalysisSchemaTest::SandboxName);
	});

	AfterEach([this]() // UE 5.8: 5.8 specs have no AfterAll
	{
		Sandbox.Reset();
	});

	Describe("Large trees", [this]()
	{
		AfterEach([this]()
		{
			TestBP = nullptr;
		});

		It("returns every widget in a large tree and reports the whole-result count", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_LargeTree_"), TestCounter, Sandbox->Folder());

			UVerticalBox* Stack = TestBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Stack"));
			TestBP->WidgetTree->RootWidget = Stack;

			// Real construction of 505 children (no approximation) -- WidgetTree::ConstructWidget is a
			// plain NewObject call with no Slate widget construction outside a live viewport, so this
			// is cheap even at this count. Total tree size = 1 (Stack) + 505 children = 506. Any cut
			// to an oversized response is applied downstream (TCS), not here.
			constexpr int32 NumChildren = 505;
			for (int32 Index = 0; Index < NumChildren; ++Index)
			{
				UTextBlock* Child = TestBP->WidgetTree->ConstructWidget<UTextBlock>(
					UTextBlock::StaticClass(), *FString::Printf(TEXT("Text_%d"), Index));
				Stack->AddChildToVerticalBox(Child);
			}

			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(TestBP);

			TestEqual(TEXT("every widget is returned"), Result.Widgets.Num(), 506);
			TestEqual(TEXT("AvailableWidgetCount reports the whole result"), Result.AvailableWidgetCount, 506);

			if (Result.Widgets.IsEmpty()) return;

			// Stack=index0, Text_0..Text_504 fill index1..505: the last entry is a complete,
			// correctly-identified widget.
			TestEqual(TEXT("last entry is the last widget in the tree, fully identified"),
				Result.Widgets.Last().WidgetName, FName(TEXT("Text_504")));
			TestTrue(TEXT("last entry's class resolves"),
				Result.Widgets.Last().WidgetClassPath.IsValid());
		});
	});

	Describe("PropertyNameFilter narrowing", [this]()
	{
		BeforeEach([this]()
		{
			BuildFixture();
		});
		AfterEach([this]()
		{
			TestBP = nullptr;
		});

		It("narrows Properties and SlotProperties to the filter, even when other fields differ from default", [this]()
		{
			UTextBlock* First = Cast<UTextBlock>(TestBP->WidgetTree->FindWidget(FName(TEXT("First"))));
			TestNotNull(TEXT("First found"), First);
			if (!First) return;

			First->SetColorAndOpacity(FSlateColor(FLinearColor::Red)); // non-default, matches filter
			// Non-default, but does NOT match "ColorAndOpacity" -- must not appear in SlotProperties.
			UVerticalBoxSlot* Slot = Cast<UVerticalBoxSlot>(First->Slot);
			TestNotNull(TEXT("VBox slot"), Slot);
			if (Slot) Slot->SetPadding(FMargin(4, 0, 0, 0));

			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(
				TestBP, First, /*MaxDepth=*/0, /*PropertyNameFilter=*/TEXT("ColorAndOpacity"),
				/*bSkipDefaultValues=*/true, /*bIncludeFullDescription=*/false);

			TestEqual(TEXT("only First in scope (MaxDepth=0)"), Result.Widgets.Num(), 1);
			if (Result.Widgets.Num() != 1) return;

			const FWidgetDescriptionEntry& Entry = Result.Widgets[0];
			TestEqual(TEXT("Properties narrowed to exactly the matching property"), Entry.Properties.Num(), 1);
			if (Entry.Properties.Num() == 1)
			{
				TestEqual(TEXT("matching property is ColorAndOpacity"), Entry.Properties[0].PropertyName, FString(TEXT("ColorAndOpacity")));
			}
			TestEqual(TEXT("SlotProperties excludes Padding (does not match filter)"), Entry.SlotProperties.Num(), 0);
		});

		It("includes a still-default-valued property with a correct, non-empty ExportedValue when bSkipDefaultValues=false", [this]()
		{
			UTextBlock* First = Cast<UTextBlock>(TestBP->WidgetTree->FindWidget(FName(TEXT("First"))));
			TestNotNull(TEXT("First found"), First);
			if (!First) return;

			// First's ColorAndOpacity is left untouched at its class default -- the regression case:
			// bSkipDefaultValues=false is documented as including default-matching properties, which
			// requires ExportPropertyValue to actually format them instead of returning "".
			const FProperty* ColorProperty = First->GetClass()->FindPropertyByName(FName(TEXT("ColorAndOpacity")));
			TestNotNull(TEXT("ColorAndOpacity property found"), ColorProperty);
			if (!ColorProperty) return;
			const FString ExpectedValue = FWidgetPropertyReflectionUtils::ExportPropertyValue(ColorProperty, First);
			TestFalse(TEXT("sanity: the value this test compares against is itself non-empty"), ExpectedValue.IsEmpty());

			// No filter: ColorAndOpacity is looked up by name below, so the other properties emitted
			// alongside it do not matter.
			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(
				TestBP, First, /*MaxDepth=*/0, /*PropertyNameFilter=*/TEXT(""),
				/*bSkipDefaultValues=*/false, /*bIncludeFullDescription=*/false);

			TestEqual(TEXT("only First in scope (MaxDepth=0)"), Result.Widgets.Num(), 1);
			if (Result.Widgets.Num() != 1) return;

			const FWidgetDescriptionEntry& Entry = Result.Widgets[0];
			const FWidgetPropertyEntry* ColorEntry = Entry.Properties.FindByPredicate(
				[](const FWidgetPropertyEntry& PropertyEntry) { return PropertyEntry.PropertyName == TEXT("ColorAndOpacity"); });

			TestNotNull(TEXT("default-matching ColorAndOpacity still appears"), ColorEntry);
			if (!ColorEntry) return;

			TestFalse(TEXT("ExportedValue is not empty for a default-valued property"), ColorEntry->ExportedValue.IsEmpty());
			TestEqual(TEXT("ExportedValue matches ExportPropertyValue's own output"), ColorEntry->ExportedValue, ExpectedValue);
		});

		It("omits the Slot object path from default output", [this]()
		{
			UTextBlock* First = Cast<UTextBlock>(TestBP->WidgetTree->FindWidget(FName(TEXT("First"))));
			TestNotNull(TEXT("First found"), First);
			if (!First) return;

			// Default params: PropertyNameFilter=TEXT(""), bSkipDefaultValues=true.
			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(TestBP, First);

			TestTrue(TEXT("First is in scope"), Result.Widgets.Num() > 0);
			for (const FWidgetDescriptionEntry& Entry : Result.Widgets)
			{
				const bool bHasSlotProperty = Entry.Properties.ContainsByPredicate(
					[](const FWidgetPropertyEntry& PropertyEntry) { return PropertyEntry.PropertyName == TEXT("Slot"); });
				TestFalse(TEXT("Slot object path absent from Properties by default"), bHasSlotProperty);
			}
		});
	});

	Describe("bIncludeFullDescription toggle", [this]()
	{
		BeforeEach([this]()
		{
			BuildFixture();
		});
		AfterEach([this]()
		{
			TestBP = nullptr;
		});

		It("false leaves Description empty while Widgets stays fully populated", [this]()
		{
			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(
				TestBP, nullptr, -1, TEXT(""), true, /*bIncludeFullDescription=*/false);

			TestTrue(TEXT("Description empty"), Result.Description.IsEmpty());
			TestEqual(TEXT("Widgets still fully populated"), Result.Widgets.Num(), 4);
		});

		It("true populates Description with Widgets[N] <-> line[N] correlation", [this]()
		{
			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(
				TestBP, nullptr, -1, TEXT(""), true, /*bIncludeFullDescription=*/true);

			TestFalse(TEXT("Description non-empty"), Result.Description.IsEmpty());
			TestEqual(TEXT("Widgets still fully populated"), Result.Widgets.Num(), 4);

			TArray<FString> Lines;
			Result.Description.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);
			TestEqual(TEXT("one Description line per Widgets entry"), Lines.Num(), Result.Widgets.Num());
			if (Lines.Num() != Result.Widgets.Num()) return;

			for (int32 Index = 0; Index < Result.Widgets.Num(); ++Index)
			{
				const FWidgetDescriptionEntry& Entry = Result.Widgets[Index];
				const FString ExpectedPrefix = FString::Printf(TEXT("[%d] %s %s"),
					Index, *Entry.Widget->GetClass()->GetName(), *Entry.WidgetName.ToString());
				TestTrue(FString::Printf(TEXT("line[%d] correlates with Widgets[%d]"), Index, Index),
					Lines[Index].Contains(ExpectedPrefix));
			}
		});

		It("escapes line breaks inside a value so the line correlation survives", [this]()
		{
			// ComboBoxString::SelectedOption is a top-level FString, which exports unquoted.
			UComboBoxString* Combo = TestBP->WidgetTree->ConstructWidget<UComboBoxString>(UComboBoxString::StaticClass(), TEXT("Combo"));
			UVerticalBox* ComboStack = Cast<UVerticalBox>(TestBP->WidgetTree->FindWidget(TEXT("Stack")));
			TestNotNull(TEXT("Stack found"), ComboStack);
			if (!ComboStack) return;
			ComboStack->AddChildToVerticalBox(Combo);

			const FStrProperty* SelectedOptionProperty = FindFProperty<FStrProperty>(UComboBoxString::StaticClass(), TEXT("SelectedOption"));
			TestNotNull(TEXT("SelectedOption property found"), SelectedOptionProperty);
			if (!SelectedOptionProperty) return;
			SelectedOptionProperty->SetPropertyValue_InContainer(Combo, TEXT("FirstLine\nSecondLine\\nThird"));

			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(
				TestBP, nullptr, -1, TEXT(""), true, /*bIncludeFullDescription=*/true);

			TArray<FString> Lines;
			Result.Description.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);
			TestEqual(TEXT("one Description line per Widgets entry"), Lines.Num(), Result.Widgets.Num());
			TestTrue(TEXT("the line break is escaped on the Combo line"),
				Lines.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("FirstLine\\nSecondLine\\\\nThird")); }));

			const FWidgetDescriptionEntry* ComboEntry = Result.Widgets.FindByPredicate(
				[](const FWidgetDescriptionEntry& Entry) { return Entry.WidgetName == FName(TEXT("Combo")); });
			const FWidgetPropertyEntry* SelectedOption = ComboEntry ? ComboEntry->Properties.FindByPredicate(
				[](const FWidgetPropertyEntry& Property) { return Property.PropertyName == TEXT("SelectedOption"); }) : nullptr;
			TestNotNull(TEXT("SelectedOption entry found"), SelectedOption);
			if (SelectedOption) TestEqual(TEXT("structured value keeps the raw line break"), SelectedOption->ExportedValue, FString(TEXT("FirstLine\nSecondLine\\nThird")));
		});
	});

	Describe("Named-slot hosts", [this]()
	{
		BeforeEach([this]()
		{
			BuildNamedSlotFixture();
		});
		AfterEach([this]()
		{
			TestBP = nullptr;
		});

		It("reports Depth, Parent and NamedSlotHost for each entry of a whole-tree walk", [this]()
		{
			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(TestBP);
			TestEqual(TEXT("widget count"), Result.Widgets.Num(), 4);
			if (Result.Widgets.Num() != 4) return;

			const FWidgetDescriptionEntry& Root = Result.Widgets[0];
			const FWidgetDescriptionEntry& Host = Result.Widgets[1];
			const FWidgetDescriptionEntry& Slotted = Result.Widgets[2];
			const FWidgetDescriptionEntry& Inner = Result.Widgets[3];

			TestEqual(TEXT("walk order"), Slotted.WidgetName, FName(TEXT("Slotted")));
			TestEqual(TEXT("Root depth"), Root.Depth, 0);
			TestNull(TEXT("Root has no Parent"), Root.Parent.Get());
			TestNull(TEXT("Root has no NamedSlotHost"), Root.NamedSlotHost.Get());
			TestEqual(TEXT("Host depth"), Host.Depth, 1);
			TestEqual(TEXT("Host parent is Root"), Host.Parent.Get(), Cast<UPanelWidget>(Root.Widget.Get()));
			TestEqual(TEXT("Slotted depth"), Slotted.Depth, 2);
			TestNull(TEXT("Slotted has no Parent"), Slotted.Parent.Get());
			TestEqual(TEXT("Slotted host is Host"), Slotted.NamedSlotHost.Get(), Host.Widget.Get());
			TestEqual(TEXT("Inner depth"), Inner.Depth, 3);
			TestEqual(TEXT("Inner parent is Slotted"), Inner.Parent.Get(), Cast<UPanelWidget>(Slotted.Widget.Get()));
			TestNull(TEXT("Inner has no NamedSlotHost"), Inner.NamedSlotHost.Get());
		});

		It("finds the host when StartWidget is itself named-slot content", [this]()
		{
			UWidget* Slotted = TestBP->WidgetTree->FindWidget(TEXT("Slotted"));
			UWidget* Host = TestBP->WidgetTree->FindWidget(TEXT("Host"));

			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(TestBP, Slotted);
			TestEqual(TEXT("subtree widget count"), Result.Widgets.Num(), 2);
			if (Result.Widgets.IsEmpty()) return;

			TestEqual(TEXT("StartWidget is entry 0"), Result.Widgets[0].Widget.Get(), Slotted);
			TestEqual(TEXT("StartWidget depth resets to 0"), Result.Widgets[0].Depth, 0);
			TestEqual(TEXT("StartWidget host is found"), Result.Widgets[0].NamedSlotHost.Get(), Host);

			const FWidgetDescriptionResult FullResult = UWidgetStaticAnalysisToolset::DescribeWidget(
				TestBP, Slotted, -1, TEXT(""), true, /*bIncludeFullDescription=*/true);
			TArray<FString> Lines;
			FullResult.Description.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);
			TestFalse(TEXT("Description lines are produced"), Lines.IsEmpty());
			if (Lines.IsEmpty()) return;
			TestTrue(TEXT("StartWidget line names its slot"), Lines[0].Contains(TEXT("# named-slot:Content")));
		});

		It("leaves NamedSlotHost null when StartWidget is a panel child", [this]()
		{
			UWidget* Host = TestBP->WidgetTree->FindWidget(TEXT("Host"));

			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(TestBP, Host, /*MaxDepth=*/0);
			TestEqual(TEXT("MaxDepth 0 returns only StartWidget"), Result.Widgets.Num(), 1);
			if (Result.Widgets.IsEmpty()) return;

			TestNull(TEXT("no NamedSlotHost"), Result.Widgets[0].NamedSlotHost.Get());
			TestNotNull(TEXT("Parent still set"), Result.Widgets[0].Parent.Get());
		});

		It("tags named-slot content in the Description text", [this]()
		{
			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(
				TestBP, nullptr, -1, TEXT(""), true, /*bIncludeFullDescription=*/true);

			TArray<FString> Lines;
			Result.Description.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);
			TestEqual(TEXT("line count"), Lines.Num(), 4);
			if (Lines.Num() == 4) TestTrue(TEXT("Slotted line names its slot"), Lines[2].Contains(TEXT("# named-slot:Content")));
		});
	});

	Describe("bInherited correctness", [this]()
	{
		AfterEach([this]()
		{
			TestBP = nullptr;
		});

		It("is true for a widget whose name is already a property on the Blueprint's parent class", [this]()
		{
			TestBP = MakeWidgetBlueprintWithTestParentClass(TEXT("WBP_Inherited_"), TestCounter);
			TestNotNull(TEXT("TestBP created"), TestBP);
			if (!TestBP) return;

			UVerticalBox* Stack = TestBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Stack"));
			// Name matches UDescribeWidgetTestParentWidget::InheritedText.
			UTextBlock* Inherited = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("InheritedText"));
			// Name has no counterpart on the parent class.
			UTextBlock* Fresh = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("FreshText"));

			Stack->AddChildToVerticalBox(Inherited);
			Stack->AddChildToVerticalBox(Fresh);
			TestBP->WidgetTree->RootWidget = Stack;

			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(TestBP);

			const FWidgetDescriptionEntry* InheritedEntry = Result.Widgets.FindByPredicate(
				[](const FWidgetDescriptionEntry& Entry) { return Entry.WidgetName == FName(TEXT("InheritedText")); });
			const FWidgetDescriptionEntry* FreshEntry = Result.Widgets.FindByPredicate(
				[](const FWidgetDescriptionEntry& Entry) { return Entry.WidgetName == FName(TEXT("FreshText")); });

			TestNotNull(TEXT("InheritedText entry found"), InheritedEntry);
			TestNotNull(TEXT("FreshText entry found"), FreshEntry);
			if (InheritedEntry) TestTrue(TEXT("InheritedText is bInherited=true"), InheritedEntry->bInherited);
			if (FreshEntry) TestFalse(TEXT("FreshText is bInherited=false"), FreshEntry->bInherited);
		});

		It("is false for a widget named after a non-widget property on the parent class", [this]()
		{
			TestBP = MakeWidgetBlueprintWithTestParentClass(TEXT("WBP_InheritedName_"), TestCounter);
			TestNotNull(TEXT("TestBP created"), TestBP);
			if (!TestBP) return;

			// UUserWidget::Padding is an FMargin, not a widget.
			UTextBlock* Padding = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Padding"));
			TestBP->WidgetTree->RootWidget = Padding;

			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(TestBP);

			TestEqual(TEXT("one widget"), Result.Widgets.Num(), 1);
			if (Result.Widgets.Num() == 1) TestFalse(TEXT("Padding is bInherited=false"), Result.Widgets[0].bInherited);
		});
	});

	Describe("Error paths", [this]()
	{
		BeforeEach([this]()
		{
			BuildFixture();
		});
		AfterEach([this]()
		{
			TestBP = nullptr;
		});

		It("null WidgetBlueprint raises a script error and returns a default-constructed result", [this]()
		{
			// RaiseScriptError fires through the Blueprint VM (visible to AI); a direct C++ call like
			// this one never reaches that machinery, so no AddExpectedError is needed here -- matches
			// the established idiom in UMGToolSetTest.cpp's own FUMGToolSetTest_Errors spec.
			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(nullptr);

			TestTrue(TEXT("Description empty"), Result.Description.IsEmpty());
			TestEqual(TEXT("no widgets"), Result.Widgets.Num(), 0);
			TestEqual(TEXT("AvailableWidgetCount default"), Result.AvailableWidgetCount, 0);
		});

		It("StartWidget not in the given WidgetBlueprint's tree raises a script error and returns a default-constructed result", [this]()
		{
			UWidgetBlueprint* OtherBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_Foreign_"), TestCounter, Sandbox->Folder());
			UTextBlock* Foreign = OtherBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Foreign"));
			OtherBP->WidgetTree->RootWidget = Foreign;

			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(TestBP, Foreign);

			TestTrue(TEXT("Description empty"), Result.Description.IsEmpty());
			TestEqual(TEXT("no widgets"), Result.Widgets.Num(), 0);
		});

		It("MaxDepth < -1 clamps to -1 (no limit) with a Warning log, not a script error", [this]()
		{
			// A Warning-level UE_LOG (not RaiseScriptError) -- automation tests only fail on
			// Error/Fatal-severity log lines by default, so no AddExpectedError is needed; the
			// functional assertion below is that the call still succeeds as if MaxDepth=-1.
			const FWidgetDescriptionResult Result = UWidgetStaticAnalysisToolset::DescribeWidget(
				TestBP, nullptr, /*MaxDepth=*/-5);

			TestEqual(TEXT("clamped to -1 (no limit): full tree still walked"), Result.Widgets.Num(), 4);
		});
	});

	Describe("GetWidgetTreeDepth", [this]()
	{
		BeforeEach([this]()
		{
			BuildFixture();
		});
		AfterEach([this]()
		{
			TestBP = nullptr;
		});

		It("returns 2 for the standard fixture (Border > VBox > TextBlock)", [this]()
		{
			TestEqual(TEXT("depth"), UWidgetStaticAnalysisToolset::GetWidgetTreeDepth(TestBP), 2);
		});

		It("returns -1 on null WidgetBlueprint", [this]()
		{
			TestEqual(TEXT("depth"), UWidgetStaticAnalysisToolset::GetWidgetTreeDepth(nullptr), -1);
		});

		It("returns 0 for a lone root and -1 for a tree with no root", [this]()
		{
			UWidgetBlueprint* LoneRootBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_LoneRoot_"), TestCounter, Sandbox->Folder());
			LoneRootBP->WidgetTree->RootWidget = LoneRootBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Only"));
			TestEqual(TEXT("lone root depth"), UWidgetStaticAnalysisToolset::GetWidgetTreeDepth(LoneRootBP), 0);

			UWidgetBlueprint* EmptyBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_EmptyTree_"), TestCounter, Sandbox->Folder());
			TestEqual(TEXT("empty tree depth"), UWidgetStaticAnalysisToolset::GetWidgetTreeDepth(EmptyBP), -1);
		});

		It("measures from StartWidget", [this]()
		{
			TestEqual(TEXT("depth below Stack"),
				UWidgetStaticAnalysisToolset::GetWidgetTreeDepth(TestBP, TestBP->WidgetTree->FindWidget(TEXT("Stack"))), 1);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
