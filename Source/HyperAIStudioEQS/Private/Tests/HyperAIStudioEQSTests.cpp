// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Services/BTService_RunEQS.h"
#include "BehaviorTree/Tasks/BTTask_RunEQSQuery.h"
#include "DataProviders/AIDataProvider_QueryParams.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnvironmentQuery/Contexts/EnvQueryContext_Querier.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/Generators/EnvQueryGenerator_SimpleGrid.h"
#include "EnvironmentQuery/Tests/EnvQueryTest_Distance.h"
#include "HyperAIStudioEQSGate.h"
#include "HyperAIStudioEQSToolset.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

namespace HyperAIStudio::EQSTests
{
	using C = FHyperAIStudioEQSContracts;
	namespace Gate = HyperAIStudio::EQS::Gate;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FHyperAIEQSOp Op(const TCHAR* Kind, const TCHAR* Type = TEXT(""), const TCHAR* Params = TEXT(""))
	{
		FHyperAIEQSOp Out;
		Out.Kind = Kind;
		Out.Type = Type;
		Out.Params = Params;
		return Out;
	}

	FHyperAIEQSOp TreeOp(const TCHAR* Kind, const FString& Tree, const TCHAR* Parent, const TCHAR* Key = TEXT(""), const TCHAR* Type = TEXT(""), const TCHAR* Params = TEXT(""))
	{
		FHyperAIEQSOp Out = Op(Kind, Type, Params);
		Out.BehaviorTree = Tree;
		Out.Parent = Parent;
		Out.BlackboardKey = Key;
		return Out;
	}

	FString TestAsset(const TCHAR* Name)
	{
		return FString::Printf(TEXT("/Game/HyperAITests/EQS/%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	/** Runs a request with approval off, as an agent with one-shot edits allowed would. */
	FHyperAIEQSPlanReport Run(const FString& Asset, TArray<FHyperAIEQSOp> Ops, const bool bDryRun)
	{
		FHyperAIEQSPlanRequest Request;
		Request.Asset = Asset;
		Request.Operations = MoveTemp(Ops);
		Request.bDryRun = bDryRun;
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bApproval = Settings->bRequireApprovalForAgentEdits;
		Settings->bRequireApprovalForAgentEdits = false;
		FHyperAIEQSPlanReport Report = C::ApplyPlan(Request);
		Settings->bRequireApprovalForAgentEdits = bApproval;
		return Report;
	}

	bool Refused(const FHyperAIEQSPlanReport& Report, const TCHAR* Says)
	{
		return !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Says);
	}

	UEnvQuery* LoadQuery(const FString& Path)
	{
		return FindObject<UEnvQuery>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
	}

	FString Setting(const FHyperAIEQSOptionView& Option, const TCHAR* Name)
	{
		for (const FString& Each : Option.Settings)
		{
			if (Each.StartsWith(FString(Name) + TEXT("="))) return Each.Mid(FCString::Strlen(Name) + 1);
		}
		return FString();
	}

	/** A Behavior Tree with a Blackboard holding a Vector key and a Bool key, made outside any undo step. */
	UBehaviorTree* MakeTree(const FString& Path, const bool bBlackboard)
	{
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		UPackage* Package = CreatePackage(*Path);
		UBehaviorTree* Tree = NewObject<UBehaviorTree>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
		if (bBlackboard)
		{
			UPackage* BlackboardPackage = CreatePackage(*(Path + TEXT("_BB")));
			UBlackboardData* Blackboard = NewObject<UBlackboardData>(BlackboardPackage, *(FPackageName::GetShortName(Path) + TEXT("_BB")), RF_Public | RF_Standalone | RF_Transactional);
			FBlackboardEntry& Cover = Blackboard->Keys.AddDefaulted_GetRef();
			Cover.EntryName = TEXT("CoverLocation");
			Cover.KeyType = NewObject<UBlackboardKeyType_Vector>(Blackboard);
			FBlackboardEntry& Alert = Blackboard->Keys.AddDefaulted_GetRef();
			Alert.EntryName = TEXT("IsAlert");
			Alert.KeyType = NewObject<UBlackboardKeyType_Bool>(Blackboard);
			Tree->BlackboardAsset = Blackboard;
		}
		return Tree;
	}

	AActor* Spawn(UWorld& World, const FVector& At, const FString& Label)
	{
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		ATargetPoint* Point = World.SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), FTransform(At));
		Point->SetActorLabel(Label);
		return Point;
	}

	void UndoAll(const int32 Steps)
	{
		for (int32 Index = 0; Index < Steps && GEditor; ++Index) GEditor->UndoTransaction();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioEQSParamsTest, "HyperAIStudio.NativeTools.EQS.Params", HyperAIStudio::EQSTests::Flags)

bool FHyperAIStudioEQSParamsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EQSTests;
	FString Error;
	TestEqual(TEXT("grid is the simple grid"), Gate::ResolveGenerator(TEXT("Grid"), Error), UEnvQueryGenerator_SimpleGrid::StaticClass());
	TestNull(TEXT("an unknown generator is refused"), Gate::ResolveGenerator(TEXT("spiral"), Error));
	TestTrue(TEXT("naming the generators: ") + Error, Error.Contains(TEXT("donut")) && Error.Contains(TEXT("actors_of_class")));
	TestEqual(TEXT("distance is the distance test"), Gate::ResolveTest(TEXT("distance"), Error), UEnvQueryTest_Distance::StaticClass());
	TestNull(TEXT("an unknown test is refused"), Gate::ResolveTest(TEXT("telepathy"), Error));
	TestTrue(TEXT("naming the tests: ") + Error, Error.Contains(TEXT("gameplay_tags")) && Error.Contains(TEXT("pathfinding")));
	TArray<TPair<FString, FString>> Pairs;
	TestTrue(TEXT("settings split"), Gate::SplitParams(TEXT("GridSize=500; SpaceBetween = 100 ;"), Pairs, Error) && Pairs.Num() == 2 && Pairs[1].Value == TEXT("100"));
	TestFalse(TEXT("a setting without = is refused"), Gate::SplitParams(TEXT("GridSize"), Pairs, Error));

	UEnvQueryGenerator_SimpleGrid* Grid = NewObject<UEnvQueryGenerator_SimpleGrid>(GetTransientPackage());
	TestTrue(TEXT("a number into a data provider: ") + Error, Gate::ApplyParams(*Grid, TEXT("GridSize=500; spacebetween=125"), Error));
	TestEqual(TEXT("GridSize 500"), Grid->GridSize.DefaultValue, 500.f);
	TestEqual(TEXT("names match in any case"), Grid->SpaceBetween.DefaultValue, 125.f);
	TestTrue(TEXT("param: binds a query param: ") + Error, Gate::ApplyParams(*Grid, TEXT("GridSize=param:Reach"), Error));
	const UAIDataProvider_QueryParams* Binding = Cast<UAIDataProvider_QueryParams>(Grid->GridSize.DataBinding);
	TestTrue(TEXT("named Reach"), Binding && Binding->ParamName == TEXT("Reach") && Grid->GridSize.DataField == TEXT("FloatValue"));
	TestTrue(TEXT("a plain value unbinds it"), Gate::ApplyParams(*Grid, TEXT("GridSize=400"), Error) && Grid->GridSize.DataBinding == nullptr);
	TestTrue(TEXT("a nested enum through a dot: ") + Error, Gate::ApplyParams(*Grid, TEXT("ProjectionData.TraceMode=None"), Error));
	TestEqual(TEXT("projection off"), (int32)Grid->ProjectionData.TraceMode, (int32)EEnvQueryTrace::None);
	TestTrue(TEXT("Context is the kind's context: ") + Error, Gate::ApplyParams(*Grid, TEXT("Context=item"), Error));
	TestTrue(TEXT("and querier puts it back: ") + Error, Gate::ApplyParams(*Grid, TEXT("GenerateAround=querier"), Error) && Grid->GenerateAround == UEnvQueryContext_Querier::StaticClass());
	TestFalse(TEXT("an unknown setting is refused"), Gate::ApplyParams(*Grid, TEXT("Radius=5"), Error));
	TestTrue(TEXT("naming the settings: ") + Error, Error.Contains(TEXT("GridSize")) && Error.Contains(TEXT("SpaceBetween")));
	TestFalse(TEXT("a word is not a number"), Gate::ApplyParams(*Grid, TEXT("GridSize=wide"), Error));
	TestFalse(TEXT("an unknown context is refused"), Gate::ApplyParams(*Grid, TEXT("GenerateAround=player"), Error));
	TestTrue(TEXT("naming the contexts: ") + Error, Error.Contains(TEXT("querier")));

	UEnvQueryTest_Distance* Distance = NewObject<UEnvQueryTest_Distance>(GetTransientPackage());
	TestTrue(TEXT("enums by short name, and the short setting names: ") + Error,
		Gate::ApplyParams(*Distance, TEXT("Purpose=Filter; Filter=Range; Min=0; Max=150; Scoring=InverseLinear; Clamp=None"), Error));
	TestEqual(TEXT("TestPurpose Filter"), (int32)Distance->TestPurpose, (int32)EEnvTestPurpose::Filter);
	TestEqual(TEXT("FilterType Range"), (int32)Distance->FilterType, (int32)EEnvTestFilterType::Range);
	TestEqual(TEXT("Max 150"), Distance->FloatValueMax.DefaultValue, 150.f);
	TestEqual(TEXT("InverseLinear"), (int32)Distance->ScoringEquation, (int32)EEnvTestScoreEquation::InverseLinear);
	TestFalse(TEXT("a word outside the enum is refused"), Gate::ApplyParams(*Distance, TEXT("Purpose=Guess"), Error));
	TestTrue(TEXT("naming its words: ") + Error, Error.Contains(TEXT("Score")) && Error.Contains(TEXT("FilterAndScore")));
	const TArray<FString> Changed = Gate::DescribeSettings(*Distance, true);
	TestTrue(TEXT("changed settings read back as written"), Changed.Contains(TEXT("FloatValueMax=150")) && Changed.Contains(TEXT("TestPurpose=Filter")));
	Grid->MarkAsGarbage();
	Distance->MarkAsGarbage();

	// Every kind describes its settings for hyper_eqs_inspect.
	const FHyperAIEQSInspectReport Listed = C::Inspect(FHyperAIEQSInspectRequest());
	TestTrue(TEXT("inspect without an asset lists the kinds: ") + Listed.Diagnostic, Listed.bOk && Listed.Kinds.Num() == Gate::Generators().Num() + Gate::Tests().Num() + Gate::Contexts().Num());
	const FHyperAIEQSKind* GridKind = Listed.Kinds.FindByPredicate([](const FHyperAIEQSKind& Kind) { return Kind.Kind == TEXT("grid"); });
	TestTrue(TEXT("with their settings and defaults"), GridKind && GridKind->Settings.ContainsByPredicate([](const FString& Line)
		{
			return Line.StartsWith(TEXT("GridSize: number or param:Name, default"));
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioEQSDryRunTest, "HyperAIStudio.NativeTools.EQS.DryRun", HyperAIStudio::EQSTests::Flags)

bool FHyperAIStudioEQSDryRunTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EQSTests;
	const FString Asset = TestAsset(TEXT("EQS_Dry"));
	const FHyperAIEQSPlanReport Report = Run(Asset, {
		Op(TEXT("create")),
		Op(TEXT("option.add"), TEXT("grid"), TEXT("GridSize=600; SpaceBetween=150")),
		Op(TEXT("test.add"), TEXT("distance"), TEXT("Purpose=Score; Scoring=InverseLinear")),
		Op(TEXT("test.add"), TEXT("trace"), TEXT("Purpose=Filter; Bool=false"))}, true);
	TestTrue(TEXT("checked: ") + Report.Diagnostic, Report.bOk && Report.Status == TEXT("checked"));
	TestTrue(TEXT("reports the query it would leave"), Report.Options.Num() == 1 && Report.Options[0].Generator == TEXT("grid") && Report.Options[0].Tests.Num() == 2);
	TestEqual(TEXT("with its settings"), Setting(Report.Options[0], TEXT("GridSize")), FString(TEXT("600")));
	TestTrue(TEXT("and the tests' purposes"), Report.Options.Num() == 1 && Report.Options[0].Tests.Num() == 2 && Report.Options[0].Tests[1].Purpose == TEXT("filter"));
	TestTrue(TEXT("warns the grid projects onto the navmesh"), Report.Warnings.ContainsByPredicate([](const FString& Each) { return Each.Contains(TEXT("navmesh")); }));
	TestTrue(TEXT("nothing is made"), !LoadQuery(Asset) && !FPackageName::DoesPackageExist(Asset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioEQSApplyUndoTest, "HyperAIStudio.NativeTools.EQS.ApplyAndUndo", HyperAIStudio::EQSTests::Flags)

bool FHyperAIStudioEQSApplyUndoTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EQSTests;
	if (!GEditor || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor, its undo buffer, and no play session."));
		return false;
	}
	const FString Asset = TestAsset(TEXT("EQS_Cover"));
	const int32 UndoBefore = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	FHyperAIEQSOp Composite = Op(TEXT("option.add"), TEXT("composite"));
	Composite.Children = {TEXT("circle: CircleRadius=400"), TEXT("current_location")};
	const FHyperAIEQSPlanReport Made = Run(Asset, {
		Op(TEXT("create")),
		Op(TEXT("option.add"), TEXT("donut"), TEXT("InnerRadius=200; OuterRadius=800; NumberOfRings=3; ProjectionData.TraceMode=None")),
		Op(TEXT("test.add"), TEXT("distance"), TEXT("Purpose=Score; Scoring=InverseLinear; Context=querier")),
		Op(TEXT("test.add"), TEXT("dot"), TEXT("Purpose=Score; LineA.LineFrom=querier; LineA.LineTo=item")),
		Composite}, false);
	ON_SCOPE_EXIT
	{
		// Whatever failed below: the plans taken back.
		UndoAll(GEditor->Trans->GetQueueLength() - UndoBefore - GEditor->Trans->GetUndoCount());
	};
	if (!TestTrue(TEXT("the plan runs: ") + Made.Diagnostic, Made.bOk && Made.Status == TEXT("completed"))) return false;
	TestEqual(TEXT("in one undo step"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - UndoBefore, 1);
	TestTrue(TEXT("named for the plan"), GEditor->Trans->GetUndoContext().Title.ToString().Contains(TEXT("EQS Plan")));
	UEnvQuery* Query = LoadQuery(Asset);
	if (!TestNotNull(TEXT("the query is made"), Query)) return false;
	TestEqual(TEXT("two options"), Query->GetOptions().Num(), 2);
	TestTrue(TEXT("the donut with two tests"), Query->GetOptions().Num() == 2 && Query->GetOptions()[0]->Tests.Num() == 2);
	TestTrue(TEXT("the composite with its two children"), Made.Options.Num() == 2 && Made.Options[1].Generator == TEXT("composite") && Made.Options[1].Children.Num() == 2);

	// A second plan changes the made query: the test becomes a filter.
	const FHyperAIEQSPlanReport Changed = Run(Asset, {[] { FHyperAIEQSOp Set = Op(TEXT("test.set"), TEXT(""), TEXT("Purpose=FilterAndScore; Filter=Maximum; Max=500")); Set.Option = 0; Set.Test = 0; return Set; }()}, false);
	if (!TestTrue(TEXT("test.set runs: ") + Changed.Diagnostic, Changed.bOk)) return false;
	TestEqual(TEXT("in its own undo step"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - UndoBefore, 2);
	UEnvQueryTest* First = Query->GetOptions()[0]->Tests[0];
	TestEqual(TEXT("the test filters now"), (int32)First->TestPurpose, (int32)EEnvTestPurpose::FilterAndScore);
	FHyperAIEQSInspectRequest Look;
	Look.Asset = Asset;
	const FHyperAIEQSInspectReport Read = C::Inspect(Look);
	TestTrue(TEXT("inspect reads it back: ") + Read.Diagnostic, Read.bOk && Read.Options.Num() == 2 && Read.Options[0].Tests.Num() == 2
		&& Read.Options[0].Tests[0].Purpose == TEXT("filter_and_score") && Read.Options[0].Tests[0].Settings.Contains(TEXT("FloatValueMax=500")));
	TestEqual(TEXT("and the donut's settings"), Setting(Read.Options[0], TEXT("OuterRadius")), FString(TEXT("800")));

	// Undo takes back one plan at a time.
	if (!TestTrue(TEXT("undo"), GEditor->UndoTransaction())) return false;
	TestEqual(TEXT("the test scores only again"), (int32)First->TestPurpose, (int32)EEnvTestPurpose::Score);
	if (!TestTrue(TEXT("undo again"), GEditor->UndoTransaction())) return false;
	TestFalse(TEXT("undo removes the made query"), IsValid(Query));
	TestFalse(TEXT("leaving nothing to save"), Query->GetPackage()->IsDirty());
	if (!TestTrue(TEXT("redo"), GEditor->RedoTransaction())) return false;
	TestTrue(TEXT("redo brings it back whole"), IsValid(Query) && Query->GetOptions().Num() == 2 && Query->GetOptions()[0]->Tests.Num() == 2);
	GEditor->UndoTransaction();

	// Undone, the path is free to make again.
	const FHyperAIEQSPlanReport Again = Run(Asset, {Op(TEXT("create")), Op(TEXT("option.add"), TEXT("current_location"))}, false);
	TestTrue(TEXT("the same Asset can be made again after undo: ") + Again.Diagnostic, Again.bOk && Again.Options.Num() == 1);
	TestTrue(TEXT("as a new query"), LoadQuery(Asset) && LoadQuery(Asset) != Query);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioEQSRunTest, "HyperAIStudio.NativeTools.EQS.Run", HyperAIStudio::EQSTests::Flags)

bool FHyperAIStudioEQSRunTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EQSTests;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor with a level open, its undo buffer, and no play session."));
		return false;
	}
	const FString Tag = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6);
	const FVector Origin(-23000.0, 17000.0, 500.0);
	TArray<AActor*> Spawned;
	Spawned.Add(Spawn(*World, Origin, TEXT("EQSQuerier_") + Tag));
	Spawned.Add(Spawn(*World, Origin + FVector(900.0, 0.0, 0.0), TEXT("EQSFar_") + Tag));
	Spawned.Add(Spawn(*World, Origin + FVector(0.0, 250.0, 0.0), TEXT("EQSNear_") + Tag));
	Spawned.Add(Spawn(*World, Origin + FVector(-500.0, -500.0, 0.0), TEXT("EQSMid_") + Tag));
	ON_SCOPE_EXIT
	{
		for (AActor* Actor : Spawned)
		{
			if (IsValid(Actor)) World->EditorDestroyActor(Actor, false);
		}
	};
	const int32 UndoBefore = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	ON_SCOPE_EXIT
	{
		UndoAll(GEditor->Trans->GetQueueLength() - UndoBefore - GEditor->Trans->GetUndoCount());
	};

	// Points: a 7 x 7 grid 100 apart, kept within Reach of the querier.
	const FString Points = TestAsset(TEXT("EQS_Points"));
	const FHyperAIEQSPlanReport MadePoints = Run(Points, {
		Op(TEXT("create")),
		Op(TEXT("option.add"), TEXT("grid"), TEXT("GridSize=300; SpaceBetween=100; ProjectionData.TraceMode=None")),
		Op(TEXT("test.add"), TEXT("distance"), TEXT("Purpose=FilterAndScore; Filter=Range; Min=0; Max=param:Reach; Scoring=InverseLinear"))}, false);
	if (!TestTrue(TEXT("the points query is made: ") + MadePoints.Diagnostic, MadePoints.bOk)) return false;
	TestTrue(TEXT("it reads the Reach param"), MadePoints.QueryParams.Num() == 1 && MadePoints.QueryParams[0].StartsWith(TEXT("Reach")));
	FHyperAIEQSRunRequest Ask;
	Ask.Asset = Points;
	Ask.Querier = TEXT("EQSQuerier_") + Tag;
	Ask.MaxItems = 3;
	const FHyperAIEQSRunReport Missing = C::Run(Ask);
	TestEqual(TEXT("without its param the query cannot run: ") + Missing.Diagnostic, Missing.Status, FString(TEXT("missing_param")));
	Ask.Params = TEXT("Reach=150");
	const FHyperAIEQSRunReport Near = C::Run(Ask);
	if (!TestTrue(TEXT("it runs: ") + Near.Diagnostic, Near.bOk && Near.Status == TEXT("success"))) return false;
	TestEqual(TEXT("in the editor world"), Near.World, FString(TEXT("editor")));
	TestEqual(TEXT("the 9 grid points within 150 pass"), Near.Passed, 9);
	TestEqual(TEXT("3 are listed"), Near.Items.Num(), 3);
	TestTrue(TEXT("the best is the querier's own spot, scoring 1"), Near.Items.Num() > 0 && Near.Items[0].Score > 0.99f && Near.Items[0].Location == Gate::VectorText(Origin));
	Ask.Params = TEXT("Reach=50");
	TestEqual(TEXT("a tighter reach keeps one point"), C::Run(Ask).Passed, 1);
	Ask.Params = TEXT("Speed=3");
	TestTrue(TEXT("an unknown param is refused"), C::Run(Ask).Diagnostic.Contains(TEXT("reads no param Speed")));

	// Actors: the target points, nearest first.
	const FString Targets = TestAsset(TEXT("EQS_Targets"));
	const FHyperAIEQSPlanReport MadeTargets = Run(Targets, {
		Op(TEXT("create")),
		Op(TEXT("option.add"), TEXT("actors_of_class"), TEXT("SearchedActorClass=/Script/Engine.TargetPoint; GenerateOnlyActorsInRadius=true; SearchRadius=1500")),
		Op(TEXT("test.add"), TEXT("distance"), TEXT("Purpose=Score; Scoring=InverseLinear"))}, false);
	if (!TestTrue(TEXT("the actors query is made: ") + MadeTargets.Diagnostic, MadeTargets.bOk)) return false;
	FHyperAIEQSRunRequest AskActors;
	AskActors.Asset = Targets;
	AskActors.Querier = TEXT("EQSQuerier_") + Tag;
	AskActors.RunMode = TEXT("all");
	const FHyperAIEQSRunReport Actors = C::Run(AskActors);
	if (!TestTrue(TEXT("it runs: ") + Actors.Diagnostic, Actors.bOk && Actors.ItemType == TEXT("actor"))) return false;
	TestEqual(TEXT("four target points within 1500 (the querier is one)"), Actors.Passed, 4);
	TestTrue(TEXT("the querier first, then the near one"), Actors.Items.Num() >= 2 && Actors.Items[0].Actor == TEXT("EQSQuerier_") + Tag && Actors.Items[1].Actor == TEXT("EQSNear_") + Tag);
	TestTrue(TEXT("and the far one last"), Actors.Items.Num() == 4 && Actors.Items[3].Actor == TEXT("EQSFar_") + Tag);
	AskActors.Querier = TEXT("Nobody_") + Tag;
	TestTrue(TEXT("a missing querier is refused"), C::Run(AskActors).Diagnostic.Contains(TEXT("No actor")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioEQSBehaviorTreeTest, "HyperAIStudio.NativeTools.EQS.BehaviorTree", HyperAIStudio::EQSTests::Flags)

bool FHyperAIStudioEQSBehaviorTreeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EQSTests;
	if (!GEditor || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor, its undo buffer, and no play session."));
		return false;
	}
	const FString TreePath = TestAsset(TEXT("BT_Guard"));
	UBehaviorTree* Tree = MakeTree(TreePath, true);
	const FString Bare = TestAsset(TEXT("BT_Bare"));
	MakeTree(Bare, false);
	const FString Asset = TestAsset(TEXT("EQS_Cover"));
	const int32 UndoBefore = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	ON_SCOPE_EXIT
	{
		UndoAll(GEditor->Trans->GetQueueLength() - UndoBefore - GEditor->Trans->GetUndoCount());
	};
	const TArray<FHyperAIEQSOp> Query = {Op(TEXT("create")), Op(TEXT("option.add"), TEXT("grid"), TEXT("ProjectionData.TraceMode=None")),
		Op(TEXT("test.add"), TEXT("distance"), TEXT("Purpose=Score"))};

	// Refusals first: nothing changes.
	TArray<FHyperAIEQSOp> NoKey = Query;
	NoKey.Add(TreeOp(TEXT("bt.add_composite"), TreePath, TEXT("root"), TEXT(""), TEXT("selector")));
	NoKey.Add(TreeOp(TEXT("bt.run_eqs_task"), TreePath, TEXT("Selector"), TEXT("IsAlert")));
	TestTrue(TEXT("a Bool key is refused, naming the keys that fit"), Refused(Run(Asset, NoKey, false), TEXT("CoverLocation")));
	NoKey.Last().BlackboardKey = TEXT("Nowhere");
	TestTrue(TEXT("a missing key is refused"), Refused(Run(Asset, NoKey, false), TEXT("has no key Nowhere")));
	TArray<FHyperAIEQSOp> NoBoard = Query;
	NoBoard.Add(TreeOp(TEXT("bt.add_composite"), Bare, TEXT("root"), TEXT(""), TEXT("selector")));
	NoBoard.Add(TreeOp(TEXT("bt.run_eqs_task"), Bare, TEXT("Selector"), TEXT("CoverLocation")));
	TestTrue(TEXT("a tree without a Blackboard is refused"), Refused(Run(Asset, NoBoard, false), TEXT("has no Blackboard")));
	TArray<FHyperAIEQSOp> Straight = Query;
	Straight.Add(TreeOp(TEXT("bt.run_eqs_task"), TreePath, TEXT("root"), TEXT("CoverLocation")));
	TestTrue(TEXT("a task straight under an empty root is refused"), Refused(Run(Asset, Straight, false), TEXT("add a selector or sequence first")));
	TestTrue(TEXT("and nothing was made"), !LoadQuery(Asset) && Tree->RootNode == nullptr && Tree->BTGraph == nullptr);

	TArray<FHyperAIEQSOp> Wired = Query;
	Wired.Add(TreeOp(TEXT("bt.add_composite"), TreePath, TEXT("root"), TEXT(""), TEXT("selector"), TEXT("NodeName=Find Cover")));
	Wired.Add(TreeOp(TEXT("bt.run_eqs_task"), TreePath, TEXT("Find Cover"), TEXT("CoverLocation"), TEXT(""), TEXT("RunMode=RandomBest5Pct")));
	Wired.Add(TreeOp(TEXT("bt.run_eqs_service"), TreePath, TEXT("Find Cover"), TEXT("CoverLocation"), TEXT(""), TEXT("Interval=2")));
	const FHyperAIEQSPlanReport Done = Run(Asset, Wired, false);
	if (!TestTrue(TEXT("the plan runs: ") + Done.Diagnostic, Done.bOk)) return false;
	TestEqual(TEXT("in one undo step"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - UndoBefore, 1);
	TestEqual(TEXT("three tree nodes added"), Done.TreeNodes.Num(), 3);
	UBTCompositeNode* Root = Tree->RootNode;
	TestTrue(TEXT("the tree's root is the selector"), Root && Root->IsA<UBTComposite_Selector>() && Root->GetNodeName() == TEXT("Find Cover"));
	const UBTTask_RunEQSQuery* Task = Root && Root->Children.Num() == 1 ? Cast<UBTTask_RunEQSQuery>(Root->Children[0].ChildTask) : nullptr;
	TestTrue(TEXT("with a Run EQS Query task running the query"), Task && Task->EQSRequest.QueryTemplate == LoadQuery(Asset)
		&& Task->EQSRequest.RunMode == EEnvQueryRunMode::RandomBest5Pct);
	TestTrue(TEXT("into CoverLocation"), Task && Task->GetSelectedBlackboardKey() == TEXT("CoverLocation"));
	const UBTService_RunEQS* Service = Root && Root->Services.Num() == 1 ? Cast<UBTService_RunEQS>(Root->Services[0]) : nullptr;
	TestTrue(TEXT("and a Run EQS service on the selector"), Service != nullptr);
	FHyperAIEQSInspectRequest Look;
	Look.BehaviorTree = TreePath;
	const FHyperAIEQSInspectReport Read = C::Inspect(Look);
	TestTrue(TEXT("inspect lists the composite, keys and EQS nodes: ") + Read.Diagnostic, Read.bOk && Read.Composites.Num() == 1 && Read.BlackboardKeys.Contains(TEXT("CoverLocation: Vector"))
		&& Read.TreeNodes.Num() == 2);

	if (!TestTrue(TEXT("undo"), GEditor->UndoTransaction())) return false;
	TestTrue(TEXT("undo empties the tree again"), Tree->RootNode == nullptr);
	TestFalse(TEXT("and removes the query"), LoadQuery(Asset) && IsValid(LoadQuery(Asset)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioEQSRefusalsTest, "HyperAIStudio.NativeTools.EQS.Refusals", HyperAIStudio::EQSTests::Flags)

bool FHyperAIStudioEQSRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EQSTests;
	const FString Asset = TestAsset(TEXT("EQS_Refused"));
	TestTrue(TEXT("Engine content is refused"), Refused(Run(TEXT("/Engine/AI/EQS_Thing"), {Op(TEXT("create"))}, false), TEXT("not project content")));
	TestTrue(TEXT("an unknown kind is refused, with the kinds"), Refused(Run(Asset, {Op(TEXT("create")), Op(TEXT("option.explode"))}, false), TEXT("test.add")));
	TestTrue(TEXT("create comes first"), Refused(Run(Asset, {Op(TEXT("option.add"), TEXT("grid")), Op(TEXT("create"))}, false), TEXT("create comes first")));
	TestTrue(TEXT("changing a query that is not there is refused"), Refused(Run(Asset, {Op(TEXT("option.add"), TEXT("grid"))}, false), TEXT("start the plan with create")));
	TestTrue(TEXT("an unknown generator is refused"), Refused(Run(Asset, {Op(TEXT("create")), Op(TEXT("option.add"), TEXT("spiral"))}, false), TEXT("pathing_grid")));
	TestTrue(TEXT("a test on no option is refused"), Refused(Run(Asset, {Op(TEXT("create")), Op(TEXT("test.add"), TEXT("distance"))}, false), TEXT("there is no option")));
	TestTrue(TEXT("an unknown setting is refused"), Refused(Run(Asset, {Op(TEXT("create")), Op(TEXT("option.add"), TEXT("grid"), TEXT("Size=5"))}, false), TEXT("has no setting Size")));
	TestTrue(TEXT("a composite needs children"), Refused(Run(Asset, {Op(TEXT("create")), Op(TEXT("option.add"), TEXT("composite"))}, false), TEXT("Children")));
	TestTrue(TEXT("an unknown tag is refused"), Refused(Run(Asset, {Op(TEXT("create")), Op(TEXT("option.add"), TEXT("actors_of_class"), TEXT("SearchedActorClass=Pawn")),
		Op(TEXT("test.add"), TEXT("gameplay_tags"), TEXT("TagQueryToMatch=any:No.Such.Tag.Here"))}, false), TEXT("not a gameplay tag")));
	TestTrue(TEXT("a missing tree is refused"), Refused(Run(Asset, {Op(TEXT("create")), TreeOp(TEXT("bt.add_composite"), TEXT("/Game/HyperAITests/EQS/BT_None"), TEXT("root"), TEXT(""),
		TEXT("selector"))}, false), TEXT("is not a Behavior Tree")));
	TestTrue(TEXT("refused plans make nothing"), !LoadQuery(Asset) && !FPackageName::DoesPackageExist(Asset));
	FHyperAIEQSRunRequest Ask;
	Ask.Asset = Asset;
	TestFalse(TEXT("running a missing query is refused"), C::Run(Ask).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioEQSSkillTest, "HyperAIStudio.NativeTools.EQS.Skill", HyperAIStudio::EQSTests::Flags)

bool FHyperAIStudioEQSSkillTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::EQSTests;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	FString Skill;
	if (!TestTrue(TEXT("the EQS skill ships with the plugin"), Plugin.IsValid()
		&& FFileHelper::LoadFileToString(Skill, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Skills/unreal-eqs/SKILL.md")))))
	{
		return false;
	}
	TestTrue(TEXT("it is a skill"), Skill.StartsWith(TEXT("---")) && Skill.Contains(TEXT("name: unreal-eqs")));
	for (const FString& Name : C::GetToolNames())
	{
		TestTrue(FString::Printf(TEXT("it names %s"), *Name), Skill.Contains(Name));
	}
	for (const TCHAR* Kind : {TEXT("create"), TEXT("option.add"), TEXT("option.set"), TEXT("option.remove"), TEXT("test.add"), TEXT("test.set"), TEXT("test.remove"),
		TEXT("bt.add_composite"), TEXT("bt.run_eqs_task"), TEXT("bt.run_eqs_service")})
	{
		TestTrue(FString::Printf(TEXT("it explains %s"), Kind), Skill.Contains(FString::Printf(TEXT("`%s`"), Kind)));
	}
	for (const TArray<Gate::FKindInfo>* Table : {&Gate::Generators(), &Gate::Tests()})
	{
		for (const Gate::FKindInfo& Kind : *Table)
		{
			TestTrue(FString::Printf(TEXT("and the %s kind"), Kind.Kind), Skill.Contains(FString::Printf(TEXT("`%s`"), Kind.Kind)));
		}
	}
	TestTrue(TEXT("and State Trees' Run Env Query task"), Skill.Contains(TEXT("Run Env Query")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
