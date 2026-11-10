// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/Decorators/BTDecorator_Cooldown.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"

#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeToolset.h"
#include "ToolsetRegistry/Tests/TestAssetSandbox.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace BehaviorTreeToolsetTest
{
	// The shared sandbox plus creators for the assets these tests make. Names are GUID-unique, so
	// every test can share one sandbox.
	class FAssetSandbox : public FTestAssetSandbox
	{
	public:
		FAssetSandbox() : FTestAssetSandbox(TEXT("BTToolsetTest")) {}

		UBehaviorTree* CreateTree() const
		{
			return UBehaviorTreeToolset::CreateBehaviorTree(DirectoryPath(), UniqueName(TEXT("BT")));
		}

		UBlackboardData* CreateBlackboard() const
		{
			return UBehaviorTreeToolset::CreateBlackboardData(DirectoryPath(), UniqueName(TEXT("BB")));
		}

	private:
		FDirectoryPath DirectoryPath() const
		{
			FDirectoryPath Dir;
			Dir.Path = Folder();
			return Dir;
		}
	};
}

BEGIN_DEFINE_SPEC(FBehaviorTreeToolsetSpec, "AIAssistant.Toolsets.AIModuleToolset.BehaviorTreeToolset",
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter
	| EAutomationTestFlags::CriticalPriority)
	TUniquePtr<BehaviorTreeToolsetTest::FAssetSandbox> Sandbox;
END_DEFINE_SPEC(FBehaviorTreeToolsetSpec)

void FBehaviorTreeToolsetSpec::Define()
{
	using namespace BehaviorTreeToolsetTest;
	using namespace UE::ToolsetRegistry;

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

	Describe(TEXT("Edit"), [this]()
	{
		It(TEXT("Creates a tree with a root graph node and no children yet"), [this]()
		{
			UBehaviorTree* Tree = Sandbox->CreateTree();
			TestNotNull(TEXT("Tree created"), Tree);
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::GetRootGraphNode(Tree);
			TestNotNull(TEXT("Has root graph node"), Root);
			if (Root)
			{
				TestEqual(TEXT("Root has no children yet"), UBehaviorTreeToolset::GetChildGraphNodes(Root).Num(), 0);
			}
		});

		It(TEXT("Allows only a single root composite"), [this]()
		{
			UBehaviorTree* Tree = Sandbox->CreateTree();
			TestNotNull(TEXT("First root composite"),
				UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass()));
			TestNull(TEXT("Second root composite rejected"),
				UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass()));
		});

		It(TEXT("Rejects an abstract task class with a raised error"), [this]()
		{
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());

			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([Root]() -> void
			{
				UBehaviorTreeToolset::AddTask(Root, UBTTaskNode::StaticClass());
			});
			TestTrue(TEXT("AddTask with abstract class raises 'placeable'"),
				ExceptionHandler.GetException().Contains(TEXT("placeable")));
		});

		It(TEXT("Builds a tree and reads it back through graph nodes"), [this]()
		{
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());
			TestNotNull(TEXT("Root composite added"), Root);
			UBehaviorTreeGraphNode* Task = UBehaviorTreeToolset::AddTask(Root, UBTTask_Wait::StaticClass());
			TestNotNull(TEXT("Task added"), Task);
			TestNotNull(TEXT("Branch composite added"), UBehaviorTreeToolset::AddComposite(Root, UBTComposite_Sequence::StaticClass()));

			TestEqual(TEXT("Root has two children"), UBehaviorTreeToolset::GetChildGraphNodes(Root).Num(), 2);

			UBTNode* Instance = UBehaviorTreeToolset::GetNodeInstance(Task);
			TestTrue(TEXT("Task graph node wraps a BTTask_Wait instance"),
				Instance != nullptr && Instance->IsA(UBTTask_Wait::StaticClass()));
		});

		It(TEXT("Adds and removes decorator and service subnodes"), [this]()
		{
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeGraphNode* Owner = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());
			UBehaviorTreeGraphNode* Decorator = UBehaviorTreeToolset::AddDecorator(Owner, UBTDecorator_Cooldown::StaticClass());
			UBehaviorTreeGraphNode* Service = UBehaviorTreeToolset::AddService(Owner, UBTService_DefaultFocus::StaticClass());
			TestNotNull(TEXT("Decorator added"), Decorator);
			TestNotNull(TEXT("Service added"), Service);
			TestEqual(TEXT("Owner has one decorator"), UBehaviorTreeToolset::GetDecoratorGraphNodes(Owner).Num(), 1);
			TestEqual(TEXT("Owner has one service"), UBehaviorTreeToolset::GetServiceGraphNodes(Owner).Num(), 1);

			UBehaviorTreeToolset::RemoveSubnode(Owner, Decorator);
			UBehaviorTreeToolset::RemoveSubnode(Owner, Service);
			TestEqual(TEXT("Decorators cleared"), UBehaviorTreeToolset::GetDecoratorGraphNodes(Owner).Num(), 0);
			TestEqual(TEXT("Services cleared"), UBehaviorTreeToolset::GetServiceGraphNodes(Owner).Num(), 0);
		});

		It(TEXT("An explicit subnode index reorders the bucket and keeps SubNodes consistent"), [this]()
		{
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeGraphNode* Owner = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());

			UBehaviorTreeGraphNode* First = UBehaviorTreeToolset::AddDecorator(Owner, UBTDecorator_Cooldown::StaticClass());
			UBehaviorTreeGraphNode* Service = UBehaviorTreeToolset::AddService(Owner, UBTService_DefaultFocus::StaticClass());
			// Insert ahead of the first decorator, exercising the bucket-relative index path.
			UBehaviorTreeGraphNode* Second = UBehaviorTreeToolset::AddDecorator(Owner, UBTDecorator_Cooldown::StaticClass(), /*Index=*/0);

			const TArray<UBehaviorTreeGraphNode*> Decorators = UBehaviorTreeToolset::GetDecoratorGraphNodes(Owner);
			TestEqual(TEXT("Decorators reordered: second is first"), Decorators, TArray<UBehaviorTreeGraphNode*>({ Second, First }));

			// SubNodes must be the buckets combined (decorators then services), not the bucket index applied raw.
			if (TestEqual(TEXT("SubNodes holds both decorators and the service"), Owner->SubNodes.Num(), 3))
			{
				TestTrue(TEXT("SubNodes[0] is the front decorator"), Owner->SubNodes[0] == Second);
				TestTrue(TEXT("SubNodes[1] is the trailing decorator"), Owner->SubNodes[1] == First);
				TestTrue(TEXT("SubNodes[2] is the service"), Owner->SubNodes[2] == Service);
			}
		});

		It(TEXT("Adds children in append order by default and prepends on index 0"), [this]()
		{
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());

			// Default index (-1) must append, not prepend.
			UBehaviorTreeGraphNode* First = UBehaviorTreeToolset::AddTask(Root, UBTTask_Wait::StaticClass());
			UBehaviorTreeGraphNode* Second = UBehaviorTreeToolset::AddTask(Root, UBTTask_Wait::StaticClass());
			TestEqual(TEXT("Default index appends to the end"),
				UBehaviorTreeToolset::GetChildGraphNodes(Root), TArray<UBehaviorTreeGraphNode*>({ First, Second }));

			// Explicit index 0 prepends.
			UBehaviorTreeGraphNode* Front = UBehaviorTreeToolset::AddComposite(Root, UBTComposite_Sequence::StaticClass(), /*Index=*/0);
			TestEqual(TEXT("Index 0 prepends"),
				UBehaviorTreeToolset::GetChildGraphNodes(Root), TArray<UBehaviorTreeGraphNode*>({ Front, First, Second }));
		});

		It(TEXT("Moves an existing subnode in place, preserving the node instance"), [this]()
		{
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeGraphNode* Owner = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());

			UBehaviorTreeGraphNode* First = UBehaviorTreeToolset::AddDecorator(Owner, UBTDecorator_Cooldown::StaticClass());
			UBehaviorTreeGraphNode* Second = UBehaviorTreeToolset::AddDecorator(Owner, UBTDecorator_Cooldown::StaticClass());
			UBTNode* SecondInstance = UBehaviorTreeToolset::GetNodeInstance(Second);

			// Move the second decorator to the front, then back to the end.
			UBehaviorTreeToolset::MoveSubnode(Owner, Second, 0);
			TestEqual(TEXT("Second is now first"),
				UBehaviorTreeToolset::GetDecoratorGraphNodes(Owner), TArray<UBehaviorTreeGraphNode*>({ Second, First }));

			UBehaviorTreeToolset::MoveSubnode(Owner, Second, 1);
			TestEqual(TEXT("Second moved back to the end"),
				UBehaviorTreeToolset::GetDecoratorGraphNodes(Owner), TArray<UBehaviorTreeGraphNode*>({ First, Second }));

			TestTrue(TEXT("Moved node keeps its instance (config not lost)"),
				UBehaviorTreeToolset::GetNodeInstance(Second) == SecondInstance);
		});

		It(TEXT("Removing a child composite prunes it from the tree"), [this]()
		{
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());
			UBehaviorTreeGraphNode* Branch = UBehaviorTreeToolset::AddComposite(Root, UBTComposite_Sequence::StaticClass());
			TestEqual(TEXT("One child before removal"),
				UBehaviorTreeToolset::GetChildGraphNodes(Root).Num(), 1);

			UBehaviorTreeToolset::RemoveGraphNode(Branch);
			TestEqual(TEXT("No children after removal"),
				UBehaviorTreeToolset::GetChildGraphNodes(Root).Num(), 0);
		});
	});

	Describe(TEXT("Blackboard"), [this]()
	{
		It(TEXT("Creates a usable blackboard asset"), [this]()
		{
			UBlackboardData* Blackboard = Sandbox->CreateBlackboard();
			TestNotNull(TEXT("Blackboard created"), Blackboard);
		});

		It(TEXT("Adds, lists, dedupes, and removes a key"), [this]()
		{
			UBlackboardData* Blackboard = Sandbox->CreateBlackboard();
			const int32 Initial = UBehaviorTreeToolset::GetBlackboardKeyNames(Blackboard).Num();

			TestNotNull(TEXT("Key added"),
				UBehaviorTreeToolset::AddBlackboardKey(Blackboard, TEXT("MyFlag"), UBlackboardKeyType_Bool::StaticClass()));
			TestTrue(TEXT("Key is listed"),
				UBehaviorTreeToolset::GetBlackboardKeyNames(Blackboard).Contains(FName(TEXT("MyFlag"))));
			TestEqual(TEXT("Key count grew by one"),
				UBehaviorTreeToolset::GetBlackboardKeyNames(Blackboard).Num(), Initial + 1);
			TestNull(TEXT("Duplicate key rejected"),
				UBehaviorTreeToolset::AddBlackboardKey(Blackboard, TEXT("MyFlag"), UBlackboardKeyType_Bool::StaticClass()));
			TestTrue(TEXT("Key removed"),
				UBehaviorTreeToolset::RemoveBlackboardKey(Blackboard, TEXT("MyFlag")));
			TestEqual(TEXT("Key count back to initial"),
				UBehaviorTreeToolset::GetBlackboardKeyNames(Blackboard).Num(), Initial);
		});

		It(TEXT("Rejects an abstract key type with a raised error"), [this]()
		{
			UBlackboardData* Blackboard = Sandbox->CreateBlackboard();

			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([Blackboard]() -> void
			{
				UBehaviorTreeToolset::AddBlackboardKey(Blackboard, TEXT("Bad"), UBlackboardKeyType::StaticClass());
			});
			TestTrue(TEXT("AddBlackboardKey with abstract type raises 'key type'"),
				ExceptionHandler.GetException().Contains(TEXT("key type")));
		});

		It(TEXT("Sets a decorator's blackboard key by name and resolves it safely"), [this]()
		{
			UBlackboardData* Blackboard = Sandbox->CreateBlackboard();
			UBehaviorTreeToolset::AddBlackboardKey(Blackboard, TEXT("MyFlag"), UBlackboardKeyType_Bool::StaticClass());

			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeToolset::SetBlackboard(Tree, Blackboard);
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());
			UBehaviorTreeGraphNode* DecoratorNode = UBehaviorTreeToolset::AddDecorator(Root, UBTDecorator_Blackboard::StaticClass());
			UBTNode* Decorator = UBehaviorTreeToolset::GetNodeInstance(DecoratorNode);

			// A real key resolves; the description rebuild that used to crash on a raw id now runs clean.
			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([Decorator]() -> void
			{
				UBehaviorTreeToolset::SetNodeBlackboardKey(Decorator, TEXT("BlackboardKey"), TEXT("MyFlag"));
			});
			TestTrue(TEXT("Existing key sets without error"), ExceptionHandler.GetException().IsEmpty());
		});

		It(TEXT("Raises when the key is not on the blackboard"), [this]()
		{
			UBlackboardData* Blackboard = Sandbox->CreateBlackboard();
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeToolset::SetBlackboard(Tree, Blackboard);
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());
			UBehaviorTreeGraphNode* DecoratorNode = UBehaviorTreeToolset::AddDecorator(Root, UBTDecorator_Blackboard::StaticClass());
			UBTNode* Decorator = UBehaviorTreeToolset::GetNodeInstance(DecoratorNode);

			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([Decorator]() -> void
			{
				UBehaviorTreeToolset::SetNodeBlackboardKey(Decorator, TEXT("BlackboardKey"), TEXT("Nope"));
			});
			TestTrue(TEXT("Unknown key raises 'not found'"),
				ExceptionHandler.GetException().Contains(TEXT("not found")));
		});

		It(TEXT("Raises when the node has no such key selector"), [this]()
		{
			UBlackboardData* Blackboard = Sandbox->CreateBlackboard();
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeToolset::SetBlackboard(Tree, Blackboard);
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());
			UBTNode* Composite = UBehaviorTreeToolset::GetNodeInstance(Root);

			FToolCallExceptionHandler ExceptionHandler;
			ExceptionHandler.CaptureErrorsIn([Composite]() -> void
			{
				UBehaviorTreeToolset::SetNodeBlackboardKey(Composite, TEXT("BlackboardKey"), TEXT("MyFlag"));
			});
			TestTrue(TEXT("Missing selector raises"),
				ExceptionHandler.GetException().Contains(TEXT("selector")));
		});

		It(TEXT("Offers all keys for an unfiltered selector"), [this]()
		{
			UBlackboardData* Blackboard = Sandbox->CreateBlackboard();
			UBehaviorTreeToolset::AddBlackboardKey(Blackboard, TEXT("MyFlag"), UBlackboardKeyType_Bool::StaticClass());
			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeToolset::SetBlackboard(Tree, Blackboard);
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());
			UBehaviorTreeGraphNode* DecoratorNode = UBehaviorTreeToolset::AddDecorator(Root, UBTDecorator_Blackboard::StaticClass());
			UBTNode* Decorator = UBehaviorTreeToolset::GetNodeInstance(DecoratorNode);

			// The blackboard decorator applies no type filter, so every key is offered.
			TestTrue(TEXT("Unfiltered selector offers the key"),
				UBehaviorTreeToolset::GetAllowedBlackboardKeys(Decorator, TEXT("BlackboardKey")).Contains(FName(TEXT("MyFlag"))));
		});

		It(TEXT("Filters allowed keys by type and rejects a disallowed key"), [this]()
		{
			UBlackboardData* Blackboard = Sandbox->CreateBlackboard();
			UBehaviorTreeToolset::AddBlackboardKey(Blackboard, TEXT("Flag"), UBlackboardKeyType_Bool::StaticClass());
			UBehaviorTreeToolset::AddBlackboardKey(Blackboard, TEXT("Dest"), UBlackboardKeyType_Vector::StaticClass());

			UBehaviorTree* Tree = Sandbox->CreateTree();
			UBehaviorTreeToolset::SetBlackboard(Tree, Blackboard);
			UBehaviorTreeGraphNode* Root = UBehaviorTreeToolset::AddRootComposite(Tree, UBTComposite_Selector::StaticClass());
			UBehaviorTreeGraphNode* TaskNode = UBehaviorTreeToolset::AddTask(Root, UBTTask_MoveTo::StaticClass());
			UBTNode* MoveTo = UBehaviorTreeToolset::GetNodeInstance(TaskNode);

			// MoveTo's BlackboardKey accepts Object/Vector, not Bool.
			const TArray<FName> Allowed = UBehaviorTreeToolset::GetAllowedBlackboardKeys(MoveTo, TEXT("BlackboardKey"));
			TestTrue(TEXT("Vector key offered"), Allowed.Contains(FName(TEXT("Dest"))));
			TestFalse(TEXT("Bool key not offered"), Allowed.Contains(FName(TEXT("Flag"))));

			FToolCallExceptionHandler SetHandler;
			SetHandler.CaptureErrorsIn([MoveTo]() -> void
			{
				UBehaviorTreeToolset::SetNodeBlackboardKey(MoveTo, TEXT("BlackboardKey"), TEXT("Dest"));
			});
			TestTrue(TEXT("Allowed key sets without error"), SetHandler.GetException().IsEmpty());

			FToolCallExceptionHandler RejectHandler;
			RejectHandler.CaptureErrorsIn([MoveTo]() -> void
			{
				UBehaviorTreeToolset::SetNodeBlackboardKey(MoveTo, TEXT("BlackboardKey"), TEXT("Flag"));
			});
			TestTrue(TEXT("Disallowed key raises 'not accepted'"),
				RejectHandler.GetException().Contains(TEXT("not accepted")));
		});
	});
}

#endif  // WITH_DEV_AUTOMATION_TESTS
