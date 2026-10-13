// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioDataRegistryToolset.generated.h"

/**
 * One op of a data registry plan, in order. Settings are "Name=Value; Name=Value", the names each kind takes;
 * hyper_data_registry_inspect with no Asset lists them with their types and defaults.
 */
USTRUCT(BlueprintType)
struct FHyperAIDataRegistryOp
{
	GENERATED_BODY()

	/**
	 * create (makes the registry; first, once), set (the registry's own settings), source.add, source.set,
	 * source.remove.
	 */
	UPROPERTY() FString Kind;
	/** create: the registry's type, the name its ids start with (Items in Items:Sword); unique in the project. */
	UPROPERTY() FString RegistryType;
	/**
	 * create: the struct every item is, by path: /Script/MyGame.MyItemRow (a C++ struct, F left out) or a user struct
	 * /Game/Data/S_Item.S_Item. Curve registries use /Script/Engine.RealCurve, RichCurve or SimpleCurve.
	 */
	UPROPERTY() FString ItemStruct;
	/** create: a gameplay tag whose children are the registry's ids (IdFormat's base tag); empty, ids are any name. */
	UPROPERTY() FString Tag;
	/** source.add: data_table, curve_table (one table each), data_table_meta or curve_table_meta (every table found under Paths). */
	UPROPERTY() FString SourceKind;
	/** source.add and source.set of a data_table or curve_table source: the table, /Game/Data/DT_Items. */
	UPROPERTY() FString Table;
	/** source.add and source.set of a meta source: folders (or single assets) searched for tables, /Game/Data/Items. */
	UPROPERTY() TArray<FString> Paths;
	/**
	 * source.set and source.remove: the source by its place in the list, from 0 (as inspect lists them, and as the
	 * ops before it left the list). source.add: where it goes, earlier sources winning an id; -1, the default, the end.
	 */
	UPROPERTY() int32 Index = -1;
	/** "Name=Value; Name=Value": the registry's settings (create, set) or the source's (source.add, source.set). */
	UPROPERTY() FString Params;
};

USTRUCT(BlueprintType)
struct FHyperAIDataRegistryPlanRequest
{
	GENERATED_BODY()

	/** The Data Registry asset the plan makes (with a create op) or changes, under /Game: /Game/Data/DR_Items. */
	UPROPERTY() FString Asset;
	UPROPERTY() TArray<FHyperAIDataRegistryOp> Operations;
	/** Check the plan and report what would change; nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Save the registry. Off, nothing is saved and one undo takes the whole plan back. */
	UPROPERTY() bool bSave = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIDataRegistryPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, completed, awaiting_user_approval, approval_not_queued, invalid_request, operation_failed, save_failed. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Asset;
	UPROPERTY() FString RegistryType;
	UPROPERTY() FString ItemStruct;
	/** The sources as the plan leaves them, one line each: "0 data_table /Game/Data/DT_Items.DT_Items". */
	UPROPERTY() TArray<FString> Sources;
	/** Items the sources hold in the editor, and the first names. */
	UPROPERTY() int32 ItemCount = 0;
	UPROPERTY() TArray<FString> Items;
	/** The Data Registry subsystem has the registry for its type, so games can look its items up. */
	UPROPERTY() bool bRegistered = false;
	/** The registry is under a folder Data Registry settings scan (or every loaded registry is used). */
	UPROPERTY() bool bInScannedDirectory = false;
	/** What each op did. */
	UPROPERTY() TArray<FString> Notes;
	UPROPERTY() TArray<FString> Warnings;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIDataRegistryInspectRequest
{
	GENERATED_BODY()

	/** A Data Registry asset by path. Empty: the op kinds and source kinds with their settings, and the registries under /Game. */
	UPROPERTY() FString Asset;
	/** List up to this many item names; 200 at most. */
	UPROPERTY() int32 MaxItems = 20;
};

/** A source of a registry, as the asset holds it and as it resolves in the editor. */
USTRUCT(BlueprintType)
struct FHyperAIDataRegistrySource
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = 0;
	/** data_table, curve_table, data_table_meta, curve_table_meta, or the class name of a game's own source. */
	UPROPERTY() FString Kind;
	/** A direct source's table. */
	UPROPERTY() FString Table;
	/** A meta source's search folders, and how it uses assets (SearchAssets, RegisterAssets, ...). */
	UPROPERTY() TArray<FString> SearchPaths;
	UPROPERTY() FString AssetUsage;
	/** "Name: value" for each of its settings. */
	UPROPERTY() TArray<FString> Settings;
	/** The tables it resolves to in the editor, each with its row count. */
	UPROPERTY() TArray<FString> Tables;
	/** The runtime sources the registry made from it, when the Data Registry subsystem has initialized the registry. */
	UPROPERTY() TArray<FString> RuntimeSources;
};

/** An op kind or source kind a plan takes, with its settings. */
USTRUCT(BlueprintType)
struct FHyperAIDataRegistryKind
{
	GENERATED_BODY()

	UPROPERTY() FString Kind;
	UPROPERTY() FString Summary;
	/** "Name: type, default value", one per setting Params takes. */
	UPROPERTY() TArray<FString> Settings;
};

USTRUCT(BlueprintType)
struct FHyperAIDataRegistryInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Asset;
	UPROPERTY() FString RegistryType;
	UPROPERTY() FString ItemStruct;
	/** IdFormat's base gameplay tag; empty when ids are any name. */
	UPROPERTY() FString BaseTag;
	/** The registry's own settings: timer frequency and the default cache policy. */
	UPROPERTY() TArray<FString> Settings;
	UPROPERTY() TArray<FHyperAIDataRegistrySource> Sources;
	UPROPERTY() int32 ItemCount = 0;
	UPROPERTY() TArray<FString> Items;
	/** Under a folder of Project Settings > Game > Data Registry > Directories To Scan, so the subsystem loads it. */
	UPROPERTY() bool bInScannedDirectory = false;
	UPROPERTY() TArray<FString> ScanDirectories;
	/** The subsystem holds this registry for its type, and has initialized it. */
	UPROPERTY() bool bRegistered = false;
	UPROPERTY() bool bInitialized = false;
	UPROPERTY() TArray<FString> Warnings;
	/** Without Asset: the op kinds and source kinds hyper_data_registry_apply_plan takes, and the registries under /Game. */
	UPROPERTY() TArray<FHyperAIDataRegistryKind> Kinds;
	UPROPERTY() TArray<FHyperAIDataRegistryKind> SourceKinds;
	UPROPERTY() TArray<FString> Registries;
};

USTRUCT(BlueprintType)
struct FHyperAIDataRegistryLookupRequest
{
	GENERATED_BODY()

	/** The registry by path; or give RegistryType. */
	UPROPERTY() FString Asset;
	/** The registry by its type, as games name it. */
	UPROPERTY() FString RegistryType;
	/** Item names (Sword), or ids with the type (Items:Sword); 50 at most. */
	UPROPERTY() TArray<FString> Items;
	/** text (each field "Name: value" in Unreal's text form, the default) or json (one JSON object per item). */
	UPROPERTY() FString Format;
	/** A curve registry: the time or level each curve is read at. */
	UPROPERTY() float Level = 1.f;
};

USTRUCT(BlueprintType)
struct FHyperAIDataRegistryItem
{
	GENERATED_BODY()

	/** Type:Name. */
	UPROPERTY() FString Id;
	UPROPERTY() bool bFound = false;
	/** Where it came from: "source 0 data_table /Game/Data/DT_Items.DT_Items". */
	UPROPERTY() FString Source;
	/** Other sources holding the same name, which this one hides. */
	UPROPERTY() TArray<FString> Shadowed;
	UPROPERTY() FString Struct;
	/** Format text: "Name: value" per field; a curve: its keys. */
	UPROPERTY() TArray<FString> Fields;
	/** Format json: the item as a JSON object. */
	UPROPERTY() FString Json;
	/** A curve: its value at Level. */
	UPROPERTY() bool bCurve = false;
	UPROPERTY() float Value = 0.f;
	/** Why it was not found. */
	UPROPERTY() FString Diagnostic;
};

USTRUCT(BlueprintType)
struct FHyperAIDataRegistryLookupReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Asset;
	UPROPERTY() FString RegistryType;
	UPROPERTY() FString ItemStruct;
	/** registry (the subsystem's initialized registry answered) or tables (read from the sources' tables in the editor). */
	UPROPERTY() FString ResolvedBy;
	UPROPERTY() TArray<FHyperAIDataRegistryItem> Items;
	/** Ids not found; the call still succeeds. */
	UPROPERTY() TArray<FString> Missing;
	UPROPERTY() TArray<FString> Warnings;
};

/**
 * Data Registry: registries that put one id space over many data and curve tables (and the tables game features
 * add, found by meta sources), made and given sources in one undo step, read back, and looked up item by item in the
 * editor. Complements Epic's read-only DataRegistryToolset.
 */
UCLASS()
class HYPERAISTUDIODATAREGISTRY_API UHyperAIStudioDataRegistryToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * A Data Registry asset, read only: its type, item struct, id format (base gameplay tag), timer and cache policy,
	 * each source (kind, table or meta search paths, the tables it resolves to, its runtime sources), the item count and
	 * first names, and whether Data Registry settings scan its folder so games can use it. Without Asset: the op and
	 * source kinds hyper_data_registry_apply_plan takes with their settings, and the registries under /Game.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Data")
	static FHyperAIDataRegistryInspectReport hyper_data_registry_inspect(const FHyperAIDataRegistryInspectRequest& Request);

	/**
	 * Makes or changes one Data Registry in one plan, one undo step: create it (registry type, item struct, optional
	 * base gameplay tag), set its timer and cache policy, source.add a data table, curve table or meta source over folders
	 * (game feature tables), source.set and source.remove by index. A table's row struct must match the item struct.
	 * The registry is refreshed after; the reply lists its sources and items and warns when Data Registry settings do
	 * not scan its folder. bDryRun checks it first.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Data")
	static FHyperAIDataRegistryPlanReport hyper_data_registry_apply_plan(const FHyperAIDataRegistryPlanRequest& Request);

	/**
	 * Looks items up in a Data Registry (by Asset or RegistryType) by name or Type:Name, read only: each item's fields in
	 * Unreal's text form or as JSON, the source it came from and the sources it hides; a curve registry's value at Level.
	 * Missing ids are listed, not a failure.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Data")
	static FHyperAIDataRegistryLookupReport hyper_data_registry_lookup(const FHyperAIDataRegistryLookupRequest& Request);
};

class FHyperAIStudioDataRegistryContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("data_config_localization");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiodataregistrytoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_data_registry_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_data_registry_apply_plan");
	static constexpr const TCHAR* LookupToolName = TEXT("hyper_data_registry_lookup");
	static constexpr int32 MaxOperations = 32;
	static constexpr int32 MaxSources = 64;
	static constexpr int32 MaxLookupItems = 50;
	static constexpr int32 MaxListedItems = 200;
	/** Tables a registry's meta sources resolve to that are read in the editor. */
	static constexpr int32 MaxResolvedTables = 200;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAIDataRegistryInspectReport Inspect(const FHyperAIDataRegistryInspectRequest& Request);
	static FHyperAIDataRegistryPlanReport ApplyPlan(const FHyperAIDataRegistryPlanRequest& Request);
	static FHyperAIDataRegistryLookupReport Lookup(const FHyperAIDataRegistryLookupRequest& Request);
};

class FHyperAIStudioDataRegistryRegistration final
{
public:
	void Startup();
	void Shutdown();

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
