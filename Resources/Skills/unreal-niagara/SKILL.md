---
name: unreal-niagara
description: Build or change a Niagara effect with HyperAI's Niagara tools - a new System from a template or an effect recipe (fire, explosion, magic burst, impact sparks, smoke, ribbon trail), emitters, modules, renderers, every kind of input value (random ranges, curves, links, expressions, enums), user parameters, renderer bindings, HLSL modules and GPU simulation stages - as one undoable plan, then look at it in a preview and prove it spawns. Use for any VFX work - sparks, smoke, fire, magic, impacts, trails, weather, swarms and GPU simulations.
---

# Niagara effects: discover, one plan, preview, validate

HyperAI's Niagara tools are in toolset `HyperAIStudioNiagara.HyperAIStudioNiagaraToolset`, called with
`{"request": {...}}`. Every edit goes through one `hyper_niagara_apply_plan`: up to 96 ops, checked against the System
before anything changes, applied as one undo step, compiled, saved and verified. A plan can make its System too. Plan
the whole effect, send it, then look at it.

## The loop

1. **Start.** For a stock effect, use a recipe: `hyper_niagara_apply_plan` with `recipe` (`fire`, `explosion`,
   `magic_burst`, `impact_sparks`, `smoke_plume`, `ribbon_trail`), `recipeSettings` (`["color=0.3,0.6,1", "scale=2"]`)
   and a new `targetPath`. `hyper_niagara_discover` with `kind: "recipes"` lists each recipe's settings, and
   `kind: "recipe"` returns its ops to read or adapt before sending them yourself. For anything else, start from a
   template: `kind: "templates"` lists Niagara's emitter and System templates with what each does. `explosion` is two plans, as one plan's time budget
   holds about 60 ops: `part=1` makes the flash, fireball and shockwave; then `part=2`, with the same `targetPath` and
   its new `expectedRevision` from inspect, adds the sparks and smoke.
2. **Materials first.** Recipes draw with particle materials; their reply (and discover's `materials`) names each one
   and whether it exists. Make missing ones with `hyper_material_recipe`: `particle_additive` (fire, sparks, flashes,
   magic; `shape: "ring"` for shockwaves) and `particle_translucent` (smoke and dust; `noise: "0.7"` breaks it up).
   Without them a recipe still builds with Niagara's default sprite material, but it looks plain and hard-edged.
3. **Inspect** an existing System: `hyper_niagara_inspect` with `targetPath` and `bIncludeTopology: true` (add
   `bIncludeInputValues: true` to read every module input's value). Keep `revision`. `emitters` gives the addresses
   ops use: each stack's modules, their `inputNames` with the `inputTypes` each takes (the `valueType` of a local
   value), `simulationStages`, and `renderers` with their `rendererIndex`.
4. **Apply.** `hyper_niagara_apply_plan` with `targetPath`, `expectedRevision`, `bDryRun: false`, a new `operationId`
   and `ops`. The reply waits for the edit and ends `completed`. A wrong emitter, renderer index, binding, property or
   value is refused before anything changes, naming what does exist. `bDryRun: true` checks a plan without changing
   anything. Each later plan needs the new `revision`: inspect again.
5. **Look at it.** `hyper_niagara_preview` with `targetPath` simulates the System in its own preview world and renders
   it at several ages (`ages` in seconds; 0.15, 0.5, 1 and 2 by default) into one PNG, a tile per age, left to right.
   **Open `imagePath` with your image reader and judge it yourself**: shape, colour, softness, how it moves from tile
   to tile. The numbers help: `coverage`, `meanColor`, `peakLuminance` (1 is clipping to white), `screenBox`, and
   `notes` (empty tiles, clipping, running off the edge). `background: "grey"` for smoke and anything alpha-blended;
   `view` (`front`, `side`, `top`, `three_quarter`) and `distance` to frame it.
6. **Validate.** `hyper_niagara_validate` with `policy: "authoring"` gives compile errors and stack issues (fixable ones
   carry `fixIds` for an `apply_stack_issue_fix` op). `policy: "runtime_probe"` with `expectEmitters` reports those
   emitters' live particles.

## A new System in the plan

Op 0 `create_system` makes the System at `targetPath`, which must not exist yet; `expectedRevision` stays empty.
`assetPath` copies a template System (empty makes an empty one); `value` lists the template's emitters to keep, comma
separated (empty keeps all). The ops after it build the effect:

```json
{"targetPath": "/Game/VFX/NS_Sparks.NS_Sparks", "bDryRun": false, "operationId": "sparks-1", "ops": [
 {"kind": "create_system"},
 {"kind": "add_emitter", "assetPath": "/Niagara/DefaultAssets/Templates/Emitters/OmnidirectionalBurst.OmnidirectionalBurst", "name": "Sparks"},
 {"kind": "set_input_value", "emitterName": "Sparks", "scriptName": "EmitterUpdateScript", "moduleName": "SpawnBurst_Instantaneous",
  "inputNameStack": ["Spawn Count"], "valueType": "int32", "value": "60"},
 {"kind": "set_input_value", "emitterName": "Sparks", "scriptName": "ParticleSpawnScript", "moduleName": "AddVelocity",
  "inputNameStack": ["Velocity Speed"], "valueType": "dynamic_input",
  "assetPath": "/Niagara/DynamicInputs/UniformRange/V2/RandomRangeFloat.RandomRangeFloat"},
 {"kind": "set_input_value", "emitterName": "Sparks", "scriptName": "ParticleSpawnScript", "moduleName": "AddVelocity",
  "inputNameStack": ["Velocity Speed", "Minimum"], "valueType": "float", "value": "600"},
 {"kind": "set_input_value", "emitterName": "Sparks", "scriptName": "ParticleSpawnScript", "moduleName": "AddVelocity",
  "inputNameStack": ["Velocity Speed", "Maximum"], "valueType": "float", "value": "1400"}]}
```

Plans on an existing System start from a saved System that is not open in the Niagara editor and not compiling
(`compile_not_terminal` means wait and plan again). The plan's System is saved when it completes. Undo reverts a
plan's edits in one step; a System a plan made stays as an asset until you delete it.

## Ops

| kind | fields |
|---|---|
| `create_system` | op 0 only: `assetPath` template System or empty, `value` emitters to keep |
| `add_emitter` / `remove_emitter` | `assetPath` of an emitter template and `name` (new) / `emitterName` |
| `add_module` / `remove_module` | `scriptName`, `assetPath` of a module script / `moduleName`; `emitterName` (none for System scripts); `stageName` for a stage |
| `set_module_enabled` | `scriptName` or `stageName`, `moduleName`, `bEnabled` |
| `set_input_value` | `scriptName`, `moduleName`, `inputNameStack`, `valueType`, `value` (below) |
| `add_user_parameter` / `remove_user_parameter` | `name` (`User.Tint` or `Tint`), `valueType`, `value` (the default) |
| `add_set_parameters` | `scriptName`, `name` (`Particles.Heat`), `valueType`, `value`; `moduleName` adds the entry to that Set Parameters module |
| `add_renderer` / `remove_renderer` | `emitterName`, `assetPath` of the class / `rendererIndex` (later renderers move down one) |
| `set_renderer_property` | `emitterName`, `rendererIndex`, `name` (the C++ property), `value` in Unreal text |
| `set_renderer_binding` | `emitterName`, `rendererIndex`, `name` (`ColorBinding`, `SpriteSizeBinding`, `DynamicMaterialBinding`, `SubImageIndexBinding`...), `value` the attribute (`Particles.Heat`) |
| `set_emitter_property` | `emitterName`, `name`, `value` (below) |
| `add_simulation_stage` | `emitterName`, `name`, `value` = iterations per frame, 1-64 |
| `add_hlsl_module` / `set_hlsl_module` | `scriptName` (or `stageName`), `emitterName`, `name`, `hlsl` / `name`, `hlsl` |
| `apply_stack_issue_fix` | `issueId`, `fixId` from validate |
| `create_effect_type`, `set_effect_type`, `set_effect_type_setting`, `create_data_channel`, `bake_sim_cache` | scalability, data channels, sim caches |

`scriptName` is `SystemSpawnScript`, `SystemUpdateScript`, `EmitterSpawnScript`, `EmitterUpdateScript`,
`ParticleSpawnScript`, `ParticleUpdateScript`, or `ParticleSimulationStageScript` with a `stageName`. Module names are
the stack's (`InitializeParticle`, `SpawnRate`, `GravityForce`, `ShapeLocation`); input names are the editor's,
spaces and all (`Lifetime Min`, `Uniform Sprite Size Max`, `Sphere Radius`). Names you choose are identifiers.

## Input values

`set_input_value`'s `valueType`:

- **Local values**: `float`, `int32`, `bool`, `vector2`, `vector`, `vector4`, `position`, `color` (`1,0.4,0,1`).
  Inspect's `inputTypes` says which an input takes.
- **`enum`**: `value` is the entry's name as the editor shows it or its C++ name (`Once`). A wrong one is refused with
  the entries there are. Static switches (Lifetime Mode, Shape Primitive, Velocity Mode) are enums too, and changing
  one shows or hides other inputs: an input greyed out by an edit condition is refused, so flip its switch first.
- **`dynamic_input`**: `assetPath` is the dynamic input script; then set its own inputs with a longer
  `inputNameStack` (`["Velocity Speed", "Minimum"]`) in the same plan. `hyper_niagara_discover` `kind:
  "dynamic_inputs"` with `valueType` lists them for a type, and `kind: "script_schema"` with an `assetPath` lists a
  script's inputs with their types. Everyday ones: `UniformRange/V2/RandomRangeFloat` for a range,
  `ValueFromCurve/FloatFromCurve` and `ValueFromCurve/ColorFromCurve` for a value over life.
- **`linked`**: `value` is the parameter it reads: `Particles.NormalizedAge`, `Particles.Velocity`, `Emitter.Age`,
  `User.Tint`, `Engine.DeltaTime`.
- **`hlsl_expression`**: one rvalue of the input's type (`float3(0, 0, 400) * (1 - Particles.NormalizedAge)`); no
  declarations, no `return`. CPU emitters run it on the VM: plain math only.
- **`data_interface`**: `value` is a JSON object of the data interface's properties (`{}` keeps its defaults);
  `kind: "data_interface_schema"` lists them. Curves have shorthands: `curve` (`"0:1; 0.5:0.6; 1:0"`), `color_curve`
  (`"0:1,0.8,0.3,1; 1:0.2,0,0,0"`, alpha optional) and `vector_curve` (`"0:1,1,1; 1:2,2,2"`), on a curve data
  interface input. Colour over life, the usual way:

```json
[{"kind": "add_module", "emitterName": "Fire", "scriptName": "ParticleUpdateScript", "assetPath": "/Niagara/Modules/Update/Color/Color.Color"},
 {"kind": "set_input_value", "emitterName": "Fire", "scriptName": "ParticleUpdateScript", "moduleName": "Color", "inputNameStack": ["Color"],
  "valueType": "dynamic_input", "assetPath": "/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve"},
 {"kind": "set_input_value", "emitterName": "Fire", "scriptName": "ParticleUpdateScript", "moduleName": "Color", "inputNameStack": ["Color", "ColorCurve"],
  "valueType": "color_curve", "value": "0:6,3,1,1; 0.4:4,1.2,0.25,1; 1:0,0,0,0"}]
```

`ColorFromCurve`'s `CurveIndex` already links `Particles.NormalizedAge`; disable the template's `ScaleColor` so it
does not scale the colour again. Inspect with `bIncludeInputValues` reads values back: `float 20`,
`linked Particles.NormalizedAge`, `enum Once`, `data_interface RedCurve 0:6, 0.4:4, 1:0; ...`, and dynamic inputs
with their own inputs in braces.

## Renderers, bindings and flipbooks

Renderer properties take the C++ name and Unreal's text form: enums by name (`Alignment` `VelocityAligned`,
`FacingMode` `FaceCameraPlane`, `SortMode` `ViewDistance`), vectors as a list (`SubImageSize` `4,4`), booleans,
numbers, assets by path (`Material` `/Game/VFX/M_Spark.M_Spark`). `kind: "renderer_schema"` with a renderer class
(`SpriteRenderer` works) lists its properties and bindings with their defaults. `set_renderer_binding` changes which
attribute the renderer reads.

A flipbook needs three things to agree: the material samples the sheet with a Particle SubUV node
(`hyper_material_recipe` `particle_additive` or `particle_translucent` with `flipbook: "true"` and `texture`), the
sprite renderer's `SubImageSize` is the grid (`8,8`), and a `SubUVAnimation` module
(`/Niagara/Modules/Update/SubUV/SubUVAnimation.SubUVAnimation`) runs in Particle Update. Dynamic Material Parameters
writes values only a material with a matching Dynamic Parameter node reads (the translucent recipe's `erode` reads
index 0's first channel); without one the writes are silently dropped.

## What looks good

- **Brightness**: additive effects glow from colour values above 1 (fire 3-6, sparks 8-12, flashes 10-15) and fade by
  going to black; alpha-blended smoke takes plain greys (0.1-0.4) and fades by alpha. A white blob in the preview, or
  `peakLuminance` 1 over a large area, means too bright or too many particles stacked.
- **Layers**: a convincing effect is several emitters, each one idea: flash, core, sparks, smoke, ring. Vary lifetimes
  and sizes with random ranges rather than single values.
- **Motion**: forces then the solver; size and colour change over life. Sparks read as streaks when velocity-aligned
  and stretched with speed. Fire rises (positive gravity) and flickers; smoke rises slowly, grows and thins.
- **Scale**: 1 unit is 1 cm. A campfire is about 50 cm across, an explosion 3-6 m, sparks fly at 500-1500 cm/s.
- **Loops**: bursts play once with `EmitterState` `Loop Behavior` `Once`; ambient effects loop.

## HLSL modules

`add_hlsl_module` makes a scratch pad module whose body is your HLSL, appended to the stack. Read and write
attributes by full name (`Particles.Velocity`, `Particles.SpriteSize`, `Engine.DeltaTime`, `Emitter.Age`,
`Engine.Owner.Position`); declare locals with a type; no `return`, no `#include`. Every attribute you use must exist:
make one with `add_set_parameters`, a `User.*` one with `add_user_parameter`. CPU emitters compile to the VM: plain
math (`sin`, `cos`, `abs`, `min`, `max`, `lerp`); `smoothstep` and texture sampling need GPU. Errors show in
`hyper_niagara_validate` after the compile; `set_hlsl_module` replaces a body in place.

## Simulation stages (GPU)

A simulation stage runs after Particle Update, as many times per frame as its iterations: constraints, relaxation,
settling. In UE 5.8 only GPU emitters run them, so switch first and give the emitter fixed bounds:

```json
[{"kind": "set_emitter_property", "emitterName": "Sparks", "name": "sim_target", "value": "gpu"},
 {"kind": "set_emitter_property", "emitterName": "Sparks", "name": "fixed_bounds", "value": "-500,-500,0,500,500,600"},
 {"kind": "add_simulation_stage", "emitterName": "Sparks", "name": "Relax", "value": "4"},
 {"kind": "add_hlsl_module", "emitterName": "Sparks", "stageName": "Relax", "name": "KeepAboveGround",
  "hlsl": "if (Particles.Position.z < 0.0) { Particles.Position.z = 0.0; Particles.Velocity.z = abs(Particles.Velocity.z) * 0.5; }"}]
```

Emitter properties: `sim_target` (`cpu` | `gpu`), `local_space`, `determinism`, `enabled` (`true` | `false`),
`random_seed`, `fixed_bounds` (`minx,miny,minz,maxx,maxy,maxz` or `dynamic`). Stay on CPU for small counts, events and
effects that must look the same everywhere (and in the preview); GPU for hundreds of particles and stages.

## Rules of thumb from Epic's Niagara skills

- Stack order is System, Emitter, Particle, each Spawn then Update. One-time values in Spawn; forces, then collision,
  then the solver in Particle Update.
- Hue shift does nothing to white: give the base colour some hue first.
- For ranges ("10 to 20"), use a random range, not one value.
- World space unless particles must move rigidly with the actor; attaching sets where they spawn, not how they move.
- Prefer dynamic inputs and module settings over HLSL expressions.
- Verify intent: trace every velocity and appearance input after building, and look at the preview.

## Live in play

`hyper_niagara_live` reads what plays in Play In Editor, per system and per emitter: particles, whether each emitter is
spawning, sim target, state, bounds and what is wrong. What an effect costs per frame is `hyper_profile_live`.
