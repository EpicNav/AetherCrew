---
name: unreal-materials
description: Make production materials with HyperAI's material tools - layered PBR surfaces, Single Layer Water, deferred decals, height-blended landscape layers, Custom HLSL, Substrate slabs and material functions from recipes, then graph edits, instances and compile checks. Use for any material work - surfaces, terrain, water, decals, shader code, master materials and their instances.
---

# Materials: a recipe, then the graph

HyperAI's material tools are in toolset `HyperAIStudioMaterials.HyperAIStudioMaterialsToolset`, called with
`{"request": {...}}`. Start a new material from a recipe with `hyper_material_recipe`: one approved step that builds the
graph, exposes its parameters, compiles it, checks for compile errors and writes a preview PNG. Then change it node by
node with `hyper_material_apply_plan`, and make its variations as instances.

## The loop

1. **List** the recipes: `hyper_material_recipe` with an empty `recipe` returns each one with its settings and defaults,
   and says whether Substrate is on in this project.
2. **Dry-run** the one you want: `recipe`, `path` (a new package path under `/Game`, such as `/Game/Materials/M_Rock`),
   `settings` as `[{"key": "tiling", "value": "4"}]`, and `bDryRun: true`. The reply lists the whole graph (`graph`:
   nodes, wires, outputs) and the `parameters` an instance can set, and changes nothing. A setting the recipe does not
   have is refused with the ones it does. Code recipes (`custom_hlsl`, `material_function`) are compiled off to the side
   in the dry run, so HLSL errors come back (`compile_errors`, with the compiler's messages) before anything is made.
3. **Make it**: the same request with `bDryRun: false` and a new `operationId`. With approval on, the reply is
   `awaiting_user_approval`; call `hyper_operation_wait` with the `operationId` and `bWaitThroughApproval: true`. It ends
   `completed` with `stats` (instructions, samplers, texture samples), `previewImagePath` (open it) and `saved`.
4. **Tune** with `hyper_material_inspect` and `hyper_material_apply_plan` `edit_graph`; make variations with
   `create_material_instance` and `set_instance_parameters`, setting the parameters the recipe listed. Check with
   `hyper_material_validate`.

## Recipes

| recipe | what it makes | settings (defaults) |
|---|---|---|
| `layered_pbr` | opaque surface: tinted base colour, normal with a detail normal blended over it (whiteout), ORM (R occlusion, G roughness, B metallic), tiling | `base_color_texture`, `normal_texture`, `orm_texture`, `tint` (1,1,1), `tiling` (1), `detail_normal_texture`, `detail_tiling` (8), `detail_strength` (0.5), `roughness_scale` (1) |
| `water` | Single Layer Water: shallow colour, depth absorption and scattering, two panning samples of one normal map blended, roughness, refraction | `shallow_color`, `scattering_color`, `absorption` (0.45,0.07,0.04 per metre: red goes first, so deep water reads blue-green), `normal_texture`, `normal_tiling` (4), `pan_speed` (0.03), `normal_strength` (0.4), `roughness` (0.05), `ior` (1.33) |
| `decal` | deferred decal, translucent: tinted colour whose alpha times `Opacity` is the mask, a normal faded in by `NormalStrength`, roughness | `base_color_texture`, `normal_texture`, `tint`, `opacity` (1), `normal_strength` (1), `roughness` (0.6), `tiling` (1) |
| `landscape_layers` | landscape material: one Landscape Layer Blend of N height-blended layers, each a tinted albedo whose red channel times `<Layer>Height` is its height | `layers` (Grass,Rock,Snow; 1 to 8), `texture`, `mapping_scale` (8), `roughness` (0.85) |
| `custom_hlsl` | a Custom HLSL node fed by parameters, into one output | `inputs` (`Tint=1,0.5,0.2; Amount=0.5`: one number makes a scalar parameter, three or four a vector), `output_type` (float3), `output` (base_color, emissive, roughness, metallic, opacity or normal), `description` |
| `substrate_slab` | a Substrate Slab BSDF into the Front Material: diffuse albedo, F0, roughness, normal map | `albedo`, `f0` (0.04), `roughness` (0.5), `normal_texture`, `tiling` |
| `material_function` | a material function: typed inputs, a Custom HLSL body, one output; with `callerPath`, also a material that calls it with a parameter per input | `inputs` (`Color:vector3, Strength:scalar`), `output_type` (float3), `output_name` (Result), `caller_output` (emissive or base_color), `description` |

Textures default to the engine's flat ones so a recipe compiles before your art exists; pass your own (`/Game/...`
object paths) as settings, or set them later on an instance. Every texture is a parameter, so instances swap them.

- **Code** goes in `code`, not a setting: the body of the HLSL function, ending in a `return` of the output type
  (`return Tint * Amount;`). Inputs are in scope by name.
- **Landscape layers** are the paint layer names. Give the landscape this material and paint layers of exactly these
  names: `hyper_landscape_apply_plan` `landscape.set_material`, then `landscape.add_paint_layer`.
- **Substrate** only when the project has it on (Project Settings > Rendering > Substrate, `r.Substrate`, which needs a
  restart). Otherwise the recipe answers `substrate_disabled`: use `layered_pbr`.
- **Water** needs the deferred renderer (desktop and consoles). Put it on a water body or any flat mesh.
- **A decal** is placed with a Decal actor whose box crosses the surface.
- **A function** compiles only inside a material: the dry run and the build compile it through a caller, which is saved
  only when you give `callerPath`. Other materials call it with a `function_call` node in `hyper_material_apply_plan`
  (`{"kind": "function_call", "properties": [{"key": "function", "value": "/Game/Materials/MF_Glow.MF_Glow"}]}`).

## Material Designer

Material Designer (Dynamic Material) instances are `hyper_dynamic_material_inspect` and
`hyper_dynamic_material_apply_plan`: values of existing layers and stages, and turning layers on and off. New layers
are added in the Material Designer panel.

## Quality

- One master material with parameters, instances for its variations; no duplicated masters.
- Parameters named and grouped (the recipes group them: Surface, Detail, Water, Ripples, Decal, Layers, Code).
- Only the features a surface needs: translucency, refraction and world position offset cost frame time; check
  `stats` and keep pixel shader instructions in budget.
- `hyper_material_validate` reports no errors, and the preview looks right.
