---
name: game-audio
description: Wire a game's sound with HyperAI's audio tools - Sound Cues built from a tree of nodes (random, modulator, mixer, looping, delay, attenuation, enveloper, branch, switch, parameter nodes), the mix (Sound Classes, Sound Mixes, attenuation, concurrency, submixes with reverb, EQ and compressor), paid generated sound effects and music (ElevenLabs, Stable Audio) wrapped in MetaSounds or cues, and experimental procedural MetaSound recipes (wind, rain, engine hum, UI blips, laser zaps). Use it for footstep or impact variation, ambience loops, ducking and mix setup, attenuation and voice limits, or when the user asks to generate a sound or music clip from words.
---

# Game audio: cues, the mix, generated clips and recipes

Six tools. Cues, the mix and recipes are in toolset `HyperAIStudioAudioAuthoring.HyperAIStudioAudioAuthoringToolset`
(pack `audio_metasound`); generated audio in `HyperAIStudioAudioGen.HyperAIStudioAudioGenToolset` (pack
`audio_generation`). Every edit dry-runs by default (`bDryRun` true): read the reply, then send it again with `bDryRun`
false. With approval on, the reply is `awaiting_user_approval`: call `hyper_operation_wait` with its `OperationId` and
`bWaitThroughApproval` true.

- `hyper_sound_asset_inspect`: reads Sound Cues (their graph as plan nodes, and `Issues`), Sound Classes, Sound Mixes,
  attenuation, concurrency, submixes, SoundWaves and MetaSound Sources (their inputs), by `Paths` or a `Folder`.
  `bAllSettings` lists every setting a mix plan can change, by its Unreal property name.
- `hyper_sound_cue_plan`: makes or rebuilds a Sound Cue from `Nodes` in one undo step, lays it out, checks every wave
  exists and nothing dangles, and sets `SoundClass`, `Attenuation`, `Concurrency`, `VolumeMultiplier`, `PitchMultiplier`.
- `hyper_sound_mix_plan`: makes or changes Sound Classes, Sound Mixes, attenuation, concurrency and submixes in one undo step.
- `hyper_metasound_recipe` (experimental): a procedural MetaSound Source from a recipe, its controls exposed as inputs.
- `hyper_audio_generate` and `hyper_audio_generate_status`: paid sound effects and music from words, imported and wrapped.

## Which tool

| The job | Tool |
| --- | --- |
| Variation from SoundWaves you have (footsteps, impacts, UI), the classic way | `hyper_sound_cue_plan` (a Sound Cue) |
| A procedural or parameter-driven sound (wind, engine with RPM, UI tones) | `hyper_metasound_recipe` |
| Sound Classes, Sound Mixes, ducking, attenuation, concurrency, submix effects | `hyper_sound_mix_plan` |
| Read what is there (a cue's graph, a mix, a MetaSound's inputs) | `hyper_sound_asset_inspect` |
| A clip from words, paid, when the project has no suitable waves | `hyper_audio_generate` (always approved first) |
| Designed, layered or musical sound with nothing paid | the `sound-design` skill (Decibel) |
| Revisions, CAS-checked low-level edits, the optional plugins' assets | `hyper_audio_inspect`, `hyper_audio_apply_plan`, `hyper_audio_validate` |

`hyper_audio_apply_plan` only knows five MetaSound node kinds (oscillator, gain, random, math_add, math_multiply):
do not build footsteps or impacts with it.

## Sound Cues

`Nodes` is a tree: each node has an `Id`, a `Kind` and the `Children` ids feeding it, in input order; `Root` (the first
node when empty) is what the cue plays. Each node feeds one input; a node nothing reaches is refused (it would dangle).

- `wave_player` (`Wave`, `bLooping`) is a leaf. `random` (`Weights`, `bNoRepeat`), `mixer` and `concatenator` (`Weights` =
  input volumes) take 1-32 children. `modulator` (`PitchMin/Max`, `VolumeMin/Max`), `looping` (`LoopCount`, 0 forever),
  `delay` (`DelayMin/Max` s), `attenuation` (`Attenuation` asset), `enveloper` (`VolumeCurve`, `PitchCurve` as
  (seconds, value) points, `LoopStart/End`), `continuous_modulator` (`Parameter` drives volume, `PitchParameter` pitch,
  0-1 onto the Min-Max ranges) and `wave_param` (`Parameter`: a SoundWave parameter, else its child) take one.
- `branch` (`Parameter`, a bool): children true, false, unset. `switch` (`Parameter`, an int): unset, then 0, 1, 2...
  Only these two may leave an input empty (`""`), which plays nothing for that value. `param_crossfade` (`Parameter`, a
  float): one `Fades` entry per child, the range where it fades in and out.
- `bNoRepeat` is Unreal's Randomize Without Replacement: every child plays once before any repeats. It is how a cue
  avoids the same footstep twice in a row; there is no separate "avoid last" switch in 5.8.
- An existing cue's graph is replaced only with `bReplaceGraph`. To change one, inspect it (its `CueNodes` and `CueRoot`
  are a ready plan), edit the nodes, send them back. Settings alone need no `Nodes`.

Patterns: **footsteps** - attenuation > modulator (pitch 0.95-1.05, volume 0.85-1) > random (`bNoRepeat`) over 4-8
takes, the Sound Class `SFX`, concurrency limiting to 4. **Surfaces** - a `switch` on `Surface` with one random set per
surface. **Ambient one-shots** (birds, creaks) - looping > delay (2-8 s) > random > modulator. **Speed layers** - a
`param_crossfade` on `Speed` from walk to run takes.

## The mix

`Assets` run in order, so a parent or an adjusted class made earlier in the plan can be named later. Settings go by
Unreal property name with the Details panel's text (`Volume` `0.8`, `bIsUISound` `true`, `DistanceAlgorithm`
`NaturalSound`, `AttenuationShapeExtents` `(X=400,Y=0,Z=0)`, `ResolutionRule` `StopOldest`); a wrong name or value is
refused, naming it. Parents and children are set through `Parent` (`none` detaches), never as settings.

- Classes: one tree, such as Master > SFX, Music, Voice, UI (UI `bIsUISound`, Music `bIsMusic`).
- Ducking: a `sound_mix` with an adjuster on Music (Volume 0.4) and `FadeInTime` 0.25, `FadeOutTime` 0.5, `Duration`
  -1; push it while dialogue plays. `EQBands` (up to 4, gain linear, 1 flat) turn the mix's EQ on.
- Attenuation: footsteps around `FalloffDistance` 2500 with `NaturalSound` and `bAttenuateWithLPF`; voices further.
- Concurrency: `MaxCount` 4 and `StopOldest` for footsteps; `StopLowestPriority` for weapons.
- Submixes: `Effects` of `reverb` (DecayTime, Density, WetLevel...), `eq` (`Bands`, gain in dB) and `compressor`
  (ThresholdDb, Ratio, AttackTimeMsec...). Each is a preset asset beside the submix, or `Preset` names one to reuse.

## Generated sound effects and music (paid)

Providers: `elevenlabs` (sound effects, 0.5-30 s, `bLoop` for a seamless loop, `PromptInfluence` 0-1), `elevenlabs_music`
(3-300 s, `bInstrumental`), `stability` (Stable Audio 2.5 by default, 1-190 s; `stable-audio-3` up to 380 s). Keys are
the user's: `ELEVENLABS_API_KEY`, `STABILITY_API_KEY`. Never ask for one; if the reply is `missing_key`, tell the user
which variable to set.

1. Dry-run (`bDryRun` true, the default). `Variations` lists each take with its length and price; `EstimatedCost` is the
   total, checked against the session's spend cap (`over_spend_cap` means ask the user to raise it or ask for fewer).
2. Send it with `bDryRun` false and a fresh `OperationId`. It always waits for the user's approval, then runs in the
   background: `hyper_operation_wait` (`bWaitThroughApproval` true), then `hyper_audio_generate_status`.
3. Takes land in `Folder` (default `/Game/Generated/Audio`) as `<Name>_01` and on, with a wrapper beside them:
   `metasound_random` (one take per play, never twice running, the pitch and volume ranges), `metasound_one_shot`,
   `metasound_loop`, `sound_cue_random`, or `none`. The files are kept under `Saved/HyperAIStudio/Audio/<operation_id>`.

Prompts: say the source, the action, the material and the space ("heavy oak door slams shut in a stone hall, short
tail"), not adjectives about quality. Make 3-5 takes of a one-shot for variation; one longer take for a bed. ElevenLabs
sends MP3; for a loop that must not click at its seam, prefer `bLoop` with ElevenLabs or a WAV from Stable Audio.

## Recipes (experimental)

An empty `Recipe` lists them with their inputs: `wind_loop` (Intensity, Gust Rate, Brightness), `ui_blip` (Pitch,
Length, Volume), `ui_confirm` (Pitch, Interval, Note Length, Volume), `engine_hum` (RPM, Cylinders, Load, Volume),
`rain_bed` (Intensity, Brightness, Drop Interval), `laser_zap` (Start Pitch, End Pitch, Length, Volume). `Inputs` sets
their defaults. The inputs are MetaSound parameters: drive them from gameplay (an Audio Component's Set Float Parameter,
such as RPM every frame). A recipe is a starting point: say so, and let the user tune it by ear.

## Decibel instead

Use the `sound-design` skill (Decibel, pack `sound_design`) when the sound has to be designed rather than fetched:
layered AAA impacts, creatures, music composed from synths and library notes, measured renders, or when the user wants
nothing paid. Generated clips feed Decibel: its `clip.audio` and `gen.sampler` take a SoundWave in the project by path,
so pass a take's `AssetPath` (such as `/Game/Generated/Audio/SFX_DoorSlam_02`) as `Sound`. Decibel also takes WAV, FLAC,
OGG or AIFF files on disk; ElevenLabs' files are MP3, so use their SoundWaves, while Stable Audio's are WAV.

## Checking

Audio is not heard here: the editor and its tests use a null device, so check the assets, then ask the user to listen.
After a cue plan, read `Issues` (errors stop the save; `empty_input` on a branch or switch is only a warning) and
`DurationSeconds` (-1 loops forever). After a mix plan, `hyper_sound_asset_inspect` the assets: parents list their
children, mixes their adjusters, submixes their effects.
