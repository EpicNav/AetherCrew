---
name: sound-design
description: Make sound effects, ambiance loops and background music for the game with Decibel, the project's audio suite, and prove they sound. Build a Decibel session with hyper_sound_apply_plan (synths, samplers, drum machine, effects, MIDI and trigger clips, SoundWave layers), render it with hyper_sound_render into a SoundWave and a MetaSound Source, and read its levels. Use it when the user asks for a sound, an SFX, a whoosh, a UI click, an ambience or atmosphere, a music bed, a loop or a stinger, and Decibel is installed.
---
# Sound design with Decibel

Three tools, in toolset `HyperAIStudioDecibel.HyperAIStudioDecibelToolset`. They exist only when the Decibel plugin is enabled.

- `hyper_sound_inspect`: `bCatalog` lists every device (generators, audio effects, MIDI effects); `DeviceId` gives one device's parameters with their ranges and defaults; with `bPresets` its presets; `SessionPath` reads a session back; `bSessions` lists the project's sessions.
- `hyper_sound_apply_plan`: builds or edits a session (`bCreate` for a new one) as one undo step, then saves it. A plan runs on a copy first, so a failing op changes nothing, and the report names the op that failed. Values out of a parameter's range are fitted and listed in `Notes`.
- `hyper_sound_render`: renders the session offline into a SoundWave, with `bMetaSound` a MetaSound Source playing it (looping or one-shot), with `bStems` one SoundWave per track, and measures it. Its `Issues` are what a listener would hear wrong.

## The loop

1. `hyper_sound_inspect` with `bCatalog`, then `DeviceId` for each device you will set. Never guess a parameter id.
2. `hyper_sound_apply_plan` with the whole session in one plan: `session.set`, `track.add`, `device.add`, `device.set` or `device.preset`, clips.
3. `hyper_sound_render`. Read `PeakDb`, `RmsDb`, `LeadingSilenceSeconds` and `Issues`.
4. Change one thing (`device.set`, `clip.remove` and a new clip, `track.set`) and render again until there are no issues and the levels are right.

Name sessions `DS_<Name>`; the render names the SoundWave `SW_<Name>` and the MetaSound `MS_<Name>` beside it. Put them under `/Game/Audio/SFX`, `/Game/Audio/Ambience` or `/Game/Audio/Music`.

## Tracks, devices and clips

- An `instrument` track plays MIDI (`clip.midi`, notes with pitch 0-127, start and length in beats, velocity 0-1) and trigger clips (`clip.trigger`: one hit at the clip's start, held for its length) through the generator at the head of its chain. Add the generator first, then effects.
- An `audio` track plays SoundWaves already in the project (`clip.audio`, with gain, pitch in semitones, fades in beats).
- An `fx` track is a return. Effects on the `master` track shape the whole mix: put `fx.limit` there so nothing clips.
- Beats follow the session's tempo (`session.set` `Bpm`, default 120: one beat is half a second).

## Recipes

**One-shot effect (UI click, zap, whoosh, pickup, hit).** `session.set` `OneShot` 1. An instrument track with `gen.synth`, fired by one `clip.trigger` at beat 0, short (`LengthBeats` 0.25-1). Shape it with the amp envelope: `ampatk` (attack, seconds, near 0 for a click, 0.05-0.3 for a swell), `ampdec`, `ampsus` (0 for a pluck), `amprel`. Colour it with the filter: `flton` 1, `cutoff` (0-1), `res`. Oscillator and noise settings are in the synth's parameters from `hyper_sound_inspect`. Then `fx.delay`, `fx.verb` or `fx.reverb` for space, `fx.shaper` or `fx.pdist` for grit, `fx.eq` or `fx.filter` to fit it. Render with `bMetaSound`. A one-shot must start at once: `LeadingSilenceSeconds` under 0.05.

**Ambience loop.** `session.set` `Loop` 1 with `LoopStartBeat` 0 and `LoopEndBeat` at the loop's end. Audio tracks of the project's SoundWaves (wind, room tone, birds, machinery), each `clip.audio` spanning the loop with fades so the seam is smooth; vary their `Volume` and `Pan` with `track.set`. `fx.verb` and `fx.eq` on the master. Render with `bLoop` and `bMetaSound`: the render takes the loop region, and the SoundWave and MetaSound loop.

**Music bed.** `session.set` `Bpm`, time signature, and a loop over whole bars (4 beats a bar in 4/4). One instrument track per part, each with `gen.synth` (or `gen.sampler`): a pad of long chord notes (three or four notes a chord, a bar each), a bass of root notes an octave or two down, a lead or arpeggio on top. Keep every part in one key. `fx.chorus` and `fx.verb` on pads, `fx.comp` on the master before `fx.limit`. Render with `bLoop`, `bMetaSound` and `bStems` so each part can be mixed or swapped in the game.

## Levels

- Peak: at most -1 dBFS; `bClipped` means lower track volumes or add `fx.limit` on the master.
- Average (`RmsDb`): one-shots around -18 to -10 dBFS, ambience beds around -30 to -20, music around -20 to -14. A loud effect in a quiet bed is the point; a bed as loud as the effects is not.
- Silent means a track has no generator, no clips, or its clips sit outside the render.

## What does not work yet

- Decibel's Weapon, Impact, Engine and Piano Designers (`gen.weapon`, `gen.impact`, `gen.engine`, `gen.piano`) render silent offline from a session built here; the render says so. Build guns, impacts and engines from `gen.synth`, `gen.sampler`, the project's SoundWaves and effects for now.
- VST3 plug-ins are left out of offline renders, as Decibel's own bounce does.

## Using the sound

The MetaSound Source plays anywhere a sound does: an Audio Component, Play Sound at Location, an anim notify. So does the SoundWave.
