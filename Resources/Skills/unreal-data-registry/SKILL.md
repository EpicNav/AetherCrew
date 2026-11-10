---
name: unreal-data-registry
description: Set up Unreal Data Registries - one id space over many data and curve tables (and the tables game features add, merged by meta sources) - make a registry, give it data table, curve table and meta sources, tune its cache, register its folder in Data Registry settings, and look items up by id to check them; also how games read items with FDataRegistryId and UDataRegistrySubsystem in Blueprint and C++.
---

# Data Registries

A Data Registry puts one id space over many tables. Games ask for `Items:Sword` and the registry finds the row in
whichever of its sources holds it: a data table, a curve table, or every table a meta source finds under a folder,
including the tables a game feature plugin adds while it is active. Earlier sources win a name both hold, so a patch
table placed first overrides the base table without editing it.

| work | tool |
|---|---|
| the op and source kinds with their settings; a registry's type, item struct, sources, the tables they resolve to, its items, and whether it is in a scanned folder | `hyper_data_registry_inspect` |
| make a registry and add, change or remove its sources and settings, one undo step | `hyper_data_registry_apply_plan` |
| check items by id: each field, which source it came from, which sources it hides; a curve's value at a level | `hyper_data_registry_lookup` |

Toolset `HyperAIStudioDataRegistry.HyperAIStudioDataRegistryToolset`, each tool called with `{"request": {...}}`. The
pack needs the Data Registry plugin; without it its tools are not there. Epic's read-only `DataRegistryToolset59.DataRegistryTools59`
(ListRegistries, GetRegistryInfo, ListItems, GetItems) reads the registries the running subsystem holds; these tools
also make and edit them, and read a registry the subsystem has not loaded.

## The ids

- An id is `FDataRegistryId`: the registry's type (`RegistryType`, unique in the project) and an item name (the row
  name). In text it is `Type:Name`.
- With a base gameplay tag (`Tag` on `create`, IdFormat's base tag), the ids are that tag's children and the rows are
  named by the full tag (`Item.Weapon.Sword`); id pickers in Blueprints and Details then offer the tags. Without one,
  any name is an id.
- A property a designer picks an item with is an `FDataRegistryId` UPROPERTY; `meta = (ItemStruct = "MyItemRow")`
  limits the picker to registries of that struct.

## Reading items in a game

- Blueprint: Get Data Registry Item (`UDataRegistrySubsystem::GetCachedItemBP`, true when the item is in memory), Find
  Data Registry Item (`FindCachedItemBP`, with Found / Not Found exits), Acquire Data Registry Item (`AcquireItemBP`,
  loads it if needed and calls back), Evaluate Data Registry Curve (`EvaluateDataRegistryCurve`), and Get Possible Data
  Registry Id List.
- C++: `UDataRegistrySubsystem::Get()->GetCachedItem<FMyItemRow>(ItemId)` returns a pointer or null;
  `AcquireItem(ItemId, FDataRegistryItemAcquiredCallback::CreateUObject(...))` loads it first;
  `EvaluateCachedCurve(OutValue, OutCurve, ItemId, Level)` reads a curve. Precached tables (the default) answer
  GetCachedItem at once; a table with `bPrecacheTable=false` answers only after AcquireItem.
- The subsystem loads only registries in the folders of Project Settings > Game > Data Registry > Directories To Scan
  (or every loaded registry when Initialize All Loaded Registries is on). A registry elsewhere exists, but games
  cannot use it.
- A game feature plugin adds its tables with its Add Data Registry Source action, which registers them with a meta
  source whose AssetUsage accepts registered assets (`RegisterAssets` or `SearchAndRegisterAssets`).

## A plan

```json
{"request": {"Asset": "/Game/Data/DR_Items",
 "Operations": [
  {"Kind": "create", "RegistryType": "Items", "ItemStruct": "/Script/MyGame.MyItemRow"},
  {"Kind": "source.add", "SourceKind": "data_table", "Table": "/Game/Data/DT_Items"},
  {"Kind": "source.add", "SourceKind": "data_table_meta", "Paths": ["/Game/Data/Items"], "Params": "AssetUsage=SearchAndRegisterAssets"},
  {"Kind": "set", "Params": "MaxNumberKept=200; ForceKeepSeconds=30"}]}}
```

The item struct here is the C++ `FMyItemRow` (`USTRUCT() struct FMyItemRow : public FTableRowBase`), named without
its F; a user struct goes by its path, `/Game/Data/S_Item.S_Item`. Try the plan with `bDryRun: true` first: it checks
everything and lists what would change. Then run it; with approval on it waits in the Activity panel: call
`hyper_operation_wait` with `bWaitThroughApproval: true`. One Edit > Undo takes the whole plan back. `bSave: true`
saves the registry.

`Params` are `"Name=Value; Name=Value"`; `hyper_data_registry_inspect` with no Asset lists every setting with its type
and default. Names match in any case, and a bool's `b` is optional (`PrecacheTable=false`).

## Op kinds

- `create` comes first, once: `RegistryType`, `ItemStruct`, optional `Tag`, and `Params` for the settings `set` takes.
  The type must not be another registry's. Without `create` the plan changes the registry at `Asset`.
- `set`: the registry's settings: `TimerUpdateFrequency` (seconds between cache checks) and the default cache policy,
  `bCacheIsAlwaysVolatile`, `bUseCurveTableCacheVersion`, `MinNumberKept`, `MaxNumberKept`, `ForceKeepSeconds`,
  `ForceReleaseSeconds`; also `RegistryType`, `ItemStruct` and `BaseGameplayTag` to change those.
- `source.add`: a source of `SourceKind`; `Index` places it (earlier sources win an id; -1, the default, is the end):
  - `data_table`: one data table, its `Table`. Its rows must be the item struct or a struct made from it, or the plan
    is refused.
  - `curve_table`: one curve table, for a registry whose item struct is `/Script/Engine.RealCurve` (any curve table),
    `RichCurve` (cubic tables) or `SimpleCurve` (linear tables).
  - `data_table_meta`: every data table with matching rows under `Paths` (folders or single assets, recursive), plus
    tables registered at run time. Settings: `AssetUsage` (`SearchAssets`, the default here; `RegisterAssets`;
    `SearchAndRegisterAssets`; `NoAssets`), `IncludePatterns` and `ExcludePatterns` (wildcards on package names),
    `AssetBaseClass`, and the table rules below.
  - `curve_table_meta`: the same for curve tables.
  - Every table source takes `bPrecacheTable` (load the whole table with the registry, the default; off, items load on
    AcquireItem) and `CachedTableKeepSeconds`.
- `source.set`: source `Index` (as inspect numbers them, after the ops before it): a new `Table`, `Paths` or `Params`.
- `source.remove`: source `Index`.

After the edit the registry picks up its sources: each table source reloads its table, and when the Data Registry
subsystem runs the registry is registered again for its type, as an edit in its own editor does.

## Register the folder

The reply's `bInScannedDirectory` and `Warnings` say when the registry is outside the folders Data Registry settings
scan. Add its folder to Project Settings > Game > Data Registry > Directories To Scan, or with Epic's
ConfigSettingsToolset (the DataRegistrySettings section, `DirectoriesToScan`). HyperAI does not change project
settings itself. In the editor the subsystem starts at load (or at the first Play In Editor when its delay setting is
on); a folder added in Project Settings reinitializes the subsystem at once (one set through the config file alone
waits for an editor restart). Run `hyper_data_registry_inspect` again to see `bRegistered`.

## Check it

1. `hyper_data_registry_inspect` with `Asset`: `Sources` with the `Tables` each resolves to and their row counts,
   `ItemCount` and `Items`, `bRegistered` (the subsystem has it) and `Warnings` (tables skipped because their rows do
   not match, item names outside the base tag, an unscanned folder).
2. `hyper_data_registry_lookup` with `Items: ["Sword", "Items:Shield"]`: each item's `Fields` (`Format: "json"` for a
   JSON object), its `Source`, and `Shadowed`, the other sources holding the same name. Missing ids are listed in
   `Missing` and the call still succeeds. A curve registry's items give `Value` at `Level`. `ResolvedBy` says whether
   the running registry answered or the tables were read directly (the subsystem has not loaded the registry).

## Pitfalls

- A table whose rows are another struct is refused by the plan; one found by a meta source is skipped and named in
  inspect's warnings. Make the table with the item struct (Epic's `EditorToolset.DataTableTools.create`) or point the
  registry at the right struct.
- Two registries cannot share a type; the subsystem keeps one and drops the other.
- A meta source made in the Details panel starts with AssetUsage NoAssets and finds nothing; the plan's `source.add`
  sets `SearchAssets`.
- A row named outside the base tag is reachable by name but not through tag pickers.
- Plans are refused during Play In Editor, and registries go under `/Game` only (a meta source may search plugin
  folders).
