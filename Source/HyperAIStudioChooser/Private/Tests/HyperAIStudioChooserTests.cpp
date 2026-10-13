// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Chooser.h"
#include "ChooserPropertyAccess.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/EngineTypes.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HyperAIStudioChooserGate.h"
#include "HyperAIStudioChooserToolset.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace HyperAIStudio::ChooserTests
{
	using C = FHyperAIStudioChooserContracts;
	namespace Gate = HyperAIStudio::Chooser::Gate;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	/** The context: a character movement component, whose speed, movement mode and rotation flag are Blueprint-visible. */
	const TCHAR* const Movement = TEXT("/Script/Engine.CharacterMovementComponent");
	const TCHAR* const Cube = TEXT("/Engine/BasicShapes/Cube");
	const TCHAR* const Sphere = TEXT("/Engine/BasicShapes/Sphere");
	const TCHAR* const Cylinder = TEXT("/Engine/BasicShapes/Cylinder");
	const TCHAR* const Cone = TEXT("/Engine/BasicShapes/Cone");

	FHyperAIChooserOp Op(const TCHAR* Kind)
	{
		FHyperAIChooserOp Out;
		Out.Kind = Kind;
		return Out;
	}

	FHyperAIChooserOp Create()
	{
		FHyperAIChooserOp Out = Op(TEXT("create"));
		Out.OutputObjectType = TEXT("/Script/Engine.StaticMesh");
		Out.ContextClasses = {Movement};
		Out.ContextDirections = {TEXT("both")};
		return Out;
	}

	FHyperAIChooserOp Column(const TCHAR* Kind, const TCHAR* Binding, const TCHAR* Params = TEXT(""))
	{
		FHyperAIChooserOp Out = Op(TEXT("column.add"));
		Out.ColumnKind = Kind;
		Out.Binding = Binding;
		Out.Params = Params;
		return Out;
	}

	FHyperAIChooserOp Row(const TCHAR* Result, TArray<FString> Cells)
	{
		FHyperAIChooserOp Out = Op(TEXT("row.add"));
		Out.Result = Result;
		Out.Cells = MoveTemp(Cells);
		return Out;
	}

	/**
	 * Locomotion by speed, movement mode and the rotation flag: slow is the cube, walking fast with the flag the sphere,
	 * falling fast the cylinder, anything else the cone; an output column writes the row's number to JumpZVelocity.
	 */
	TArray<FHyperAIChooserOp> Locomotion()
	{
		FHyperAIChooserOp Fallback = Op(TEXT("fallback.set"));
		Fallback.Result = Cone;
		Fallback.Cells = {TEXT(""), TEXT(""), TEXT(""), TEXT("9")};
		return {
			Create(),
			Column(TEXT("float_range"), TEXT("MaxWalkSpeed")),
			Column(TEXT("enum"), TEXT("Context0.MovementMode")),
			Column(TEXT("output_float"), TEXT("JumpZVelocity")),
			// A filter added after an output goes before it: the output moves to column 3.
			Column(TEXT("bool"), TEXT("bOrientRotationToMovement")),
			Row(Cube, {TEXT("..150"), TEXT("any"), TEXT(""), TEXT("0")}),
			Row(Sphere, {TEXT("150.."), TEXT("MOVE_Walking"), TEXT("true"), TEXT("1")}),
			Row(Cylinder, {TEXT("150.."), TEXT("Falling"), TEXT("any"), TEXT("2")}),
			Fallback};
	}

	FString TestAsset(const TCHAR* Name)
	{
		return FString::Printf(TEXT("/Game/HyperAITests/Chooser/%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	/** Runs a request with approval off, as an agent with one-shot edits allowed would. */
	FHyperAIChooserPlanReport Run(const FString& Asset, TArray<FHyperAIChooserOp> Ops, const bool bDryRun)
	{
		FHyperAIChooserPlanRequest Request;
		Request.Asset = Asset;
		Request.Operations = MoveTemp(Ops);
		Request.bDryRun = bDryRun;
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bApproval = Settings->bRequireApprovalForAgentEdits;
		Settings->bRequireApprovalForAgentEdits = false;
		FHyperAIChooserPlanReport Report = C::ApplyPlan(Request);
		Settings->bRequireApprovalForAgentEdits = bApproval;
		return Report;
	}

	FHyperAIChooserEvaluateReport Evaluate(const FString& Asset, const TCHAR* Values)
	{
		FHyperAIChooserEvaluateRequest Request;
		Request.Asset = Asset;
		Request.Values = Values;
		return C::Evaluate(Request);
	}

	bool Refused(const FHyperAIChooserPlanReport& Report, const TCHAR* Says)
	{
		return !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Says);
	}

	UChooserTable* Find(const FString& Path)
	{
		return FindObject<UChooserTable>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
	}

	void UndoAll(const int32 Steps)
	{
		for (int32 Index = 0; Index < Steps && GEditor; ++Index) GEditor->UndoTransaction();
	}

	/** The plan's undo steps taken back, whatever the test found. */
	int32 StepsSince(const int32 Before)
	{
		return GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() - Before - GEditor->Trans->GetUndoCount() : 0;
	}

	/** The number after "= " in an output line, or after nothing in a read value. */
	double NumberIn(const FString& Text)
	{
		int32 At = INDEX_NONE;
		return FCString::Atod(Text.FindLastChar(TEXT('='), At) ? *Text.Mid(At + 1).TrimStartAndEnd() : *Text);
	}

	const FHyperAIChooserColumnFilter* FilterOf(const FHyperAIChooserEvaluateReport& Report, const int32 Column)
	{
		return Report.Filtered.FindByPredicate([Column](const FHyperAIChooserColumnFilter& Each) { return Each.Column == Column; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioChooserCellsTest, "HyperAIStudio.NativeTools.Chooser.Cells", HyperAIStudio::ChooserTests::Flags)

bool FHyperAIStudioChooserCellsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChooserTests;
	double Min = 0.0, Max = 0.0;
	bool bNoMin = false, bNoMax = false;
	FString Error;
	TestTrue(TEXT("0..300 is a range"), Gate::ParseRange(TEXT("0..300"), Min, Max, bNoMin, bNoMax, Error) && Min == 0.0 && Max == 300.0 && !bNoMin && !bNoMax);
	TestTrue(TEXT("..150 has no minimum"), Gate::ParseRange(TEXT(" ..150 "), Min, Max, bNoMin, bNoMax, Error) && bNoMin && !bNoMax && Max == 150.0);
	TestTrue(TEXT("150.. has no maximum"), Gate::ParseRange(TEXT("150.."), Min, Max, bNoMin, bNoMax, Error) && !bNoMin && bNoMax && Min == 150.0);
	TestTrue(TEXT("empty is any"), Gate::ParseRange(TEXT(""), Min, Max, bNoMin, bNoMax, Error) && bNoMin && bNoMax);
	TestTrue(TEXT("negatives and fractions"), Gate::ParseRange(TEXT("-2.5..0.5"), Min, Max, bNoMin, bNoMax, Error) && Min == -2.5 && Max == 0.5);
	TestFalse(TEXT("a word is not a range"), Gate::ParseRange(TEXT("fast"), Min, Max, bNoMin, bNoMax, Error));
	TestTrue(TEXT("and the refusal shows the form: ") + Error, Error.Contains(TEXT("min..max")));
	TestTrue(TEXT("a backwards range parses, for wrapped columns"), Gate::ParseRange(TEXT("135..-135"), Min, Max, bNoMin, bNoMax, Error) && Min == 135.0 && Max == -135.0);
	TestTrue(TEXT("an open end takes the closed end's number"), Gate::ParseRange(TEXT("150.."), Min, Max, bNoMin, bNoMax, Error) && Max == 150.0);
	TestFalse(TEXT("a range of words is refused"), Gate::ParseRange(TEXT("a..b"), Min, Max, bNoMin, bNoMax, Error));
	TestEqual(TEXT("ranges format back"), Gate::FormatRange(0.0, 300.0, false, false), FString(TEXT("0..300")));
	TestEqual(TEXT("open ranges too"), Gate::FormatRange(0.0, 150.0, true, false), FString(TEXT("..150")));
	TestEqual(TEXT("and any"), Gate::FormatRange(0.0, 0.0, true, true), FString(TEXT("any")));

	uint8 Bool = 9;
	TestTrue(TEXT("true"), Gate::ParseBoolCell(TEXT("True"), Bool, Error) && Bool == 1);
	TestTrue(TEXT("false"), Gate::ParseBoolCell(TEXT("no"), Bool, Error) && Bool == 0);
	TestTrue(TEXT("empty is any"), Gate::ParseBoolCell(TEXT(""), Bool, Error) && Bool == 2);
	TestFalse(TEXT("maybe is refused"), Gate::ParseBoolCell(TEXT("maybe"), Bool, Error));

	FString Value;
	TestEqual(TEXT("a value matches it"), Gate::ParseComparison(TEXT("Walking"), Value), 0);
	TestEqual(TEXT("!value matches the others"), Gate::ParseComparison(TEXT("!Walking"), Value), 1);
	TestEqual(TEXT("naming the value"), Value, FString(TEXT("Walking")));
	TestEqual(TEXT("* is any"), Gate::ParseComparison(TEXT("*"), Value), 2);

	TArray<TPair<FString, FString>> Settings;
	TestTrue(TEXT("settings parse"), Gate::ParseSettings(TEXT("bWrapInput=true; MinValue = -90;"), Settings, Error) && Settings.Num() == 2 && Settings[1].Key == TEXT("MinValue")
		&& Settings[1].Value == TEXT("-90"));
	TestFalse(TEXT("a setting without = is refused"), Gate::ParseSettings(TEXT("bWrapInput"), Settings, Error));

	const UEnum* Mode = StaticEnum<EMovementMode>();
	if (TestNotNull(TEXT("EMovementMode"), Mode))
	{
		const int32 Walking = Gate::FindEnumIndex(*Mode, TEXT("MOVE_Walking"));
		TestTrue(TEXT("MOVE_Walking is a value"), Walking != INDEX_NONE && Mode->GetValueByIndex(Walking) == MOVE_Walking);
		TestEqual(TEXT("and so is Walking"), Gate::FindEnumIndex(*Mode, TEXT("walking")), Walking);
		TestEqual(TEXT("Running is not"), Gate::FindEnumIndex(*Mode, TEXT("Running")), INDEX_NONE);
		TestFalse(TEXT("MAX is not listed"), Gate::EnumWords(*Mode).Contains(TEXT("MOVE_MAX")));
	}

	TestNotNull(TEXT("float_range is a column kind"), Gate::FindColumnKind(TEXT("Float_Range")));
	TestNull(TEXT("speed is not"), Gate::FindColumnKind(TEXT("speed")));
	for (const Gate::FColumnKindSpec& Spec : Gate::ColumnKinds())
	{
		if (FCString::Strcmp(Spec.Kind, TEXT("pose_match")) == 0) continue;
		TestNotNull(FString::Printf(TEXT("this 5.8 has the %s column (%s)"), Spec.Kind, Spec.Struct), Gate::ColumnStruct(Spec));
	}

	// The kinds as an agent discovers them.
	const FHyperAIChooserInspectReport Kinds = C::Inspect(FHyperAIChooserInspectRequest());
	TestTrue(TEXT("inspect without an asset lists the column types: ") + Kinds.Diagnostic, Kinds.bOk && Kinds.ColumnKinds.Num() >= Gate::ColumnKinds().Num() - 1);
	const FHyperAIChooserColumnKind* Range = Kinds.ColumnKinds.FindByPredicate([](const FHyperAIChooserColumnKind& Each) { return Each.Kind == TEXT("float_range"); });
	TestTrue(TEXT("float_range binds a double and lists its settings"), Range && Range->Input == TEXT("double") && Range->Role == TEXT("filter")
		&& Range->Settings.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("bWrapInput: bool")); }));
	const FHyperAIChooserColumnKind* Pose = Kinds.ColumnKinds.FindByPredicate([](const FHyperAIChooserColumnKind& Each) { return Each.Kind == TEXT("pose_match"); });
	TestTrue(TEXT("pose_match is listed, or says it needs the PoseSearch plugin"), Pose && (!Pose->Cells.StartsWith(TEXT("not in this editor")) || Pose->Cells.Contains(TEXT("PoseSearch"))));
	TestEqual(TEXT("and every op"), Kinds.Ops.Num(), Gate::OpKinds().Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioChooserDryRunTest, "HyperAIStudio.NativeTools.Chooser.DryRun", HyperAIStudio::ChooserTests::Flags)

bool FHyperAIStudioChooserDryRunTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChooserTests;
	const FString Asset = TestAsset(TEXT("CT_Dry"));
	const int32 Before = GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0;
	const FHyperAIChooserPlanReport Done = Run(Asset, Locomotion(), /*bDryRun=*/true);
	if (!TestTrue(TEXT("the dry run checks: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("checked"))) return false;
	TestEqual(TEXT("4 columns"), Done.Columns, 4);
	TestEqual(TEXT("3 rows"), Done.Rows, 3);
	TestTrue(TEXT("the notes say where the bool column went"), Done.Notes.ContainsByPredicate([](const FString& Note)
		{
			return Note.Contains(TEXT("bool column at index 2")) && Note.Contains(TEXT("Context0.bOrientRotationToMovement"));
		}));
	TestTrue(TEXT("and what each row gives"), Done.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("row 1 gives asset /Engine/BasicShapes/Sphere")); }));
	TestTrue(TEXT("a dry run makes nothing"), !Find(Asset) && !FPackageName::DoesPackageExist(Asset));
	TestEqual(TEXT("and adds no undo step"), GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0, Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioChooserApplyUndoTest, "HyperAIStudio.NativeTools.Chooser.ApplyAndUndo", HyperAIStudio::ChooserTests::Flags)

bool FHyperAIStudioChooserApplyUndoTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChooserTests;
	if (!GEditor || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor with its undo buffer, and no play session."));
		return false;
	}
	const FString Asset = TestAsset(TEXT("CT_Locomotion"));
	const int32 Before = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	const FHyperAIChooserPlanReport Done = Run(Asset, Locomotion(), false);
	const int32 Steps = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - Before;
	ON_SCOPE_EXIT
	{
		// Whatever failed below: the plans taken back.
		UndoAll(StepsSince(Before));
	};
	if (!TestTrue(TEXT("the plan runs: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("completed"))) return false;
	TestEqual(TEXT("in one undo step"), Steps, 1);
	TestTrue(TEXT("named for the plan"), GEditor->Trans->GetUndoContext().Title.ToString().Contains(TEXT("Chooser Plan")));
	UChooserTable* Made = Find(Asset);
	if (!TestNotNull(TEXT("the chooser table is made"), Made)) return false;
	TestTrue(TEXT("it gives static meshes"), Made->OutputObjectType.Get() == UStaticMesh::StaticClass());
	TestTrue(TEXT("reading a character movement component, both ways"), Made->ContextData.Num() == 1 && Made->ContextData[0].GetPtr<FContextObjectTypeClass>()
		&& Made->ContextData[0].Get<FContextObjectTypeClass>().Class == UCharacterMovementComponent::StaticClass()
		&& Made->ContextData[0].Get<FContextObjectTypeClass>().Direction == EContextObjectDirection::ReadWrite);
	TestEqual(TEXT("4 columns"), Made->ColumnsStructs.Num(), 4);
	TestEqual(TEXT("3 rows"), Made->ResultsStructs.Num(), 3);
	TestEqual(TEXT("each column holds a cell per row"), Made->DisabledRows.Num(), 3);
	TestEqual(TEXT("filters first, the output last"), Gate::KindOf(Made->ColumnsStructs[3]), FString(TEXT("output_float")));
	TestEqual(TEXT("row 1's speed"), Gate::CellText(Made->ColumnsStructs[0], 1), FString(TEXT("150..")));
	TestEqual(TEXT("its movement mode"), Gate::CellText(Made->ColumnsStructs[1], 1), FString(TEXT("MOVE_Walking")));
	TestEqual(TEXT("its rotation flag"), Gate::CellText(Made->ColumnsStructs[2], 1), FString(TEXT("true")));
	TestEqual(TEXT("row 0's empty cell is any"), Gate::CellText(Made->ColumnsStructs[2], 0), FString(TEXT("any")));
	TestEqual(TEXT("the fallback output"), Gate::CellText(Made->ColumnsStructs[3], ChooserColumn_SpecialIndex_Fallback), FString(TEXT("9")));
	TestEqual(TEXT("and the fallback"), Gate::ResultText(Made->FallbackResult), FString(TEXT("asset /Engine/BasicShapes/Cone.Cone")));

	// Read back through the tool.
	FHyperAIChooserInspectRequest Look;
	Look.Asset = Asset;
	const FHyperAIChooserInspectReport Read = C::Inspect(Look);
	if (!TestTrue(TEXT("inspect reads it: ") + Read.Diagnostic, Read.bOk && Read.Columns.Num() == 4 && Read.Rows.Num() == 3)) return false;
	TestEqual(TEXT("the first column's binding"), Read.Columns[0].Binding, FString(TEXT("Context0.MaxWalkSpeed")));
	TestTrue(TEXT("which resolves"), Read.Columns[0].Problem.IsEmpty());
	TestEqual(TEXT("the output's role"), Read.Columns[3].Role, FString(TEXT("output")));
	TestEqual(TEXT("row 2's result"), Read.Rows[2].Result, FString(TEXT("asset /Engine/BasicShapes/Cylinder.Cylinder")));
	TestTrue(TEXT("row 2's cells, in column order"), Read.Rows[2].Cells == TArray<FString>({TEXT("150.."), TEXT("MOVE_Falling"), TEXT("any"), TEXT("2")}));
	TestEqual(TEXT("one parameter, both ways"), Read.Parameters.Num() == 1 ? Read.Parameters[0].Direction : FString(), FString(TEXT("both")));

	// A second plan changes it: a setting, a row's cells, a row removed.
	FHyperAIChooserOp Wrap = Op(TEXT("column.set"));
	Wrap.Column = 0;
	Wrap.Params = TEXT("WrapInput=true; MinValue=0; MaxValue=1000");
	FHyperAIChooserOp Retune = Op(TEXT("row.set"));
	Retune.Row = 0;
	Retune.Cells = {TEXT("..200"), TEXT("!MOVE_Falling"), TEXT(""), TEXT("5")};
	FHyperAIChooserOp Drop = Op(TEXT("row.remove"));
	Drop.Row = 2;
	const FHyperAIChooserPlanReport Edited = Run(Asset, {Wrap, Retune, Drop}, false);
	if (!TestTrue(TEXT("the edit runs: ") + Edited.Diagnostic, Edited.bOk && Edited.Rows == 2)) return false;
	TestEqual(TEXT("in one more undo step"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - Before, 2);
	TestEqual(TEXT("row 0 is retuned"), Gate::CellText(Made->ColumnsStructs[0], 0), FString(TEXT("..200")));
	TestEqual(TEXT("to any mode but falling"), Gate::CellText(Made->ColumnsStructs[1], 0), FString(TEXT("!MOVE_Falling")));
	TestTrue(TEXT("the setting is set"), Gate::SettingsOf(Made->ColumnsStructs[0], Made).Contains(TEXT("bWrapInput = True")));
	TestEqual(TEXT("and the cylinder row is gone"), Made->ResultsStructs.Num(), 2);
	TestTrue(TEXT("with its cells"), Gate::CellText(Made->ColumnsStructs[3], 2).IsEmpty() && Gate::CellText(Made->ColumnsStructs[3], 1) == TEXT("1"));
	TestTrue(TEXT("undo the edit"), GEditor->UndoTransaction());
	TestEqual(TEXT("the row is back"), Made->ResultsStructs.Num(), 3);
	TestEqual(TEXT("and row 0's speed"), Gate::CellText(Made->ColumnsStructs[0], 0), FString(TEXT("..150")));

	// One undo takes the table away; redo brings it back whole.
	if (!TestTrue(TEXT("undo the make"), GEditor->UndoTransaction())) return false;
	TestFalse(TEXT("undo removes the chooser table"), IsValid(Made));
	TestFalse(TEXT("leaving nothing to save"), Made->GetPackage()->IsDirty());
	if (!TestTrue(TEXT("redo"), GEditor->RedoTransaction())) return false;
	TestTrue(TEXT("redo brings it back whole"), IsValid(Made) && Made->ColumnsStructs.Num() == 4 && Made->ResultsStructs.Num() == 3
		&& Gate::CellText(Made->ColumnsStructs[1], 2) == TEXT("MOVE_Falling"));
	GEditor->UndoTransaction();
	// Undone, the path is free to make again.
	const FHyperAIChooserPlanReport Again = Run(Asset, {Create(), Row(Cube, {})}, false);
	TestTrue(TEXT("the same Asset can be made again after undo: ") + Again.Diagnostic, Again.bOk && Again.Rows == 1);
	UChooserTable* Remade = Find(Asset);
	TestTrue(TEXT("as a new table"), Remade && IsValid(Remade) && Remade != Made);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioChooserEvaluateTest, "HyperAIStudio.NativeTools.Chooser.Evaluate", HyperAIStudio::ChooserTests::Flags)

bool FHyperAIStudioChooserEvaluateTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChooserTests;
	if (!GEditor || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor with its undo buffer, and no play session."));
		return false;
	}
	const FString Asset = TestAsset(TEXT("CT_Evaluate"));
	const int32 Before = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	const FHyperAIChooserPlanReport Done = Run(Asset, Locomotion(), false);
	ON_SCOPE_EXIT
	{
		UndoAll(StepsSince(Before));
	};
	if (!TestTrue(TEXT("the table is made: ") + Done.Diagnostic, Done.bOk)) return false;

	const FHyperAIChooserEvaluateReport Slow = Evaluate(Asset, TEXT("MaxWalkSpeed=100"));
	TestTrue(TEXT("slow picks row 0, the cube: ") + Slow.Diagnostic, Slow.bOk && Slow.Row == 0 && Slow.Result == TEXT("/Engine/BasicShapes/Cube.Cube"));
	TestTrue(TEXT("its output column wrote 0"), Slow.Outputs.Num() == 1 && Slow.Outputs[0].Contains(TEXT("JumpZVelocity")) && NumberIn(Slow.Outputs[0]) == 0.0);
	TestTrue(TEXT("and the speed column turned away the fast rows"), FilterOf(Slow, 0) && FilterOf(Slow, 0)->RowsOut == TArray<int32>({1, 2}) && NumberIn(FilterOf(Slow, 0)->Input) == 100.0);

	const FHyperAIChooserEvaluateReport Walk = Evaluate(Asset, TEXT("MaxWalkSpeed=450; MovementMode=MOVE_Walking; bOrientRotationToMovement=true"));
	TestTrue(TEXT("walking fast picks row 1, the sphere: ") + Walk.Diagnostic, Walk.bOk && Walk.Row == 1 && Walk.Result == TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	TestTrue(TEXT("it alone passed"), Walk.Passed == TArray<int32>({1}));
	TestTrue(TEXT("writing 1"), Walk.Outputs.Num() == 1 && NumberIn(Walk.Outputs[0]) == 1.0);
	TestTrue(TEXT("the values given are listed"), Walk.Inputs.Contains(TEXT("Context0.MovementMode = MOVE_Walking")));

	const FHyperAIChooserEvaluateReport Fall = Evaluate(Asset, TEXT("MaxWalkSpeed=450; MovementMode=Falling"));
	TestTrue(TEXT("falling fast picks row 2, the cylinder: ") + Fall.Diagnostic, Fall.bOk && Fall.Row == 2 && Fall.Result == TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

	const FHyperAIChooserEvaluateReport None = Evaluate(Asset, TEXT("MaxWalkSpeed=450; MovementMode=Walking; bOrientRotationToMovement=false"));
	TestTrue(TEXT("walking fast without the flag passes no row; the fallback gives the cone: ") + None.Diagnostic, None.bOk && None.Row == -1 && None.bFallback
		&& None.Result == TEXT("/Engine/BasicShapes/Cone.Cone") && None.Passed.IsEmpty());
	TestTrue(TEXT("the bool column turned away row 1"), FilterOf(None, 2) && FilterOf(None, 2)->RowsOut == TArray<int32>({1}));
	TestTrue(TEXT("and the fallback's output was written"), None.Outputs.Num() == 1 && NumberIn(None.Outputs[0]) == 9.0);

	FHyperAIChooserEvaluateRequest Wrong;
	Wrong.Asset = Asset;
	Wrong.Values = TEXT("MovementMode=Running");
	const FHyperAIChooserEvaluateReport Refused = C::Evaluate(Wrong);
	TestTrue(TEXT("a value the enum lacks is refused, with its values: ") + Refused.Diagnostic, !Refused.bOk && Refused.Diagnostic.Contains(TEXT("MOVE_Walking")));
	Wrong.Values = TEXT("Speedy=3");
	TestTrue(TEXT("so is a property no parameter has"), !C::Evaluate(Wrong).bOk);
	TestEqual(TEXT("evaluating adds no undo step"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - Before, 1);
	TestFalse(TEXT("nor makes anything to save beyond the plan"), Done.Saved.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioChooserRefusalsTest, "HyperAIStudio.NativeTools.Chooser.Refusals", HyperAIStudio::ChooserTests::Flags)

bool FHyperAIStudioChooserRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChooserTests;
	const FString Asset = TestAsset(TEXT("CT_Refused"));
	const int32 Before = GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0;
	TestTrue(TEXT("Engine content is refused"), Refused(Run(TEXT("/Engine/BasicShapes/CT_Shapes"), Locomotion(), false), TEXT("not project content")));
	TestTrue(TEXT("so is plugin content"), Refused(Run(TEXT("/HyperAISb3f9f07c5a33V1/CT_Shapes"), Locomotion(), false), TEXT("not project content")));
	TestTrue(TEXT("an unknown column kind is refused, with the kinds"), Refused(Run(Asset, {Create(), Column(TEXT("speed"), TEXT("MaxWalkSpeed"))}, false), TEXT("float_range")));
	TestTrue(TEXT("an unknown op is refused, with the ops"), Refused(Run(Asset, {Create(), Op(TEXT("row.explode"))}, false), TEXT("fallback.set")));
	TestTrue(TEXT("a property no parameter has is refused"), Refused(Run(Asset, {Create(), Column(TEXT("float_range"), TEXT("NoSuchSpeed"))}, false),
		TEXT("no parameter has a property 'NoSuchSpeed'")));
	TestTrue(TEXT("so is one of the wrong type, naming the ones that fit"), Refused(Run(Asset, {Create(), Column(TEXT("float_range"), TEXT("bOrientRotationToMovement"))}, false),
		TEXT("MaxWalkSpeed")));
	TestTrue(TEXT("a filter needs a binding"), Refused(Run(Asset, {Create(), Column(TEXT("bool"), TEXT(""))}, false), TEXT("needs Binding")));
	TestTrue(TEXT("an unknown setting is refused, with the settings"), Refused(Run(Asset, {Create(), Column(TEXT("float_range"), TEXT("MaxWalkSpeed"), TEXT("Wobble=3"))}, false),
		TEXT("bWrapInput")));
	TArray<FHyperAIChooserOp> Short = Locomotion();
	Short[5].Cells = {TEXT("..150")};
	TestTrue(TEXT("a row with too few cells is refused"), Refused(Run(Asset, Short, false), TEXT("Cells has 1 value(s) for 4 column(s)")));
	TArray<FHyperAIChooserOp> BadCell = Locomotion();
	BadCell[5].Cells[0] = TEXT("fast");
	TestTrue(TEXT("a cell its column cannot read is refused"), Refused(Run(Asset, BadCell, false), TEXT("min..max")));
	TArray<FHyperAIChooserOp> BadMode = Locomotion();
	BadMode[6].Cells[1] = TEXT("Running");
	TestTrue(TEXT("an enum cell outside its enum is refused, with the values"), Refused(Run(Asset, BadMode, false), TEXT("MOVE_Falling")));
	TestTrue(TEXT("a missing result asset is refused"), Refused(Run(Asset, {Create(), Row(TEXT("/Game/Nowhere/SM_Missing"), {})}, false), TEXT("Nothing is at")));
	TestTrue(TEXT("a result of the wrong class is refused"), Refused(Run(Asset, {Create(), Row(TEXT("/Engine/BasicShapes/BasicShapeMaterial"), {})}, false), TEXT("not a StaticMesh")));
	TestTrue(TEXT("changing a table that is not there is refused"), Refused(Run(Asset, {Row(Cube, {})}, false), TEXT("start the plan with create")));
	TestTrue(TEXT("create comes first"), Refused(Run(Asset, {Create(), Create()}, false), TEXT("create comes first")));
	TestTrue(TEXT("a row past the end is refused"), Refused(Run(Asset, {Create(), Op(TEXT("row.remove"))}, false), TEXT("is not a row")));
	TestTrue(TEXT("refused plans make nothing"), !Find(Asset) && !FPackageName::DoesPackageExist(Asset));
	TestEqual(TEXT("and add no undo step"), GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0, Before);

	// A refused plan on an existing table leaves it as it was: the ops ran on a copy.
	const FHyperAIChooserPlanReport Made = Run(Asset, Locomotion(), false);
	ON_SCOPE_EXIT
	{
		UndoAll(StepsSince(Before));
	};
	if (!TestTrue(TEXT("a table to refuse edits on: ") + Made.Diagnostic, Made.bOk)) return false;
	FHyperAIChooserOp Drop = Op(TEXT("row.remove"));
	Drop.Row = 0;
	FHyperAIChooserOp Bad = Op(TEXT("column.set"));
	Bad.Column = 7;
	Bad.Params = TEXT("Disabled=true");
	TestTrue(TEXT("a later op's refusal names the op"), Refused(Run(Asset, {Drop, Bad}, false), TEXT("Operation 1 (column.set): Column 7 is not a column")));
	TestEqual(TEXT("and the earlier op did not run"), Find(Asset) ? Find(Asset)->ResultsStructs.Num() : -1, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioChooserSkillTest, "HyperAIStudio.NativeTools.Chooser.Skill", HyperAIStudio::ChooserTests::Flags)

bool FHyperAIStudioChooserSkillTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ChooserTests;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	FString Skill;
	if (!TestTrue(TEXT("the chooser skill ships with the plugin"), Plugin.IsValid()
		&& FFileHelper::LoadFileToString(Skill, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Skills/unreal-chooser/SKILL.md")))))
	{
		return false;
	}
	TestTrue(TEXT("it is a skill"), Skill.StartsWith(TEXT("---")) && Skill.Contains(TEXT("name: unreal-chooser")));
	for (const FString& Name : C::GetToolNames())
	{
		TestTrue(FString::Printf(TEXT("it names %s"), *Name), Skill.Contains(Name));
	}
	for (const FString& Kind : Gate::OpKinds())
	{
		TestTrue(FString::Printf(TEXT("it explains %s"), *Kind), Skill.Contains(FString::Printf(TEXT("`%s`"), *Kind)));
	}
	for (const Gate::FColumnKindSpec& Spec : Gate::ColumnKinds())
	{
		TestTrue(FString::Printf(TEXT("and the %s column"), Spec.Kind), Skill.Contains(FString::Printf(TEXT("`%s`"), Spec.Kind)));
	}
	for (const TCHAR* Word : {TEXT("hyper_pose_search_apply_plan"), TEXT("Evaluate Chooser"), TEXT("HyperAIStudioChooser.HyperAIStudioChooserToolset")})
	{
		TestTrue(FString::Printf(TEXT("and %s"), Word), Skill.Contains(Word));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
