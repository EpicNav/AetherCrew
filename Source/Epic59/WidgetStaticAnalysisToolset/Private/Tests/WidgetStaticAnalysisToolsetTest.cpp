// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "WidgetStaticAnalysisToolset.h"
#include "WidgetStaticAnalysisSchemaTestFixtures.h"
#include "WidgetStaticAnalysisSchemaTestFlags.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "MVVMViewModelBase.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

// Shell-level caps, ScopeFilter, and null-argument errors.

BEGIN_DEFINE_SPEC(
	FWidgetStaticAnalysisToolsetTest,
	"AIAssistant.Toolsets.WidgetStaticAnalysisToolset.Shell",
	WidgetStaticAnalysisSchemaTest::Flags)

UWidgetBlueprint* TestBP = nullptr;
int32 TestCounter = 0;

/** Builds RootWidget = VerticalBox("Stack") with NumChildren UTextBlock children, all at class
 *  default. Used for large-result tests. */
void BuildTextBlockTree(int32 NumChildren);

/** Builds a VerticalBox("Stack") root with NumChildren fixture-widget children, each contributing
 *  exactly one Unconnected delegate row (its inherited OnVisibilityChanged). Used for large-result
 *  tests without needing a Blueprint compile or disk save. */
void BuildDelegateFixtureTree(int32 NumChildren);

END_DEFINE_SPEC(FWidgetStaticAnalysisToolsetTest)

void FWidgetStaticAnalysisToolsetTest::BuildTextBlockTree(int32 NumChildren)
{
	TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ShellCap_"), TestCounter);

	UVerticalBox* Stack = TestBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Stack"));
	TestBP->WidgetTree->RootWidget = Stack;

	for (int32 Index = 0; Index < NumChildren; ++Index)
	{
		UTextBlock* Child = TestBP->WidgetTree->ConstructWidget<UTextBlock>(
			UTextBlock::StaticClass(), *FString::Printf(TEXT("Text_%d"), Index));
		Stack->AddChildToVerticalBox(Child);
	}
}

void FWidgetStaticAnalysisToolsetTest::BuildDelegateFixtureTree(int32 NumChildren)
{
	TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ShellCap_"), TestCounter);

	UVerticalBox* Stack = TestBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Stack"));
	TestBP->WidgetTree->RootWidget = Stack;

	for (int32 Index = 0; Index < NumChildren; ++Index)
	{
		UWidgetStaticAnalysisTestNativeUserWidget* Child = TestBP->WidgetTree->ConstructWidget<UWidgetStaticAnalysisTestNativeUserWidget>(
			UWidgetStaticAnalysisTestNativeUserWidget::StaticClass(), *FString::Printf(TEXT("Delegate_%d"), Index));
		Stack->AddChildToVerticalBox(Child);
	}
}

void FWidgetStaticAnalysisToolsetTest::Define()
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

	Describe("TraceWidgetPropertyOverrides: large trees are returned whole; also proves PropertyNameFilter reaches FWidgetPropertyOriginScanner", [this]()
	{
		It("501 traced rows: every Chains row is returned and AvailablePropertyCount matches", [this]()
		{
			BuildTextBlockTree(501);
			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(TestBP, TEXT("Text"));
			TestEqual(TEXT("Chains.Num()"), Result.Chains.Num(), 501);
			TestEqual(TEXT("AvailablePropertyCount"), Result.AvailablePropertyCount, 501);
		});
	});

	Describe("GetWidgetConnectionMap: large maps are returned whole, and ScopeFilter wiring", [this]()
	{
		It("501 connections: every row is returned and AvailableConnectionCount matches", [this]()
		{
			BuildDelegateFixtureTree(501);
			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);
			TestEqual(TEXT("Connections.Num()"), Result.Connections.Num(), 501);
			TestEqual(TEXT("AvailableConnectionCount"), Result.AvailableConnectionCount, 501);
		});

		It("stress: 20000 connections come back whole; logs time and exported size", [this]()
		{
			constexpr int32 NumConnections = 20000;
			BuildDelegateFixtureTree(NumConnections);

			const double StartSeconds = FPlatformTime::Seconds();
			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);
			const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

			FString Exported;
			FWidgetConnectionMapResult::StaticStruct()->ExportText(Exported, &Result, nullptr, nullptr, PPF_None, nullptr);

			AddInfo(FString::Printf(TEXT("GetWidgetConnectionMap: %d rows in %.3f s, exported text %d chars"),
				Result.Connections.Num(), ElapsedSeconds, Exported.Len()));
			TestEqual(TEXT("Connections.Num()"), Result.Connections.Num(), NumConnections);
			TestEqual(TEXT("AvailableConnectionCount"), Result.AvailableConnectionCount, NumConnections);
		});

		It("ScopeFilter keeps rows whose Kind name matches and drops the rest", [this]()
		{
			BuildDelegateFixtureTree(3);
			TestEqual(TEXT("\"Delegate\" keeps all three rows"),
				UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP, TEXT("Delegate")).Connections.Num(), 3);
			TestEqual(TEXT("\"VM\" matches no rows"),
				UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP, TEXT("VM")).Connections.Num(), 0);
		});
	});

	Describe("Python exposure", [this]()
	{
		It("every tool is PythonCallable, since the Python generator skips AICallable-only functions", [this]()
		{
			for (const TCHAR* Name : { TEXT("DescribeWidget"), TEXT("GetWidgetTreeDepth"), TEXT("TraceWidgetPropertyOverrides"),
				TEXT("GetWidgetConnectionMap"), TEXT("GetViewModelConnectionMap") })
			{
				const UFunction* Function = UWidgetStaticAnalysisToolset::StaticClass()->FindFunctionByName(Name);
				TestTrue(FString::Printf(TEXT("%s is PythonCallable"), Name), Function && Function->HasMetaData(TEXT("PythonCallable")));
			}
		});
	});

	Describe("Error paths: null required arguments", [this]()
	{
		It("TraceWidgetPropertyOverrides(nullptr) returns a default-constructed result", [this]()
		{
			// No AddExpectedError needed; RaiseScriptError needs a Blueprint VM frame.
			const FWidgetPropertyOverrideResult Result = UWidgetStaticAnalysisToolset::TraceWidgetPropertyOverrides(nullptr);
			TestEqual(TEXT("no Chains"), Result.Chains.Num(), 0);
			TestEqual(TEXT("no Winners"), Result.Winners.Num(), 0);
		});

		It("GetWidgetConnectionMap(nullptr) returns a default-constructed result", [this]()
		{
			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(nullptr);
			TestEqual(TEXT("no Connections"), Result.Connections.Num(), 0);
		});

		It("GetViewModelConnectionMap(nullptr) returns a default-constructed result", [this]()
		{
			const FViewModelConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetViewModelConnectionMap(TSubclassOf<UMVVMViewModelBase>(nullptr));
			TestEqual(TEXT("no Widgets"), Result.Widgets.Num(), 0);
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
