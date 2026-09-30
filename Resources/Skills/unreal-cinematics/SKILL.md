---
name: unreal-cinematics
description: Shoot, cut and render cinematics with HyperAI's Sequencer and Movie Render tools. Frame shots by shot language (extreme wide to extreme close, eye level to overhead, three-quarter, over the shoulder, lens and camera move) with focus and camera cuts; key actors and properties, fades and audio; build master sequences from shots; review the cut for jump cuts and line crossings; then render drafts and finals with Movie Render Queue configs or Movie Render Graphs. Use for cutscenes, trailers, intros and any camera work in Sequencer.
---

# Cinematics: shoot, cut, render

| work | read | edit |
|---|---|---|
| level sequences: shots, cameras, keys, cuts, fades, audio, master sequences | `hyper_sequence_inspect` | `hyper_sequence_apply_plan` |
| renders: Movie Render Queue configs, Movie Render Graphs, render jobs | `hyper_movie_render_inspect` | `hyper_movie_render_apply_plan` |

A sequence plan is checked before anything changes and applied as one undo step. The sequence is saved, but the
cameras it placed stay unsaved in the level unless `bSave: true`. Every reply ends with `Review`, the cut as a film
editor sees it. A render plan returns `rendering`: wait on its `OperationId` with `hyper_operation_wait` for the
frames. With approval on, both wait in the Activity panel first.

Units: `Location`, `Points` and `Center` are cm, key `Time`s and `Duration`s are seconds, and lenses are mm.

## The loop

1. **Look.** Find where your subjects stand and which way they face. Use `hyper_sequence_inspect` to read an existing
   sequence.
2. **Plan the coverage.** Write the shot list before you place a camera: which moment each shot serves, its size, and
   whose point of view.
3. **Shoot.** Send one plan with `sequence.create` and a `shot.frame` op for each shot, in order.
4. **Review.** Fix every `Review` note, or say why it is intended. Re-framing a shot with the same `Camera` and
   `Time` replaces its keys and its cut, so nothing doubles up.
5. **Render a draft**, look at the frames, then render the final.

## Shot language

`shot.frame` places a cine camera (or reuses `Camera`), frames `Subjects` and cuts to it at `Time` for `Duration`.

| Framing | shows | default lens | stop |
|---|---|---|---|
| `extreme_wide` | the place; people are small in it | 18 mm | f/8 |
| `wide` | the whole scene | 24 mm | f/5.6 |
| `full` | head to toe | 35 mm | f/4 |
| `medium_wide` | from mid-thigh up | 35 mm | f/4 |
| `medium` | from the waist up | 50 mm | f/2.8 |
| `medium_close` | from the chest up | 65 mm | f/2.8 |
| `close` | head and shoulders | 85 mm | f/2 |
| `extreme_close` | the face or the eyes | 100 mm | f/2 |

- `Side` is the camera's place around the subject: `front`, `three_quarter_left`, `three_quarter_right`,
  `profile_left`, `profile_right` or `back`. `Over` names the actor whose shoulder the camera looks past.
  `Side: left` puts it over the left shoulder.
- `Angle` is `eye` (the default), `high` (looking down, which diminishes), `low` (looking up, which empowers),
  `overhead`, or `dutch` (tilted, for unease).
- `Move` is `static`, `push_in` (into emotion), `pull_out` (a reveal), `truck_left` or `truck_right`, `pedestal_up`
  or `pedestal_down`, `orbit_left` or `orbit_right`, `crane_up` or `crane_down`, or `dolly_zoom` (vertigo). Moves
  ease in and out. `bHandheld` adds a small drift, for tension.
- Two `Subjects` make a two-shot, aimed between them from the side they face.
- Focus tracks the first subject; `FocusOn` names another. `FocalLength` and `Aperture` override the defaults.
  `AspectRatio: 2.39` letterboxes for widescreen, and `Filmback` picks a sensor such as `Super 35mm`.

A two-person scene covered the classic way: establish, go closer, shot and reverse shot, then a close-up for the beat.

```json
{"Sequence": "/Game/Cinematics/LS_Confrontation", "bSave": true, "OperationId": "confrontation-1", "Operations": [
 {"Kind": "sequence.create", "FrameRate": 24, "Duration": 14},
 {"Kind": "shot.frame", "Subjects": ["Hero", "Rival"], "Framing": "wide", "Move": "push_in", "Time": 0, "Duration": 4},
 {"Kind": "shot.frame", "Subjects": ["Hero"], "Over": "Rival", "Framing": "medium_close", "Time": 4, "Duration": 3},
 {"Kind": "shot.frame", "Subjects": ["Rival"], "Over": "Hero", "Side": "left", "Framing": "medium_close", "Time": 7, "Duration": 3},
 {"Kind": "shot.frame", "Subjects": ["Hero"], "Framing": "close", "Side": "three_quarter_left", "Angle": "low", "Time": 10, "Duration": 4},
 {"Kind": "fade.add", "Keys": [{"Time": 0, "Value": 1}, {"Time": 0.75, "Value": 0}]},
 {"Kind": "audio.add", "Asset": "/Game/Audio/Music/MX_Confrontation", "Time": 0}]}
```

The two over-the-shoulder shots sit over opposite shoulders (right, then left), which keeps both cameras on the same
side of the line between the two people.

## Editing rules the review enforces

- **The 180-degree line.** Once two people share a shot, every camera stays on one side of the line between them.
  Otherwise they swap sides of the screen.
- **The 30-degree rule.** Two cuts in a row on the same subject need 30 degrees more around them, or a size two steps
  apart (`medium` to `close`). Anything less is a jump cut.
- **Hold the shot.** Under a second is a flash cut. Most shots run 2 to 6 seconds, and a long take should earn it.
- **Coverage.** Every moment of the sequence has a camera. A subject is never hidden behind geometry, and a camera
  never sits inside it.

Beyond the review: cut on action, open a scene wide before going close, and keep close-ups for the moments that
matter.

## By hand

- `camera.add` places a camera at `Location` looking at `LookAt`. `cut.add` cuts to a camera at `Time`.
- `keys.transform` keys an actor's `Location` and `Rotation`, with `Interp` set to `auto`, `linear` or `constant`.
- `keys.property` keys a float, double or bool property, such as `CineCameraComponent.CurrentFocalLength`,
  `CineCameraComponent.FocusSettings.ManualFocusDistance` for a rack focus, or `LightComponent.Intensity`.
- A master sequence holds shots: `shot.add` places each shot's level sequence at `Time`.

## Rendering

```json
{"OperationId": "confrontation-render", "Operations": [
 {"Kind": "config.create", "Asset": "/Game/Cinematics/MRQ_Final", "Preset": "final", "Format": "exr", "Resolution": {"X": 3840, "Y": 2160}},
 {"Kind": "render", "Sequence": "/Game/Cinematics/LS_Confrontation", "Asset": "/Game/Cinematics/MRQ_Final"}]}
```

- `Preset: draft` renders one sample with no warm-up, which is fast, for checking the cut. `final` accumulates eight
  temporal samples after warm-up frames and raises motion blur, depth of field and shadow quality.
- `Format` is `png` for review, `exr` for grading and compositing, `jpg`, `bmp`, or `mp4` (with its plugin).
- `Settings` set anything else by name, such as `output.FileNameFormat`, `camera.ShutterTiming` or `high_res.TileCount`.
  `ConsoleVariables` add console variables.
- A Movie Render Graph (`graph.create`, copied from Epic's default) is tuned by node, such as
  `GlobalOutputSetting.OutputFrameRate`. Pass it as the render's `Asset`.
- `Executor: local` renders here in the editor, off screen, with the level as it is now, for up to ten minutes.
  `remote` renders in a new process from the saved level and assets, and outlives the wait. Frames go to
  `Saved/MovieRenders/<sequence>_<time>` unless `Directory` names another folder, and the reply says where.
