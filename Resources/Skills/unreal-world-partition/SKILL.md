---
name: unreal-world-partition
description: Set up streaming in a World Partition level with HyperAI's World Partition tools - runtime and editor data layers (made, parented, given a start state, filled and emptied), actors streamed by distance or kept always loaded with their runtime grid and HLOD layer, HLOD layers made and tuned, runtime grid cell size and loading range, and editor regions loaded so far-off actors can be edited - then read it all back and build HLODs. Use for open worlds, level streaming, data layers, HLODs, loading ranges, editor loading regions, streaming sources and One File Per Actor questions.
---

# Streaming a World Partition level

HyperAI's World Partition tools are in toolset `HyperAIStudioWorldPartition.HyperAIStudioWorldPartitionToolset`,
called through `call_tool` with `{"request": {...}}`. `hyper_world_partition_inspect` reads the open level;
`hyper_world_partition_apply_plan` changes it in one plan of up to 64 ops (at most 1000 actors per op). Every op is
checked before anything changes, and a mistake is refused with the names that do exist. The edits are one undo step
(Edit > Undo takes the whole plan back). The level stays unsaved unless `bSave: true`; new data layer and HLOD layer
assets are always saved. `bDryRun: true` only checks. With approval on, the reply is `awaiting_user_approval`: call
`hyper_operation_wait` with its `OperationId` and `bWaitThroughApproval: true`. The level must use World Partition.

Units: `Center` is cm; a region's `Size` is meters; `Settings` are Unreal's own properties in Unreal's text form and cm
(`CellSize 25600`, `LoadingRange 51200`).

## Read first

`{"request": {"bActors": true, "bSettings": true}}` returns:

- `bStreaming`, `RuntimeHash` (spatial hash with grids, or hash set with runtime partitions), `Grids` (name, cell size,
  loading range, HLOD layer), `DefaultHLODLayer`.
- `DataLayers`: `Name`, `Asset`, `Type` (Runtime or Editor), `InitialRuntimeState`, `bVisible`, `bLoadedInEditor`,
  `Parent`, and `LoadedActors` against `Actors` in the whole level.
- `HLODLayers` in use: `LayerType`, `bSpatiallyLoaded`, `CellSize`, `LoadingRange`, `Parent`, `Actors`, and with
  `bSettings` every setting.
- `Actors`, `LoadedActors`, `AlwaysLoaded` (with `bActors`, the first 50 `AlwaysLoadedActors`), `HLODActors` built,
  `Regions` loaded in the editor, and `BuildHLODs`: the editor menu and the commandlet line that build HLODs.

## Ops

Each op is `{"Kind", ...}`. Actors are named by label; a label two loaded actors share is refused.

| Kind | fields |
|---|---|
| `data_layer.create` | `DataLayer` (letters, digits, `_`); `Asset` (default `/Game/DataLayers/<DataLayer>`; a layer is named after its asset); `Type` Runtime (default) or Editor; `Parent` (same type); `InitialRuntimeState` Unloaded, Loaded or Activated (runtime only); `Visible`, `LoadedInEditor` true or false |
| `data_layer.set` | `DataLayer` and at least one of `Parent` (or none), `InitialRuntimeState`, `Visible`, `LoadedInEditor` |
| `data_layer.delete` | `DataLayer`; its actors stay in the level |
| `data_layer.add_actors`, `data_layer.remove_actors` | `DataLayer`, `Actors` |
| `actors.set` | `Actors` and at least one of `SpatiallyLoaded` (true streams by distance, false is always loaded), `RuntimeGrid` (a grid name, or none for the default), `HLODLayer` (an asset path, or none) |
| `hlod_layer.create` | `Asset` (a new `/Game/Folder/HLOD_Name`), `Settings`: `LayerType` (Instancing, MeshMerge, MeshSimplify, MeshApproximate), `bIsSpatiallyLoaded`, `CellSize`, `LoadingRange`, `ParentLayer`; `BuilderSettings` for the builder its type uses |
| `hlod_layer.set` | `Asset` (an existing layer) and `Settings` or `BuilderSettings` |
| `world_partition.set` | `HLODLayer`: the level's default HLOD layer, or none |
| `world_partition.set_grid` | `Grid` (by name; the first when empty), `Settings`: `CellSize`, `LoadingRange`, `bBlockOnSlowStreaming`, `Priority` |
| `region.load` | `Center` (cm), `Size` (meters across), `Label` |
| `region.unload` | `Label`, or empty for every region this tool loaded |

- Only loaded actors can be edited. An actor outside the loaded area is refused by name unless the same plan loads a
  region: regions load before the plan's edits and unload after them, outside the undo step.
- A wrong setting name is refused with the names the type has; a wrong value with the property's type and its value now.
- Some actors cannot change whether they are spatially loaded (the reply names the actor and its class).

## A worked plan

A town 300 m across: its interiors on an editor layer for organizing, a festival on a runtime layer that starts
unloaded, the quest manager always loaded, buildings standing in as merged meshes from afar, and the default grid
loading a little further for horses:

```json
{"request": {"Operations": [
 {"Kind": "region.load", "Center": {"X": 52000, "Y": -18000, "Z": 0}, "Size": {"X": 300, "Y": 300}, "Label": "Town"},
 {"Kind": "data_layer.create", "DataLayer": "Town_Interiors", "Type": "Editor"},
 {"Kind": "data_layer.create", "DataLayer": "Town_Festival", "InitialRuntimeState": "Unloaded"},
 {"Kind": "data_layer.add_actors", "DataLayer": "Town_Festival", "Actors": ["Festival_Stage", "Festival_Lanterns"]},
 {"Kind": "actors.set", "Actors": ["QuestManager_Town"], "SpatiallyLoaded": "false"},
 {"Kind": "hlod_layer.create", "Asset": "/Game/HLOD/HLOD_Buildings",
  "Settings": [{"Name": "LayerType", "Value": "MeshMerge"}]},
 {"Kind": "actors.set", "Actors": ["Town_Hall", "Town_Inn"], "HLODLayer": "/Game/HLOD/HLOD_Buildings"},
 {"Kind": "world_partition.set_grid", "Settings": [{"Name": "LoadingRange", "Value": "38400"}]},
 {"Kind": "region.unload", "Label": "Town"}],
 "bDryRun": true, "bSave": false, "OperationId": "town-streaming"}}
```

Read the dry run's `Diagnostic`, then send it again with `bDryRun: false`. `Notes` say what each op did, `Saved` lists
what was written, and `Applied` counts the ops. On `operation_failed` the ops before the failing one stay applied,
unsaved; undo takes them back.

## Choosing

- Runtime data layers hold what comes and goes with game state: a destroyed camp, a festival, a quest's props. Editor
  data layers only organize the editor and do nothing in game.
- Keep always loaded only what must exist everywhere: managers, level scripting, actors referenced from far away.
  Everything else streams; always-loaded actors cost memory across the whole world.
- HLOD: Instancing for foliage and repeated props, MeshMerge or MeshSimplify for buildings, MeshApproximate for large
  far shapes. Leave grid `CellSize` and `LoadingRange` alone without a reason such as a fast vehicle.

## What the tools do not do

- **Build HLODs.** HLOD layers set up here have no HLOD actors until built: inspect's `BuildHLODs` gives the editor
  menu (Build > Build HLODs, which saves, closes and reopens the level) and the commandlet. Ask the user before either.
- **Switch runtime data layers during play.** `InitialRuntimeState` is all a plan sets. The game changes a layer with
  the Data Layer Manager's `SetDataLayerRuntimeState` (by data layer asset) or `SetDataLayerInstanceRuntimeState`, in
  Blueprint or C++ (the unreal-blueprint skill).
- **Streaming sources.** The player controller streams the world around the player by default. Anything else that
  should stream the world in (a cinematic camera, a teleport target) needs a `WorldPartitionStreamingSourceComponent`
  on an actor, added as Blueprint or C++ work; nothing here adds one.
- **One File Per Actor and conversion.** A World Partition level saves each actor in its own package (One File Per
  Actor), so `bSave` writes the level and the changed actors' files. Converting a non-World Partition level (Tools >
  Convert Level, or the WorldPartitionConvertCommandlet) is outside these tools: ask the user to run it.
- `hyper_worldbuilding_apply_plan` (`HyperAIStudioWorldbuilding.HyperAIStudioWorldbuildingToolset`) also names
  `world_partition.*`, `data_layer.set_state` and `hlod.build` variants, but it only checks plans: a real run answers
  `staged_backend_required`. Make every World Partition change with `hyper_world_partition_apply_plan`.

## Checking

Run `hyper_world_partition_inspect` again: each data layer's type, start state and `Actors` count, the actors' new
`AlwaysLoaded` total, the HLOD layers with their actor counts, the grid's new loading range, and `Regions` empty once
unloaded. After an HLOD build, `HLODActors` is above 0. For landscapes, foliage and PCG in the same level, see the
unreal-level-design and unreal-pcg skills.
