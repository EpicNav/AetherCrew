// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_ActorsOfClass.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_SimpleGrid.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Distance.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Random.h"

#include "EnvironmentQueryGraphNode.h"
#include "EnvironmentQueryToolset.h"
#include "ToolsetRegistry/Tests/TestAssetSandbox.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace EnvironmentQueryToolsetTest
{
	// The shared sandbox plus a creator for the queries these tests make. Names are GUID-unique, so
	// every test can share one sandbox.
	class FAssetSandbox : public FTestAssetSandbox
	{
	public:
		FAssetSandbox() : FTestAssetSandbox(TEXT("EQSToolsetTest")) {}

		UEnvQuery* CreateQuery() const
		{
			FDirectoryPath Dir;
			Dir.Path = Folder();
			return UEnvironmentQueryToolset::CreateEnvironmentQuery(Dir, UniqueName(TEXT("EQS")));
		}
	};
}

BEGIN_DEFINE_SPEC(FEnvironmentQueryToolsetSpec, "AIAssistant.Toolsets.AIModuleToolset.EnvironmentQueryToolset",
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter
	| EAutomationTestFlags::CriticalPriority)
	TUniquePtr<EnvironmentQueryToolsetTest::FAssetSandbox> Sandbox;
END_DEFINE_SPEC(FEnvironmentQueryToolsetSpec)

void FEnvironmentQueryToolsetSpec::Define()
{
	using namespace EnvironmentQueryToolsetTest;

	// UE 5.8: no BeforeAll in 5.8's automation specs; the fixture is per test.
	BeforeEach([this]()
	{
		Sandbox = MakeUnique<FAssetSandbox>();
	});

	// UE 5.8: no AfterAll in 5.8's automation specs.
	AfterEach([this]()
	{
		Sandbox.Reset();
	});
	using namespace UE::ToolsetRegistry;

	Describe(TEXT("Edit"), [this]()
	{
		It(TEXT("Creates a query with a root graph node and no options yet"), [this]()
		{
			UEnvQuery* Query = Sandbox->CreateQuery();
			TestNotNull(TEXT("Query created"), Query);
			TestNotNull(TEXT("Has root graph node"), UEnvironmentQueryToolset::GetRootGraphNode(Query));
			TestEqual(TEXT("No options yet"), UEnvironmentQueryToolset::GetOptionGraphNodes(Query).Num(), 0);
		});

		It(TEXT("Rejects an abstract generator class with a raised error"), [this]()
		{
			UEnvQuery* Query = Sandbox->CreateQuery();

			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([Query]() -> void
			{
				UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator::StaticClass());
			});
			TestTrue(TEXT("AddOption with abstract class raises 'placeable'"),
				ExceptionHandler.GetException().Contains(TEXT("placeable")));
		});

		It(TEXT("Adds an option wrapping the requested generator"), [this]()
		{
			UEnvQuery* Query = Sandbox->CreateQuery();

			UEnvironmentQueryGraphNode* Option = UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator_SimpleGrid::StaticClass());
			TestNotNull(TEXT("Option added"), Option);
			TestEqual(TEXT("Query has one option node"), UEnvironmentQueryToolset::GetOptionGraphNodes(Query).Num(), 1);

			UEnvQueryGenerator* Generator = UEnvironmentQueryToolset::GetGenerator(Option);
			TestTrue(TEXT("Option wraps a SimpleGrid generator"),
				Generator != nullptr && Generator->IsA(UEnvQueryGenerator_SimpleGrid::StaticClass()));

			// The built query rebuilds its Options array from the graph.
			if (TestEqual(TEXT("Built query has one option"), Query->GetOptions().Num(), 1))
			{
				TestTrue(TEXT("Built option holds the SimpleGrid generator"),
					Query->GetOptions()[0]->Generator != nullptr
					&& Query->GetOptions()[0]->Generator->IsA(UEnvQueryGenerator_SimpleGrid::StaticClass()));
			}
		});

		It(TEXT("Adds options in append order by default and prepends on index 0"), [this]()
		{
			UEnvQuery* Query = Sandbox->CreateQuery();

			UEnvironmentQueryGraphNode* First = UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator_SimpleGrid::StaticClass());
			UEnvironmentQueryGraphNode* Second = UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator_ActorsOfClass::StaticClass());
			TestEqual(TEXT("Default index appends to the end"),
				UEnvironmentQueryToolset::GetOptionGraphNodes(Query), TArray<UEnvironmentQueryGraphNode*>({ First, Second }));

			UEnvironmentQueryGraphNode* Front = UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator_SimpleGrid::StaticClass(), /*Index=*/0);
			TestEqual(TEXT("Index 0 prepends"),
				UEnvironmentQueryToolset::GetOptionGraphNodes(Query), TArray<UEnvironmentQueryGraphNode*>({ Front, First, Second }));
		});

		It(TEXT("Rejects an abstract test class with a raised error"), [this]()
		{
			UEnvQuery* Query = Sandbox->CreateQuery();
			UEnvironmentQueryGraphNode* Option = UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator_SimpleGrid::StaticClass());

			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([Option]() -> void
			{
				UEnvironmentQueryToolset::AddTest(Option, UEnvQueryTest::StaticClass());
			});
			TestTrue(TEXT("AddTest with abstract class raises 'placeable'"),
				ExceptionHandler.GetException().Contains(TEXT("placeable")));
		});

		It(TEXT("Adds tests in order and reflects them in the built option"), [this]()
		{
			UEnvQuery* Query = Sandbox->CreateQuery();
			UEnvironmentQueryGraphNode* Option = UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator_SimpleGrid::StaticClass());

			// Default index (-1) must append, not prepend.
			UEnvironmentQueryGraphNode* Distance = UEnvironmentQueryToolset::AddTest(Option, UEnvQueryTest_Distance::StaticClass());
			UEnvironmentQueryGraphNode* Random = UEnvironmentQueryToolset::AddTest(Option, UEnvQueryTest_Random::StaticClass());
			TestEqual(TEXT("Default index appends"),
				UEnvironmentQueryToolset::GetTestGraphNodes(Option), TArray<UEnvironmentQueryGraphNode*>({ Distance, Random }));

			// Explicit index 0 prepends.
			UEnvironmentQueryGraphNode* FrontTest = UEnvironmentQueryToolset::AddTest(Option, UEnvQueryTest_Random::StaticClass(), /*Index=*/0);
			TestEqual(TEXT("Index 0 prepends"),
				UEnvironmentQueryToolset::GetTestGraphNodes(Option), TArray<UEnvironmentQueryGraphNode*>({ FrontTest, Distance, Random }));

			UEnvQueryOption* OptionInstance = UEnvironmentQueryToolset::GetOptionInstance(Option);
			if (OptionInstance && TestEqual(TEXT("Built option has three tests"), OptionInstance->Tests.Num(), 3))
			{
				TestEqual(TEXT("First test's TestOrder is 0"), OptionInstance->Tests[0]->TestOrder, 0);
				TestEqual(TEXT("Last test's TestOrder is 2"), OptionInstance->Tests[2]->TestOrder, 2);
			}
		});

		It(TEXT("Moves an existing test in place, preserving the test instance"), [this]()
		{
			UEnvQuery* Query = Sandbox->CreateQuery();
			UEnvironmentQueryGraphNode* Option = UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator_SimpleGrid::StaticClass());

			UEnvironmentQueryGraphNode* First = UEnvironmentQueryToolset::AddTest(Option, UEnvQueryTest_Distance::StaticClass());
			UEnvironmentQueryGraphNode* Second = UEnvironmentQueryToolset::AddTest(Option, UEnvQueryTest_Random::StaticClass());
			UEnvQueryTest* SecondInstance = UEnvironmentQueryToolset::GetTestInstance(Second);

			UEnvironmentQueryToolset::MoveTest(Option, Second, 0);
			TestEqual(TEXT("Second is now first"),
				UEnvironmentQueryToolset::GetTestGraphNodes(Option), TArray<UEnvironmentQueryGraphNode*>({ Second, First }));

			UEnvironmentQueryToolset::MoveTest(Option, Second, 1);
			TestEqual(TEXT("Second moved back to the end"),
				UEnvironmentQueryToolset::GetTestGraphNodes(Option), TArray<UEnvironmentQueryGraphNode*>({ First, Second }));

			TestTrue(TEXT("Moved test keeps its instance (config not lost)"),
				UEnvironmentQueryToolset::GetTestInstance(Second) == SecondInstance);
		});

		It(TEXT("Excludes a disabled test from the built option"), [this]()
		{
			UEnvQuery* Query = Sandbox->CreateQuery();
			UEnvironmentQueryGraphNode* Option = UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator_SimpleGrid::StaticClass());
			UEnvironmentQueryToolset::AddTest(Option, UEnvQueryTest_Distance::StaticClass());
			UEnvironmentQueryGraphNode* Random = UEnvironmentQueryToolset::AddTest(Option, UEnvQueryTest_Random::StaticClass());

			UEnvQueryOption* OptionInstance = UEnvironmentQueryToolset::GetOptionInstance(Option);
			TestTrue(TEXT("Both tests present while enabled"), OptionInstance && OptionInstance->Tests.Num() == 2);

			UEnvironmentQueryToolset::SetTestEnabled(Random, false);
			TestTrue(TEXT("Disabled test excluded from built option"), OptionInstance && OptionInstance->Tests.Num() == 1);
			// The graph node is kept even though the built option drops it.
			TestEqual(TEXT("Test node still on the option"),
				UEnvironmentQueryToolset::GetTestGraphNodes(Option).Num(), 2);
		});

		It(TEXT("Removes a test and an option"), [this]()
		{
			UEnvQuery* Query = Sandbox->CreateQuery();
			UEnvironmentQueryGraphNode* Option = UEnvironmentQueryToolset::AddOption(Query, UEnvQueryGenerator_SimpleGrid::StaticClass());
			UEnvironmentQueryGraphNode* Test = UEnvironmentQueryToolset::AddTest(Option, UEnvQueryTest_Distance::StaticClass());
			TestEqual(TEXT("One test before removal"), UEnvironmentQueryToolset::GetTestGraphNodes(Option).Num(), 1);

			UEnvironmentQueryToolset::RemoveTest(Option, Test);
			TestEqual(TEXT("No tests after removal"), UEnvironmentQueryToolset::GetTestGraphNodes(Option).Num(), 0);

			UEnvironmentQueryToolset::RemoveOption(Option);
			TestEqual(TEXT("No options after removal"), UEnvironmentQueryToolset::GetOptionGraphNodes(Query).Num(), 0);
		});
	});
}

#endif  // WITH_DEV_AUTOMATION_TESTS
