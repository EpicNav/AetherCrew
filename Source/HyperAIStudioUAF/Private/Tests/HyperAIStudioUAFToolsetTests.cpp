// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioUAFToolset.h"

#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "AnimNextRigVMAsset.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Graph/AnimNextAnimationGraph.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioUAFCookbook.h"
#include "HyperAIStudioUAFGate.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "RenderingThread.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::UAF::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* const Idle = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Unarmed/MM_Unarmed_Idle_Ready.MM_Unarmed_Idle_Ready");
	const TCHAR* const Jog = TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Unarmed/BS_MM_Unarmed_Jog_Walk.BS_MM_Unarmed_Jog_Walk");

	const TCHAR* const SequencePlayer = TEXT("FSequencePlayerTrait");
	const TCHAR* const BlendSpacePlayer = TEXT("FBlendSpacePlayerTrait");
	const TCHAR* const BlendByBool = TEXT("FBlendByBoolTrait");
	const TCHAR* const BlendSmoother = TEXT("FBlendSmootherTrait");

	FHyperAIUAFOp Op(const TCHAR* Kind, const TCHAR* Node, const TCHAR* Trait = TEXT(""), const TCHAR* Pin = TEXT(""),
		const TCHAR* Value = TEXT(""), const TCHAR* To = TEXT(""), const TCHAR* Name = TEXT(""))
	{
		FHyperAIUAFOp Result;
		Result.Kind = Kind;
		Result.Node = Node;
		Result.Trait = Trait;
		Result.Pin = Pin;
		Result.Value = Value;
		Result.To = To;
		Result.Name = Name;
		return Result;
	}

	/** Idle and a jog blend space, blended by a bool and smoothed, with the blend driven by variables. */
	TArray<FHyperAIUAFOp> Locomotion()
	{
		return {
			Op(TEXT("add_variable"), TEXT(""), TEXT(""), TEXT(""), TEXT("float"), TEXT(""), TEXT("Speed")),
			Op(TEXT("add_variable"), TEXT(""), TEXT(""), TEXT(""), TEXT("bool"), TEXT(""), TEXT("IsMoving")),
			Op(TEXT("add_node"), TEXT("Idle")),
			Op(TEXT("add_trait"), TEXT("Idle"), SequencePlayer),
			Op(TEXT("set_pin"), TEXT("Idle"), SequencePlayer, TEXT("AnimSequence"), Idle),
			Op(TEXT("add_node"), TEXT("Jog")),
			Op(TEXT("add_trait"), TEXT("Jog"), BlendSpacePlayer),
			Op(TEXT("set_pin"), TEXT("Jog"), BlendSpacePlayer, TEXT("BlendSpace"), Jog),
			Op(TEXT("bind_pin"), TEXT("Jog"), BlendSpacePlayer, TEXT("XAxisSamplePoint"), TEXT("Speed")),
			Op(TEXT("add_node"), TEXT("Locomotion")),
			Op(TEXT("add_trait"), TEXT("Locomotion"), BlendByBool),
			Op(TEXT("add_trait"), TEXT("Locomotion"), BlendSmoother),
			Op(TEXT("bind_pin"), TEXT("Locomotion"), BlendByBool, TEXT("bCondition"), TEXT("IsMoving")),
			Op(TEXT("link"), TEXT("Idle"), BlendByBool, TEXT("FalseChild"), TEXT(""), TEXT("Locomotion")),
			Op(TEXT("link"), TEXT("Jog"), BlendByBool, TEXT("TrueChild"), TEXT(""), TEXT("Locomotion")),
			Op(TEXT("link_output"), TEXT("Locomotion"))};
	}

	bool AssetsPresent(FAutomationTestBase& Test)
	{
		for (const TCHAR* Path : {Idle, Jog})
		{
			if (!LoadObject<UObject>(nullptr, Path))
			{
				Test.AddInfo(FString::Printf(TEXT("%s is missing; the Lyra mannequin content is needed for this test."), Path));
				return false;
			}
		}
		return true;
	}

	/** The traits this suite builds with have to be registered, which needs the UAF plugins loaded. */
	bool TraitsPresent(FAutomationTestBase& Test)
	{
		for (const TCHAR* Name : {SequencePlayer, BlendSpacePlayer, BlendByBool, BlendSmoother})
		{
			const bool bFound = Gate::ReadTraitCatalog(Name).ContainsByPredicate(
				[Name](const FHyperAIUAFTrait& Trait) { return Trait.Name == Name; });
			if (!bFound)
			{
				Test.AddInfo(FString::Printf(TEXT("The trait %s is not registered; the UAF plugins are not loaded."), Name));
				return false;
			}
		}
		return true;
	}

	FString TestPath()
	{
		const FString Name = TEXT("AG_HyperAITest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		return FString::Printf(TEXT("/Game/__HyperAIStudioTests/UAF/%s.%s"), *Name, *Name);
	}

	const FHyperAIUAFNode* FindNode(const TArray<FHyperAIUAFNode>& Nodes, const TCHAR* Name)
	{
		return Nodes.FindByPredicate([Name](const FHyperAIUAFNode& Node) { return Node.Name == Name; });
	}

	const FHyperAIUAFPin* FindPin(const FHyperAIUAFNode& Node, const TCHAR* Trait, const TCHAR* Pin)
	{
		const FHyperAIUAFTrait* Found = Node.Traits.FindByPredicate([Trait](const FHyperAIUAFTrait& Entry) { return Entry.Name == Trait; });
		return Found ? Found->Pins.FindByPredicate([Pin](const FHyperAIUAFPin& Entry) { return Entry.Name == Pin; }) : nullptr;
	}

	bool HasIssue(const TArray<FHyperAIUAFIssue>& Issues, const TCHAR* Code)
	{
		return Issues.ContainsByPredicate([Code](const FHyperAIUAFIssue& Issue) { return Issue.Code == Code; });
	}

	bool PumpUntil(TFunctionRef<bool()> Done, const double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Deadline) return false;
			FTSTicker::GetCoreTicker().Tick(0.016f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FlushRenderingCommands();
		}
		return true;
	}

	void Discard(UObject* Asset)
	{
		if (Asset) ObjectTools::ForceDeleteObjects({Asset}, /*bShowConfirmation=*/false);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUAFCohortTest,
	"HyperAIStudio.UAF.Contracts.ExactThreeToolCohort",
	HyperAIStudio::UAF::Tests::Flags)

bool FHyperAIStudioUAFCohortTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Names = FHyperAIStudioUAFContracts::GetToolNames();
	TestEqual(TEXT("three tools"), Names.Num(), 3);
	TestEqual(TEXT("one variant per tool"), FHyperAIStudioUAFContracts::GetAdapterDescriptor().Variants.Num(), Names.Num());
	int32 Callables = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioUAFToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->HasMetaData(TEXT("AICallable"))) continue;
		++Callables;
		TestTrue(*FString::Printf(TEXT("%s is a manifest tool"), *It->GetName()), Names.Contains(It->GetName()));
		const FString ToolTip = It->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description within 400 characters"), *It->GetName()), !ToolTip.IsEmpty() && ToolTip.Len() <= 400);
	}
	TestEqual(TEXT("AICallable functions match the manifest"), Callables, Names.Num());

	TestTrue(TEXT("the cookbook is in the project"), HyperAIStudio::UAF::EnsureCookbook());
	FString Cookbook;
	FFileHelper::LoadFileToString(Cookbook, *HyperAIStudio::UAF::GetCookbookPath());
	TestTrue(TEXT("and covers the tool, the stack rule and what it does not build"),
		Cookbook.Contains(TEXT("hyper_uaf_apply_plan")) && Cookbook.Contains(TEXT("base trait")) && Cookbook.Contains(TEXT("Modules")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUAFCompilationStateAuditTest,
	"HyperAIStudio.UAF.Contracts.CompilationStateAudit",
	HyperAIStudio::UAF::Tests::Flags)

bool FHyperAIStudioUAFCompilationStateAuditTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// CompileStatusOf reads this one protected UPROPERTY by name; a rename or retype should fail here, not silently.
	const FEnumProperty* Property = FindFProperty<FEnumProperty>(UUAFRigVMAsset::StaticClass(), TEXT("CompilationState"));
	if (!TestNotNull(TEXT("UUAFRigVMAsset still has a CompilationState enum property"), Property)) return false;
	const UEnum* Enum = Property->GetEnum();
	if (!TestNotNull(TEXT("with a reflected enum"), Enum)) return false;
	for (const TCHAR* Name : {TEXT("CompiledWithErrors"), TEXT("CompiledWithWarnings"), TEXT("CompiledWithSuccess"), TEXT("Invalid")})
	{
		TestTrue(*FString::Printf(TEXT("%s is still a state"), Name), Enum->GetIndexByNameString(Name) != INDEX_NONE);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUAFTraitCatalogTest,
	"HyperAIStudio.UAF.Graph.TraitCatalog",
	HyperAIStudio::UAF::Tests::Flags)

bool FHyperAIStudioUAFTraitCatalogTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::UAF::Tests;
	namespace Gate = HyperAIStudio::UAF::Gate;
	if (!TraitsPresent(*this)) return true;
	const TArray<FHyperAIUAFTrait> All = Gate::ReadTraitCatalog(FString());
	TestTrue(TEXT("the registry has traits"), All.Num() > 10);
	const FHyperAIUAFTrait* Player = All.FindByPredicate([](const FHyperAIUAFTrait& Trait) { return Trait.Name == SequencePlayer; });
	if (TestNotNull(TEXT("the sequence player is in the catalog"), Player))
	{
		TestEqual(TEXT("as a base trait"), Player->Mode, FString(TEXT("base")));
		TestTrue(TEXT("with its AnimSequence pin"), Player->Pins.ContainsByPredicate(
			[](const FHyperAIUAFPin& Pin) { return Pin.Name == TEXT("AnimSequence"); }));
	}
	const FHyperAIUAFTrait* Smoother = All.FindByPredicate([](const FHyperAIUAFTrait& Trait) { return Trait.Name == BlendSmoother; });
	if (TestNotNull(TEXT("the blend smoother is in the catalog"), Smoother))
	{
		TestEqual(TEXT("as an additive trait"), Smoother->Mode, FString(TEXT("additive")));
	}
	const FHyperAIUAFTrait* Blend = All.FindByPredicate([](const FHyperAIUAFTrait& Trait) { return Trait.Name == BlendByBool; });
	if (TestNotNull(TEXT("blend by bool is in the catalog"), Blend))
	{
		TestTrue(TEXT("and its children read as pose pins"), Blend->Pins.ContainsByPredicate(
			[](const FHyperAIUAFPin& Pin) { return Pin.Name == TEXT("TrueChild") && Pin.bPose; }));
	}
	TestTrue(TEXT("the filter narrows the catalog"), Gate::ReadTraitCatalog(TEXT("BlendByBool")).Num() < All.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUAFGraphTest,
	"HyperAIStudio.UAF.Graph.LocomotionBuildCompileAndChecks",
	HyperAIStudio::UAF::Tests::Flags)

bool FHyperAIStudioUAFGraphTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::UAF::Tests;
	namespace Gate = HyperAIStudio::UAF::Gate;
	if (!AssetsPresent(*this) || !TraitsPresent(*this)) return true;
	FString Error;
	UUAFAnimGraph* Graph = Gate::Create(TestPath(), Error);
	if (!TestNotNull(TEXT("a UAF animation graph is created: ") + Error, Graph)) return false;
	ON_SCOPE_EXIT { Discard(Graph); };

	auto Rejects = [&](const TArray<FHyperAIUAFOp>& Ops, const TCHAR* Expected)
	{
		FString Why;
		const bool bValid = Gate::ValidateOps(Graph, Ops, Why);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Why), !bValid && Why.Contains(Expected));
	};
	Rejects({Op(TEXT("add_trait"), TEXT("Nowhere"), SequencePlayer)}, TEXT("no trait stack named"));
	Rejects({Op(TEXT("add_node"), TEXT("A")), Op(TEXT("add_trait"), TEXT("A"), TEXT("FNotATrait"))}, TEXT("no registered trait"));
	Rejects({Op(TEXT("add_node"), TEXT("A")), Op(TEXT("add_trait"), TEXT("A"), BlendSmoother)}, TEXT("add a base trait"));
	Rejects({Op(TEXT("add_node"), TEXT("A")), Op(TEXT("add_trait"), TEXT("A"), SequencePlayer),
		Op(TEXT("add_trait"), TEXT("A"), BlendByBool)}, TEXT("already has the base trait"));
	Rejects({Op(TEXT("add_node"), TEXT("A")), Op(TEXT("add_trait"), TEXT("A"), SequencePlayer),
		Op(TEXT("set_pin"), TEXT("A"), SequencePlayer, TEXT("Nope"), TEXT("1"))}, TEXT("has no pin named"));
	Rejects({Op(TEXT("add_node"), TEXT("A")), Op(TEXT("add_trait"), TEXT("A"), BlendByBool),
		Op(TEXT("set_pin"), TEXT("A"), BlendByBool, TEXT("TrueChild"), TEXT("1"))}, TEXT("is a pose pin"));
	Rejects({Op(TEXT("add_node"), TEXT("A")), Op(TEXT("add_trait"), TEXT("A"), SequencePlayer),
		Op(TEXT("bind_pin"), TEXT("A"), SequencePlayer, TEXT("PlayRate"), TEXT("Missing"))}, TEXT("no variable named"));
	Rejects({Op(TEXT("add_node"), TEXT("2Bad"))}, TEXT("identifier"));
	TestTrue(TEXT("the locomotion plan validates: ") + Error, Gate::ValidateOps(Graph, Locomotion(), Error));

	{
		const FScopedTransaction Transaction(NSLOCTEXT("HyperAIStudioUAFTests", "Build", "Build"));
		TestTrue(TEXT("the locomotion graph builds: ") + Error, Gate::ApplyOps(*Graph, Locomotion(), Error));
	}
	FString FirstError;
	int32 Errors = 0;
	int32 Warnings = 0;
	const bool bCompiled = Gate::Compile(*Graph, FirstError, Errors, Warnings);
	TestTrue(*FString::Printf(TEXT("and compiles (%d errors, first: %s)"), Errors, *FirstError), bCompiled);
	TestNotEqual(TEXT("its compile status is not an error"), Gate::CompileStatusOf(*Graph), FString(TEXT("errors")));

	TArray<FHyperAIUAFVariable> Variables;
	TArray<FHyperAIUAFNode> Nodes;
	FString OutputNode;
	Gate::Read(*Graph, Variables, Nodes, OutputNode);
	TestEqual(TEXT("the blend feeds the output pose"), OutputNode, FString(TEXT("Locomotion")));
	TestEqual(TEXT("three trait stacks"), Nodes.Num(), 3);
	const FHyperAIUAFVariable* Speed = Variables.FindByPredicate([](const FHyperAIUAFVariable& V) { return V.Name == TEXT("Speed"); });
	TestTrue(TEXT("Speed is a float and something reads it"), Speed && Speed->Type == TEXT("float") && Speed->bUsed);
	if (const FHyperAIUAFNode* Blend = FindNode(Nodes, TEXT("Locomotion")))
	{
		TestTrue(TEXT("the blend reaches the output"), Blend->bReachesOutput);
		TestEqual(TEXT("with two traits"), Blend->Traits.Num(), 2);
		const FHyperAIUAFPin* TrueChild = FindPin(*Blend, BlendByBool, TEXT("TrueChild"));
		TestTrue(TEXT("and the jog feeding its true child"), TrueChild && TrueChild->bLinked && TrueChild->Value == TEXT("Jog"));
		const FHyperAIUAFPin* Condition = FindPin(*Blend, BlendByBool, TEXT("bCondition"));
		TestTrue(TEXT("and IsMoving driving its condition"), Condition && Condition->bLinked && Condition->Value == TEXT("IsMoving"));
	}
	if (const FHyperAIUAFNode* IdleNode = FindNode(Nodes, TEXT("Idle")))
	{
		const FHyperAIUAFPin* Sequence = FindPin(*IdleNode, SequencePlayer, TEXT("AnimSequence"));
		TestTrue(TEXT("the idle stack plays its sequence"), Sequence && Sequence->Value.Contains(TEXT("MM_Unarmed_Idle_Ready")));
	}

	bool bAllocates = false;
	TArray<FHyperAIUAFIssue> Issues = Gate::Check(*Graph, bAllocates);
	TestTrue(TEXT("the built graph allocates a runnable instance"), bAllocates);
	TestFalse(TEXT("and has no errors"), Issues.ContainsByPredicate([](const FHyperAIUAFIssue& Issue) { return Issue.Severity == TEXT("error"); }));
	TestFalse(TEXT("no stack is orphaned"), HasIssue(Issues, TEXT("orphan_stack")));

	// Plant the classic mistakes and check each is caught; then undo them.
	const FString Built = Gate::ComputeRevision(*Graph);
	{
		const FScopedTransaction Transaction(NSLOCTEXT("HyperAIStudioUAFTests", "Break", "Break"));
		TestTrue(TEXT("the mistakes apply: ") + Error, Gate::ApplyOps(*Graph, {
			Op(TEXT("add_node"), TEXT("Stray")),
			Op(TEXT("add_variable"), TEXT(""), TEXT(""), TEXT(""), TEXT("float"), TEXT(""), TEXT("Unread")),
			Op(TEXT("remove_node"), TEXT("Jog"))}, Error));
	}
	Issues = Gate::Check(*Graph, bAllocates);
	TestTrue(TEXT("a stack with no traits is an error"), HasIssue(Issues, TEXT("empty_stack")));
	TestTrue(TEXT("and nothing reaches it"), HasIssue(Issues, TEXT("orphan_stack")));
	TestTrue(TEXT("a pose pin with nothing feeding it is flagged"), HasIssue(Issues, TEXT("unlinked_pose")));
	TestTrue(TEXT("a variable nothing reads is flagged"), HasIssue(Issues, TEXT("unused_variable")));
	TestNotEqual(TEXT("the revision moved"), Gate::ComputeRevision(*Graph), Built);

	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestEqual(TEXT("one undo restores the graph"), Gate::ComputeRevision(*Graph), Built);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUAFFrontDoorTest,
	"HyperAIStudio.UAF.Contracts.PlanFrontDoor",
	HyperAIStudio::UAF::Tests::Flags)

bool FHyperAIStudioUAFFrontDoorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::UAF::Tests;
	FHyperAIUAFInspectRequest Inspect;
	Inspect.TargetPath = TEXT("C:/Nope");
	TestEqual(TEXT("non-/Game paths are refused"), FHyperAIStudioUAFContracts::Inspect(Inspect).Status, FString(TEXT("invalid_request")));
	Inspect.TargetPath.Reset();
	TestEqual(TEXT("and an empty request without the catalog flag"), FHyperAIStudioUAFContracts::Inspect(Inspect).Status, FString(TEXT("invalid_request")));
	if (TraitsPresent(*this))
	{
		Inspect.bIncludeTraitCatalog = true;
		Inspect.TraitFilter = TEXT("Blend");
		const FHyperAIUAFInspectReport Catalog = FHyperAIStudioUAFContracts::Inspect(Inspect);
		TestTrue(*FString::Printf(TEXT("the catalog comes back without a target (%s)"), *Catalog.Diagnostic),
			Catalog.bOk && Catalog.TraitCatalog.Num() > 0 && Catalog.Nodes.IsEmpty());
	}
	const FString Path = TestPath();
	Inspect = {};
	Inspect.TargetPath = Path;
	TestEqual(TEXT("a missing graph says so"), FHyperAIStudioUAFContracts::Inspect(Inspect).Status, FString(TEXT("target_not_found")));

	FHyperAIUAFApplyPlanRequest Plan;
	Plan.TargetPath = Path;
	Plan.Ops = {Op(TEXT("add_node"), TEXT("A"))};
	TestEqual(TEXT("editing needs a revision"), FHyperAIStudioUAFContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_target_or_revision")));
	Plan.bCreate = true;
	if (!TraitsPresent(*this)) return true;
	Plan.Ops = {Op(TEXT("add_node"), TEXT("A")), Op(TEXT("add_trait"), TEXT("A"), TEXT("FNotATrait"))};
	TestEqual(TEXT("bad ops are refused before anything is prepared"), FHyperAIStudioUAFContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_op")));
	if (!AssetsPresent(*this)) return true;
	Plan.Ops = Locomotion();
	if (!FHyperAIStudioUAFContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("uaf_graph is not admitted here; the prepared dry run is skipped."));
		return true;
	}
	const FHyperAIUAFApplyPlanReport DryRun = FHyperAIStudioUAFContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk && DryRun.bTrustedPrepared);
	TestEqual(TEXT("and creates nothing"), FHyperAIStudioUAFContracts::Inspect(Inspect).Status, FString(TEXT("target_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUAFEndToEndTest,
	"HyperAIStudio.UAF.Execution.JournaledCreateCompileAndValidate",
	HyperAIStudio::UAF::Tests::Flags)

bool FHyperAIStudioUAFEndToEndTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::UAF::Tests;
	// Creates an asset through the executor, so it runs only in unattended test runs.
	if (!FApp::IsUnattended())
	{
		AddInfo(TEXT("Runs only in unattended test runs."));
		return true;
	}
	if (!AssetsPresent(*this) || !TraitsPresent(*this)
		|| !FHyperAIStudioUAFContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("Mannequin content or UAF traits missing, or uaf_graph is not admitted here; skipped."));
		return true;
	}
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bRequiredApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT { Settings->bRequireApprovalForAgentEdits = bRequiredApproval; };

	FHyperAIUAFApplyPlanRequest Plan;
	Plan.TargetPath = TestPath();
	Plan.bCreate = true;
	Plan.Ops = Locomotion();
	const FHyperAIUAFApplyPlanReport DryRun = FHyperAIStudioUAFContracts::BuildPlan(Plan);
	if (!TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk))
	{
		return false;
	}
	Plan.bDryRun = false;
	Plan.OperationId = TEXT("uaf-e2e-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Plan.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAIUAFApplyPlanReport Submitted = FHyperAIStudioUAFContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("submitted (%s: %s)"), *Submitted.Status, *Submitted.Diagnostic), Submitted.bExecutionSubmitted);
	ON_SCOPE_EXIT { Discard(HyperAIStudio::UAF::Gate::Resolve(Plan.TargetPath, false)); };
	FHyperAIStudioTypedArtifactOperationStatus Status;
	FString StatusError;
	PumpUntil([&]()
	{
		return !Submitted.bExecutionSubmitted
			|| (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Plan.OperationId, Status, StatusError) && Status.bTerminal);
	}, 60.0);
	TestEqual(*FString::Printf(TEXT("operation completed (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));

	FHyperAIUAFInspectRequest Inspect;
	Inspect.TargetPath = Plan.TargetPath;
	const FHyperAIUAFInspectReport Inspected = FHyperAIStudioUAFContracts::Inspect(Inspect);
	TestTrue(*FString::Printf(TEXT("the graph exists and compiled (%s: %s)"), *Inspected.Status, *Inspected.CompileStatus),
		Inspected.bOk && Inspected.CompileStatus != TEXT("errors"));
	TestEqual(TEXT("with its three trait stacks"), Inspected.Nodes.Num(), 3);
	FHyperAIUAFValidateRequest Validate;
	Validate.TargetPath = Plan.TargetPath;
	const FHyperAIUAFValidateReport Validated = FHyperAIStudioUAFContracts::Validate(Validate);
	TestEqual(*FString::Printf(TEXT("validate finds no errors (%s)"), *Validated.Diagnostic), Validated.ErrorCount, 0);
	TestTrue(TEXT("and the graph allocates a runnable instance"), Validated.bAllocatesInstance);
	return true;
}

#endif
