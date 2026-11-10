---
name: unreal-modeling
description: Model 3D props in Unreal with HyperAI's Geometry Script tools, free and offline - rocks, boulders, standing stones, pillars, arches, ruined walls, towers, crystal clusters, mushrooms, tree trunks, trees, bushes, grass tufts from recipes, or any shape from primitives, revolves, sweeps, booleans, plane cuts, mirrors, extrude, inset, offset, bevel, Catmull-Clark subdivision, noise, bend/twist/flare and ripple warps, remeshing and UVs - written as static meshes with collision and vertex-colour tints. Use for placeholder and stylised props, kitbash parts and set dressing, and to hand a mesh to a person in Modeling Mode to edit by hand, then read the change back. Also checks, unwraps (xatlas, PatchBuilder, projections, lightmap UVs) and bakes textures (normal, AO, curvature, colour carried across) for any static mesh, generated and imported ones too; not for making paid textured models (hyper_mesh_generate) or terrain.
---

# Modeling props with Geometry Script

| work | tool |
|---|---|
| op kinds with their fields and Params, recipes, limits; the static meshes in a folder; one mesh in full | `hyper_model_inspect` |
| build meshes from ops and recipes, then write them as static meshes | `hyper_model_apply_plan` |
| check written meshes as props: triangles, size, pivot, open edges, UVs, slots, collision | `hyper_model_validate` |
| hand a mesh to a person in Modeling Mode, selected and framed; read the edit back | `hyper_model_handoff` |
| check any static mesh's UV channel: overlap, flips, islands, texel density, lightmap texels; a layout PNG | `hyper_model_uv_check` |
| lay a channel out again (xatlas, patch, projections, repack, lightmap UVs), checked before and after | `hyper_model_uv_apply` |
| bake normal, AO, curvature, position, colour and other maps into textures and a material instance | `hyper_model_bake` |
| ready a generated or imported model: unwrap bad UVs, carry its textures, bake onto a decimated copy | `hyper_model_prepare` |

Toolset `HyperAIStudioModeling.HyperAIStudioModelingToolset`. The pack needs the Geometry Script plugin.

The plan works on **named meshes in memory**. An `add.*` op or a `recipe` makes a mesh (a second `add.*` with the
same `Mesh` adds another part to it), later ops change it, and `write_asset` saves it as a static mesh. Nothing
touches the project until `write_asset`, so build freely and look at the numbers first:

1. `bDryRun: true` builds every mesh and reports each one's `Triangles`, `Size`, `Min`/`Max`, `bClosed`,
   `OpenEdges`, `Components` (separate pieces), `UVSets` and `MaterialIds`, and what each `write_asset` would write
   (triangles, slots, collision shapes). A recipe's ops are listed in `ExpandedOps`. `bPreviewDryRun: true` also
   draws what each `write_asset` would write as a PNG, `Assets[].PreviewPath` (slower: a temporary mesh each).
2. The same plan with `bDryRun: false` and a fresh `OperationId` builds it again and writes the assets, saved, in
   one undo step, and draws a PNG of each asset, `Assets[].PreviewPath`: open it with your image reader to see what
   you made (Notes say when the editor cannot draw). With approval on it waits in the Activity panel, and that
   reply already names each `Assets[].PreviewPath`: call `hyper_operation_wait` with `bWaitThroughApproval: true`,
   whose diagnostic starts by saying which were drawn, then open those paths.
3. `hyper_model_validate` on the assets, `Policy` `placeholder` (the default) or `production`.
   `hyper_model_inspect` with `Asset` and `bPreview: true` draws an existing static mesh again.

A mistake is refused before anything is built, naming the op by its number, the field and what it takes; an op
that fails while building is named the same way, and nothing is written.

## Units and conventions

- Centimeters, Z up: 100 is a meter. A door is 220 cm tall, a crate 60 to 100 cm, a tree 400 to 1200 cm.
- Vectors are `"x,y,z"`, rotations `"pitch,yaw,roll"`, a scale one number or `"x,y,z"`.
- `Size` is the shape's bounds in cm. `add.*` shapes sit on their **base center** at `Location` (`Origin=center`
  centers them). Recipes end with the pivot on the base, so the asset stands on the ground where it is placed.
- `Params` are `"Name=Value; Name=Value"`; each kind takes its own (`hyper_model_inspect` lists them, with ranges
  and defaults). Colours are `#rrggbb` or `r,g,b` in 0-1.
- Mesh names are 1 to 48 letters, digits, `_` or `-`. `drop` discards a mesh you no longer need.

## Op kinds

- **Shapes:** `add.box`, `add.sphere` (`Style=box` spreads triangles evenly: best before noise), `add.cylinder`,
  `add.cone` (`Top` = top radius as a share of the base), `add.capsule`, `add.torus`, `add.disc`, `add.stairs`,
  `add.curved_stairs`. Each takes `Size`, `Location`, `Rotation`, `Scale`, and `Steps`, `Material`, `Color`,
  `Groups` (face, quad or single polygroups: bevel, extrude and subdivide work per group), `Origin`.
- **Profiles:** `add.revolve` turns `Points` `"r,z; r,z"` (radius, height) about Z: vases, domes, mushroom caps,
  columns. With `Ring=<radius>` the Points are a closed cross-section turned at that radius (`Degrees=180` makes an
  arch's ring). `add.sweep` drags a cross-section (`Points`, or `circle:radius,sides`, or `rect:width,depth`) along
  `Path` `"x,y,z; ..."`, scaling from `StartScale` to `EndScale`: trunks, branches, roots, pipes, rails, vines.
  `add.extrude_polygon` raises an outline `Height` cm. Profiles face outward whichever way their points run.
- **Combining:** `append` (a copy of `Source` added into `Mesh`), `copy`, `array.linear` (`Count` copies, each one
  more step of `Location`/`Rotation`/`Scale`), `array.radial` (`Count` around Z, `Radius` out, `Jitter` varies them),
  `scatter_on` (copies of `Source` strewn over `Mesh`'s surface where it faces within `Up` degrees of up, `Spacing`
  apart, random `Tilt` and scale), `boolean` (`Op=union|subtract|intersect` with `Source`), `self_union` (fuses
  overlapping parts into one watertight surface), `plane_cut` (cuts away the side `Normal` points to), `mirror`.
- **Faces:** `extrude`, `inset`, `offset_faces`, `bevel` (no `Select`: every polygroup edge), `shell`.
  `offset_faces` moves the selected faces `Distance` cm along their own normals, with side walls where they meet the
  rest, where `extrude` pushes the whole selection one way: a band around a round shape (a barrel's hoops) needs
  `offset_faces`; a negative Distance sinks the faces into a recess. With no `Select` it swells (or with a negative
  Distance shrinks) a closed mesh, its faces kept parallel (`Mode=parallel`; `face` and `vertex` average the
  normals instead), and thickens an open sheet into a slab: piece by piece, in a mesh of several. Moved faces keep
  their polygroups. `Area=face` moves every face on its own with walls of its own: 7 triangles where there was 1.
- **Deform:** `noise` (`Magnitude` cm, `Frequency` = 1 / feature size in cm, `Seed`), `bend`, `twist`, `flare`,
  `wave`, `smooth`. Noise moves vertices, so give it some: `Style=box` spheres, box `Steps`, `tessellate` or `remesh`
  first. `wave` moves every vertex up to `Magnitude` cm along `Direction` (`0,0,1`) by a sine `Wavelength` cm long:
  `Mode=line` waves travel along `Along` (`1,0,0`: corrugated sheets, flags), `Mode=ripple` rings spread from
  `Location` (the mesh's center by default); `Phase` in degrees shifts them. It takes no `Select`, and needs vertices
  too, about 8 or more per Wavelength (box `Steps`, `tessellate`, `subdivide`). A 3D wave is not offered: Unreal 5.8
  leaves it unimplemented.
- **Topology:** `tessellate` (Level 1-3), `subdivide` (Level 1-3, `Cage=groups|quads`), `remesh` (`Triangles`),
  `simplify` (`Triangles`, `Planar=true` or `Tolerance`), `solidify` (a watertight copy through a voxel grid,
  `Resolution` 8-256), `morphology`, `repair`.
- **Surface:** `normals` (`Mode=smooth|hard|split`), `uv` (`Mode=box|planar|cylinder|auto`, `Scale` cm per tile),
  `material` (`Id` 0-15 on the selected faces: one material slot per id), `color` (vertex colour; `Glow` 0-1 makes
  it emissive with the default material).
- **Placing:** `transform` moves the whole mesh, `pivot` (`At=base|center|x,y,z`) sets its origin.
- **Writing:** `write_asset` with `Asset` under `/Game` (`/Game/Env/Props/SM_Crate`), Params `Collision`
  (`convex` by default, `box`, `hulls:N` for concave shapes, `auto`, `complex`, `none`), `Nanite` (off by default),
  `Materials` (`0:/Game/M/MI_Stone,1:/Game/M/MI_Moss`), `Replace=true` to overwrite an existing static mesh
  (undoable; its other LODs and hi-res source go too, and a redirector's path is refused), `LightmapUV`. It writes
  the mesh as it is at that op: later ops on it, a `drop` too, leave that asset alone.

`Select` picks faces for extrude, inset, offset_faces, bevel, noise, smooth, uv, material and color: `all`, `top`,
`bottom`, `normal:nx,ny,nz,maxdeg`, `box:x0,y0,z0,x1,y1,z1`, `sphere:x,y,z,r`, `plane:ox,oy,oz,nx,ny,nz` or
`material:id`, with Params `Invert=true` and `Grow=n`. Read a dry run's `Min`/`Max` before writing a box or sphere
selector; a box selector picks the faces whose corners all lie inside it.

### subdivide or tessellate

`tessellate` splits every triangle where it lies: the shape stays (`Mode=pn` rounds it a little from its normals),
corners stay sharp, and the triangles grow (Level+1)^2 times. `subdivide` is Catmull-Clark: it reads each polygroup
as one polygon of a cage, only its corners counting, then refines and smooths the cage, so hard shapes come out
soft and slightly inside their cage; every Level multiplies the faces by 4 (a cage of quads of T triangles makes
T x 4^Level).

- Build what you subdivide with `Groups=quad`: proportions kept, edges rounded. `Groups=face` turns a box into a
  pebble. `Groups=single`, and round sides with `Groups=face` (cylinders, cones, capsules, tori, revolves, sweeps),
  are no cage: they are refused, naming the polygroup and the fix.
- After booleans, `solidify`, `morphology` or `remesh`, give `Cage=quads`: the triangles are paired into quads first.
- Hold an edge crisp with more quads near it (box `Steps`) or a `bevel` first. Normals come out smooth
  (`normals Mode=split` brings hard edges back).
- The result keeps its groups, so give one subdivide a higher Level rather than subdividing twice.

## Recipes

`{"Kind": "recipe", "Mesh": "Rock", "Params": "Shape=rock; Size=150; Seed=3; Detail=2"}` builds a whole prop into
`Mesh` from ordinary ops (listed in `ExpandedOps`). `Size` is in cm (across for rock, bush and crystal_cluster, tall
for the rest; arch: its width; ruined_wall: its length). `Seed` varies it: the same seed always builds the same mesh.
`Detail` 1-3 trades triangles for shape. `Tint` and `Accent` override its two colours. `Location`, `Rotation` and
`Scale` place the finished model in the working space (useful when appending recipes into one kitbash mesh).

| recipe | default size | notes |
|---|---|---|
| `rock` | 150 | faceted boulder, flat base, moss (Accent) on top |
| `standing_stone` | 300 | weathered monolith, slightly twisted and leaning |
| `pillar` | 400 | column on a plinth, capital, rings at Detail 2+ |
| `arch` | 400 wide | piers and a half-round arch with a keystone: write with `Collision=hulls:4` or `complex` |
| `ruined_wall` | 500 long | bites, slanted broken top, rubble at its foot (separate pieces): `Collision=hulls:6` |
| `tower` | 1200 | round tower, battlements, window slits, conical roof (Accent) |
| `crystal_cluster` | 120 | glowing shards (slot 1, Accent) on a rock base (slot 0) |
| `mushroom` | 80 | flared, leaning stem (Accent) and a domed cap (Tint) with spots |
| `tree_trunk` | 400 | tapering trunk, flared roots, branch stubs |
| `tree` | 600 | trunk (slot 0) and a lumpy canopy (slot 1, Accent) |
| `bush` | 150 | fused leafy lumps, flat underneath |
| `grass_tuft` | 40 | curved blades (separate pieces), darker at the root; write with `Collision=none` |

Make variations with seeds: three rocks are three recipes with `Seed` 1, 2 and 3 and three `write_asset`s.

## Examples

A rock set, checked first:

```json
{"Operations": [
 {"Kind": "recipe", "Mesh": "RockA", "Params": "Shape=rock; Size=120; Seed=1"},
 {"Kind": "recipe", "Mesh": "RockB", "Params": "Shape=rock; Size=180; Seed=2; Detail=3"},
 {"Kind": "write_asset", "Mesh": "RockA", "Asset": "/Game/Env/Rocks/SM_Rock_A"},
 {"Kind": "write_asset", "Mesh": "RockB", "Asset": "/Game/Env/Rocks/SM_Rock_B", "Params": "Collision=hulls:3"}],
 "bDryRun": true}
```

A lantern post from primitives: a post with a bevelled cap, a cross arm, a lantern with a window cut through it.

```json
{"Operations": [
 {"Kind": "add.box", "Mesh": "Post", "Size": "16,16,260", "Params": "Color=#5a4632"},
 {"Kind": "add.box", "Mesh": "Post", "Location": "0,0,260", "Size": "24,24,8", "Params": "Color=#3a3a3a"},
 {"Kind": "add.box", "Mesh": "Post", "Location": "25,0,230", "Size": "60,8,8", "Params": "Color=#5a4632"},
 {"Kind": "self_union", "Mesh": "Post"},
 {"Kind": "bevel", "Mesh": "Post", "Params": "Distance=1.5"},
 {"Kind": "add.box", "Mesh": "Lamp", "Location": "50,0,190", "Size": "26,26,34", "Params": "Color=#2b2b2b"},
 {"Kind": "add.box", "Mesh": "Window", "Location": "50,0,196", "Size": "40,18,22"},
 {"Kind": "boolean", "Mesh": "Lamp", "Source": "Window", "Params": "Op=subtract"},
 {"Kind": "drop", "Mesh": "Window"},
 {"Kind": "add.sphere", "Mesh": "Lamp", "Location": "50,0,198", "Size": "14", "Params": "Origin=center; Material=1"},
 {"Kind": "color", "Mesh": "Lamp", "Select": "material:1", "Params": "Color=#ffc861; Glow=0.8"},
 {"Kind": "append", "Mesh": "Post", "Source": "Lamp"},
 {"Kind": "uv", "Mesh": "Post", "Params": "Mode=box; Scale=50"},
 {"Kind": "write_asset", "Mesh": "Post", "Asset": "/Game/Env/Props/SM_LanternPost", "Params": "Collision=hulls:2"}],
 "bDryRun": false, "OperationId": "lantern-post"}
```

A vase turned from a profile, smoothed, glazed blue. The profile starts and ends half a centimeter off the axis, so
its ends cap closed:

```json
{"Operations": [
 {"Kind": "add.revolve", "Mesh": "Vase", "Points": "0.5,0; 18,0; 22,10; 14,40; 9,52; 12,60; 0.5,60", "Params": "Steps=32; Color=#3a5f9e"},
 {"Kind": "normals", "Mesh": "Vase", "Params": "Mode=smooth"},
 {"Kind": "uv", "Mesh": "Vase", "Params": "Mode=cylinder; Scale=60"},
 {"Kind": "write_asset", "Mesh": "Vase", "Asset": "/Game/Env/Props/SM_Vase"}],
 "bDryRun": false, "OperationId": "vase"}
```

A cushion: a box of quad groups subdivided at Level 2, so its edges round off and its faces stay flat in the middle
(a box's `Steps` is the vertices along each edge, so `Steps=4` makes 3 quads a side):

```json
{"Operations": [
 {"Kind": "add.box", "Mesh": "Cushion", "Size": "60,60,16", "Params": "Steps=4; Groups=quad; Color=#8a3b3b"},
 {"Kind": "subdivide", "Mesh": "Cushion", "Params": "Level=2"},
 {"Kind": "uv", "Mesh": "Cushion", "Params": "Mode=box; Scale=60"},
 {"Kind": "write_asset", "Mesh": "Cushion", "Asset": "/Game/Env/Props/SM_Cushion"}],
 "bDryRun": false, "OperationId": "cushion"}
```

A barrel with iron hoops. `HeightSteps=8` puts a ring of vertices every 10 cm, so each hoop is the band of faces
between two rings, picked by a box that holds both; `offset_faces` moves it out along the bulging staves, where
`extrude` would push it one way:

```json
{"Operations": [
 {"Kind": "add.cylinder", "Mesh": "Barrel", "Size": "60,60,90", "Params": "Steps=24; HeightSteps=8; Color=#7a5230"},
 {"Kind": "flare", "Mesh": "Barrel", "Params": "Percent=10"},
 {"Kind": "offset_faces", "Mesh": "Barrel", "Select": "box:-45,-45,9,45,45,21", "Params": "Distance=1.5"},
 {"Kind": "offset_faces", "Mesh": "Barrel", "Select": "box:-45,-45,69,45,45,81", "Params": "Distance=1.5"},
 {"Kind": "color", "Mesh": "Barrel", "Select": "box:-45,-45,9,45,45,21", "Params": "Color=#3a3a3a"},
 {"Kind": "color", "Mesh": "Barrel", "Select": "box:-45,-45,69,45,45,81", "Params": "Color=#3a3a3a"},
 {"Kind": "uv", "Mesh": "Barrel", "Params": "Mode=cylinder; Scale=90"},
 {"Kind": "write_asset", "Mesh": "Barrel", "Asset": "/Game/Env/Props/SM_Barrel"}],
 "bDryRun": false, "OperationId": "barrel"}
```

A flag on a pole: a thin box with vertices every 6 cm, waved sideways from the pole, where the wave starts:

```json
{"Operations": [
 {"Kind": "add.box", "Mesh": "Flag", "Location": "75,0,250", "Size": "150,2,90", "Params": "Steps=26; Color=#b0302a"},
 {"Kind": "wave", "Mesh": "Flag", "Location": "0,0,250", "Params": "Magnitude=6; Wavelength=90; Along=1,0,0; Direction=0,1,0"},
 {"Kind": "add.cylinder", "Mesh": "Flag", "Size": "6,6,350", "Params": "Steps=12; Color=#5a4632"},
 {"Kind": "write_asset", "Mesh": "Flag", "Asset": "/Game/Env/Props/SM_Flag", "Params": "Collision=hulls:2"}],
 "bDryRun": false, "OperationId": "flag"}
```

## Scale, budgets and quality

- A plan has up to 64 ops (160 once recipes expand), 32 meshes at once and writes up to 8 assets. A working mesh
  holds up to 250000 triangles; an asset up to 100000 without Nanite (250000 with `Nanite=true`). Triangles are
  predicted before anything runs and counted after every op; a plan builds within 8 seconds. `tessellate` and
  `subdivide` go to Level 3; subdivide's triangles are counted from its cage before it runs, and the walls of
  extrude, inset and offset_faces from the faces they pick. Props for a level want 500 to 5000 triangles:
  `simplify` heavy meshes.
- A write draws its PNG previews within 4 seconds all told; an asset past that, or one written while the editor
  cannot draw, has none and a note says so, and the write stands all the same.
- `bClosed` (no open edges) matters for booleans, solidify, lighting and collision. `self_union` or `repair`
  closes most; `solidify` rebuilds anything watertight.
- Placeholders get a lit material whose base colour is the vertex colour (`/Game/HyperAI/Modeling/M_HyperAIVertexColor`,
  made on first use), so tints read without textures; `Materials` replaces it per slot, and `material` ops plus
  `uv` ready a mesh for the project's own materials.
- Undo takes back a `Replace=true` write. New assets are not deleted by undo; delete them in the content browser.

## Handing a mesh to a person

Some shapes are quicker by hand: a sculpted dent, a few faces pulled out, a cleaner silhouette. Unreal's Modeling
Mode (the Modeling Tools Editor Mode plugin, Beta) does that, but its tools need a person's clicks and an Accept.
`hyper_model_handoff` gets the mesh ready for them:

```json
{"Asset": "/Game/Env/Rocks/SM_Rock_A", "Tool": "Sculpt"}
```

1. Optionally `bDryRun: true` first: it says which actor it would use or put in the level, and changes nothing.
2. The hand-off uses an actor already showing the mesh, or puts one in the level (folder `HyperAI/Modeling`, label
   `HyperAI_Edit_<asset>` with characters a label cannot hold as `_`, at `Location` or where the level viewport looks;
   `Label` or `Location` always puts a new one). Putting one there is one undo step, and waits for approval when
   approval is on; approved after another level was opened, or during Play in Editor, it is refused and changes
   nothing. `Actor` hands over an actor in the level by label or path instead.
3. It selects only that actor, frames it in the level viewports, brings the level editor in front of HyperAI Studio
   (`bLeaveTabsAlone: true` leaves the tabs as they are) and opens Modeling Mode. Selection, framing and the mode are
   not undo steps.
4. No tool is started: `Tool` (PolyEdit, TriEdit, PolyGroups, Sculpt, Smooth, Remesh, Simplify, UVs) names the
   palette and button in `ToolHint` as the person's Modeling Mode shows them (`Model > PolyGroup Edit` on the default
   palette, `PolyModel > PolyEd` on the legacy one), and `bToolReady` says whether that button accepts the selection.
   `NextSteps` is what to tell the person.
5. The person edits, clicks Accept, and saves (File > Save All). Accept writes into the asset itself, so every actor
   showing it changes (`SharedBy` counts the others). A mesh with a hi-res source is edited there: `HiResTriangles`
   counts it, and the reply's triangle counts and slow-tool note use it.
6. Then read it back: `hyper_model_inspect` with `Asset` reports `Fingerprint`; it differs from the hand-off's when
   the geometry, UVs, normals, hard edges, colours, polygroups or other layers changed, and Details say "changed
   since" the hand-off, or "unchanged", and whether it is unsaved (the editor remembers hand-offs until it closes;
   after a restart compare the two Fingerprints yourself). Material assignments and collision are not in it.
   `hyper_model_validate` checks it as a prop again.

A later `write_asset` with `Replace=true` on that path overwrites the person's edit: build on the edited asset
instead (place it, scatter it, or hand it over again). The hand-off refuses during Play in Editor, with the plugin
off, in an unattended editor, for an asset outside `/Game` or not a static mesh, and while a tool is open in
Modeling Mode or any other editor mode, since opening Modeling Mode would cancel it (the person Accepts or Cancels
it first).

## UVs and textures for any static mesh

These four work on any static mesh in the project (under `/Game` or a project plugin), not just ones a plan wrote:
imported, generated, hand-made. Engine content can be checked but not changed, and cooked or read-only assets are
refused.

**Check first, read-only.** `hyper_model_uv_check` with `Asset`, `LOD` (`-1` is the hi-res Nanite source; a LOD
reduced from another is read from the built mesh), `Channel` and `TextureSize`. `Verdict` is `good`, `usable`
(warnings only), `bad` or `missing`, and `Problems` says why, errors first: `overlap` (percent of the UV area covered
twice, and which islands, `3+7` or `5 itself` for a fold), `flipped`, `degenerate`, `no_uv_area`, then warnings
`outside_unit`, `texel_density` (spread over 4x), `fragmented`, `low_coverage`. `Metrics` has the numbers (islands,
seam length, density at TextureSize in texels per meter). `Lightmap` checks the asset's lightmap channel at its
lightmap resolution (or `LightmapResolution`): shared texels, islands closer than `LightmapPadding`, UVs outside
0-1, and whether the build makes it. Open `LayoutPath`: islands in muted colours with light outlines, overlaps red,
flipped triangles magenta, degenerate ones yellow, the 0-1 square framed. Overlap is fine for tiling or mirrored
textures, and wrong for bakes and lightmaps.

**Lay it out again.** `hyper_model_uv_apply` with `Method`:

- `xatlas` (the default) or `patch` (PatchBuilder) unwrap from scratch, then pack into 0-1 with `Padding` texels
  between islands at `Resolution` (4 at 1024 by default).
- `box`, `planar`, `cylinder` project, then pack (`bRepack: true`, for baking), or keep `TileSize` cm per tile for
  tiling materials (`bRepack: false`, which leaves UVs outside 0-1 on purpose).
- `repack` keeps the channel's islands and packs them again with the padding.
- `lightmap` has the mesh build its lightmap UVs with Unreal's own packer from `SourceChannel` into `Channel` (1 by
  default) at `Resolution` (64), and sets the asset's lightmap index and resolution.

`Channel` one past the last adds a channel. `LODs` defaults to `[0]`: each LOD unwrapped gets its own layout, so a
texture made for LOD 0 will not fit another; let other LODs be reduced from LOD 0 instead. Every other channel,
material slot, section, normal and colour stays as it was. `bDryRun: true` builds it in memory: `LODs[]` gives the
check before and after, `bImproved`, and `LayoutPath` the new layout. A layout that would come out worse than it went
in is refused. Written, it is one undo step; the asset is read back (`bVerified`) and saved.

**Bake.** `hyper_model_bake` lays maps out in the target's `TargetChannel` (it wants islands that do not overlap:
unwrap first, `TargetUVs` says how it is). `Source` is the detailed mesh (its hi-res source when it has one); empty
bakes the target from itself (AO, curvature, position, vertex colour, its own texture). `Maps`:

| map | texture | what for |
|---|---|---|
| `normal` | T_x_N, TC_Normalmap | tangent-space detail of Source on the target; carries Source's own normal map too |
| `ao` | T_x_AO, TC_Masks, and T_x_ORM | ambient occlusion (`OcclusionRays`, `OcclusionDistance`) |
| `bent_normal`, `object_normal` | BN (normal map), ON (exact, linear) | |
| `curvature`, `height`, `position` | CV, H (grayscale), P (exact, linear) | height maps `ProjectionDistance` cm either side |
| `vertex_color` | VC, sRGB | vertex colours into a texture |
| `texture` | BC, sRGB | a texture carried from Source's UVs (`SourceTexture`, or its slot 0 base colour) |
| `multi_texture` | BC, sRGB | each slot's base colour texture, one texture |
| `material` | BC, N and ORM | Source drawn in its own preview world with render captures: base colour, normal, roughness and metal; needs an editor that renders, and not with texture or normal |

`Resolution` (power of two, 16-4096), `Samples` (1, 4, 16, 64), `Padding` (dilation, pixels). Data maps are linear,
colour maps sRGB. With `ao`, `bPackORM` writes ORM (red occlusion, green `Roughness`, blue `Metallic`). `Material:
"new"` makes `MI_x_Baked`, an instance of `/Game/HyperAI/Modeling/M_HyperAIBakedSurface` (BaseColor, Normal, ORM
texture parameters, BaseColorTint, Roughness, Metallic); a path updates that material instance. `AssignSlots: "all"`
(or `"0,2"`) puts it on the target. Textures already there are refused unless `bReplace: true`. `bDryRun` names the
textures; with `bPreviewDryRun` it also bakes at 128 px and draws them. The written maps are drawn side by side at
`PreviewPath`, and each texture's `Min`/`Mean`/`Max` brightness says whether it holds anything.

**Generated models.** After `hyper_mesh_generate` imports a model (Tripo, Meshy, Hi3D: dense, often with overlapping
or fragmented UVs and one base colour texture), call `hyper_model_prepare` with its static mesh:

```json
{"Asset": "/Game/Generated/SM_Dragon", "LowPoly": "/Game/Generated/SM_Dragon_Low", "TargetTriangles": 8000, "Resolution": 2048, "bDryRun": true}
```

It checks channel 0; when the verdict is bad or missing (`UnwrapUVs: auto`; `always` or `never`), it unwraps LOD 0
with xatlas and carries the model's base colour and normal textures onto the new layout (`T_Dragon_BC`, `T_Dragon_N`
in `MI_Dragon_Baked`, on every slot). With `LowPoly` it writes a decimated copy, unwrapped, with normal, AO (and ORM)
and base colour baked from the original (`Maps` changes which), its own material instance and the original's simple
collision. `bLightmapUVs` adds lightmap UVs in channel 1. Read the dry run's `Steps`, then send it with `bDryRun:
false`; open `LayoutPath`, `MapsPreviewPath` and `PreviewPath`. Undo takes back the asset's own change; new textures,
materials and the low-poly stay until deleted.

## Using the results

The assets are ordinary static meshes. Scatter them over the ground with `hyper_pcg_scatter_apply_plan` (an ecosystem
species' `Meshes`, rocks with `bAlignToGround`, trees upright) or `hyper_foliage_apply_plan`. Write a few seeds of each
prop so the scatter does not repeat itself.

To place one by hand:

- With the level_design pack, use `hyper_spatial_apply_plan` `place` with `bDropToGround: true`, as the
  unreal-level-design skill says. Give it a Location Z above the ground, such as (HeightMax + 5) x 100 from
  `hyper_landscape_inspect` (HeightMax is meters, Location cm), because the drop traces only downward.
- Without that pack, find the ground first with Epic's `SceneTools.trace_world`, from high above down past the lowest
  ground (the ground is the start's Z less the distance it returns). Then call `add_to_scene_from_asset` at that
  height. Its `snap_to_ground` searches only a few times the prop's height around the given Z, and it does not say
  when it misses.
