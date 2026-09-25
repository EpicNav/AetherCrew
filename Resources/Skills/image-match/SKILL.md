---
name: image-match
description: Make a scene, environment, level layout, material, effect or UI look like a reference image, and prove it. Capture the attempt, score it against the reference with hyper_image_compare in a session, change one thing, and compare again until the score reaches the target or stops improving. Use it when the user has concept art, a photo or a generated image and says "make it look like this", "match this lighting" or "build this level", and for any HyperAI Match job. Covers Ultra Dynamic Sky.
---
# Match a reference image

In Claude Code, the same loop runs as a workflow, `/hyperai-match <reference image> <what to match>`. Prefer that when it is available.

The Match page's **Match it** starts this loop as a HyperAI Match job: it opens a chat with the user's preferred agent, gives it the reference and a session id, and after each of the agent's turns reads the session and sends it back to work with what still differs, until the target. In a job, use the session id and target the job gives you, and end each turn with a one-line status.

A match is a loop with a scorekeeper. You make the changes. `hyper_image_compare` (toolset `HyperAIStudioLighting.HyperAIStudioLightingToolset`) says how close you are, where the attempt differs, and when to stop. It changes nothing in the project.

## 1. Before the first attempt

- **Look at the reference yourself.** Open the PNG with your image reader. Say in a sentence what makes the look: time of day, key light, palette, contrast, mood, the main shapes. The numbers measure; you decide what matters.
- **No reference yet?** Generate one with `hyper_image_generate`; see `.hyperai/docs/HyperAIStudio-ImageGenCookbook.md`. If your client cannot draw (Claude Code), use `model_id: codex_image_gen`.
- **Pick the mode:**
  - `aligned`: the reference frames the same view as your capture, such as a photo from a matched camera, a material ball against its own preview, a UI mock at the same size, or an earlier capture.
  - `look`: concept art or a generated image that will never line up pixel for pixel. Structure is not scored there; palette, tone, layout and detail are.
- **Pick a target:**
  - About 85 in aligned mode.
  - About 75 in look mode against concept art, which no render reaches exactly.
  - Raise it when the user asks for "perfect".
- **Lock exposure** before a scene or environment match: `hyper_lighting_apply_plan` with `lock_exposure`. Otherwise every change is re-brightened by the eye adapting, and the score chases it.

## 2. Capture the attempt, by domain

| Domain | Capture | Levers, strongest first |
|---|---|---|
| Scene, environment | `hyper_capture_view` from a camera framed like the reference (source `camera`, the reference's aspect ratio) | `hyper_lighting_apply_plan`: sun pitch, yaw, temperature and intensity; sky light; fog; sky atmosphere; post process. For a painted grade, such as a warm sky over violet shadows, grade apart with `shadows_gain`, `shadows_offset` and `highlights_gain`. Then placement and materials. |
| Material | A camera on a preview mesh in the level, captured with `hyper_capture_view`; or the asset preview a material edit writes | Base colour, roughness, metallic, normal strength, emissive, texture tiling: the material tools or parameter edits |
| Effect (Niagara) | `hyper_capture_view` with `frameCount` 3-4 and `intervalSeconds` 0.25; compare the frame at the reference's moment | Spawn rate, colour over life, size, lifetime, velocity: the Niagara tools |
| UI | A playtest `Capture` step in PIE, at the mock's resolution | Widget layout, colours, fonts, sizes: the UI tools |

### A sky built from a Blueprint: Ultra Dynamic Sky

`hyper_lighting_inspect` lists such a sky as a `sky_system` record, with the variables that move its look and their values. Ultra Dynamic Sky, Ultra Dynamic Weather, the engine's Sun Sky and the old Sky Sphere are recognised, child Blueprints of them too.

- Ultra Dynamic Sky owns its sun, moon, sky light, fog and atmosphere. `set_sun`, `set_sky_light`, `set_fog` and `set_sky_atmosphere` are refused while it is in the level: an edit to a light it owns would be undone on its next update, and adding one would give the level a second sun.
- Change it with `set_sky_system`. The property is a variable by the name its Details panel shows, and the value is a number, true or false, an enum's display name, or Unreal's text form such as `(R=1,G=0.6,B=0.3,A=1)`. Leave `actor_label` empty and the first sky with that variable takes it.
- `lock_exposure` also sets its Apply Exposure Settings to false, since the sky's own exposure would move a locked exposure again; `unlock_exposure` sets it back.
- Its lights are not in physical units, so the EV100 to lock at is far below real sunlight's 15, often -4 to 2. Lock, capture, and move the EV100 until the frame's `luminanceP50` is near 0.18: a black or white capture means the exposure is wrong, not the light.
- Post process still works beside it: `set_post_process` for the grade.

| What the reference shows | Ultra Dynamic Sky variables |
|---|---|
| Time of day, the sun's height | Time of Day (0 to 2400: 600 dawn, 1200 noon, 1800 dusk) |
| The sun's direction | Sun Yaw, Sun Pitch |
| Key light strength and colour | Sun Light Intensity, Sun Light Color; Moon Light Intensity at night |
| Fill from the sky | Sky Light Intensity, Night Brightness |
| Overcast, haze, murk | Cloud Coverage, Fog, Dust Amount, Volumetric Fog |
| Overall brightness and grade | Overall Intensity, Saturation, Contrast, the Exposure Bias variables for day, dawn and dusk, night and cloudy |

With Ultra Dynamic Weather in the level, cloud coverage, fog, dust, rain and snow come from it: set them there, for example its Weather preset or its Rain and Snow.

- With no `candidateImagePath`, `hyper_image_compare` captures the active level viewport. That is fine for a quick look, but a named camera repeats exactly, so use one for a loop.
- After changing many lights at once, wait a few seconds before capturing: a real-time sky light takes time to catch up. If a score jumps with no reason, capture again with nothing changed before you believe it.

## 3. The loop

Call `hyper_image_compare` every iteration with the same `sessionId` (letters, digits, - and _) and a `note` saying what you changed:

```json
{"request": {"referenceImagePath": "Content/References/Generated/dusk/dusk_01.png",
  "candidateImagePath": "Saved/HyperAIStudio/Captures/dusk-cam/frame_00.png",
  "mode": "look", "targetScore": 75, "sessionId": "dusk_lighting", "note": "sun pitch -8, temperature 3800 K", "grid": 4}}
```

Each call:

1. **Read the report.**
   - `score`, and each measure (`paletteScore`, `toneScore`, `layoutScore`, `detailScore`, and `structureScore` in aligned mode).
   - The overall deltas: `exposureDeltaEV` (positive: brighten), `colorTemperatureDeltaK`, `saturationDelta`, `contrastRatioDelta`.
   - `regions`, worst first. Each has a name, a difference in words, and both average colours.
   - `suggestions`, strongest first.
2. **Open `sideBySidePath` and look.** The worst cells are outlined in red. `diffPath` is a heat map of where the colours differ.
3. **Make one change**, or one coherent group, such as the sun's angle and temperature together. Take the strongest suggestion or the worst region, and pick the lever from the table above.
4. **Capture again and compare again,** with a new note.

Stop when:

- `bMatched` is true: the score reached the target. Keep that iteration.
- `bPlateau` is true: three iterations without a point gained. Change lever categories, for example from light to materials, or from colour to placement. If a second plateau follows, stop and keep the best iteration.
- You reach 12 iterations. Stop and keep the best (`bestIteration`, `bestScore`).
- The report says `session_reference_changed`: that session belongs to another reference. Start a new `sessionId`.

## Building a level to match a reference

When the reference is a place to build, not only a light to match, work as a level designer first and a lighting artist last:

1. **Read the reference:** where its camera stands (eye height, angle, lens), the big forms and where they sit (ground, walls, buildings, landmarks, water, foliage), their materials, the time of day and the light.
2. **Frame it.** Place a CineCameraActor labelled `HyperAI_MatchCamera` where the reference's camera would stand, at the reference's aspect ratio, and capture that camera for every attempt.
3. **Use what the project has.** Search the content for meshes and materials that fit before reaching for engine shapes, and recall project memory for its conventions.
4. **Big to small, at real scale:**
   - Block out the largest forms first, then the mid-size forms, then detail. The level design pack's player metrics give the scale: a person is 180 cm, a door 220 cm high.
   - Put things on the ground by tracing down.
   - Keep the space playable, with paths a player can walk.
5. **Materials next, lighting last,** with the lighting steps above.
6. **Levers by kind:** layout and silhouette (placement, scale, the shapes still missing), then materials (colour, roughness), then light, then the grade. A plateau means the next kind.
7. **Save the level** when the score reaches the target.

## 4. Report

- The score of each iteration, and what changed in it (the session `history` has both).
- The best iteration's side-by-side path, and what still differs, in your words and the report's.
- The user can see the whole session in HyperAI Studio's Match page: pick the **Image match** source and the same reference. Every iteration shows as a strip beside it.

## Rules

- Change the project only through the domain tools. Their edits wait for approval like any other.
- Do not save between iterations unless the user asks. Lighting edits are unsaved by default, so a bad iteration is one undo away.
- A high score with an image that looks wrong is not a match. Say what you see.
