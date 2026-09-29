---
name: sound-design
description: Make sound effects, creature vocals, ambiance loops and background music for the game with Decibel, the project's audio suite, built the way a AAA sound designer builds them, and prove they sound. Build a Decibel session with hyper_sound_apply_plan (layers of sound-library takes, samplers playing library notes, synths, effects, a shared convolution space on returns, automation lanes, reversed and granular clips), render it with hyper_sound_render into a SoundWave and a MetaSound Source, seamless loops included, and measure it. Use it when the user asks for a sound, an SFX, an impact, a creature, a whoosh, a UI click, an ambience or atmosphere, a music bed or theme, a loop or a stinger, and Decibel is installed.
---
# Sound design with Decibel

Three tools, in toolset `HyperAIStudioDecibel.HyperAIStudioDecibelToolset`. They exist only when the Decibel plugin is enabled.

- `hyper_sound_inspect`: `bCatalog` lists every device (generators, audio effects, MIDI effects); `DeviceId` gives one device's parameters with their ranges and defaults; with `bPresets` its presets; `SessionPath` reads a session back, sends and automation included; `bSessions` lists the project's sessions.
- `hyper_sound_apply_plan`: builds or edits a session (`bCreate` for a new one; an existing one is refused, so make a new version per attempt) as one undo step, then saves it. A plan runs on a copy first, so a failing op changes nothing, and the report names the op that failed. Values out of a parameter's range are fitted and listed in `Notes`.
- `hyper_sound_render`: renders the session offline into a SoundWave, with `bMetaSound` a MetaSound Source playing it (looping or one-shot), with `bStems` one SoundWave per track (the track alone, the others muted), and measures it. Its `Issues` are what a listener would hear wrong. The WAV is kept under `Saved/HyperAIStudio/Sound`.

## The loop

1. `hyper_sound_inspect` with `bCatalog`, then `DeviceId` for each device you will set. Never guess a parameter id.
2. `hyper_sound_apply_plan` with the whole session in one plan.
3. `hyper_sound_render`. Read `PeakDb`, `RmsDb`, `LeadingSilenceSeconds` and `Issues`.
4. Measure before you judge (see Measuring), change one thing, render again as a new session version.

Name sessions `DS_<Name>`; the render names the SoundWave `SW_<Name>` and the MetaSound `MS_<Name>` beside it, or `OutputPath` names it. Put them under `/Game/Audio/SFX`, `/Game/Audio/Ambience` or `/Game/Audio/Music`. At 60 BPM (`session.set` `Bpm` 60) a beat is a second, which makes timing arithmetic easy.

## Tracks, clips and devices

- An `audio` track plays `clip.audio`: a SoundWave in the project, or a sound file on disk by its absolute path (WAV, FLAC, OGG, AIFF, such as a sound library's), imported into `/Game/Audio/Library` once and reused after. `SourceStartSeconds` and `SourceSeconds` play one stretch of it: library files often hold several takes one after another, so pick one. `Gain` is a factor (up to 15.8, +24 dB), fades are in beats.
- `Pitch` (semitones) reads the source tape-style, at 2^(pitch/12): pitched down, it plays slower and longer. A stretch of `SourceSeconds` lasts `SourceSeconds / 2^(pitch/12)` on the timeline, so set `LengthBeats` from that; a moment `t` seconds into the stretch lands `t / 2^(pitch/12)` after the clip starts.
- `bReverse` plays the stretch backwards: a reverse swell that ends, at its loudest, on the moment it leads into.
- `StretchMode` `texture` turns the clip into a grain cloud: `Pitch` then sets how fast the grains walk the sound (-12 makes it twice as long), `TextureTranspose` their pitch, `TextureGrainMs` their size and `TextureJitter` (0-1) how far each strays. The end of a roar walked at half speed, a fifth down, jittered, is a tail's residue. `complex` stretches transparently, keeping pitch.
- An `instrument` track plays MIDI (`clip.midi`, notes with pitch 0-127, start and length in beats, velocity 0-1) and trigger clips through the generator at the head of its chain.
- An `fx` track is a return. `track.send` feeds a track into one (`SendTo`, `SendGain` 0-4, `SendTap` post, pre or prefx). `track.set` sets `Volume` (0-4) and `Pan` (-1 to 1).
- `lane.set` automates a track (or the master): `Param` volume (dB), pan (-1 to 1), pitch (semitones), or a device parameter's id with `DeviceId` (in that parameter's units: a filter's `fltcut`, a reverb's mix, a synth's `cutoff`), `Points` of beat, value and curve (below 0 moves most at the start of the segment, as a decay or a closing filter does). A lane holds its first and last values beyond its points. **A volume lane is the fader while it plays**: put the level in the lane, or it plays at the lane's value whatever `Volume` says.

### Instruments from sound-library notes

`gen.sampler` plays a sound across the keys. `device.add` it with `Sound` and `SamplerMode` (`one_shot`, `loop`, `granular`); set `root` to the MIDI key it was recorded at and `pitch` for the cents it is off, measured, never read from the file name; `sampstart`, `loopstart`, `loopend` are fractions of the file, `xfadelen` a fraction of the loop; `ampatk`, `amprel` in seconds. A sound holding a chord (a choir's open fifth) plays that chord on every note. An inharmonic bell transposed keeps partials outside the key: build a tuned bell from `gen.synth` sines instead (hum, prime, minor-third tierce, quint, nominal).

### Effects worth knowing

- `fx.verb` is a convolution reverb and takes a `Sound` as its impulse response: a designed room (a stone cellar, a forest, a hall) makes a stack sound like one place. With a loaded IR, `vdecay` 1 and `vdamp` 0 leave it as designed; `vmix` 1 on a return.
- `fx.eq`: five bands at 80 Hz, 240 Hz, 1 kHz, 3.5 kHz and 10 kHz by default (`eqNfreq` v means 20 x 1000^v Hz); `eqNtype` 0 bell, 1 low shelf, 2 high shelf, 3 notch, 4 high pass, 5 low pass, 6 band pass. `fx.filter` takes the same types.
- `fx.lowend` in Synthesize mode (`lemode` 3) fires a sine AT ITS CUTOFF on every onset it hears, decaying about 7 dB per 100 ms: a tuned sub-thud. Set `lecut` to the sub's frequency (40-55 Hz: v = log10(Hz/20)/3), `lesolo` 1 on a return, and boost it (`fx.util` `utgain`), because it comes back quiet. It does not follow a voice's pitch: a cutoff left at 240 Hz leaves a 240 Hz drone.
- `fx.comp`, `fx.limit`, `fx.shaper` (`shshape` 1 hard clip), `fx.trans` (sharpen attacks), `fx.chorus` and `fx.util` (width, gain), `fx.vocoder` (carriers: noise, sidechain, self, or left channel modulating right).

## The AAA stack (impacts, creatures, attacks)

A sound that feels designed rather than pasted has every one of these, each on its own track:

1. **Anticipation** (a telegraph, before frame zero): a sharp breath in, low stone grinding (pitched down, low-passed) and a creak rising under the load, a reverse swell ending on the strike. It stays well under the strike and stops just before it: the strike lands out of a breath of quiet.
2. **Frame zero**: a snap on top (slap, knuckle, bone splinter, leather), the meat right after it (low smack, low sustained punch body), a sub-thud (a designed low kick plus the fx.lowend thud), and the material it lands in (gravel, concrete, bone, wet flesh). Trim every impact layer to ~20 ms before its loudest moment: a layer that speaks early smears the strike.
3. **Vocal body**: the performance's mid-section a few semitones down, entering 30-60 ms after the strike, with a sine or triangle (`gen.synth`, `apart` 1; for a triangle 16 odd partials at -12 dB/oct) held under it at 40-55 Hz, its level in a volume lane that falls with the voice.
4. **Wet grit**: micro-layers of saliva snaps, slimy throat clicks, gargle and a throat gulp, band-passed (~300 Hz-8 kHz), about 6 dB under the voice.
5. **Glue** in the middle: a band-passed, saturated layer bridging the top and the rumble: metal that screams like a throat, a driven growl, a distorted low-mid impact.
6. **The tail**: debris settling, the voice's last moment smeared into grains, a filter closing (`lane.set` on an `fx.filter`'s `fltcut`) and the pitch sinking (a pitch lane) as the energy drains.
7. **One space**: every layer sends to one return with `fx.verb` on a designed IR (high-passed ~160 Hz so it never muddies), its filter closing over the tail; transients send little, bodies and voices more.

Keep one set of performers across a creature's attack, hit, pain and roars so they sound like one throat; make two or three variants of each by swapping takes.

## Recipes

**Ambience loop.** Beds that run the whole loop plus the crossfade, spot sounds placed well inside it, and ONE shared space: every track sends to a return with a forest (or cave, or hall) IR; distance is the send and a low-pass, not a reverb per sound. Move things: a volume lane that gusts the wind, pan lanes that fly a crow across or circle a whisper, a filter that closes as a sound recedes, a band-passed ghostly bed breathing slowly under it. Render with `DurationSeconds` the loop's length and `LoopCrossfadeSeconds` (such as 4), `bLoop`, `bMetaSound`.

**Music.** One instrument track per part (samplers from library notes, `gen.synth` pad and sub, tuned synth bells), all sending to one hall return. Humanize (a few ms and a little velocity per note, section downbeats left together), shape phrases and sections with volume lanes, open a pad's filter into the climax, build into it (a riser and a reverse swell cresting on its downbeat over a drum roll), put weight under its downbeats (a tuned sub-thud, metal), and let it leave (a downlifter, the hall's filter closing into the breakdown). End the last bar on the chord that leads back to the first; render whole bars with `LoopTailSeconds`.

## Measuring

- Render stems for analysis WITHOUT `NormalizePeakDb` (normalizing scales each stem on its own, so their levels stop being comparable).
- Judge levels K-weighted (the loudness curve: +4 dB above ~1.7 kHz, a high-pass at 38 Hz): raw RMS lets a sub kick outvote a crack the ear hears first.
- Check each layer's windows around frame zero (wind-up, the first 60 ms, the body, the tail) and the band shares: energy under 60 Hz above ~40% wastes headroom (the voice ends up quiet after normalizing); a growl's body sits in 250 Hz-1 kHz.
- `NormalizePeakDb` (such as -1) lands a set at one peak. Decibel's limiter lets the fastest transients through: end a master with `fx.limit` at a ceiling (-4 dB) and a hard clip (`fx.shaper` `shshape` 1, `shdrive` 4, `shgain` -4, `shamt` 1) at the same level, and drive the limiter with `lpregain` only as far as the crest you want (15-17 dB for creatures and impacts).
- A spectrogram shows what numbers miss: a steady line is a drone, a vertical line a click or a hit, a wall a hard cut.

## What does not work yet

- Decibel's Weapon, Impact, Engine and Piano Designers (`gen.weapon`, `gen.impact`, `gen.engine`, `gen.piano`) render silent offline from a session built here; the render says so.
- VST3 plug-ins are left out of offline renders, as Decibel's own bounce does.

## Using the sound

The MetaSound Source plays anywhere a sound does: an Audio Component, Play Sound at Location, an anim notify. So does the SoundWave.
