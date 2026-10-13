// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDataRegistryToolset.h"

class FProperty;
class UCurveTable;
class UDataRegistry;
class UDataRegistrySource;
class UDataTable;
class UScriptStruct;

/**
 * The data registry plan's checks and writes, the settings it sets by reflection, and how a registry's sources resolve
 * to tables in the editor. UDataRegistry keeps RegistryType, ItemStruct, IdFormat and DataSources protected with no
 * setters, so they are written through their properties, as the Details panel does. Game thread only.
 */
namespace HyperAIStudio::DataRegistry::Gate
{
	/** create, set, source.add, source.set, source.remove, in the order the docs list them. */
	const TArray<FString>& OpKinds();
	/** data_table, curve_table, data_table_meta, curve_table_meta. */
	const TArray<FString>& SourceKinds();
	/** The engine class of a source kind; null when Kind is not one. */
	UClass* SourceClassOf(const FString& Kind);
	/** The kind of a source object; a game's own source class by its name. */
	FString SourceKindOf(const UDataRegistrySource* Source);
	bool IsMetaKind(const FString& Kind);

	/**
	 * A setting Params can name: an editable property of the object, or of one of the setting structs it holds (the cache
	 * policy, a table source's rules, a meta source's search rules, the id format), which are named by their own names.
	 */
	struct FLeaf
	{
		/** Its own name: MaxNumberKept. */
		FString Name;
		/** With the struct holding it: DefaultCachePolicy.MaxNumberKept. */
		FString Path;
		/** The struct property holding it, or null. */
		FProperty* Outer = nullptr;
		FProperty* Property = nullptr;
	};

	TArray<FLeaf> Leaves(const UClass& Class);
	/** By name or path, in any case, spaces left out, a bool's b optional. */
	const FLeaf* FindLeaf(const TArray<FLeaf>& Leaves, const FString& Name);
	void* LeafValue(const FLeaf& Leaf, UObject& Object);
	const void* LeafValue(const FLeaf& Leaf, const UObject& Object);
	/** The value as text, an enum by its short name. */
	FString LeafText(const FLeaf& Leaf, const UObject& Object);
	/** "Name: type, default value" (or "value" with bCurrent) for each leaf of Class, read from Values (its defaults when null). */
	TArray<FString> DescribeLeaves(const UClass& Class, const UObject* Values, bool bCurrent);

	/** "Name=Value; Name=Value" into pairs; a part without '=' is refused. */
	bool SplitParams(const FString& Text, TArray<TPair<FString, FString>>& Out, FString& OutError);

	/** A setting checked and ready to import. */
	struct FAssign
	{
		FLeaf Leaf;
		/** Text Unreal's import takes. */
		FString Import;
		/** The value as the reply shows it. */
		FString Shown;
		/** An object or class setting: what it names. */
		TObjectPtr<UObject> Resolved = nullptr;
	};

	/**
	 * Text into what Property takes: true/false, numbers, enums by short or display name, gameplay tags by name, asset
	 * and class paths (which must be there and of the right class), lists "a, b" of strings. Refused with what it takes.
	 */
	bool CoerceValue(const FProperty& Property, const FString& Text, FAssign& InOut, FString& OutError);
	/** Every setting in Params against Class's leaves; an unknown name is refused with the names it takes. */
	bool ParseAssigns(const UClass& Class, const FString& Params, TArray<FAssign>& Out, FString& OutError);
	/** Imports each assign into Object; the caller has called Modify() first. */
	bool ApplyAssigns(UObject& Object, const TArray<FAssign>& Assigns, FString& OutError);

	/**
	 * A struct by path or name: /Script/Engine.RichCurve, /Script/MyGame.FMyRow (the F is dropped), MyRow, or a user
	 * struct /Game/Data/S_Item(.S_Item). Null with a reason when there is none.
	 */
	UScriptStruct* FindStruct(const FString& Text, FString& OutReason);
	/** A table's rows are of Item (or a struct made from it). */
	bool RowsMatch(const UScriptStruct* Rows, const UScriptStruct* Item);
	/** Why a curve table cannot serve Item (curve items only; rich or simple curves as the table holds); empty when it can. */
	FString CurveMismatch(const UCurveTable& Table, const UScriptStruct* Item);

	/** A source as the plan leaves it. */
	struct FSimSource
	{
		FString Kind;
		UClass* Class = nullptr;
		/** The source object there already; null for one the plan adds. */
		TObjectPtr<UDataRegistrySource> Existing = nullptr;
		TArray<FAssign> Assigns;
		/** The table it reads, as the plan leaves it (direct sources). */
		TObjectPtr<UObject> Table = nullptr;
		FString TablePath;
		/** Its search folders (meta sources). */
		TArray<FString> Paths;
		FString Describe(int32 Index) const;
	};

	struct FPlan
	{
		/** Package path, /Game/... */
		FString AssetPath;
		bool bMake = false;
		TObjectPtr<UDataRegistry> Existing = nullptr;
		/** The registry's settings in order, create's RegistryType, ItemStruct and Tag first. */
		TArray<FAssign> RegistryAssigns;
		FName RegistryType;
		TObjectPtr<const UScriptStruct> ItemStruct = nullptr;
		FString Tag;
		TArray<FSimSource> Sources;
		bool bSourcesChanged = false;
		/** What each op will do. */
		TArray<FString> Notes;
		TArray<FString> Warnings;
	};

	/** The registry an undone plan made at PackagePath, kept as garbage for redo; null when there is none. */
	UDataRegistry* UndoneAt(const FString& PackagePath);
	/** Another registry, on disk or in memory, already using Type; empty when none. */
	FString TypeTakenBy(FName Type, const FString& OwnPackage);

	/** The request, checked: names, paths, settings, tables against the item struct. Nothing changes. */
	bool Check(const FHyperAIDataRegistryPlanRequest& Request, FPlan& OutPlan, FString& OutError);
	/** The registry as the plan leaves it, inside the plan's undo step (the registry Modify()'d or just made). */
	bool Write(UDataRegistry& Registry, const FPlan& Plan, FString& OutError);

	/** A table a registry's source reads in the editor. */
	struct FResolvedTable
	{
		int32 SourceIndex = 0;
		FString Kind;
		TObjectPtr<UDataTable> DataTable = nullptr;
		TObjectPtr<UCurveTable> CurveTable = nullptr;
		FString Path;
		FString Describe() const;
	};

	/**
	 * The tables Registry's sources read, in the order the registry asks them (earlier sources win): a direct source's
	 * table, and the tables a meta source's search rules find in the asset registry whose rows match the item struct. Notes
	 * say what was skipped. Tables are loaded quietly when on disk; nothing is changed.
	 */
	TArray<FResolvedTable> ResolveTables(const UDataRegistry& Registry, TArray<FString>& OutNotes);
	/** Every item name the tables hold, first occurrence first. */
	TArray<FName> ItemNames(const TArray<FResolvedTable>& Tables);

	/** Data Registry settings' Directories To Scan, as the subsystem matches them. */
	TArray<FString> ScanDirectories();
	/** The subsystem would load a registry at Path: under a scanned folder, or every loaded registry is used. */
	bool IsScanned(const FString& Path);
	/** The warning for a registry outside the scanned folders, with how to add it. */
	FString ScanWarning(const FString& PackagePath);

	/**
	 * The registry picks up its sources: each source reloads its table, and when the Data Registry subsystem is running the
	 * registry is (re)registered for its type and reinitialized, as an edit in its editor does. Notes say what happened.
	 */
	void Refresh(UDataRegistry& Registry, TArray<FString>& OutNotes);
	/** The subsystem has this registry for its type. */
	bool IsRegistered(const UDataRegistry& Registry);

	/** Item memory as "Name: value" per field, each exported with the item as its own delta. */
	TArray<FString> ExportFields(const UScriptStruct& Struct, const void* Memory);
}
