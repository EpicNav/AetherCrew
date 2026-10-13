// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioChooserToolset.h"
#include "StructUtils/InstancedStruct.h"

class FProperty;
class FStructProperty;
class UChooserTable;
class UEnum;
struct FChooserPropertyBinding;

/**
 * The chooser plan's checks and what it writes, and the read helpers inspect and evaluate share. A plan is checked by
 * running every op on a transient copy of the table (or a new transient table for create), so a refusal leaves the asset
 * untouched; the same ops then run on the asset inside one undo step. Columns are the engine's own column structs, held
 * in UChooserTable::ColumnsStructs and kept row for row with ResultsStructs as the Chooser editor keeps them
 * (SetNumRows, InsertRows, DeleteRows). Game thread only.
 */
namespace HyperAIStudio::Chooser::Gate
{
	/** A column type the plan adds by a word. */
	struct FColumnKindSpec
	{
		const TCHAR* Kind = TEXT("");
		/** The column struct's path, such as /Script/Chooser.BoolColumn. */
		const TCHAR* Struct = TEXT("");
		/** What its cells take as text. */
		const TCHAR* Cells = TEXT("");
		/** filter, scoring, output or random. */
		const TCHAR* Role = TEXT("");
	};

	/** Every column kind, in the order the docs list them. */
	const TArray<FColumnKindSpec>& ColumnKinds();
	const FColumnKindSpec* FindColumnKind(const FString& Kind);
	/** The plan's word for a column struct, or null when the plan has none. */
	const FColumnKindSpec* KindOfStruct(const UScriptStruct* Struct);
	/** Every struct derived from FChooserColumnBase this editor has loaded, as the Chooser editor lists column types. */
	TArray<UScriptStruct*> AllColumnStructs();
	/** The column struct of a kind among AllColumnStructs; null when its plugin is not loaded (pose_match without PoseSearch). */
	UScriptStruct* ColumnStruct(const FColumnKindSpec& Spec);

	/** The op kinds, in order. */
	const TArray<FString>& OpKinds();
	/** Each op with the fields it takes, for hyper_chooser_inspect. */
	TArray<FString> DescribeOps();

	/** "Name=Value; Name=Value", trimmed, in order; a part without '=' is refused. */
	bool ParseSettings(const FString& Text, TArray<TPair<FString, FString>>& Out, FString& OutError);
	/** A number: 12, -3.5. */
	bool ParseNumber(const FString& Text, double& Out);
	/** A bool cell: 1 true, 0 false, 2 any (empty, any or *). */
	bool ParseBoolCell(const FString& Text, uint8& Out, FString& OutError);
	/** A float range cell: min..max, ..max, min.., or any (empty); min may pass max (a range across a wrapped input). */
	bool ParseRange(const FString& Text, double& OutMin, double& OutMax, bool& bOutNoMin, bool& bOutNoMax, FString& OutError);
	FString FormatRange(double Min, double Max, bool bNoMin, bool bNoMax);
	/** A comparison cell: 0 equal to Value, 1 not equal (!Value), 2 any (empty, any or *). */
	int32 ParseComparison(const FString& Text, FString& OutValue);
	/** The index of an enum value by its short name (Walking), full name (MOVE_Walking, EStance::Crouch) or display name; INDEX_NONE when none. */
	int32 FindEnumIndex(const UEnum& Enum, const FString& Text);
	/** The enum's value names, comma separated. */
	FString EnumWords(const UEnum& Enum);

	/** The binding property ("Binding") of a column's input struct, or null when it has none. */
	FStructProperty* BindingPropertyOf(const UScriptStruct* InputStruct);
	/** A column's binding as it stands; null when its input has none. */
	const FChooserPropertyBinding* BindingOf(const FInstancedStruct& Column);
	/** The type of property a column's input binds (bool, double, enum, FGameplayTagContainer, object, FName...), from its BindingType. */
	FString BindingTypeOf(const FInstancedStruct& Column);
	/** Whether Property can be bound by an input of TypeFilter, as the Chooser editor's binding menu decides. */
	bool CanBind(const FProperty& Property, const FString& TypeFilter);

	/** A property path on a table's parameters, resolved. */
	struct FBindingPath
	{
		int32 ContextIndex = 0;
		TArray<FName> Chain;
		bool bRoot = false;
		const FProperty* Leaf = nullptr;
		FString Text;
	};

	/**
	 * Speed, Context1.Speed or Context0.Velocity.X against the table's parameters: each step a Blueprint-visible property,
	 * through structs only, the last one of a type TypeFilter takes; a bare ContextN binds a struct parameter whole.
	 */
	bool ResolveBinding(const UChooserTable& Table, const FString& Path, const FString& TypeFilter, FBindingPath& Out, FString& OutError);
	/** Context0.Speed, or empty when unbound. */
	FString DescribeBinding(const FChooserPropertyBinding& Binding);
	/** Why a column's binding does not resolve against its table; empty when it does or it is unbound. */
	FString BindingProblem(const UChooserTable& Table, const FInstancedStruct& Column);

	/** The plan's word for a column (float_range...), or its struct's name. */
	FString KindOf(const FInstancedStruct& Column);
	/** filter, scoring, output or random. */
	FString RoleOf(const FInstancedStruct& Column);
	/** A cell as row.add takes it; Row may be the fallback (-2) for an output column. */
	FString CellText(const FInstancedStruct& Column, int32 Row);
	/** asset <path>, soft asset <path>, class <path>, chooser <path>, nested <name>, or none. */
	FString ResultText(const FInstancedStruct& Result);
	/** The column's settings as Name = Value, the names Params takes. */
	TArray<FString> SettingsOf(const FInstancedStruct& Column, UObject* Owner);
	/** A column type's settings as "Name: type, default". */
	TArray<FString> DescribeSettings(const UScriptStruct& Struct);
	/** input, output or both. */
	FString DirectionText(uint8 Direction);

	/** What could go wrong in play. */
	TArray<FString> Warn(const UChooserTable& Table);

	/** Compiles the table's bound columns and results (and its nested tables), as UChooserTable::Compile does, skipping inputs left unbound. */
	void CompileTable(UChooserTable& Table);

	/** The chooser table an undone plan made at PackagePath, kept as garbage for redo; null when there is none. */
	UChooserTable* UndoneAt(const FString& PackagePath);

	/** A chooser table by path, quietly; null with why when it is not one. */
	UChooserTable* LoadTable(const FString& Path, FString& OutError);

	struct FPlan
	{
		/** Package path, /Game/... */
		FString AssetPath;
		/** The plan makes the table (create); otherwise it changes Existing. */
		bool bCreate = false;
		TObjectPtr<UChooserTable> Existing = nullptr;
		/** What the ops did on the copy. */
		TArray<FString> Notes;
		TArray<FString> Warnings;
		int32 Columns = 0;
		int32 Rows = 0;
	};

	/** The request, checked by running every op on a transient copy: names, paths, kinds, bindings, cells. */
	bool Check(const FHyperAIChooserPlanRequest& Request, FPlan& OutPlan, FString& OutError);

	/** The ops on Table; bReal calls Modify first (inside the caller's undo step). OutError names the op that failed. */
	bool Apply(UChooserTable& Table, const TArray<FHyperAIChooserOp>& Operations, bool bCreate, bool bReal, TArray<FString>& OutNotes, FString& OutError);
}
