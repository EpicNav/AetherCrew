---
name: unreal-level-design
description: Build and dress levels with HyperAI's world tools - landscapes from heightmaps or noise, sculpted and painted by height, slope, distance from roads and in patches, with roads laid as splines; foliage types scattered by density, slope, height and paint layer, placed at points, thinned and erased; procedural PCG scatter and ecosystems over large areas (biomes on paint layers, species layered big to small, stands and clumps), retuned and regenerated, and any PCG graph read back as recipes; World Partition data layers, streaming, HLOD layers, runtime grids and editor regions; blockouts; props placed, dropped to the ground and moved to clear spots, spatial questions and rendering cost; and checks against player metrics and Map Check. Use for terrain, open worlds, set dressing and level layout.
---

# Levels: terrain, roads, foliage and streaming

| work | read | edit |
|---|---|---|
| landscapes: make, sculpt, paint, edit layers, roads | `hyper_landscape_inspect` | `hyper_landscape_apply_plan` |
| foliage: types, scatter, place, thin, erase | `hyper_foliage_inspect` | `hyper_foliage_apply_plan` |
| procedural scatter and ecosystems: PCG graphs and volumes | `hyper_pcg_scatter_inspect` | `hyper_pcg_scatter_apply_plan` |
| World Partition: data layers, streaming, HLOD layers, grids, editor regions | `hyper_world_partition_inspect` | `hyper_world_partition_apply_plan` |
| blockouts: blocks, ramps, stairs, markers | `hyper_level_design_inspect` | `hyper_level_blockout_apply_plan` |
| placing props: place meshes, drop to the ground, move to clear spots | `hyper_spatial_query` | `hyper_spatial_apply_plan` |
| rendering cost: triangles, draw calls, instances, shadows, lights | `hyper_scene_cost_report` | |
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
2. **Roads.** Lay roads and paths through the landscape, so the ground under them is leveled and painted. Paint
   anything that follows the roads (verges, riverbanks) once they are laid.
3. **Foliage and scatter.** Scatter last, so it reads the final ground, slopes and paint. Dress areas by hand with
   foliage, and cover large areas you will retune with a PCG ecosystem (or a one-layer PCG scatter).
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
  With no such material yet, `landscape.create_material` makes one: `{"Kind": "landscape.create_material",
  "Landscape": "Valley", "Material": "/Game/Landscape/M_ValleyColors"}` shows each of the landscape's paint layers
  in a flat color and sets it on the landscape. `PaintLayers` names other layers, `LayerColors` gives their colors
  in order (R, G, B 0 to 1; picked from the name otherwise: grass green, rock grey, snow white), and without
  `Landscape` a later `landscape.create` or `landscape.set_material` in the same plan can take it.
- Rules later in the list win where they overlap. `Breakup` (0 to 1) roughens band edges so they do not read as lines.
- Paint biomes as a mosaic, not bands: `Patch` (meters across) and `Coverage` (the share of the ground) lay a layer in
  patches, such as forest in 150 m patches over 60% of the gentle ground with meadow between. `DistanceMax` keeps a
  layer within that many meters of the roads and other landscape splines (a verge, a riverbank), and `DistanceMin`
  that far away. A PCG ecosystem then grows a biome on each layer.
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

## Procedural scatter and ecosystems

PCG follows rules over a whole area: change a rule and regenerate, rather than editing instances. A one-layer scatter
puts one set of meshes on the ground. Rocks over the valley, two meshes weighted 3 to 1, kept off slopes past 35
degrees:

```json
{"Operations": [
 {"Kind": "graph.create", "Graph": "/Game/PCG/PCG_ValleyRocks",
  "Meshes": [{"Mesh": "/Game/Env/SM_Rock_A", "Weight": 3}, {"Mesh": "/Game/Env/SM_Rock_B", "Weight": 1}],
  "Density": 0.05, "Spacing": 4, "SlopeMax": 35, "Scale": {"X": 0.7, "Y": 1.4}, "RandomYaw": "true", "Seed": 11},
 {"Kind": "volume.create", "Graph": "/Game/PCG/PCG_ValleyRocks", "Volume": "ValleyRocks",
  "Center": {"X": 0, "Y": 0, "Z": 0}, "Size": {"X": 400, "Y": 400}, "Height": 600}],
 "bDryRun": false, "OperationId": "valley-rocks"}
```

- `Density` is points per square meter, where foliage counts per 100 m2: 0.02 for sparse trees, 0.05 to 0.5 for
  rocks, 4 for grass. `Spacing` keeps instances that many meters apart. `SlopeMax` keeps them off steeper ground,
  and 0 allows any. `Scale` is a uniform range, Min (X) to Max (Y). New graphs stand instances upright;
  `AlignToGround: "true"` leans them with the slope, for rocks and debris.

An ecosystem is a whole natural environment in one graph: biomes on landscape paint layers, each with its species
listed big to small, and every species keeping out of the room the ones before it take. A forest and an alpine
meadow over the valley, off the road and clear of tagged buildings, the volume covering the whole landscape:

```json
{"Operations": [
 {"Kind": "ecosystem.create", "Graph": "/Game/PCG/PCG_Valley",
  "AvoidLayers": ["Dirt"], "AvoidTags": ["Building"], "AvoidMargin": 3, "RoadMargin": 3,
  "Biomes": [
   {"Name": "Forest", "Layer": "Grass", "SlopeMax": 30, "Height": {"X": 0, "Y": 140},
    "Species": [
     {"Name": "Pines", "Meshes": [{"Mesh": "/Game/Env/SM_Pine_A", "Weight": 3}, {"Mesh": "/Game/Env/SM_Pine_B"}],
      "Density": 0.02, "Footprint": 4, "Clearance": 1.25, "Patch": 80, "Coverage": 0.7, "CullDistance": 400},
     {"Name": "Bushes", "Meshes": [{"Mesh": "/Game/Env/SM_Bush"}], "Density": 0.01, "Footprint": 1.5,
      "Cluster": 5, "ClusterRadius": 3, "CullDistance": 120},
     {"Name": "Rocks", "Meshes": [{"Mesh": "/Game/Env/SM_Rock"}], "Density": 0.03, "Footprint": 1.5,
      "Scale": {"X": 0.5, "Y": 1.5}, "bAlignToGround": true, "Tilt": 10, "Sink": 20, "CullDistance": 150},
     {"Name": "Ferns", "Meshes": [{"Mesh": "/Game/Env/SM_Fern"}], "Density": 1, "Footprint": 0.6,
      "Patch": 15, "Coverage": 0.5, "KeepOutOf": ["Rocks"], "CullDistance": 60}]},
   {"Name": "Meadow", "Layer": "Grass", "Height": {"X": 140, "Y": 400},
    "Species": [{"Name": "Grass", "Meshes": [{"Mesh": "/Game/Env/SM_Grass"}], "Density": 3, "Footprint": 0.4,
      "CullDistance": 50}]}]},
 {"Kind": "volume.create", "Graph": "/Game/PCG/PCG_Valley", "Volume": "ValleyLife", "Cover": "Valley",
  "Partitioned": "true"}],
 "bDryRun": false, "OperationId": "valley-life"}
```

- List species big to small: canopy trees, understory, bushes, rocks, debris, ground cover. Each keeps out of the
  room (`Footprint` x `Clearance`) of every species before it in its biome; `KeepOutOf` names only some (the ferns
  above grow under the pines but not on rocks), and `["none"]` none.
- Starting points, per square meter and meters. With `Cluster`, `Density` counts clumps.

  | species | Density | Footprint | CullDistance |
  |---|---|---|---|
  | canopy trees | 0.005 to 0.03 | 4 to 8 | 300 to 600, or 0 with HLOD |
  | understory trees | 0.01 to 0.05 | 2 to 4 | 200 to 400 |
  | bushes | 0.1 to 0.5, clumps 0.005 to 0.02 | 1 to 2 | 80 to 150 |
  | rocks | 0.02 to 0.2 | 0.5 to 3 | 100 to 250 |
  | debris, branches | 0.1 to 1 | 0.3 to 1 | 30 to 60 |
  | grass, ground cover | 1 to 5 | 0.2 to 0.6 | 30 to 80 |

- `Patch` and `Coverage` grow a species in stands with clearings between: stands about `Patch` meters across over
  `Coverage` of the biome. Trees take 50 to 200 m stands, ferns and flowers 10 to 30 m; without them a species spreads
  evenly. `Cluster` stamps clumps of that many over `ClusterRadius` meters.
- Trees and bushes stand upright on slopes; `bAlignToGround` leans rocks and debris with the ground. `Tilt` adds a
  random lean in degrees, and `Sink` pushes bases into the ground in cm so they do not float.
- `Height` bands (meters above zero) and `SlopeMax` place biomes by elevation: the treeline, scree on the heights,
  reeds just above the water. `LayerMin` (0.5) is how much paint a biome needs.
- `RoadMargin` keeps every species that many meters from the paved edge of the landscape's roads (all its landscape
  splines), measured to each footprint: 2 to 5 keeps tree crowns, which reach past their footprint, off a road.
  `AvoidLayers` keeps everything off road and riverbank layers too. `AvoidTags` keeps `AvoidMargin` meters clear
  around tagged actors, such as houses and camps.
- The same names and `Seed` grow the same way every time; change `Seed` for another arrangement.
- `Cover` sizes a volume to a landscape. `Partitioned: "true"` generates in World Partition cells that stream with
  the world: use it in a World Partition level more than a few hundred meters across. Species that later ones keep
  out of, and clumps, are sampled a few meters past each cell, so species keep out of each other across its edge;
  the plan's note says how far (at most 128 m).

Reading and changing:

- `hyper_pcg_scatter_inspect` with a `Graph` reads any PCG graph, a marketplace generator as well as yours: one recipe
  per mesh spawner, the nodes its points pass through with the settings that differ from defaults, its meshes and
  cull distances. `Detail: "nodes"` adds every node, `"full"` every edge too, and `Filter` narrows to a name, title or
  mesh. Read a generator before you copy its method.
- For an ecosystem, `Ecosystem` in that result is the op that builds it, as `ecosystem.set` takes it. Change what you
  need and send it back whole, with `volume.generate`: `ecosystem.set` rebuilds the graph from the biomes it is given.
- The volume generates after the plan, as a job: call `hyper_operation_wait` with the `OperationId`. Its result counts
  the instances of each mesh, and says why when none grew.
- To change a scatter, send `graph.set` (or `ecosystem.set`) and `volume.generate` in one plan. Every volume running
  the graph follows the new rules when it regenerates. `volume.clear` removes a volume's instances.
- A volume grows on the landscape inside it, so make it tall enough to hold the ground, or `Cover` the landscape.
  Landscapes made with `hyper_landscape_apply_plan` are found at once. In a World Partition level, PCG finds a
  landscape kept whole (not split into streaming proxies) only once the level is saved, and `bSave: true` saves it
  before generating.
- Foliage does what PCG here does not: keeping out from under roofs, and hero pieces placed at points.

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

## Placing props

Ask before placing: `hyper_spatial_query` answers up to 32 questions in one call: traces and sweeps, overlaps, the
ground under a point (height and slope), a clear spot of a given size near a point, the distance and gap between two
actors, and the nearest actors by class, tag or label.

```json
{"Queries": [
 {"Kind": "ground", "Point": {"X": 1200, "Y": -300, "Z": 0}},
 {"Kind": "clear_spot", "Point": {"X": 1200, "Y": -300, "Z": 0}, "Extent": {"X": 60, "Y": 60, "Z": 50}, "MaxSlope": 20},
 {"Kind": "nearest", "Point": {"X": 1200, "Y": -300, "Z": 0}, "TagFilter": "Cover", "MaxResults": 4}]}
```

Then place and settle props in one undo step with `hyper_spatial_apply_plan`:

```json
{"Operations": [
 {"Kind": "place", "Mesh": "/Game/Props/SM_Crate", "Label": "Camp_Crate_01", "Folder": "Camp/Props",
  "Location": {"X": 1200, "Y": -300, "Z": 500}, "Rotation": {"Pitch": 0, "Yaw": 35, "Roll": 0}, "bDropToGround": true},
 {"Kind": "drop_to_ground", "Actors": ["Camp_Rock_01", "Camp_Rock_02"], "bAlignToNormal": true, "Sink": 5},
 {"Kind": "move_to_clear_spot", "Actors": ["Camp_Barrel"], "Clearance": 20}],
 "bDryRun": true}
```

- A dry run returns `Moves`: where each actor would go. Send the same plan with `bDryRun: false` to apply it.
- Ground traces pass through foliage and PCG instances, so a rock never lands on a tree (`bIncludeFoliage: true` makes
  them ground), and through the plan's other actors, so a plan never stacks its own actors on each other.
- Name actors by label or path. A label several actors share, a locked or attached actor (move its parent) and one in
  an unloaded region are refused by name. Every op is computed against the level as it was before the plan, and a
  clear spot keeps off where the plan places and drops things.
- A prop's bounds are what it shows: a pickup's overlap sphere or trigger box does not lift it off the ground.

`hyper_scene_cost_report` says what the loaded level, a box or named actors cost to render: triangles with and without
Nanite, estimated draw and shadow draws, instances per mesh, lights, translucent materials, the heaviest actors and
meshes, and findings such as heavy meshes drawn without Nanite. Draw calls are estimates; `MeasuredLastFrameDrawCalls`
is what the last rendered frame drew. In World Partition it covers loaded actors only and says how many are not.

## Checking

`hyper_level_design_validate` runs Map Check and plays the layout against the player's metrics: markers on walkable
ground, navmesh routes from starts to goals, corridor widths, sightlines and cover. Fix what it reports, then run it
again.
