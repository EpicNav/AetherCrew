---
name: game-audio
description: Wire a game's sound with HyperAI's audio tools - Sound Cues built from a tree of nodes, MetaSound Sources built from any registered node (Wave Player, Random Get, envelopes, filters, noise, triggers, delays), ready recipes from the project's own SoundWaves (footsteps, layered impacts, whooshes, crossfading ambience loops) and procedural ones (wind, rain, engine hum, UI blips, laser zaps), sounds rendered offline and measured (LUFS, peak, onset, centroid, tail, clipping), the editor's preview with solo and mute, the mix (Sound Classes, Sound Mixes, attenuation, concurrency, submixes) and paid generated clips (ElevenLabs, Stable Audio). Use it for footstep or impact variation, whooshes and UI sounds, ambience loops, MetaSound graphs, checking how loud or long a sound is, ducking and mix setup, or when the user asks to generate a sound from words.
---

# Game audio: cues, MetaSounds, recipes, listening, the mix and generated clips

Cues, MetaSounds, recipes, analysis, preview and the mix are in toolset
`HyperAIStudioAudioAuthoring.HyperAIStudioAudioAuthoringToolset` (pack `audio_metasound`); generated audio in
`HyperAIStudioAudioGen.HyperAIStudioAudioGenToolset` (pack `audio_generation`). Every edit dry-runs by default (`bDryRun` true): read the reply, then send it again with `bDryRun`
false. With approval on, the reply is `awaiting_user_approval`: call `hyper_operation_wait` with its `OperationId` and
`bWaitThroughApproval` true.

- `hyper_sound_asset_inspect`: reads Sound Cues (their graph as plan nodes, and `Issues`), Sound Classes, Sound Mixes,
  attenuation, concurrency, submixes, SoundWaves and MetaSound Sources (their inputs), by `Paths` or a `Folder`.
  `bAllSettings` lists every setting a mix plan can change, by its Unreal property name.
- `hyper_sound_cue_plan`: makes or rebuilds a Sound Cue from `Nodes` in one undo step, lays it out, checks every wave
  exists and nothing dangles, and sets `SoundClass`, `Attenuation`, `Concurrency`, `VolumeMultiplier`, `PitchMultiplier`.
- `hyper_sound_mix_plan`: makes or changes Sound Classes, Sound Mixes, attenuation, concurrency and submixes in one undo step.
- `hyper_metasound_recipe`: a ready MetaSound Source (or, for the wave recipes, a Sound Cue) from a recipe, its controls
  exposed as inputs: `footstep`, `impact`, `whoosh`, `crossfade_loop` from the project's SoundWaves; procedural ones too.
- `hyper_metasound_node_search` and `hyper_metasound_node_describe`: the MetaSound node registry, by words, category and
  pin types, and each class's pins (types, defaults, descriptions, enum values).
- `hyper_metasound_graph_plan`: a MetaSound Source from any registered nodes, wires, graph inputs and outputs, in one
  undo step through Epic's MetaSound Builder; or nodes and wires added to an existing source.
- `hyper_audio_analyze`: renders a MetaSound offline (no audio device needed) or reads a SoundWave, and measures it.
- `hyper_audio_preview`: plays a sound in the editor for the user, sets its parameters, lists active sounds and mixes,
  solos and mutes. Nothing is saved; with no audio device only `status` works.
- `hyper_audio_generate` and `hyper_audio_generate_status`: paid sound effects and music from words, imported and wrapped.

## Which tool

| The job | Tool |
| --- | --- |
| Footsteps, impacts, whooshes or an ambience loop from SoundWaves you have | `hyper_metasound_recipe` with `Waves` (a MetaSound, or `Kind` `sound_cue`) |
| Any other MetaSound: your own graph of nodes | `hyper_metasound_node_search`, then `hyper_metasound_graph_plan` |
| A Sound Cue graph of your own (random, switch, crossfade by parameter...) | `hyper_sound_cue_plan` |
| A procedural or parameter-driven sound (wind, engine with RPM, UI tones, whoosh from noise) | `hyper_metasound_recipe` |
| How loud, long, bright or varied a sound is; whether it clips, finishes or clicks | `hyper_audio_analyze` |
| Let the user hear it, solo a class, see what is playing | `hyper_audio_preview` |
| Sound Classes, Sound Mixes, ducking, attenuation, concurrency, submix effects | `hyper_sound_mix_plan` |
| Read what is there (a cue's graph, a mix, a MetaSound's inputs) | `hyper_sound_asset_inspect` |
| A clip from words, paid, when the project has no suitable waves | `hyper_audio_generate` (always approved first) |
| Designed, layered or musical sound with nothing paid | the `sound-design` skill (Decibel) |
| Revisions, CAS-checked low-level edits, the optional plugins' assets | `hyper_audio_inspect`, `hyper_audio_apply_plan`, `hyper_audio_validate` |

`hyper_audio_apply_plan` only knows five MetaSound node kinds (oscillator, gain, random, math_add, math_multiply):
do not build footsteps or impacts with it; use the recipes or `hyper_metasound_graph_plan`.

The loop for any sound: build it (recipe or graph plan), `hyper_audio_analyze` it (several `Plays` for variation), fix
what `Issues` names, then offer `hyper_audio_preview` play so the user hears it.

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

## Recipes

An empty `Recipe` lists them with their inputs. `Inputs` sets their defaults; they are MetaSound parameters, so gameplay
can drive them (an Audio Component's Set Float Parameter, such as RPM every frame).

From the project's own SoundWaves (`Waves`, found with `hyper_sound_asset_inspect` on a `Folder`; nothing paid):

- `footstep`: one take per step, never the same twice running, `Pitch Jitter` semitones up or down and `Volume
  Jitter` (0.15 picks 0.85-1). 4-8 takes of one surface; play it per step. One-shot.
- `impact`: `Waves` are the transient layer (the crack), `Tails` the tail layer (debris, ring, room) started `Tail
  Delay` later; `Transient Gain`, `Tail Gain`. It finishes when both layers have. Tails may be empty.
- `whoosh`: a band pass swept from `Sweep Low` up to `Sweep High` and back over `Length`, through the takes, or pink
  noise when `Waves` is empty.
- `crossfade_loop`: an endless bed from 1 or more takes: two looping players crossfading every `Segment` seconds, each
  picking a new take when it is silent. It fades in over the first `Segment`. One take makes any wave loop seamlessly.

`Kind` `sound_cue` makes the Sound Cue version through the same approval as `hyper_sound_cue_plan`: footstep a
modulator over a no-repeat random; impact a mixer of the two random layers, the tail behind a delay; whoosh without
the sweep; crossfade_loop the takes looped back to back without crossfades.

Procedural: `wind_loop` (Intensity, Gust Rate, Brightness), `ui_blip` (Pitch, Length, Volume), `ui_confirm` (Pitch,
Interval, Note Length, Volume), `engine_hum` (RPM, Cylinders, Load, Volume), `rain_bed` (Intensity, Brightness, Drop
Interval), `laser_zap` (Start Pitch, End Pitch, Length, Volume). They are starting points to tune by ear.

## MetaSound graphs

1. Find each node with `hyper_metasound_node_search` (`Query` "wave player", "random get", "ad envelope", "state
   variable filter", "trigger delay", "noise"; `InputType`/`OutputType` such as Audio or Trigger), then
   `hyper_metasound_node_describe` for its pins. A `Class` is `Namespace.Name.Variant`: `UE.Wave Player.Mono`,
   `Array.Random Get.WaveAsset:Array`, `AD Envelope.AD Envelope.Audio`, `UE.Multiply.Audio by Float`,
   `UE.State Variable Filter.Audio`, `UE.Trigger Delay`, `AudioMixer.Audio Mixer (Mono, 2)`.
2. `hyper_metasound_graph_plan`: `Nodes` (an `Id`, the `Class`, pin `Defaults` by name), `Inputs` (graph inputs:
   `Name`, `Type`, `Default`; a `WaveAsset:Array` input takes a comma list of SoundWave paths), and `Connections`
   (`FromNode`.`FromPin` -> `ToNode`.`ToPin`). Node `graph` is the graph itself: `On Play` and the graph inputs as
   `From`; `Out Mono` (or `Out Left`/`Out Right` with `Format` stereo) and `On Finished` as `To`.
3. Wires join one type: Audio to Audio, Float to Float, Trigger to Trigger. An input takes one wire (mix with a node).
   Enum pins take a name or number; WaveAsset pins an asset path.
4. A one-shot must wire its last player's (or envelope's) `On Finished`/`On Done` to `graph`.`On Finished`, or it plays
   until stopped; the dry run warns of it and of outputs nothing reaches.
5. An existing source: the same plan adds nodes and wires; its nodes are named by the GUIDs `hyper_sound_asset_inspect`
   lists (`node <GUID> <Class>`), with `Class` left empty.

## Listening: hyper_audio_analyze

Renders a MetaSound Source through its own generator, with no audio device (`Method` `metasound_offline`), or reads a
SoundWave's samples (`sound_wave_pcm`). Per play: `IntegratedLufs` (BS.1770, gated), `MaxMomentaryLufs`, `PeakDb`,
`RmsDb`, `OnsetMs`, `PeakMs`, `SpectralCentroidHz` (bright or dark), `TailSeconds` (peak to 60 dB down), `ClippedSamples`,
`bFinished` (one-shots), `LoopSeamDb`; `bEnvelope` adds the level every 20 ms. `Plays` (up to 16) renders fresh plays
and reports `LoudnessSpreadLu` and `CentroidSpreadHz`; `Parameters` sets graph inputs for the render.

Rough targets before the mix: footsteps -30 to -20 LUFS with 1-3 LU of spread, peaks under -1 dBFS, onset under
10 ms; impacts their transient within a few ms and a tail as long as the space; UI sounds short and bright (centroid
above 1.5 kHz). `Issues` names what to fix: silent, clipping, too_quiet, too_loud, late_onset, never_finishes,
clicks_at_seam, no_variation. A Sound Cue is not rendered: analyze its waves.

## Preview and what is playing: hyper_audio_preview

`play` (`Sound`, `StartTime`, `Parameters`), `set` (`Parameters`, `VolumeMultiplier`, `PitchMultiplier` on what is
playing), `stop`, `status` (`ActiveSounds`, `ActiveMixes`, solos and mutes), `solo`/`mute` (`Target` a Sound Class,
Submix, SoundWave or Sound Cue; `bEnable`), `clear`. It is the user's ears, not yours: measure with
`hyper_audio_analyze`, then offer a play. Clear solos and mutes when done.

## Decibel instead

Use the `sound-design` skill (Decibel, pack `sound_design`) when the sound has to be designed rather than fetched:
layered AAA impacts, creatures, music composed from synths and library notes, measured renders, or when the user wants
nothing paid. Generated clips feed Decibel: its `clip.audio` and `gen.sampler` take a SoundWave in the project by path,
so pass a take's `AssetPath` (such as `/Game/Generated/Audio/SFX_DoorSlam_02`) as `Sound`. Decibel also takes WAV, FLAC,
OGG or AIFF files on disk; ElevenLabs' files are MP3, so use their SoundWaves, while Stable Audio's are WAV.

## Checking

Audio is heard through `hyper_audio_analyze` (MetaSounds and SoundWaves): measure every sound you make. After a cue plan, read `Issues` (errors stop the save; `empty_input` on a branch or switch is only a warning) and
`DurationSeconds` (-1 loops forever). After a mix plan, `hyper_sound_asset_inspect` the assets: parents list their
children, mixes their adjusters, submixes their effects.
