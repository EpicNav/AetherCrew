---
name: unreal-niagara
description: Build or change a Niagara effect with HyperAI's Niagara tools - emitters from templates, modules and their inputs, custom HLSL modules, GPU simulation stages, emitter and renderer settings - as one undoable plan, then prove it compiles and spawns. Use for any VFX work - sparks, smoke, fire, magic, impacts, trails, weather, swarms and GPU simulations.
---

# Niagara effects: inspect, one plan, validate

HyperAI's Niagara tools are in toolset `HyperAIStudioNiagara.HyperAIStudioNiagaraToolset`, called with
`{"request": {...}}`. Every edit goes through one `hyper_niagara_apply_plan`: up to 16 ops, checked against the System
before anything changes, applied as one undo step, compiled, saved and verified. Plan the whole effect, then send it.

## The loop

1. **Inspect.** `hyper_niagara_inspect` with `targetPath` (`/Game/VFX/NS_Sparks.NS_Sparks`) and `bIncludeTopology: true`.
   Keep `revision`. `emitters` gives the addresses ops use: emitter names, each script stack's modules and inputs,
   `simulationStages` with their modules, and `renderers` with their `rendererIndex`. `scratchPadModules` lists the HLSL
   modules already in the System.
2. **Apply.** `hyper_niagara_apply_plan` with `targetPath`, `expectedRevision` (the revision), `bDryRun: false`, a new
   `operationId`, and `ops`. The reply waits for the edit and ends `completed`. A wrong emitter, stage, renderer index,
   property name or value is refused before anything changes, naming what does exist; fix the op and send again. A
   `bDryRun: true` call checks a plan without changing anything. Each plan needs the new `revision`: inspect again.
3. **Validate.** `hyper_niagara_validate` with `policy: "authoring"` gives compile errors and stack issues (fixable ones
   carry `fixIds` for an `apply_stack_issue_fix` op). Then `policy: "runtime_probe"` with `expectEmitters: ["Sparks"]`
   simulates the System and reports those emitters' live particles: the effect is done when they are `live`, not when it
   compiles. Without `expectEmitters` every emitter must be live, and a one-shot burst that has finished is not.

**A new System.** Epic's `CreateNiagaraSystem` (toolset `NiagaraToolsets.NiagaraToolset_System`) needs a template:
`{"assetName": "NS_Sparks", "assetPath": "/Game/VFX", "templateSystem": {"refPath":
"/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst.DirectionalBurst"}}`. Its emitters come along: disable the
ones you do not want (`set_emitter_property` `enabled` `false`) or remove them with Epic's `RemoveEmitter`. Save it
(`AssetTools.save_assets`) before the first plan: plans start from a saved System that is not open in the Niagara editor
and not compiling (`compile_not_terminal` means wait and plan again). Just after a save, inspect can answer
`revision_incomplete` once while the asset registry catches up; inspect again.

## Ops

| kind | fields |
|---|---|
| `add_emitter` | `assetPath` of an emitter template (`/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain`), `name` (new) |
| `add_module` | `scriptName`, `assetPath` of a module script, `emitterName` (none for System scripts); or `stageName` for a simulation stage |
| `set_module_enabled` | `scriptName` or `stageName`, `moduleName`, `bEnabled` |
| `set_input_value` | `scriptName`, `moduleName`, `inputNameStack` (`["Gravity"]`), `valueType` float / int32 / bool / vector / color, `value` (`-980`, `0,0,-980`, `1,0.4,0,1`) |
| `add_renderer` | `emitterName`, `assetPath` of the class (`/Script/Niagara.NiagaraSpriteRendererProperties`, `...MeshRendererProperties`, `...RibbonRendererProperties`, `...LightRendererProperties`) |
| `set_renderer_property` | `emitterName`, `rendererIndex`, `name` (the C++ property), `value` in Unreal text |
| `set_emitter_property` | `emitterName`, `name`, `value` (below) |
| `add_simulation_stage` | `emitterName`, `name`, `value` = iterations per frame, 1-64 (default 1) |
| `add_hlsl_module` | `scriptName` (or `stageName`), `emitterName`, `name`, `hlsl` |
| `set_hlsl_module` | `name` (a scratch pad module from inspect's `scratchPadModules`, or one an earlier op adds), `hlsl` (its new body) |
| `apply_stack_issue_fix` | `issueId`, `fixId` from validate |
| `create_effect_type`, `set_effect_type`, `set_effect_type_setting` | scalability budgets; see the tool guide |

`scriptName` is one of `SystemSpawnScript`, `SystemUpdateScript`, `EmitterSpawnScript`, `EmitterUpdateScript`,
`ParticleSpawnScript`, `ParticleUpdateScript`, or `ParticleSimulationStageScript` with a `stageName` (a `stageName` alone
does the same). Names you choose (emitters, stages, HLSL modules) are identifiers: letters, digits and underscores. An
`add_emitter` name must be new, or Niagara would rename the emitter and later ops would miss it.

Emitter properties: `sim_target` (`cpu` | `gpu`), `local_space` and `determinism` (`true` | `false`), `random_seed`
(an integer), `fixed_bounds` (`minx,miny,minz,maxx,maxy,maxz`, or `dynamic`), `enabled`. GPU emitters cannot measure
their own bounds: give them `fixed_bounds` whenever you switch to `gpu`.

Renderer properties take the C++ name and Unreal's text form: enums by name (`Alignment` `VelocityAligned`,
`FacingMode` `FaceCameraPlane`, `SortMode` `ViewDistance`), vectors and colors as a list (`SubImageSize` `4,4`),
booleans, numbers, and assets by path (`Material` `/Game/VFX/M_Spark.M_Spark`). A misspelt name is refused with the
closest real ones. Attribute bindings (`ColorBinding`, `PositionBinding` and the like) are set in the Niagara editor.

## HLSL modules

`add_hlsl_module` makes a scratch pad module in the System whose body is your HLSL, and appends it to the stack: the
graph is the input map, one Custom HLSL node and the output. Read and write attributes by their full names:

```hlsl
// Drag, then a swirl around the emitter's axis.
Particles.Velocity *= 1.0 - 1.5 * Engine.DeltaTime;
float3 P = Particles.Position;
Particles.Velocity += float3(-P.y, P.x, 0.0) * 2.0 * Engine.DeltaTime;
```

- Namespaces: `Particles.*` (particle scripts and stages), `Emitter.*`, `System.*`, `User.*`, `Engine.DeltaTime`,
  `Engine.Time`. Declare locals with a type (`float3 Dir = ...;`). No `return`, no `#include`.
- Every attribute you use must already exist in the emitter: an HLSL module can change `Particles.Velocity`,
  `Particles.Color`, `Particles.SpriteSize` or `Particles.Age`, but cannot create a new one. To add an attribute, set it
  first with Epic's `AddSetParametersModule` in `ParticleSpawnScript`, then read and write it from HLSL. A `User.*`
  parameter must be added with Epic's `AddUserVariables` first.
- Order matters: the module runs where it is appended, last in its stack. In `ParticleUpdateScript` it runs after
  `Particle State`, so a change to `Particles.Age` or `Particles.Lifetime` takes effect the next frame.
- CPU emitters compile the HLSL to the VM: plain math only. Texture sampling and data interface calls need a GPU emitter.
- HLSL errors appear in `hyper_niagara_validate` (authoring) after the compile. A System with compile errors still takes
  plans, so repair it in one: `set_module_enabled` `false` on the broken module (disabled modules are not compiled) and
  `add_hlsl_module` with a new name and the corrected code. Validate again; remove the broken one with Epic's
  `RemoveModule` once the compile is clean.

**Editing one.** `set_hlsl_module` replaces a scratch pad module's body in place, wherever it sits in a stack, as one
undo step; the System recompiles with the plan. Use it to tune or repair code rather than adding a module beside it.

## Simulation stages (GPU)

A simulation stage is an extra stack that runs after Particle Update, as many times per frame as its iterations, over
every particle. Use one for work that needs repeated passes: constraints, relaxation, settling, flocking corrections.
In UE 5.8 they run only on GPU emitters, so a plan for a CPU emitter switches it first:

```json
[{"kind": "set_emitter_property", "emitterName": "Sparks", "name": "sim_target", "value": "gpu"},
 {"kind": "set_emitter_property", "emitterName": "Sparks", "name": "fixed_bounds", "value": "-500,-500,0,500,500,600"},
 {"kind": "add_simulation_stage", "emitterName": "Sparks", "name": "Relax", "value": "4"},
 {"kind": "add_hlsl_module", "emitterName": "Sparks", "stageName": "Relax", "name": "KeepAboveGround",
  "hlsl": "if (Particles.Position.z < 0.0) { Particles.Position.z = 0.0; Particles.Velocity.z = abs(Particles.Velocity.z) * 0.5; }"}]
```

`add_module` and `set_module_enabled` also take a `stageName`. Inputs of modules inside a stage cannot be set yet: write
the values into an HLSL module there. Stages that iterate a data interface (grids, render targets) come from Epic's
emitter templates.

## CPU or GPU

`set_emitter_property` `sim_target` `cpu` or `gpu` switches an emitter. GPU takes many more particles and runs
simulation stages and texture sampling, but cannot measure its own bounds (give it `fixed_bounds`) and its counts read
back a frame late. Stay on CPU for small counts, events, collisions that spawn other emitters and effects that must be
deterministic on every platform.

## Live in play

`hyper_niagara_live` reads what plays in Play In Editor, read-only, per system and per emitter: particles alive, spawned
during a short watch (`sampleSeconds`, 0.5 by default) and so whether it is `spawning`, its `simTarget`, state and
bounds, and `issues`: compile errors, emitters disabled or culled, an active emitter with nothing alive or spawning,
particles alive with empty bounds (it can be culled), shaders still caching. `filter` narrows it to a system, actor or
component; `errors` lists every issue in one line each. It says what an effect is doing; what it costs per frame is
`hyper_profile_live` (sampled game-thread, render-thread and GPU time per system). Use both when an effect looks wrong
or slow in game.

## A worked plan

Sparks that fly up, slow down and spin into a vortex, on a System made as above:

```json
[{"kind": "add_emitter", "assetPath": "/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain", "name": "Sparks"},
 {"kind": "set_renderer_property", "emitterName": "Sparks", "rendererIndex": 0, "name": "Alignment", "value": "VelocityAligned"},
 {"kind": "add_hlsl_module", "emitterName": "Sparks", "scriptName": "ParticleUpdateScript", "name": "Vortex",
  "hlsl": "float3 P = Particles.Position;\nParticles.Velocity += float3(-P.y, P.x, 0.0) * 2.0 * Engine.DeltaTime;\nParticles.Velocity *= 1.0 - 0.8 * Engine.DeltaTime;"}]
```

Then `hyper_niagara_validate` with `authoring`, then `runtime_probe` with `expectEmitters: ["Sparks"]`. Inspect shows
`Vortex` last in Particle Update and in `scratchPadModules`.

## What stays in Epic's Niagara tools

Removing an emitter, module, renderer or user parameter; adding user parameters and Set Parameters modules; reading a
renderer's or module's full schema (`GetRendererSchema`, `GetModuleSchemaFromAsset`); dynamic inputs. Epic's
`SetEmitterData` and `SetRendererData` write properties without undo and without telling the renderer or the compiler:
use `set_emitter_property` and `set_renderer_property`, which do both.
