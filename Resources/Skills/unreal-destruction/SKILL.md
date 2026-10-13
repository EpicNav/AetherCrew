---
name: unreal-destruction
description: Make things break in Unreal with Chaos destruction - fracture static meshes or level actors into geometry collections (Voronoi, clustered, radial impacts, planar and slice cuts, brick walls, noisy cut faces), cluster the pieces into levels that break in chunks, set damage thresholds per level, collision per piece size, mass, initial state, ground anchors, an inside material, Nanite and proxies, in one undoable plan; then prove it breaks in play with the playtest bot's Damage, Fire and AssertBroken steps. Also when to use Epic's PhysicsAssetToolset, ChaosClothAssetToolset and DataflowAgentToolset for physics assets, cloth and Dataflow graphs.
---

# Destruction with Chaos geometry collections

| work | tool |
|---|---|
| the op kinds with their Params; a geometry collection's levels, pieces, thresholds, collision, materials, mass, anchors and the actors using it | `hyper_destruction_inspect` |
| make a geometry collection and fracture, cluster and tune it, one undo step | `hyper_destruction_apply_plan` |
| prove it breaks in play: Damage or Fire, then AssertBroken | `hyper_verify_run` (copies of the level) or `hyper_playtest_run` (Play In Editor) |

Toolset `HyperAIStudioDestruction.HyperAIStudioDestructionToolset`. The pack needs the Geometry Collection and Fracture
plugins (Chaos Editor enables both); without them its tools are not there.

The plan builds the pieces **in memory** first, as Unreal's Dataflow fracture nodes do, and writes the asset once at
the end, so look at the numbers before anything changes:

1. `bDryRun: true` builds every piece and reports `Leaves` (pieces that never break further), `Clusters`, `Bones`,
   `Levels` (per level: pieces, clusters, leaves, the threshold that breaks it, smallest and largest piece in cm),
   `MassKg`, `Size`/`Min`/`Max`, `Warnings`, and an exploded PNG in `PreviewPath` to open with your image reader.
2. The same plan with `bDryRun: false` makes the asset (unsaved unless `bSave: true`), places its actor, and replies
   with the same numbers plus `Actors` and `VerifyCase`. With approval on it waits in the Activity panel: call
   `hyper_operation_wait` with `bWaitThroughApproval: true`.
3. Run `VerifyCase` with `hyper_verify_run` (it is the whole request): it damages the collection in a copy of the
   level and passes when at least half its pieces broke off; read the AssertBroken detail for what fell. In a World
   Partition level the copies stream only saved actors, so save the level and asset first (the reply's `Notes` say
   so), or run the same steps with `hyper_playtest_run`, which plays unsaved edits.

One Edit > Undo takes back the whole plan: the asset goes, the placed actor goes, replaced actors come back. A plan
saved with `bSave: true` keeps its file on disk; delete the asset in the content browser to remove it.

A mistake is refused before anything is built, naming the op by its number, the param and what it takes. A site
count a little past the most (up to twice it) is fitted to the most and the reply's Notes say so ("Sites 600 ->
500"); far past it is refused.

## A plan

```json
{"Asset": "/Game/Destruction/GC_Wall",
 "Operations": [
  {"Kind": "make", "Sources": ["Wall_01"], "Place": "replace"},
  {"Kind": "fracture.uniform", "Params": "Sites=60; Seed=7"},
  {"Kind": "cluster", "Params": "Clusters=8"},
  {"Kind": "cleanup", "Params": "MinSize=8"},
  {"Kind": "damage", "Params": "Thresholds=250000, 20000; Collision=true"},
  {"Kind": "collision", "Params": "Implicit=convex; Sizes=15:sphere"},
  {"Kind": "mass", "Params": "Density=2400"},
  {"Kind": "anchor", "Params": "Ground=true; Height=10"},
  {"Kind": "material.internal", "Material": "/Game/Materials/M_ConcreteBroken"}]}
```

Values are cm, vectors `"x,y,z"` in the collection's own space (its pivot at 0,0,0). `Params` are
`"Name=Value; Name=Value"`; `hyper_destruction_inspect` with no Asset lists every kind's params with ranges and
defaults.

## Op kinds

- `make` comes first. `Sources` are static meshes by path and level actors by label, in one list. `Place`:
  `replace` (actors only, their default: the collection actor takes their place and they are deleted), `beside`
  (a meter clear of them along Y), `at` (at `Location`) or `none` (the asset only, the default for meshes).
  `SplitIslands=true` makes each mesh's separate parts pieces of their own. Without `make` the plan changes the
  existing asset at `Asset`.
- `fracture.uniform`: Voronoi, `Sites` cells spread evenly, the same `Seed` the same pieces. Each site inside the
  mesh makes one piece: 12 sites in a box make 12 pieces.
- `fracture.clustered`: `Clusters` clumps of `SitesPerCluster` sites within `ClusterRadius` (share of the size): fine
  debris around impact points, big pieces between.
- `fracture.radial`: `Rings` x `AngularSteps` cells about the centre (`Location`, from the middle of the pieces) in
  the plane of `Normal`, out to `Radius`; `Variability` cm makes it look broken rather than drawn. Windows, impacts.
- `fracture.planar`: `Planes` `"px,py,pz,nx,ny,nz | ..."` and/or `Count` random planes. Clean cuts, snapped boards.
- `fracture.slice`: `X`, `Y`, `Z` cuts across each axis, `AngleVariation` and `OffsetVariation` to roughen. Planks,
  tiles, a grid of blocks: X=1; Y=1; Z=1 makes 8.
- `fracture.brick`: `Length`, `Height`, `Depth` and a `Bond` (stretcher, stack, english, header, flemish);
  `Location` shifts the courses. Masonry walls.
- Every cutter takes `Level` (-1, the default, cuts every leaf; a level number cuts the pieces at that level again),
  `Grout` (cm of gap), `Noise` (cm the cut faces move: rough rock and concrete; 0 keeps them flat and light),
  `NoiseFrequency`, `NoiseOctaves`, `NoiseSpacing`, `SplitIslands`, `Seed`.
- `cluster`: groups the pieces under each cluster at `Level` into `Clusters` (or clusters about `Size` cm), adding a
  level: the object breaks into chunks first, the chunks into pieces later. `Connected=true` keeps each chunk's
  pieces touching.
- `cleanup`: pieces under `MinSize` cm (cube root of their volume) are merged into a neighbour (`Mode=merge`) or
  removed (`Mode=remove`, which loses their mass).
- `damage`: `Thresholds` per cluster level, the whole object's first (deeper levels use the last);
  `SizeSpecific=true` uses per-size thresholds instead; `Collision` turns collision damage on or off on the placed
  actors.
- `collision`: `Implicit` (convex, box, sphere, capsule, levelset, none) for the pieces, `Type` volumetric or
  surface, `Margin`, and `Sizes` `"15:sphere, 60:box"` for small pieces.
- `mass`: `Density` kg/m3 (concrete 2400, brick 1900, wood 600, glass 2500) or a total `Mass` kg, and a physical
  material in the op's `Material`.
- `state`: how every piece starts. `State` dynamic falls at once; kinematic or static holds still, and the pieces
  a `Damage` field (or Epic's master field) reaches turn dynamic and fall; sleeping waits for a touch. Chaos gives a
  cluster its pieces' state, so a whole that holds while only hit pieces fall is `anchor`'s job, not this one's.
- `anchor`: `Ground=true` pins the pieces within `Height` cm of the bottom, `Box` those inside a box, `Clear` first
  removes every anchor. Anchored pieces never fall, so a wall keeps its base.
- `material.internal`: the op's `Material` on every face fracturing cut inside, whichever op cut it.
- `rendering`: `Nanite`, and `Renderer=root_proxy` with proxy static meshes in `Sources` (or the original meshes)
  drawn until the first break: cheap while whole.

## Choosing numbers

- Strain thresholds are in Chaos strain units. Unreal's defaults, 500000, 50000, 5000, need a heavy hit; a crate a
  player kicks wants a few thousand; glass a few hundred. The bot's `Damage` applies twice the highest threshold
  unless told otherwise.
- 20 to 80 pieces per object reads well and simulates cheaply; past 1000 warns. Cluster big fractures so they break
  in chunks first. Pieces under 5 cm jitter and cost: `cleanup MinSize=5`.
- Convex collision is right for most debris; spheres or boxes for many small pieces (`Sizes`).
- A wall or pillar standing in the level wants `anchor Ground=true`; otherwise it falls as one body the moment play
  starts. Anchored, it stands, and what a hit breaks off falls. `state kinematic` holds the whole of it instead, until
  a Damage field frees pieces; collisions then break clusters but leave the pieces where they are. A prop meant to
  tumble when hit stays dynamic.

## Proving it breaks

`VerifyCase` is the usual proof. Write your own with the playtest bot's steps:

```json
{"cases": [{"name": "wall-breaks", "timeoutSeconds": 12, "steps": [
  {"action": "Wait", "seconds": 0.5},
  {"action": "Damage", "target": "GC_Wall", "radius": 150},
  {"action": "WaitUntil", "until": "AssertBroken", "target": "GC_Wall", "compare": ">=", "value": "20", "timeoutSeconds": 5},
  {"action": "Wait", "seconds": 1.5},
  {"action": "AssertBroken", "target": "GC_Wall", "compare": ">=", "value": "20"}]}]}
```

- `Damage`: a strain field at `location` (the target's middle when not given) reaching `radius` cm; `value` sets the
  strain; `seconds` keeps it on.
- `Fire`: a 40 cm, 200 kg test ball from the player's view at `value` cm/s (3000), or dropped from `range` cm
  above the target: collision damage, as a thrown object or a falling rock would do.
- `AssertBroken`: counts the pieces broken off the target's geometry collections (every one when target is empty)
  against `compare` and `value`; its detail counts the clusters broken and names the pieces that fell, how far.

A collection that breaks in the copy but not in your game usually lost to the game's own settings: the actor's
damage from collisions off, a threshold raised on the actor, or anchors.

## Physics assets, cloth and Dataflow graphs: Epic's toolsets

HyperAI adds no tools for these; Epic's are there whenever their plugins are on. Use them directly:

| work | Epic's toolset (plugin) | functions |
|---|---|---|
| ragdoll and character physics assets | `PhysicsToolsets.PhysicsAssetToolset` (Physics Toolsets) | CreateFromMesh, GetBodyNames, GetBodyShapes, SetSphere, SetCapsule, SetBox, RemoveShape, AddBody, RemoveBody, Get/SetBodyPhysicsMode, Get/SetBodyMassScale, GetConstraints, AddConstraint, SetConstraintLimits, RemoveConstraint |
| clothing on skeletal meshes | `ChaosClothAssetToolset.ChaosClothAssetToolset` | CreateClothingAsset, AssignClothingToSection, RemoveClothingFromSection, ListClothingAssets, GetSectionClothing, ConvertClothingAssetCommonToChaosClothAsset |
| Dataflow graphs (cloth, flesh, geometry collection graphs) | `DataflowAgent.DataflowAgentToolset` (Dataflow Agent) | CreateGraph, GetGraphStructure, ListNodeTypes, GetNodeTypeSchema, AddNode, UpdateNode, GetNodeInfo, RepositionNode, RemoveNode, ConnectNodePins, DisconnectNodePins, List/Add/Remove/SetVariable, Add/RemoveCommentBox, ListDataflowCompatibleAssetTypes, CreateDataflowCompatibleAsset(FromTemplate), ListDataflowTemplatesForAssetClass, AssignDataflowTemplate |

- A ragdoll: `PhysicsAssetToolset.CreateFromMesh`, then shape and constraint fixes with its setters; read it back and
  validate it with HyperAI's `hyper_physics_inspect` and `hyper_physics_validate` (physics_chaos pack), which also
  preflight solver and collision changes with `hyper_physics_apply_plan`.
- A geometry collection someone wants driven by a Dataflow graph (a procedural fracture that re-runs when the source
  mesh changes): `DataflowAgentToolset.ListDataflowTemplatesForAssetClass("GeometryCollection")`, then
  `CreateDataflowCompatibleAssetFromTemplate` or `AssignDataflowTemplate`, and edit the graph's nodes
  (`ListNodeTypes`, `AddNode`, `ConnectNodePins`). For a one-off fracture, `hyper_destruction_apply_plan` is quicker and
  undoable in one step; `hyper_destruction_inspect` reads either kind.
- Cloth: `ChaosClothAssetToolset` for clothing on a skeletal mesh, `DataflowAgentToolset` for a Chaos Cloth Asset's
  graph.

## Limits

500 Voronoi sites per op (520 is fitted to 500, 1001 refused), 2000 pieces per collection and per brick or slice
op, 32 ops, 32 sources, 300000 source triangles. Assets go under `/Game` only. Plans are refused during Play In
Editor.
