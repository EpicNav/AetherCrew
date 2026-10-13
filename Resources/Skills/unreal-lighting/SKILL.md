---
name: unreal-lighting
description: Light a level with HyperAI's lighting tools - sun, sky light, height fog and volumetric fog, sky atmosphere, volumetric clouds, post process and colour grading, locked exposure, Lumen and other renderer settings on the post process volume, point, spot and rect lights, reflection captures, Blueprint skies such as Ultra Dynamic Sky, and the surfaces the light lands on - in one undoable plan, then capture the view to check it. Use for lighting, time of day, mood, fog, atmosphere, exposure, post process, Lumen and look-dev work; for matching a reference image use the image-match skill.
---

# Lighting a level

HyperAI's lighting tools are in toolset `HyperAIStudioLighting.HyperAIStudioLightingToolset`, called through
`call_tool` with `{"request": {...}}`:

| tool | does |
|---|---|
| `hyper_lighting_inspect` | the level's sun, sky light, fog, atmosphere, post process, reflection captures and Blueprint skies with their settings; the local lights and materials in view; how the project renders light (`Rendering`); `bExposureLocked`; the `Revision` a plan needs |
| `hyper_lighting_apply_plan` | 1 to 32 ops applied in order as one undo step; unsaved unless `bSave` |
| `hyper_capture_view` | PNG frames from a camera, a transform or the level viewport, with brightness and colour metrics; changes nothing |
| `hyper_lighting_compare`, `hyper_image_compare` | measure a view against a reference image (see the image-match skill) |

To match a reference image, follow the image-match skill and its `lighting-lookdev.md`: reading the frame, surfaces
and physical values, Lumen reflection settings and the mistakes that cost earlier matches are all there.

## Ops

Each op is `{"Kind", "ActorLabel", "Property", "Value"}`, every value a string. For the stock kinds an empty
`ActorLabel` takes the first actor of that kind and adds one if the level has none.

| Kind | Property: value |
|---|---|
| `set_sun` | `intensity` (lux), `temperature` (K), `color` (r,g,b), `pitch`, `yaw` (degrees), `source_angle` |
| `set_sky_light` | `intensity`, `color` |
| `set_fog` | `density`, `height_falloff`, `start_distance`, `inscattering_color`, `volumetric_fog` (true/false), `volumetric_scattering_distribution` (-0.9 to 0.9), `volumetric_albedo`, `volumetric_extinction_scale` |
| `set_sky_atmosphere` | `rayleigh_scale` (0 to 2, default 0.0331), `mie_scale` (0 to 5, default 0.003996): raw coefficients, so ten times the default is already haze |
| `set_clouds` | volumetric clouds, added if missing; `material:<parameter>` such as `material:Cloud_GlobalCoverage` |
| `set_post_process` | `exposure_bias`, `white_temp`, `saturation`, `contrast`, `gamma`, `shadows_gain`, `shadows_offset`, `highlights_gain` (r,g,b), `color_grading_lut` (a LUT image or texture, or none) |
| `lock_exposure`, `unlock_exposure` | value: the EV100 to expose for (manual metering); unlock takes none |
| `set_light`, `add_light` | point, spot and rect lights; `add_light` property `point`, `spot` or `rect`, value its location `x,y,z` |
| `add_reflection_capture`, `set_reflection_capture` | property `sphere` or `box`, value its location; then `InfluenceRadius`, `box_extent`, `Brightness` |
| `set_sky_system` | a Blueprint sky's variable by its Details name (`Time of Day`, `Cloud Coverage`) |
| `set_actor_lighting` | `[component:<name>/]cast_shadow`, `emissive_light_source`, `affect_indirect_lighting`, `affect_distance_field_lighting`, `visible_in_reflection_captures`, `visible_in_ray_tracing` |
| `set_actor_shadow` | value false lets light through an actor's meshes |
| `set_actor_material`, `set_material_parameter`, `set_actor_material_parameter` | surfaces; see `lighting-lookdev.md` |

- **Any Details setting.** Beyond these keys every stock kind takes any setting its Details panel shows, by C++ name
  or label: `set_sun` `BloomScale`, `set_sky_light` `OcclusionTint`, `set_fog` `SecondFogData`, `set_post_process`
  `BloomIntensity`, `LocalExposureShadowContrastScale`, `VignetteIntensity`. Values are numbers, true/false, an enum's
  name, `r,g,b`, or Unreal's text form; a post-process setting set to `default` goes back to the level's.
- **Lumen and the renderer** are post-process settings: `set_post_process` `DynamicGlobalIlluminationMethod` and
  `ReflectionMethod` (Lumen, ScreenSpace, None), `LumenSceneDetail`, `LumenFinalGatherQuality`,
  `LumenSceneLightingUpdateSpeed`, `LumenReflectionQuality`, `LumenMaxRoughnessToTraceReflections`. They act through
  the unbound post process volume the tool edits.
- **Local lights** (`set_light`): `intensity` in the light's `intensity_units` (unitless, candelas, lumens, ev, nits),
  `color`, `temperature` with `use_temperature`, `attenuation_radius`, `source_radius`, `soft_source_radius`,
  `source_length`, `inner_cone_angle`, `outer_cone_angle`, `source_width`, `source_height`, `barn_door_angle`,
  `barn_door_length`, `cast_shadows`, `volumetric_scattering`, `indirect_intensity`, `specular_scale`, `enabled`,
  `ies_profile`, `light_function`, and for light actors `location`, `rotation`, `aim_at`. A light inside a Blueprint
  is `component:<name>/<setting>`.
- **Nothing is deleted.** `enabled false` switches a light off; undo takes an added light away. New lights are movable
  and marked as added by HyperAI. These tools never build static lighting.
- **A Blueprint sky** (Ultra Dynamic Sky and others) owns its sun, sky light, fog and atmosphere: with one in the level
  `set_sun`, `set_sky_light`, `set_fog` and `set_sky_atmosphere` are refused. Use `set_sky_system`.
- **Not here:** project-wide rendering settings (hardware ray tracing, the project's default GI method, mesh distance
  fields, Substrate, MegaLights) live in Project Settings and often need a restart. `Rendering` reports them; ask the
  user before changing them. A second directional light (a moon) is not a kind here either.
- `hyper_worldbuilding_apply_plan`'s `environment.set_lighting`, `set_fog`, `set_sky` and `set_post_process` variants
  only check plans; light with `hyper_lighting_apply_plan`.

## A worked plan

Late afternoon: exposure locked first, a warm low sun with softer shadows, sky fill, light shafts in volumetric fog,
Lumen bounce settling faster, and a warm lamp on the porch.

```json
{"request": {"bDryRun": true, "ExpectedRevision": "<Revision from hyper_lighting_inspect>", "Ops": [
 {"Kind": "lock_exposure", "Value": "12"},
 {"Kind": "set_sun", "Property": "pitch", "Value": "-14"},
 {"Kind": "set_sun", "Property": "yaw", "Value": "235"},
 {"Kind": "set_sun", "Property": "temperature", "Value": "4300"},
 {"Kind": "set_sun", "Property": "intensity", "Value": "20000"},
 {"Kind": "set_sun", "Property": "source_angle", "Value": "1.5"},
 {"Kind": "set_sky_light", "Property": "intensity", "Value": "1.2"},
 {"Kind": "set_fog", "Property": "volumetric_fog", "Value": "true"},
 {"Kind": "set_fog", "Property": "density", "Value": "0.03"},
 {"Kind": "set_post_process", "Property": "LumenSceneLightingUpdateSpeed", "Value": "2"},
 {"Kind": "add_light", "ActorLabel": "Porch_Lamp", "Property": "point", "Value": "1200,-340,260"},
 {"Kind": "set_light", "ActorLabel": "Porch_Lamp", "Property": "temperature", "Value": "2400"},
 {"Kind": "set_light", "ActorLabel": "Porch_Lamp", "Property": "use_temperature", "Value": "true"},
 {"Kind": "set_light", "ActorLabel": "Porch_Lamp", "Property": "attenuation_radius", "Value": "900"}]}}
```

The dry run answers `planned` with a `PlanHash`, and `Notes` on values that are valid but rarely meant (an atmosphere
scale far past its default). Send the same request with `bDryRun: false`, a new `OperationId` and `ExpectedPlanHash`
set to `PlanHash`; the reply waits until the edit is done. With approval on it answers `awaiting_user_approval`: call
`hyper_operation_wait` with the `OperationId` and `bWaitThroughApproval: true`. `stale_revision` means the lighting
changed since inspection, `plan_hash_mismatch` that the plan changed since its dry run: inspect or dry-run again. Plans
are refused while Play In Editor runs. Leave `bSave` off until the look is right, then save with `bSave: true`.

## Habits

- Lock exposure before judging any change: otherwise the eye adaptation re-brightens every frame. Physical lights sit
  near EV100 15 at noon and 12 overcast; aim the frame's `LuminanceP50` near 0.18.
- Light first, then fog and atmosphere, then the grade. Change by ratios (twice, half) and a few levers per plan.
- A dull or plastic surface under the right light is a material problem: tune the surface, not the light.

## Checking

`hyper_capture_view` from a named camera (`Source` `camera`, `CameraActorLabel`, up to 1920 x 1080) writes PNGs under
`Saved/HyperAIStudio/Captures` and waits for global illumination, sky captures and fog to settle (`bSettle`, up to 6 s;
`bSettleTimedOut` when the picture kept changing). Open the frame and look at it. Its `Metrics` give `LuminanceP5`,
`LuminanceP50`, `LuminanceP95`, `ContrastRatio`, `ColorTemperatureK` and the shadow and highlight shares;
`bShadersStillCompiling` means capture again. Run `hyper_lighting_inspect` again to read back each value set.
