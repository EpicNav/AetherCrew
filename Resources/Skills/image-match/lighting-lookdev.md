# Lighting and look-dev for a match

How a lighting artist takes a frame from close to right. Light only shows where it lands, so a match needs four things right: the light, the surfaces it lands on, how the renderer carries it, and the grade. Every lever here is a `hyper_lighting_apply_plan` op unless it says otherwise.

## 1. Read the frame before touching it

Say what the reference shows on each line before the first change. The score's regions say where the attempt differs; this list says why.

| Look at | What it tells you |
|---|---|
| The key light | Its direction, how hard its shadow edges are, its colour temperature |
| The shadows | How dark they get, and their colour: blue from the sky, warm from bounce |
| Reflections | Which surfaces mirror the world (wet ground, glass, metal, polished stone, car paint) and how sharply |
| Glow | What gives light (windows, neon, fire, screens), and whether it lights what is around it |
| Air | Haze, fog, shafts, the glow round a low sun |
| Grade | The palette, the contrast, where black and white sit |

## 2. Know how the project renders

`hyper_lighting_inspect` reports `rendering`, how this project carries light. It decides what a change can do.

| Fact | What it means for a match |
|---|---|
| `gi_method` lumen | Bounce light is real, and emissive surfaces light their surroundings. A change takes frames to settle; `hyper_capture_view` waits for it. |
| `reflection_method` lumen | Smooth surfaces reflect the scene. A surface rougher than `lumen_max_roughness_to_trace` (0.4 by default) reflects only a blur of it. Reflection captures add little. |
| `lumen_tracing` software | Needs `mesh_distance_fields`. What is off screen reflects only coarsely, so a mirror shows sharp detail only of what is on screen. Walls thinner than 10 cm leak light. Small meshes are left out of Lumen's scene, so a small glowing one stops lighting its surroundings unless `emissive_light_source` is on. |
| `lumen_tracing` hardware | Off-screen reflections keep their detail. `LumenRayLightingMode` hit lighting gives the truest reflections, at a cost. |
| `reflection_method` screen_space or none | Reflections come from what is on screen and from reflection captures, so place captures where reflective surfaces are. |
| `substrate` true | Materials are Substrate. Their parameters work the same. |
| `megalights` true | Many shadowed local lights are cheap. |
| `static_lighting` true | The project can bake lighting. These tools never build it: their lights are movable. |

## 3. Surfaces: when the light is right and the frame still looks wrong

A dull floor under the right sun is a material problem, not a light problem. Do not raise the light to make up for a surface; tune the surface.

`hyper_lighting_inspect` lists `materials`: those on what the view shows, most of the frame first (`max_materials`, 12 by default, up to 48). Each has:
- its `path` and `kind`, its `baseMaterial`, `blendMode`, `shadingModel` and `bTwoSided`
- `viewShare`, `actorCount`, and the nearest `actors` with the `slot` the material ops take
- its `parameters` with their values now
- `notes` on what keeps it from taking light as the real surface would: unlit, a roughness above the trace limit, a low specular, a metallic between 0 and 1, a base colour outside the physical range

### Changing a surface in the level

- **`set_material_parameter`**: `actor_label` names any actor showing the material, and `property` is `[component:<name>/][slot:<index or name>/]<parameter>`.
  - The level gets its own instance of the material under `/Game/HyperAI/LookDev/<level>/`, and every slot in the level that showed the material shows the instance. Modular pieces stay alike.
  - The project's material asset and other levels are never changed.
- **`set_actor_material_parameter`**: the same, for that one actor's slot alone. It keeps what it changes, and takes the rest from the level's instance.
- **Values**: a number, a linear colour `r,g,b` or `r,g,b,a`, a texture's path, or `default` to drop the change.
- **Saving**: the instances save with the plan when `bSave` is true. Undo reverts a change.
- **No such parameter**: when the material fixes the value, or the parameter is a static switch, the error lists the parameters it has. Expose one with `hyper_material_apply_plan` (the materials tools), or give the slot another material with `set_actor_material`.

### Values that read as real

From Epic's physically based materials guide (linear values):

| Non-metal | Base colour |
|---|---|
| Charcoal, fresh asphalt | 0.02 |
| Worn asphalt | 0.08 |
| Bare soil | 0.13 |
| Green grass | 0.21 |
| Desert sand | 0.36 |
| Fresh concrete | 0.51 |
| Ocean ice | 0.56 |
| Fresh snow | 0.81 |

| Metal | Base colour (r, g, b) |
|---|---|
| Iron | 0.560, 0.570, 0.580 |
| Silver | 0.972, 0.960, 0.915 |
| Aluminium | 0.913, 0.921, 0.925 |
| Gold | 1.000, 0.766, 0.336 |
| Copper | 0.955, 0.637, 0.538 |
| Chromium | 0.550, 0.556, 0.554 |

- **Metallic** is 0 or 1. Values between only where dirt or rust covers metal.
- **Specular** stays 0.5 (about 4% reflectance) for nearly every non-metal. Water 0.255, ice 0.224, skin 0.35, quartz 0.57.
- **Roughness** 0 is a mirror and 1 is matte. Wet and polished surfaces sit low, worn and dusty ones high, and real surfaces vary across their area: a flat value reads as plastic.

### What the reference shows, and the lever

| What the reference shows | Lever |
|---|---|
| Wet ground, a polished floor, glossy paint mirroring the lights | Lower that material's `Roughness` below `lumen_max_roughness_to_trace`. Keep `Specular` 0.5. |
| Everything looks like plastic | Raise and vary roughness (its texture's multiplier where the material has one), `Specular` 0.5, base colours inside the table |
| Chalky, washed-out surfaces | A base colour too bright: bring non-metals under 0.81 |
| Dull grey metal | `Metallic` 1, the metal's base colour from the table, `Roughness` 0.1 to 0.4 |
| Windows, neon or screens that glow and light what is round them | Raise the material's emissive strength. On a small mesh, `set_actor_lighting` `emissive_light_source` true. A strong source lights best with a local light inside it, the emissive giving its look. |
| Glass and water mirroring the scene | Under Lumen a translucent surface reflects sharply only with High Quality Translucency Reflections: `set_post_process` `LumenFrontLayerTranslucencyReflections` true, with the project's `translucency_front_layer_reflections` on |
| A black hole in a shadowed corner | A base colour under 0.02, or an unlit material (the notes say which) |
| Leaves, cloth or skin glowing where light passes through | A two-sided foliage or subsurface material, which no parameter gives: `set_actor_material` to one |

## 4. Reflections and bounce: the renderer's settings

These are `set_post_process` settings by name, with their defaults in Unreal Engine 5.8. They cost frame time, so reach for them when the light and surfaces are right but the result is noisy, blotchy or slow to settle.

| Setting | Default | What it does |
|---|---|---|
| `LumenMaxRoughnessToTraceReflections` | 0.4 | Surfaces up to this roughness get their own reflection rays; higher reflects rougher surfaces sharply, at a GPU cost |
| `LumenReflectionQuality` | 1 | Up to 2 for less noise |
| `LumenRayLightingMode` | the project's | Hit lighting, with hardware tracing, for the truest reflections |
| `LumenMaxReflectionBounces` | 1 | Reflections within reflections, with hardware hit lighting |
| `LumenReflectionsScreenTraces` | on | Screen traces keep on-screen detail in reflections |
| `LumenFrontLayerTranslucencyReflections` | off | Mirror reflections on the front layer of glass and water |
| `LumenFinalGatherQuality` | 1 | Up for less noise in bounce light |
| `LumenSceneDetail` | 1 | Up so smaller objects take part in bounce light |
| `LumenSceneViewDistance`, `LumenMaxTraceDistance` | 20000 | How far Lumen's scene and its traces reach |
| `LumenSceneLightingUpdateSpeed`, `LumenFinalGatherLightingUpdateSpeed` | 1 | How fast bounce light catches up with a change |
| `LumenDiffuseColorBoost` | 1 | Above 1 brightens bounce, but is not physically correct |
| `LumenSkylightLeaking` | 0 | Lifts dark interiors with sky light, at the cost of realism |

**Reflection captures** matter where `reflection_method` is not lumen:
- `add_reflection_capture`: property `sphere` or `box`, value its location.
- `set_reflection_capture` changes one: `InfluenceRadius` for a sphere, `box_extent` and `BoxTransitionDistance` for a box, `Brightness` for either.
- Each recaptures by itself in the editor. Put one in each room or street with reflective surfaces, the largest round the space and smaller ones at reflective spots.

## 5. Exposure and units

- **Lock exposure first** with `lock_exposure` and an EV100: about 15 for noon sun, 12 overcast, 8 a bright interior, 4 a dim one, -2 moonlight. A sky whose lights are not physical sits far lower; move the EV100 until the frame's `luminanceP50` is near 0.18.
- **Units**: the sun is in lux. The sky light and emissive surfaces are in candelas per square metre. Point, spot and rect lights use candelas, lumens or unitless.
- **Change by ratios**, twice or half, rather than guessing absolute values.

## 6. Mistakes that cost earlier matches

- **Chasing a surface with light.** Raising the sun because a floor looks dull, or brightening everything because a window does not glow. Tune the material.
- **Comparing with exposure unlocked.** Every score then measures the eye adapting.
- **Changing many things at once.** The score cannot say which one helped.
- **A note that names no values.** HyperAI's study of the job learns from each note, so write `Roughness 0.85 -> 0.2 on M_Floor`, not `tweaks`.
- **Flattening the frame to lift shadows.** `LumenDiffuseColorBoost` and `LumenSkylightLeaking` brighten every shadow evenly. Soften shadows with the sun's `source_angle`, the sky light and fills first.
- **Reflection captures under Lumen reflections.** They add little; tune roughness instead.
- **An emissive mesh expected to light the room.** With software Lumen a small one stops once it leaves the screen: `emissive_light_source`, or a local light.

## Sources

- Epic Games, [Physically Based Materials](https://dev.epicgames.com/documentation/en-us/unreal-engine/physically-based-materials-in-unreal-engine)
- Epic Games, [Lumen Global Illumination and Reflections](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine)
- Epic Games, [Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)
- Epic Games, [Using Physical Lighting Units](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-physical-lighting-units-in-unreal-engine)
- Epic Games, [Unreal Engine 5 goes all-in on dynamic global illumination with Lumen](https://www.unrealengine.com/en-US/tech-blog/unreal-engine-5-goes-all-in-on-dynamic-global-illumination-with-lumen)
