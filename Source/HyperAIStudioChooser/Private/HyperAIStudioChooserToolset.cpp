// Games by Hyper 2026.

#include "HyperAIStudioChooserToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Chooser.h"
#include "ChooserIndexArray.h"
#include "ChooserPropertyAccess.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorUndoClient.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioChooserGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "Math/RandomStream.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioChooser, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioChooser"

FString FHyperAIStudioChooserContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioChooser.HyperAIStudioChooserToolset");
}

TArray<FString> FHyperAIStudioChooserContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName, EvaluateToolName};
}

bool FHyperAIStudioChooserContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::ChooserTools::Private
{
	using C = FHyperAIStudioChooserContracts;
	namespace Gate = HyperAIStudio::Chooser::Gate;

	/**
	 * Chooser tables a plan made or changed. Undo and redo restore a table's columns by serialization, which leaves their
	 * bindings uncompiled (the compiled form is not saved), so each is compiled again; and a made table is marked garbage
	 * on undo and alive on redo, which this tells the asset registry so the Content Browser keeps in step.
	 */
	class FTouchedTables final : public FSelfRegisteringEditorUndoClient
	{
	public:
		void Made(UChooserTable* Table)
		{
			Edited(Table);
			Listed.Add(Table, true);
		}

		void Edited(UChooserTable* Table)
		{
			Tables.RemoveAll([](const TWeakObjectPtr<UChooserTable>& Each) { return Each.IsStale(/*bIncludingGarbage=*/false); });
			Tables.AddUnique(Table);
		}

		virtual void PostUndo(bool bSuccess) override { Sync(); }
		virtual void PostRedo(bool bSuccess) override { Sync(); }

	private:
		void Sync()
		{
			for (int32 Index = Tables.Num() - 1; Index >= 0; --Index)
			{
				UChooserTable* Table = Tables[Index].Get(/*bEvenIfGarbage=*/true);
				if (!Table)
				{
					Tables.RemoveAt(Index);
					continue;
				}
				const bool bAlive = IsValid(Table);
				if (bAlive) Gate::CompileTable(*Table);
				bool* Known = Listed.Find(Table);
				if (!Known || *Known == bAlive) continue;
				// Undone, nothing of it is left to save; redone, it is unsaved work again.
				if (bAlive)
				{
					FAssetRegistryModule::AssetCreated(Table);
					Table->MarkPackageDirty();
				}
				else
				{
					FAssetRegistryModule::AssetDeleted(Table);
					Table->GetPackage()->SetDirtyFlag(false);
				}
				Listed.Add(Table, bAlive);
			}
		}

		TArray<TWeakObjectPtr<UChooserTable>> Tables;
		TMap<TWeakObjectPtr<UChooserTable>, bool> Listed;
	};

	/** Made on first use and let go at module shutdown, while the editor it registered with is still there. */
	TUniquePtr<FTouchedTables> GTouchedTables;

	FTouchedTables& TouchedTables()
	{
		if (!GTouchedTables) GTouchedTables = MakeUnique<FTouchedTables>();
		return *GTouchedTables;
	}

	FString ResultTypeText(const EObjectChooserResultType Type)
	{
		switch (Type)
		{
		case EObjectChooserResultType::ClassResult: return TEXT("class");
		case EObjectChooserResultType::NoPrimaryResult: return TEXT("none");
		default: return TEXT("object");
		}
	}

	/** The plan for real: the table in one undo step, then saved if asked. */
	FHyperAIChooserPlanReport Execute(const FHyperAIChooserPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIChooserPlanReport Report;
		Report.OperationId = OperationId;
		Gate::FPlan Plan;
		FString Error;
		if (!Gate::Check(Request, Plan, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		UChooserTable* Table = nullptr;
		{
			const FScopedTransaction Transaction(LOCTEXT("ChooserPlan", "HyperAI: Chooser Plan"));
			if (Plan.bCreate)
			{
				// What an undone plan made here is out of the way first: this plan's step ends the redo that kept it.
				if (UChooserTable* Undone = Gate::UndoneAt(Plan.AssetPath))
				{
					Undone->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
				}
				UPackage* Package = CreatePackage(*Plan.AssetPath);
				Table = NewObject<UChooserTable>(Package, *FPackageName::GetShortName(Plan.AssetPath), RF_Public | RF_Standalone | RF_Transactional);
				// Recorded as made in this undo step: garbage when the step began, alive when it ended. Undo marks it garbage
				// again (the asset goes, as an undone actor does) and redo brings it back.
				Table->MarkAsGarbage();
				Table->Modify();
				Table->ClearGarbage();
				FAssetRegistryModule::AssetCreated(Table);
				TouchedTables().Made(Table);
			}
			else
			{
				Table = Plan.Existing;
				TouchedTables().Edited(Table);
			}
			TArray<FString> Notes;
			if (!Gate::Apply(*Table, Request.Operations, Plan.bCreate, /*bReal=*/true, Notes, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Asset = Table->GetPathName();
				Report.Diagnostic = Error + TEXT(" The ops before it ran, unsaved; Edit > Undo takes them back.");
				return Report;
			}
			Report.Notes = MoveTemp(Notes);
			// The bindings compiled as the Chooser editor compiles them, and its open editors told the table changed.
			Gate::CompileTable(*Table);
			Table->PostEditChange();
			Table->MarkPackageDirty();
		}
		Report.Asset = Table->GetPathName();
		Report.Columns = Table->ColumnsStructs.Num();
		Report.Rows = Table->ResultsStructs.Num();
		Report.Warnings = Gate::Warn(*Table);
		if (Request.bSave)
		{
			if (!FHyperAIStudioSourceControl::SaveAsset(*Table, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Table->GetPathName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%s: %d column(s), %d row(s)%s; %s. Check it with hyper_chooser_evaluate."), *Report.Asset, Report.Columns, Report.Rows,
			Table->FallbackResult.IsValid() ? TEXT(", with a fallback") : TEXT(""),
			Request.bSave ? TEXT("saved") : TEXT("unsaved, and one Edit > Undo takes the whole plan back"));
		return Report;
	}

	FHyperAIChooserPlanReport DryRun(const FHyperAIChooserPlanRequest& Request, const FString& OperationId, Gate::FPlan& Plan)
	{
		FHyperAIChooserPlanReport Report;
		Report.OperationId = OperationId;
		Report.Asset = Plan.AssetPath;
		Report.Columns = Plan.Columns;
		Report.Rows = Plan.Rows;
		Report.Notes = Plan.Notes;
		Report.Warnings = Plan.Warnings;
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked on a copy: %s would have %d column(s) and %d row(s); nothing was changed."), Request.Operations.Num(),
			*Plan.AssetPath, Plan.Columns, Plan.Rows);
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIChooserPlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Asset;
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	/** One parameter of an evaluation: its type and the memory the table reads and writes. */
	struct FParameter
	{
		const UStruct* Type = nullptr;
		uint8* Container = nullptr;
	};

	/** A bound property's value in the parameters, in Unreal's text form. */
	FString ReadBinding(const FChooserPropertyBinding& Binding, const TArray<FParameter>& Parameters)
	{
		if (!Parameters.IsValidIndex(Binding.ContextIndex)) return TEXT("?");
		const UStruct* Type = Parameters[Binding.ContextIndex].Type;
		uint8* Container = Parameters[Binding.ContextIndex].Container;
		if (Binding.PropertyBindingChain.IsEmpty()) return Binding.IsBoundToRoot && Type ? FString::Printf(TEXT("(the whole %s)"), *Type->GetName()) : FString(TEXT("?"));
		for (int32 Index = 0; Index < Binding.PropertyBindingChain.Num(); ++Index)
		{
			const FProperty* Property = Type ? FindFProperty<FProperty>(Type, Binding.PropertyBindingChain[Index]) : nullptr;
			if (!Property || !Container) return TEXT("(a function)");
			if (Index == Binding.PropertyBindingChain.Num() - 1)
			{
				FString Out;
				Property->ExportText_InContainer(0, Out, Container, Container, nullptr, PPF_None);
				return Out.Left(300);
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			{
				Container = Struct->ContainerPtrToValuePtr<uint8>(Container);
				Type = Struct->Struct;
			}
			else if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property))
			{
				UObject* Value = Object->GetObjectPropertyValue_InContainer(Container);
				if (!Value) return TEXT("None");
				Container = reinterpret_cast<uint8*>(Value);
				Type = Value->GetClass();
			}
			else
			{
				return TEXT("?");
			}
		}
		return TEXT("?");
	}

	/** Sets "[ContextN.]Name[.Member]=Value" on the parameters; false with why. */
	bool SetValue(const UChooserTable& Table, const TArray<FParameter>& Parameters, const FString& Name, const FString& InValue, FString& OutDone, FString& OutError)
	{
		TArray<FString> Parts;
		Name.ParseIntoArray(Parts, TEXT("."), true);
		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (!FHyperAIStudioNames::IsName(Part, TEXT("Values"), &OutError)) return false;
		}
		if (Parts.IsEmpty())
		{
			OutError = TEXT("a value has no name; write Name=Value.");
			return false;
		}
		int32 ContextIndex = INDEX_NONE;
		int32 First = 0;
		if (Parts[0].StartsWith(TEXT("Context"), ESearchCase::IgnoreCase) && Parts[0].Len() > 7 && Parts[0].Mid(7).IsNumeric())
		{
			ContextIndex = FCString::Atoi(*Parts[0].Mid(7));
			First = 1;
			if (!Parameters.IsValidIndex(ContextIndex) || Parts.Num() < 2)
			{
				OutError = FString::Printf(TEXT("%s is not a parameter with a property named after it; the table has %d parameter(s)."), *Name, Parameters.Num());
				return false;
			}
		}
		else
		{
			for (int32 Index = 0; Index < Parameters.Num() && ContextIndex == INDEX_NONE; ++Index)
			{
				if (FindFProperty<FProperty>(Parameters[Index].Type, *Parts[0])) ContextIndex = Index;
			}
			if (ContextIndex == INDEX_NONE)
			{
				TArray<FString> Types;
				for (int32 Index = 0; Index < Parameters.Num(); ++Index) Types.Add(FString::Printf(TEXT("Context%d %s"), Index, *Parameters[Index].Type->GetName()));
				OutError = FString::Printf(TEXT("no parameter has a property '%s'; the parameters are %s."), *Parts[0], *FString::Join(Types, TEXT(", ")));
				return false;
			}
		}
		const UStruct* Type = Parameters[ContextIndex].Type;
		uint8* Container = Parameters[ContextIndex].Container;
		FString Path = FString::Printf(TEXT("Context%d"), ContextIndex);
		for (int32 Index = First; Index < Parts.Num(); ++Index)
		{
			FProperty* Property = FindFProperty<FProperty>(Type, *Parts[Index]);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no property '%s'."), *Type->GetName(), *Parts[Index]);
				return false;
			}
			Path += TEXT(".") + Property->GetName();
			if (Index < Parts.Num() - 1)
			{
				const FStructProperty* Struct = CastField<FStructProperty>(Property);
				if (!Struct)
				{
					OutError = FString::Printf(TEXT("%s is not a struct; only struct members are reached with a dot."), *Path);
					return false;
				}
				Container = Struct->ContainerPtrToValuePtr<uint8>(Container);
				Type = Struct->Struct;
				continue;
			}
			FString Value = InValue.TrimStartAndEnd();
			if (CastField<FBoolProperty>(Property))
			{
				uint8 Bool = 2;
				FString Ignored;
				if (!Gate::ParseBoolCell(Value, Bool, Ignored) || Bool == 2)
				{
					OutError = FString::Printf(TEXT("%s is true or false, not '%s'."), *Path, *Value);
					return false;
				}
				Value = Bool == 1 ? TEXT("True") : TEXT("False");
			}
			const UEnum* Enum = CastField<FEnumProperty>(Property) ? CastField<FEnumProperty>(Property)->GetEnum()
				: CastField<FByteProperty>(Property) ? CastField<FByteProperty>(Property)->Enum.Get() : nullptr;
			if (Enum)
			{
				const int32 Found = Gate::FindEnumIndex(*Enum, Value);
				if (Found == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("'%s' is not a value of %s; its values are %s."), *Value, *Path, *Gate::EnumWords(*Enum));
					return false;
				}
				Value = Enum->GetNameStringByIndex(Found);
			}
			// The import makes names of the text, and loads an asset it names (making the package when nothing is there).
			if (!FHyperAIStudioNames::CanImportText(*Property, Value, *Path, &OutError)) return false;
			const TCHAR* Rest = Property->ImportText_InContainer(*Value, Container, nullptr, PPF_None);
			if (!Rest || !FString(Rest).TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not a value for %s (%s)."), *InValue, *Path, *Property->GetCPPType());
				return false;
			}
			FString Now;
			Property->ExportText_InContainer(0, Now, Container, Container, nullptr, PPF_None);
			OutDone = FString::Printf(TEXT("%s = %s"), *Path, *Now);
		}
		return true;
	}
}

FHyperAIChooserInspectReport FHyperAIStudioChooserContracts::Inspect(const FHyperAIChooserInspectRequest& Request)
{
	using namespace HyperAIStudio::ChooserTools::Private;
	FHyperAIChooserInspectReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (Request.Asset.TrimStartAndEnd().IsEmpty())
	{
		for (UScriptStruct* Struct : Gate::AllColumnStructs())
		{
			FInstancedStruct Column;
			Column.InitializeAs(Struct);
			const Gate::FColumnKindSpec* Spec = Gate::KindOfStruct(Struct);
			FHyperAIChooserColumnKind& Kind = Report.ColumnKinds.AddDefaulted_GetRef();
			Kind.Kind = Spec ? Spec->Kind : TEXT("");
			Kind.Struct = Struct->GetStructPathName().ToString();
			Kind.DisplayName = Struct->GetDisplayNameText().ToString();
			Kind.Role = Gate::RoleOf(Column);
			Kind.Input = Gate::BindingTypeOf(Column);
			Kind.Cells = Spec ? Spec->Cells : TEXT("not written by the plan; edit them in the Chooser editor");
			Kind.Settings = Gate::DescribeSettings(*Struct);
		}
		for (const Gate::FColumnKindSpec& Spec : Gate::ColumnKinds())
		{
			if (Gate::ColumnStruct(Spec)) continue;
			FHyperAIChooserColumnKind& Kind = Report.ColumnKinds.AddDefaulted_GetRef();
			Kind.Kind = Spec.Kind;
			Kind.Struct = Spec.Struct;
			Kind.Role = Spec.Role;
			Kind.Cells = FString::Printf(TEXT("not in this editor: %s"), FCString::Strcmp(Spec.Kind, TEXT("pose_match")) == 0 ? TEXT("needs the PoseSearch plugin") : TEXT("its plugin is not loaded"));
		}
		Report.Ops = Gate::DescribeOps();
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UChooserTable::StaticClass()->GetClassPathName(), Assets);
		for (const FAssetData& Each : Assets)
		{
			const FString Path = Each.GetObjectPathString();
			if (Path.StartsWith(TEXT("/Game/")) && Report.Assets.Num() < 200) Report.Assets.Add(Path);
		}
		Report.Assets.Sort();
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%d column type(s) in this editor, %d op kind(s) hyper_chooser_apply_plan takes, and %d chooser table(s) under /Game; give Asset to read one."),
			Report.ColumnKinds.Num(), Report.Ops.Num(), Report.Assets.Num());
		return Report;
	}
	FString Error;
	UChooserTable* Table = Gate::LoadTable(Request.Asset, Error);
	if (!Table)
	{
		Report.Diagnostic = Error;
		return Report;
	}
	const UChooserTable* Owner = Table->GetContextOwner();
	Report.Asset = Table->GetPathName();
	Report.ResultType = ResultTypeText(Owner->ResultType);
	Report.OutputObjectType = Owner->OutputObjectType ? Owner->OutputObjectType->GetPathName() : FString();
	TConstArrayView<FInstancedStruct> Context = Table->GetContextData();
	for (int32 Index = 0; Index < Context.Num(); ++Index)
	{
		FHyperAIChooserParameter& Parameter = Report.Parameters.AddDefaulted_GetRef();
		Parameter.Name = FString::Printf(TEXT("Context%d"), Index);
		if (const FContextObjectTypeClass* Class = Context[Index].GetPtr<FContextObjectTypeClass>())
		{
			Parameter.Kind = TEXT("class");
			Parameter.Type = Class->Class ? Class->Class->GetPathName() : FString(TEXT("None"));
		}
		else if (const FContextObjectTypeStruct* Struct = Context[Index].GetPtr<FContextObjectTypeStruct>())
		{
			Parameter.Kind = TEXT("struct");
			Parameter.Type = Struct->Struct ? Struct->Struct->GetPathName() : FString(TEXT("None"));
		}
		else
		{
			Parameter.Kind = Context[Index].IsValid() ? Context[Index].GetScriptStruct()->GetName() : FString(TEXT("none"));
		}
		if (const FContextObjectTypeBase* Base = Context[Index].GetPtr<FContextObjectTypeBase>()) Parameter.Direction = Gate::DirectionText(static_cast<uint8>(Base->Direction));
	}
	for (int32 Index = 0; Index < Table->ColumnsStructs.Num(); ++Index)
	{
		const FInstancedStruct& Column = Table->ColumnsStructs[Index];
		FHyperAIChooserColumnInfo& Info = Report.Columns.AddDefaulted_GetRef();
		Info.Index = Index;
		Info.Kind = Gate::KindOf(Column);
		if (!Column.IsValid()) continue;
		Info.Struct = Column.GetScriptStruct()->GetStructPathName().ToString();
		Info.Role = Gate::RoleOf(Column);
		if (const FChooserPropertyBinding* Binding = Gate::BindingOf(Column)) Info.Binding = Gate::DescribeBinding(*Binding);
		Info.Problem = Gate::BindingProblem(*Table, Column);
		Info.Settings = Gate::SettingsOf(Column, Table);
		Info.bDisabled = Column.Get<FChooserColumnBase>().bDisabled;
		if (Info.Role == TEXT("output")) Info.Fallback = Gate::CellText(Column, ChooserColumn_SpecialIndex_Fallback);
	}
	Report.RowCount = Table->ResultsStructs.Num();
	const int32 Listed = FMath::Clamp(Request.MaxRows, 0, MaxListedRows);
	for (int32 Row = 0; Row < Table->ResultsStructs.Num() && Row < Listed; ++Row)
	{
		FHyperAIChooserRowInfo& Info = Report.Rows.AddDefaulted_GetRef();
		Info.Index = Row;
		Info.Result = Gate::ResultText(Table->ResultsStructs[Row]);
		Info.bDisabled = Table->DisabledRows.IsValidIndex(Row) && Table->DisabledRows[Row];
		for (const FInstancedStruct& Column : Table->ColumnsStructs) Info.Cells.Add(Gate::CellText(Column, Row));
	}
	Report.Fallback = Gate::ResultText(Table->FallbackResult);
	for (const TObjectPtr<UObject>& Nested : Table->GetRootChooser()->NestedObjects)
	{
		if (const UChooserTable* NestedTable = Cast<UChooserTable>(Nested.Get()))
		{
			Report.NestedChoosers.Add(FString::Printf(TEXT("nested:%s (%d column(s), %d row(s))"), *NestedTable->GetName(), NestedTable->ColumnsStructs.Num(), NestedTable->ResultsStructs.Num()));
		}
	}
	for (const FInstancedStruct& Result : Table->ResultsStructs)
	{
		if (const FEvaluateChooser* Evaluate = Result.GetPtr<FEvaluateChooser>())
		{
			if (Evaluate->Chooser) Report.NestedChoosers.AddUnique(TEXT("chooser:") + Evaluate->Chooser->GetPathName());
		}
	}
	Report.Warnings = Gate::Warn(*Table);
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%s: gives %s%s, %d parameter(s), %d column(s), %d row(s), fallback %s."), *Report.Asset, *Report.ResultType,
		Report.OutputObjectType.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Report.OutputObjectType), Report.Parameters.Num(), Report.Columns.Num(),
		Report.RowCount, *Report.Fallback);
	return Report;
}

FHyperAIChooserPlanReport FHyperAIStudioChooserContracts::ApplyPlan(const FHyperAIChooserPlanRequest& Request)
{
	using namespace HyperAIStudio::ChooserTools::Private;
	FHyperAIChooserPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("chooser-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Params"), TEXT("Cells"), TEXT("Result"), TEXT("Binding")})) return Report;
	if (GEditor && GEditor->PlayWorld)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Play In Editor is running; stop it first, then make or change the chooser table.");
		return Report;
	}
	Gate::FPlan Plan;
	FString Error;
	if (!Gate::Check(Request, Plan, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Request.bDryRun) return DryRun(Request, Report.OperationId, Plan);
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("chooser.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Plan.AssetPath;
		Summary.Effects.Add(FString::Printf(TEXT("%s the chooser table %s: %d operation(s), one undo step%s"), Plan.bCreate ? TEXT("Make") : TEXT("Change"), *Plan.AssetPath,
			Request.Operations.Num(), Request.bSave ? TEXT(", then saved") : TEXT("")));
		for (int32 Index = 0; Index < Plan.Notes.Num() && Index < 24; ++Index)
		{
			Summary.Effects.Add(TEXT("| ") + Plan.Notes[Index].Left(160));
		}
		Summary.Touches.Add(Plan.AssetPath);
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				// The editor may have moved on while the plan waited (a table made at the path, a column removed by hand),
				// so Execute checks the plan again before anything changes.
				Record(OperationId, Execute(Request, OperationId));
			}, Error))
		{
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Asset = Plan.AssetPath;
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		FHyperAIStudioApprovalGate::DescribeHeld(OperationId, Report.Status, Report.Diagnostic);
		return Report;
	}
	Report = Execute(Request, Report.OperationId);
	Record(Report.OperationId, Report);
	return Report;
}

FHyperAIChooserEvaluateReport FHyperAIStudioChooserContracts::Evaluate(const FHyperAIChooserEvaluateRequest& Request)
{
	using namespace HyperAIStudio::ChooserTools::Private;
	FHyperAIChooserEvaluateReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Values")})) return Report;
	FString Error;
	UChooserTable* Table = Gate::LoadTable(Request.Asset, Error);
	if (!Table)
	{
		Report.Diagnostic = Error;
		return Report;
	}
	Report.Asset = Table->GetPathName();
	TConstArrayView<FInstancedStruct> Context = Table->GetContextData();
	if (Context.Num() > MaxParameters)
	{
		Report.Diagnostic = FString::Printf(TEXT("The table has %d parameters; evaluation here takes at most %d."), Context.Num(), MaxParameters);
		return Report;
	}

	// The parameters as the game hands them over: a fresh object of each class parameter (its class defaults), and an
	// instance of each struct parameter, with the values given set on them.
	TArray<TStrongObjectPtr<UObject>> Objects;
	TArray<FInstancedStruct> Structs;
	Structs.SetNum(Context.Num());
	TArray<FParameter> Parameters;
	int32 ObjectCount = 0;
	for (int32 Index = 0; Index < Context.Num(); ++Index)
	{
		FParameter& Parameter = Parameters.AddDefaulted_GetRef();
		if (const FContextObjectTypeClass* Class = Context[Index].GetPtr<FContextObjectTypeClass>())
		{
			if (!Class->Class || Class->Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				Report.Diagnostic = FString::Printf(TEXT("Context%d's class %s cannot be made here (abstract, missing or replaced); evaluate it in play."), Index,
					Class->Class ? *Class->Class->GetName() : TEXT("None"));
				return Report;
			}
			if (++ObjectCount > MaxObjectParameters)
			{
				Report.Diagnostic = FString::Printf(TEXT("The table has more than %d class parameters."), MaxObjectParameters);
				return Report;
			}
			UObject* Object = NewObject<UObject>(GetTransientPackage(), Class->Class, MakeUniqueObjectName(GetTransientPackage(), Class->Class, FName(TEXT("HyperAIChooserContext"))), RF_Transient);
			Objects.Emplace(Object);
			Parameter.Type = Class->Class;
			Parameter.Container = reinterpret_cast<uint8*>(Object);
		}
		else if (const FContextObjectTypeStruct* Struct = Context[Index].GetPtr<FContextObjectTypeStruct>())
		{
			if (!Struct->Struct)
			{
				Report.Diagnostic = FString::Printf(TEXT("Context%d has no struct."), Index);
				return Report;
			}
			Structs[Index].InitializeAs(Struct->Struct);
			Parameter.Type = Struct->Struct;
			Parameter.Container = Structs[Index].GetMutableMemory();
		}
		else
		{
			Report.Diagnostic = FString::Printf(TEXT("Context%d is neither a class nor a struct parameter."), Index);
			return Report;
		}
	}
	TArray<TPair<FString, FString>> Values;
	if (!Gate::ParseSettings(Request.Values, Values, Error))
	{
		Report.Diagnostic = Error;
		return Report;
	}
	for (const TPair<FString, FString>& Value : Values)
	{
		FString Done;
		if (!SetValue(*Table, Parameters, Value.Key, Value.Value, Done, Error))
		{
			Report.Diagnostic = Error;
			return Report;
		}
		Report.Inputs.Add(Done);
	}
	FChooserEvaluationContext Evaluation;
	for (int32 Index = 0; Index < Context.Num(); ++Index)
	{
		if (Context[Index].GetPtr<FContextObjectTypeClass>()) Evaluation.AddObjectParam(reinterpret_cast<UObject*>(Parameters[Index].Container));
		else Evaluation.AddStructViewParam(FStructView(Structs[Index].GetScriptStruct(), Structs[Index].GetMutableMemory()));
	}
	FRandomStream Stream(Request.Seed);
	if (Request.Seed >= 0) Evaluation.RandomStream = &Stream;
	// Bindings are compiled, as loading the table does; undo leaves them uncompiled until something compiles them again.
	Gate::CompileTable(*Table->GetRootChooser());

	// Each filter column on its own, over every enabled row, with the column's own Filter: the rows it turns away.
	TArray<uint32> Enabled;
	for (int32 Row = 0; Row < Table->ResultsStructs.Num(); ++Row)
	{
		if (!Table->IsRowDisabled(Row)) Enabled.Add(Row);
	}
	TSet<int32> Passing;
	for (const uint32 Row : Enabled) Passing.Add(Row);
	for (int32 Index = 0; Index < Table->ColumnsStructs.Num(); ++Index)
	{
		const FInstancedStruct& Data = Table->ColumnsStructs[Index];
		if (!Data.IsValid()) continue;
		const FChooserColumnBase& Column = Data.Get<FChooserColumnBase>();
		if (Column.bDisabled || !Column.HasFilters()) continue;
		if (Column.IsRandomizeColumn())
		{
			Report.bRandom = true;
			continue;
		}
		if (Gate::KindOf(Data) == TEXT("pose_match"))
		{
			Report.Notes.Add(FString::Printf(TEXT("Column %d (pose_match) matches poses from a pose history, which only play has; the engine's choice below ran it without one."), Index));
			continue;
		}
		const FChooserPropertyBinding* Binding = Gate::BindingOf(Data);
		FHyperAIChooserColumnFilter& Filter = Report.Filtered.AddDefaulted_GetRef();
		Filter.Column = Index;
		Filter.Kind = Gate::KindOf(Data);
		Filter.Binding = Binding ? Gate::DescribeBinding(*Binding) : FString();
		Filter.Input = Binding && !Filter.Binding.IsEmpty() ? ReadBinding(*Binding, Parameters) : FString(TEXT("(unbound: every row passes)"));
		TArray<FChooserIndexArray::FIndexData> InData;
		TArray<FChooserIndexArray::FIndexData> OutData;
		InData.Reserve(Enabled.Num());
		for (const uint32 Row : Enabled) InData.Emplace(Row, 0.f);
		OutData.SetNumUninitialized(FMath::Max(1, Enabled.Num()));
		FChooserIndexArray In(InData.GetData(), InData.Num());
		for (const FChooserIndexArray::FIndexData& Row : InData) In.Push(Row);
		FChooserIndexArray Out(OutData.GetData(), Enabled.Num());
		const int32 ScratchSize = Column.GetScratchAreaSize();
		uint8* ScratchMemory = ScratchSize > 0 ? static_cast<uint8*>(FMemory::Malloc(ScratchSize, 16)) : nullptr;
		const TArrayView<uint8> Scratch(ScratchMemory, ScratchSize);
		Column.InitializeScratchArea(Scratch);
		Column.Filter(Evaluation, In, Out, Scratch);
		Column.DeinitializeScratchArea(Scratch);
		if (ScratchMemory) FMemory::Free(ScratchMemory);
		TSet<int32> Kept;
		for (const FChooserIndexArray::FIndexData& Row : Out) Kept.Add(static_cast<int32>(Row.Index));
		for (const uint32 Row : Enabled)
		{
			if (!Kept.Contains(static_cast<int32>(Row))) Filter.RowsOut.Add(static_cast<int32>(Row));
		}
		Passing = Passing.Intersect(Kept);
	}
	Report.Passed = Passing.Array();
	Report.Passed.Sort();

	// The engine's own choice, as an Evaluate Chooser node makes it; the table keeps the row it picked for its debugger.
	Table->SetDebugSelectedRows({});
	TSoftObjectPtr<UObject> Chosen;
	bool bChosen = false;
	UChooserTable::EvaluateChooser(Evaluation, Table, FObjectChooserBase::FObjectChooserSoftObjectIteratorCallback::CreateLambda(
		[&Chosen, &bChosen](const TSoftObjectPtr<UObject>& Result)
		{
			Chosen = Result;
			bChosen = true;
			return FObjectChooserBase::EIteratorStatus::Stop;
		}));
	const TArray<int32>& Selected = Table->GetDebugSelectedRows();
	if (!Selected.IsEmpty())
	{
		if (Selected[0] == ChooserColumn_SpecialIndex_Fallback) Report.bFallback = bChosen && Table->FallbackResult.IsValid();
		else Report.Row = Selected[0];
	}
	const bool bNoResult = Table->GetContextOwner()->ResultType == EObjectChooserResultType::NoPrimaryResult;
	if (bChosen && !bNoResult) Report.Result = Chosen.ToSoftObjectPath().ToString();
	if (Table->ResultsStructs.IsValidIndex(Report.Row))
	{
		const FInstancedStruct& Won = Table->ResultsStructs[Report.Row];
		const UChooserTable* Inner = Won.GetPtr<FEvaluateChooser>() ? Won.Get<FEvaluateChooser>().Chooser.Get()
			: Won.GetPtr<FNestedChooser>() ? Won.Get<FNestedChooser>().Chooser.Get() : nullptr;
		if (Inner)
		{
			const TArray<int32>& InnerRows = Inner->GetDebugSelectedRows();
			Report.Nested = FString::Printf(TEXT("%s, %s"), *Inner->GetPathName(), InnerRows.IsEmpty() ? TEXT("row unknown")
				: InnerRows[0] == ChooserColumn_SpecialIndex_Fallback ? TEXT("its fallback") : *FString::Printf(TEXT("row %d"), InnerRows[0]));
		}
	}
	for (int32 Index = 0; Index < Table->ColumnsStructs.Num(); ++Index)
	{
		const FInstancedStruct& Data = Table->ColumnsStructs[Index];
		if (!Data.IsValid() || Gate::RoleOf(Data) != TEXT("output") || Data.Get<FChooserColumnBase>().bDisabled) continue;
		const FChooserPropertyBinding* Binding = Gate::BindingOf(Data);
		const FString Bound = Binding ? Gate::DescribeBinding(*Binding) : FString();
		Report.Outputs.Add(FString::Printf(TEXT("Column %d (%s %s) = %s"), Index, *Gate::KindOf(Data), *Bound, Bound.IsEmpty() ? TEXT("(unbound)") : *ReadBinding(*Binding, Parameters)));
	}
	if (Report.bRandom && Report.Passed.Num() > 1 && Request.Seed < 0)
	{
		Report.Notes.Add(TEXT("A Randomize column picked among the rows that passed; another call may pick another. Give Seed to repeat a pick."));
	}
	Report.Notes.Add(TEXT("The table's debugger now shows this evaluation's row, as after any evaluation in the editor; nothing in the asset changed."));
	Report.bOk = true;
	if (Report.Row >= 0)
	{
		Report.Diagnostic = FString::Printf(TEXT("Row %d won%s%s; %d row(s) passed every filter."), Report.Row, Report.Result.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", giving %s"), *Report.Result),
			Report.Nested.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" through %s"), *Report.Nested), Report.Passed.Num());
	}
	else if (Report.bFallback)
	{
		Report.Diagnostic = FString::Printf(TEXT("No row passed; the fallback gave %s."), Report.Result.IsEmpty() ? TEXT("nothing") : *Report.Result);
	}
	else
	{
		Report.Diagnostic = FString::Printf(TEXT("No row was chosen (%d passed every filter but gave nothing, or none passed) and there is no fallback; the table gives nothing."), Report.Passed.Num());
	}
	return Report;
}

FHyperAIChooserInspectReport UHyperAIStudioChooserToolset::hyper_chooser_inspect(const FHyperAIChooserInspectRequest& Request)
{
	return FHyperAIStudioChooserContracts::Inspect(Request);
}

FHyperAIChooserPlanReport UHyperAIStudioChooserToolset::hyper_chooser_apply_plan(const FHyperAIChooserPlanRequest& Request)
{
	return FHyperAIStudioChooserContracts::ApplyPlan(Request);
}

FHyperAIChooserEvaluateReport UHyperAIStudioChooserToolset::hyper_chooser_evaluate(const FHyperAIChooserEvaluateRequest& Request)
{
	return FHyperAIStudioChooserContracts::Evaluate(Request);
}

void FHyperAIStudioChooserRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioChooserRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioChooserRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioChooserToolset::StaticClass(),
			FHyperAIStudioChooserContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioChooser, Warning, TEXT("Could not unregister the chooser toolset: %s"), *Error);
		}
	}
	HyperAIStudio::ChooserTools::Private::GTouchedTables.Reset();
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioChooserRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioChooserContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioChooser, Verbose, TEXT("The chooser source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioChooserToolset::StaticClass(),
		FHyperAIStudioChooserContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioChooser, Error, TEXT("Chooser cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
