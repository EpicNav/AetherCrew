---
name: unreal-level-design
description: Build and dress levels with HyperAI's world tools - landscapes from heightmaps or noise, sculpted and painted by height and slope, with roads laid as splines; foliage types scattered by density, slope, height and paint layer, placed at points, thinned and erased; World Partition data layers, streaming, HLOD layers, runtime grids and editor regions; blockouts; and checks against player metrics and Map Check. Use for terrain, open worlds, set dressing and level layout.
---

# Levels: terrain, roads, foliage and streaming

| work | read | edit |
|---|---|---|
| landscapes: make, sculpt, paint, edit layers, roads | `hyper_landscape_inspect` | `hyper_landscape_apply_plan` |
| foliage: types, scatter, place, thin, erase | `hyper_foliage_inspect` | `hyper_foliage_apply_plan` |
| World Partition: data layers, streaming, HLOD layers, grids, editor regions | `hyper_world_partition_inspect` | `hyper_world_partition_apply_plan` |
| blockouts: blocks, ramps, stairs, markers | `hyper_level_design_inspect` | `hyper_level_blockout_apply_plan` |
| player metrics, routes, sightlines, Map Check | | `hyper_level_design_validate` |

Each plan is checked before anything changes. A mistake is refused with the names that do exist, so fix the op and
send it again. The plan is applied as one undo step and leaves the level unsaved unless `bSave: true`. New assets
(layer infos, foliage types, data layers, HLOD layers) are always saved. `bDryRun: true` only checks. With approval
on, a plan waits in the Activity panel: call `hyper_operation_wait` with its `OperationId` and
`bWaitThroughApproval: true`.

Units: `Location`, `Center` and `Points` are cm, as Unreal places things. Sizes, radii, widths and heights the tools
name themselves are meters. `Settings` are Unreal's own properties, in Unreal's units (cm) and text form.

## Order of work

1. **Ground.** Make or shape the landscape, then paint it.
2. **Roads.** Lay roads and paths through the landscape, so the ground under them is leveled and painted.
3. **Foliage.** Scatter it last, so it reads the final ground, slopes and paint.
4. **Streaming.** In a World Partition level, set up data layers, always-loaded actors and HLOD layers.
5. **Check.** Run `hyper_level_design_validate`, and look at the level from where the player stands.

## Ground and roads

A valley: rolling noise, a flat camp site, paint by slope and height, and a dirt road through the camp.

```json
{"Operations": [
 {"Kind": "landscape.create", "Landscape": "Valley", "Size": 1008, "Resolution": 100, "HeightRange": 512,
  "Noise": {"Seed": 7, "Amplitude": 80, "Wavelength": 600, "Octaves": 6},
  "Material": "/Game/Landscape/M_Landscape", "PaintLayers": ["Grass", "Dirt", "Rock", "Snow"]},
 {"Kind": "landscape.sculpt", "Landscape": "Valley", "Mode": "flatten", "Center": {"X": 0, "Y": 0, "Z": 0},
  "Radius": 30, "Falloff": 15, "Height": 20},
 {"Kind": "landscape.paint", "Landscape": "Valley",
  "Rules": [{"Layer": "Grass"}, {"Layer": "Rock", "SlopeMin": 32}, {"Layer": "Snow", "HeightMin": 150}]},
 {"Kind": "landscape.road", "Landscape": "Valley", "Road": "ValleyRoad", "Width": 6, "Falloff": 8, "Layer": "Dirt",
  "Points": [{"X": -40000, "Y": -20000, "Z": 0}, {"X": 0, "Y": 0, "Z": 0}, {"X": 30000, "Y": 25000, "Z": 0}]}],
 "bDryRun": false, "OperationId": "valley-ground"}
```

- The material's layer blend must name the paint layers. Without it the landscape is grey, however it is painted.
- Rules later in the list win where they overlap. `Breakup` (0 to 1) roughens band edges so they do not read as lines.
- Sculpt modes are raise, lower, flatten, smooth, noise, terrace, and a ramp from `Center` to `To`. An edit layer
  (`landscape.add_edit_layer`, then `EditLayer` on sculpt and paint) keeps a change separate and easy to remove.
- A road follows the ground through its points and levels it across its width. A `Mesh` lays a road surface along
  it. Keep grades walkable: under 30 degrees, per the level designer's bar.

## Foliage

Pines on the grass that stay off the road, rocks on slopes, the camp kept clear, and one hero tree.

```json
{"Operations": [
 {"Kind": "foliage_type.create", "Types": ["/Game/Foliage/FT_Pine"], "Mesh": "/Game/Env/SM_Pine", "Settings": [
   {"Name": "Density", "Value": "1.5"}, {"Name": "Radius", "Value": "400"}, {"Name": "ScaleX", "Value": "(Min=0.8,Max=1.3)"},
   {"Name": "AlignToNormal", "Value": "false"}, {"Name": "GroundSlopeAngle", "Value": "(Min=0,Max=30)"},
   {"Name": "LandscapeLayers", "Value": "(\"Grass\")"}, {"Name": "ExclusionLandscapeLayers", "Value": "(\"Dirt\")"}]},
 {"Kind": "foliage_type.create", "Types": ["/Game/Foliage/FT_Rock"], "Mesh": "/Game/Env/SM_Rock", "Settings": [
   {"Name": "Density", "Value": "2"}, {"Name": "AlignMaxAngle", "Value": "25"}, {"Name": "GroundSlopeAngle", "Value": "(Min=15,Max=60)"},
   {"Name": "CullDistance", "Value": "(Min=6000,Max=8000)"}]},
 {"Kind": "foliage.scatter", "Types": ["FT_Pine", "FT_Rock"], "Landscape": "Valley",
  "Clearings": [{"Center": {"X": 0, "Y": 0, "Z": 0}, "Radius": 35}]},
 {"Kind": "foliage.place", "Types": ["FT_Pine"], "Points": [{"X": 1800, "Y": -900, "Z": 0}]}],
 "bDryRun": false, "OperationId": "valley-foliage"}
```

- `Density` counts instances per 100 m2. Forest trees run about 1 to 3, bushes 5 to 20, and scattered rocks 1 to 5.
  `Radius` keeps a type's instances apart, in cm.
- Trees stand upright (`AlignToNormal` false). Rocks and ground cover follow the slope, within `AlignMaxAngle`.
- Scatter stands on the landscape unless `Surfaces` adds `static_mesh` or `bsp`. It keeps off ground with something
  above it, such as a roof, a bridge or a road mesh. Scattering the same region again tops it up to its density and
  never doubles it.
- Each scatter's note says how many spots it tried and why the rest were turned away (too steep, off its layers,
  crowded, in a clearing). Read it and adjust before scattering more.
- `foliage.erase` with `Fraction` 0.5 thins a region. `foliage_type.remove` takes a type out of the level.

## World Partition

Inspect first: `hyper_world_partition_inspect` shows the runtime grids, data layers with their actors, HLOD layers,
always-loaded actors and loaded editor regions.

```json
{"Operations": [
 {"Kind": "region.load", "Center": {"X": 0, "Y": 0, "Z": 0}, "Size": {"X": 200, "Y": 200}, "Label": "Camp"},
 {"Kind": "data_layer.create", "DataLayer": "Camp_Destroyed", "InitialRuntimeState": "Unloaded"},
 {"Kind": "data_layer.add_actors", "DataLayer": "Camp_Destroyed", "Actors": ["Camp_Ruins", "Camp_Embers"]},
 {"Kind": "actors.set", "Actors": ["QuestDirector"], "SpatiallyLoaded": "false"},
 {"Kind": "hlod_layer.create", "Asset": "/Game/HLOD/HLOD_Foliage", "Settings": [{"Name": "LayerType", "Value": "Instancing"}]},
 {"Kind": "world_partition.set", "HLODLayer": "/Game/HLOD/HLOD_Foliage"}],
 "bDryRun": false, "OperationId": "valley-streaming"}
```

- Runtime data layers hold content that comes and goes with game state: a destroyed camp, a quest's props, a
  season. The game activates them. Editor data layers only organize work.
- Keep managers, level scripting and anything referenced from far away always loaded (`SpatiallyLoaded` false).
  Keep everything else spatially loaded: always-loaded actors cost memory everywhere.
- HLOD layers stand in for distant content. Use Instancing for foliage and props, and MeshMerge or MeshApproximate
  for buildings. HLODs are built outside the plan: inspect's `BuildHLODs` gives the editor menu and the command.
- Actors outside the loaded regions cannot be edited. `region.load` loads an area first, and `region.unload` frees
  it afterwards. Regions load before a plan's edits and unload after them.
- `world_partition.set_grid` changes a grid's `CellSize` and `LoadingRange` (cm). Change them only with a reason,
  such as a fast vehicle that needs a longer range.

## Checking

`hyper_level_design_validate` runs Map Check and plays the layout against the player's metrics: markers on walkable
ground, navmesh routes from starts to goals, corridor widths, sightlines and cover. Fix what it reports, then run it
again.
