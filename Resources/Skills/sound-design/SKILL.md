---
name: sound-design
description: Make sound effects, creature vocals, ambiance loops and background music for the game with Decibel, the project's audio suite, and prove they sound. Build a Decibel session with hyper_sound_apply_plan (synths, samplers playing sound-library notes, drum machine, effects, MIDI and trigger clips, layers of SoundWaves or of sound files on disk), render it with hyper_sound_render into a SoundWave and a MetaSound Source, seamless loops included, and read its levels. Use it when the user asks for a sound, an SFX, an impact, a creature, a whoosh, a UI click, an ambience or atmosphere, a music bed or theme, a loop or a stinger, and Decibel is installed.
---
# Sound design with Decibel

Three tools, in toolset `HyperAIStudioDecibel.HyperAIStudioDecibelToolset`. They exist only when the Decibel plugin is enabled.

- `hyper_sound_inspect`: `bCatalog` lists every device (generators, audio effects, MIDI effects); `DeviceId` gives one device's parameters with their ranges and defaults; with `bPresets` its presets; `SessionPath` reads a session back; `bSessions` lists the project's sessions.
- `hyper_sound_apply_plan`: builds or edits a session (`bCreate` for a new one; an existing one is refused, so make a new version per attempt) as one undo step, then saves it. A plan runs on a copy first, so a failing op changes nothing, and the report names the op that failed. Values out of a parameter's range are fitted and listed in `Notes`.
- `hyper_sound_render`: renders the session offline into a SoundWave, with `bMetaSound` a MetaSound Source playing it (looping or one-shot), with `bStems` one SoundWave per track, and measures it. Its `Issues` are what a listener would hear wrong. The WAV is kept under `Saved/HyperAIStudio/Sound`.

## The loop

1. `hyper_sound_inspect` with `bCatalog`, then `DeviceId` for each device you will set. Never guess a parameter id.
2. `hyper_sound_apply_plan` with the whole session in one plan: `session.set`, `track.add`, `device.add`, `device.set` or `device.preset`, clips.
3. `hyper_sound_render`. Read `PeakDb`, `RmsDb`, `LeadingSilenceSeconds` and `Issues`.
4. Change one thing and render again, as a new session version, until there are no issues and the levels are right.

Name sessions `DS_<Name>`; the render names the SoundWave `SW_<Name>` and the MetaSound `MS_<Name>` beside it, or `OutputPath` names it. Put them under `/Game/Audio/SFX`, `/Game/Audio/Ambience` or `/Game/Audio/Music`. At 60 BPM (`session.set` `Bpm` 60) a beat is a second, which makes timing arithmetic easy.

## Tracks, devices and clips

- An `audio` track plays `clip.audio`: a SoundWave in the project, or a sound file on disk by its absolute path (WAV, FLAC, OGG, AIFF, such as a sound library's), imported into `/Game/Audio/Library` once and reused after. `SourceStartSeconds` and `SourceSeconds` play one stretch of it: library files often hold several takes one after another, so pick one. `Gain` is a factor (up to 8), fades are in beats.
- `Pitch` (semitones) reads the source tape-style, at 2^(pitch/12): pitched down, it plays slower and longer. A stretch of `SourceSeconds` lasts `SourceSeconds / 2^(pitch/12)` on the timeline, so set `LengthBeats` from that, and a moment `t` seconds into the stretch lands `t / 2^(pitch/12)` after the clip starts.
- An `instrument` track plays MIDI (`clip.midi`, notes with pitch 0-127, start and length in beats, velocity 0-1) and trigger clips (`clip.trigger`: one hit at the clip's start, held for its length) through the generator at the head of its chain. Add the generator first, then effects.
- An `fx` track is a return. Effects on the `master` track shape the whole mix. `track.set` sets a track's `Volume` (0-4) and `Pan` (-1 to 1).

### Instruments from sound-library notes

`gen.sampler` plays a sound across the keys. `device.add` it with `Sound` (a SoundWave or a file on disk) and `SamplerMode`: `one_shot` plays the sound once per note, `loop` holds it for as long as the note, `granular` clouds it. Then its params:

- `root`: the MIDI key the sound was recorded at, and `pitch` in semitones for the cents it is off (such as -0.12). Measure it before you trust a file name: find the note's fundamental and its strongest partials. A root an octave off plays everything an octave off; a root a semitone off plays every note out of key.
- `sampstart`, `loopstart`, `loopend` are fractions of the whole file, so a file holding several notes can be read from one of them; `xfadelen` is the loop's crossfade as a fraction of the loop. Loop inside the steady middle of the note, after its attack and before its decay.
- `ampatk`, `ampdec`, `ampsus`, `amprel` in seconds: 0.25-1 s attacks for strings and choirs, 1-2.5 s releases.
- A sound holding a chord or an open fifth (a choir, an organ) plays that chord on every note: roots alone make the harmony, and melodies come out in parallel fifths.
- An inharmonic sound (a gong, most bells) has partials that are not notes of the key; transposing it moves them all. When they clash, build the bell from `gen.synth` sines instead: a church bell's hum, prime, minor-third tierce, quint and nominal are notes of a minor chord.

`fx.verb` takes a `Sound` too, as its impulse response.

### Effects worth knowing

- `fx.eq`: five bands, at 80 Hz, 240 Hz, 1 kHz, 3.5 kHz and 10 kHz by default (`eqNfreq` v means 20 x 1000^v Hz). `eqNtype`: 0 bell, 1 low shelf, 2 high shelf, 3 notch, 4 high pass, 5 low pass, 6 band pass. A narrow notch (`eqNq` 4) removes one partial that clashes with the key.
- `fx.filter` (`fltype` the same numbers): a low pass is distance; so is a big `fx.reverb` with more mix.
- `fx.reverb`: `rvdecay` seconds, `rvsize`, `rvmix`, `rvpredly` ms, `rvlocut` to keep the wet signal out of the lows.
- `fx.comp`, `fx.limit` (`lceil`, `lpregain`, `llook` ms), `fx.shaper` (`shshape` 0 soft, 1 hard clip), `fx.trans`, `fx.lowend`, `fx.chorus` for width, `fx.util` for gain and width.

## Recipes

**Layered SFX from a sound library (a punch, an impact).** A track per layer: the body, a lower thud, a bright snap, a crack a few ms late, a sub and a boom for weight, a whoosh before contact. Find each take's loudest moment and align them all on one time, so they hit as one; trim what comes before a layer's hit, or it smears the attack. Master: EQ for body and bite, transient shaper, compressor, a little shaper grit, a short room.

**Creature vocals.** Human or animal performances pitched 3-6 semitones down give size; snarls give rasp; boss and giant vocals give chest; a sub or rumble gives weight to the largest. Keep one set of performers across a creature's attack, hit, pain and roars so they sound like one throat. Align layers at their onsets. A large roar can breathe in first.

**One-shot synth effect (UI click, zap, pickup).** `session.set` `OneShot` 1. An instrument track with `gen.synth`, fired by one `clip.trigger` at beat 0, short (`LengthBeats` 0.25-1). Shape it with the amp envelope (`ampatk` near 0 for a click, `ampsus` 0 for a pluck) and the filter (`flton` 1, `cutoff` 0-1, `res`), then space and grit. A one-shot must start at once: `LeadingSilenceSeconds` under 0.05.

**Ambience loop.** Beds that run the whole loop plus the crossfade (wind, drones, room tone), and spot sounds around the listener: each on its own panned track with a low pass and a big reverb for distance, all placed well inside the loop so none crosses its end. Render with `DurationSeconds` the loop's length, `LoopCrossfadeSeconds` (such as 4: the render runs past the end and crossfades that onto the start, so the loop point is seamless), `bLoop` and `bMetaSound`. Keep the beds a few dB under the spot sounds, or the forest is only wind.

**Music.** `session.set` `Bpm` and a time signature. One instrument track per part: samplers from library notes (strings, choir, organ, horns), `gen.synth` for a pad (detuned saws through a low pass, slow attack and release) and a sub (`apart` 1, a sine), bells and drums as audio clips on the beat. Keep every part in one key, voice chords close so they move smoothly, and write the dynamics into velocities: a sparse intro, the build, the climax, a breakdown. End the last bar on the chord that leads back to the first. Render with `DurationSeconds` whole bars, `LoopTailSeconds` (such as 6: what rings past the loop point is added onto the start, which keeps its first beat), `bLoop`, `bMetaSound`, and `bStems` when each part should be mixable in the game.

## Levels

- `NormalizePeakDb` (such as -1) scales the finished render so its loudest sample lands there: a set of sounds shares one peak, and nothing clips.
- Decibel's limiter lets the fastest transients through (its detector averages the left and right power and smooths it), so the loudest spike, not the body, decides the level. End the master with `fx.limit` at a ceiling (say -4 dB), then `fx.shaper` with `shshape` 1 (hard clip), `shdrive` 4, `shgain` -4 and `shamt` 1, which clips at the same level and catches only what got past. Drive the limiter with `lpregain` so the body reaches it: render once with normalizing off, read `RmsDb`, and set the pre-gain to bring the sound near the crest you want (13-16 dB for creatures and impacts, more for music).
- Average (`RmsDb`): one-shots around -18 to -10 dBFS, ambience beds around -30 to -20, music around -20 to -14. A loud effect in a quiet bed is the point; a bed as loud as the effects is not.
- Silent means a track has no generator, no clips, or its clips sit outside the render.

## What does not work yet

- Decibel's Weapon, Impact, Engine and Piano Designers (`gen.weapon`, `gen.impact`, `gen.engine`, `gen.piano`) render silent offline from a session built here; the render says so. Build guns, impacts and engines from `gen.synth`, `gen.sampler`, the project's sounds and effects for now.
- VST3 plug-ins are left out of offline renders, as Decibel's own bounce does.

## Using the sound

The MetaSound Source plays anywhere a sound does: an Audio Component, Play Sound at Location, an anim notify. So does the SoundWave.
