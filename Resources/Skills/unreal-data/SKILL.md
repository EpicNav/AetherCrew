---
name: unreal-data
description: Build and check game data in Unreal - Data Tables and their row structs, user-defined structs and enums, Curve Tables, Data Assets, project config settings (Maps and Modes and other sections) and String Tables for localizable text - with HyperAI's Data tools and Epic's data, config and string table tools. Use for item, weapon, enemy or level tables, balance curves, data-driven settings, CSV imports, struct and enum changes, default maps and game mode, and UI text that must be translatable.
---

# Game data: tables, structs, curves, config and text

HyperAI's Data tools are in toolset `HyperAIStudioData.HyperAIStudioDataToolset`, called with `{"request": {...}}`:
`hyper_data_inspect`, `hyper_data_validate` and `hyper_data_apply_plan`. If they are missing the data_config_localization
pack is off: say so. They read and check; the values themselves are written with Epic's tools:

| Job | Tool |
|---|---|
| Data Table rows and values | `editor_toolset.toolsets.data_table.DataTableTools`: `search_row_structs`, `create` (folder_path, asset_name, schema), `import_file` (a CSV file's absolute path, with schema), `get_schema`, `list_rows`, `get_rows`, `add_rows`, `set_rows`, `rename_rows`, `remove_rows` |
| Curve Tables | `editor_toolset.toolsets.curve_table.CurveTableTools`: `create`, `import_file`, `list_rows`, `add_row`, `rename_row`, `remove_row`, `add_key`, `set_keys`, `get_keys` |
| Data Assets | `editor_toolset.toolsets.data_asset.DataAssetTools.create` (asset_type a DataAsset class), then `ObjectTools.get_properties` and `set_properties` for its fields |
| Row structs and enums | `hyper_blueprint_structure_apply_plan` (unreal-blueprint skill): `struct.create`, `struct.add_field`, `struct.set_field`, `struct.rename_field`, `struct.remove_field`, `enum.create`, `enum.add_value`, `enum.rename_value`, `enum.remove_value` |
| Project and editor settings | `ConfigSettingsToolset59.ConfigSettingsToolset59`: `ListContainers`, `ListCategories`, `ListSections`, `GetSectionSchema`, `GetSectionPropertyValues`, `SetSectionProperties`, `SaveSection`, `ResetSectionToDefaults` |
| Localizable text | `editor_toolset.toolsets.string_table.StringTableTools`: `create`, `import_file`, `set_entry`, `get_entry`, `list_keys`, `remove_entry`, `get_table_id`, `get_namespace` |

Data Registries are not covered here.

## Inspect and validate

`hyper_data_inspect`: `targetPaths`, 1 to 32 object paths (`/Game/Data/DT_Items.DT_Items`), `pageSize` up to 128. It
reads loaded assets only and loads nothing: a path that exists but is not loaded is flagged `asset_not_loaded` (load it
with `AssetTools.load_asset`), and a free path reads `asset_state` `does_not_exist`, which is how you prove a new asset's
name is free. Records by `kind`:

- `data_table` (row struct in `typeId`, row `count`) and one `data_table_row` per row name; values come from `get_rows`.
- `curve_table` and `curve_table_row` (key `count`, time range in `numberValue` to `secondaryNumberValue`, default value).
- `user_struct` and `user_struct_field` (`stableId` the field GUID, `name`, `typeId`, `elementFingerprint`).
- `user_enum` and `user_enum_value` (name, integer value), in order.
- `string_table` (table id, namespace) and `data_asset` (class): identity only, so their snapshot is marked incomplete.

The report's `snapshot` holds `persistedFingerprint` (the whole projection, a plan's precondition) and the records.
`hyper_data_validate` takes `{"snapshot": ...}` copied whole from an inspect that fitted in one page (no `nextCursor`):
it re-checks the snapshot's identities and fingerprints and answers `valid` or `invalid` with issues.

## Struct and enum plans

`hyper_data_apply_plan`: `targetPath` the struct or enum, `expectedPersistedFingerprint` from inspect, up to 64
`operations` `{"kind", "subjectId", "expectedElementFingerprint", "name", "valueType", "displayName"}`:

| kind | fields |
|---|---|
| `user_struct.create`, `user_enum.create` | none (the path names it; inspect must show `does_not_exist`) |
| `user_struct.add_field` | `name`, `valueType` bool, int32, float, name, string, text, vector or rotator |
| `user_enum.add_value` | `name`, optional `displayName` |
| `user_struct.rename_field`, `user_enum.rename_value` | `subjectId` (field GUID or value name), `expectedElementFingerprint`, `name` |
| `user_struct.remove_field`, `user_enum.remove_value` | `subjectId`, `expectedElementFingerprint` |

A dry run (`bDryRun: true`) answers `dry_run_valid_zero_effect` with `effects`. Sent with `bDryRun: false`, a fresh
`operationId` and `expectedPlanHash` = its `planHash`, only one thing runs: a plan of exactly one
`user_struct.rename_field` answers `executed_fast_reversible_edit` (one undo step, and it saves the struct). Every other
plan, and every plan in Strict Safety mode, answers `staged_backend_required` with nothing changed: make it with the
Blueprint structure ops above. Renaming a field the game reads by name (a CSV column, `set_rows` keys) breaks those.

```json
{"request": {"bDryRun": true, "targetPath": "/Game/Data/S_ItemRow.S_ItemRow",
  "expectedPersistedFingerprint": "sha256:5b...", "operations": [
  {"kind": "user_struct.rename_field", "subjectId": "0b1c7e2a-...", "expectedElementFingerprint": "sha256:e4...", "name": "BasePrice"}]}}
```

## An item table, end to end

1. `hyper_data_inspect` on `/Game/Data/E_ItemType.E_ItemType`, `S_ItemRow` and `DT_Items`: all `does_not_exist`.
2. One `hyper_blueprint_structure_apply_plan` (`bSave` false): `enum.create` with `values` `["Weapon", "Armor",
   "Potion"]`; `struct.create` with `fields` `DisplayName` (text), `Type` (`enum:/Game/Data/E_ItemType`), `Price`
   (int, default 0), `Weight` (float) and `Icon` (`object:/Script/Engine.Texture2D`), each with a tooltip.
3. `DataTableTools.create` on the struct, `add_rows` with the row names, then `set_rows` with a JSON string mapping each
   row to the column names `get_schema` returns: `{"Sword": {"DisplayName": "Iron Sword", "Price": 120, "Weight": 3.5}}`;
   write an enum column the way `get_rows` shows it. A CSV goes in with `import_file` instead: its first column is the
   row name and the others must match the struct's fields.
4. Inspect all three and validate; read rows back with `get_rows`. Save only when asked (`AssetTools.save_assets`).

## Config settings

Find a section with `ListCategories` (container `Project` or `Editor`) and `ListSections`, read its property names with
`GetSectionSchema`, and record the before values with `GetSectionPropertyValues`. `SetSectionProperties` (a JSON object
of only the properties to change) writes and saves the project's config file, so name the file and the values to the
user and change nothing else. Maps and Modes holds `EditorStartupMap`, `GameDefaultMap` and `GlobalDefaultGameMode`; a
level's World Settings game mode override still wins in that level. Gameplay tags are
`GameplayTagsToolset59.GameplayTagsToolset59` (`ListTags`, `GetTagInfo`, `FindReferencersByTag`, `AddTag`); `RemoveTag` and
`RenameTag` change the whole project, so run them only when the user names the tag.

## Localization basics

Player-facing text is Text, never String or Name: Text fields are gathered for translation. Keep shared UI text in a
String Table (`StringTableTools.create`, then `set_entry` per key) and reference it from a Text property's String Table
pick or `LOCTABLE("<TableId>", "<Key>")` in C++, with the id from `get_table_id`. Gathering, translating and compiling
cultures happens in the editor's Localization Dashboard, which no tool drives: tell the user that step.

## Checking

Inspect again and validate after every change: row counts and names, field names with unchanged GUIDs, enum values in
order. Read values back with `get_rows`, `get_keys`, `get_entry` or `GetSectionPropertyValues`. After a struct or enum
change, list its referencers (`hyper_asset_dependency_graph`, direction referencers) and recompile them with
`hyper_blueprint_compile_validate`; a Switch on a grown enum has new pins to wire.
