---
name: image-match
description: Make a scene, environment, material, effect or UI look like a reference image, and prove it. Capture the attempt, score it against the reference with hyper_image_compare in a session, change one thing, and compare again until the score reaches the target or stops improving. Use it when the user has concept art, a photo or a generated image and says "make it look like this".
---
# Match a reference image

In Claude Code, the same loop runs as a workflow, `/hyperai-match <reference image> <what to match>`. Prefer that when it is available.

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

## 4. Report

- The score of each iteration, and what changed in it (the session `history` has both).
- The best iteration's side-by-side path, and what still differs, in your words and the report's.
- The user can see the whole session in the HyperAI Preview tab: pick the **Image match** source and the same reference. Every iteration shows as a strip beside it.

## Rules

- Change the project only through the domain tools. Their edits wait for approval like any other.
- Do not save between iterations unless the user asks. Lighting edits are unsaved by default, so a bad iteration is one undo away.
- A high score with an image that looks wrong is not a match. Say what you see.
