// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioDecibelToolset.generated.h"

USTRUCT(BlueprintType)
struct FHyperAISoundInspectRequest
{
	GENERATED_BODY()

	/** A Decibel session to describe, such as /Game/Audio/SFX/DS_RifleShot. */
	UPROPERTY() FString SessionPath;
	/** One device's parameters, by its catalog id (gen.weapon, fx.verb...); with bPresets, its presets. */
	UPROPERTY() FString DeviceId;
	/** Every device Decibel offers: generators, audio effects, MIDI effects. */
	UPROPERTY() bool bCatalog = false;
	/** The presets of DeviceId, or of every device that has them. */
	UPROPERTY() bool bPresets = false;
	/** The Decibel sessions in the project. */
	UPROPERTY() bool bSessions = false;
};

USTRUCT(BlueprintType)
struct FHyperAISoundDeviceInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Id;
	UPROPERTY() FString Name;
	/** generator, audio_effect, midi_effect or modulator. */
	UPROPERTY() FString Kind;
	UPROPERTY() FString Category;
	UPROPERTY() FString Description;
};

USTRUCT(BlueprintType)
struct FHyperAISoundParamInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Id;
	UPROPERTY() FString Label;
	UPROPERTY() FString Description;
	UPROPERTY() float Value = 0.f;
	UPROPERTY() float Min = 0.f;
	UPROPERTY() float Max = 1.f;
	UPROPERTY() float Default = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAISoundClipInfo
{
	GENERATED_BODY()

	/** audio, midi or trigger. */
	UPROPERTY() FString Kind;
	UPROPERTY() double StartBeat = 0.0;
	UPROPERTY() double LengthBeats = 0.0;
	UPROPERTY() int32 Notes = 0;
	/** An audio clip's SoundWave. */
	UPROPERTY() FString Sound;
};

USTRUCT(BlueprintType)
struct FHyperAISoundTrackInfo
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = 0;
	UPROPERTY() FString Name;
	/** audio, instrument or fx. */
	UPROPERTY() FString Type;
	UPROPERTY() float Volume = 1.f;
	UPROPERTY() float Pan = 0.f;
	/** The device chain, in order, by catalog id. */
	UPROPERTY() TArray<FString> Devices;
	UPROPERTY() TArray<FHyperAISoundClipInfo> Clips;
};

USTRUCT(BlueprintType)
struct FHyperAISoundInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAISoundDeviceInfo> Catalog;
	/** DeviceId's parameters, with their defaults and ranges. */
	UPROPERTY() TArray<FHyperAISoundParamInfo> Params;
	/** Preset names as <device id>/<preset>, for device.preset. */
	UPROPERTY() TArray<FString> Presets;
	UPROPERTY() TArray<FString> Sessions;
	UPROPERTY() FString SessionPath;
	UPROPERTY() int32 Bpm = 0;
	UPROPERTY() int32 TimeSigNumerator = 0;
	UPROPERTY() int32 TimeSigDenominator = 0;
	/** To the end of the last clip. */
	UPROPERTY() double LengthBeats = 0.0;
	UPROPERTY() double LengthSeconds = 0.0;
	UPROPERTY() bool bOneShot = false;
	UPROPERTY() bool bLoop = false;
	UPROPERTY() double LoopStartBeat = 0.0;
	UPROPERTY() double LoopEndBeat = 0.0;
	UPROPERTY() TArray<FHyperAISoundTrackInfo> Tracks;
	UPROPERTY() TArray<FString> MasterDevices;
};

USTRUCT(BlueprintType)
struct FHyperAISoundNote
{
	GENERATED_BODY()

	/** MIDI note, 0-127: 60 is middle C. */
	UPROPERTY() int32 Pitch = 60;
	/** From the clip's start, in beats. */
	UPROPERTY() double StartBeat = 0.0;
	UPROPERTY() double LengthBeats = 1.0;
	/** 0-1. */
	UPROPERTY() float Velocity = 0.8f;
};

USTRUCT(BlueprintType)
struct FHyperAISoundParamValue
{
	GENERATED_BODY()

	/** A parameter id from hyper_sound_inspect with DeviceId, such as wprpm. */
	UPROPERTY() FString Id;
	/** In the parameter's own units and range; a value outside it is fitted to the nearest end. */
	UPROPERTY() float Value = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAISoundOp
{
	GENERATED_BODY()

	/**
	 * session.set, track.add, track.set, track.remove, device.add, device.set, device.preset, device.remove, clip.midi,
	 * clip.trigger, clip.audio or clip.remove.
	 */
	UPROPERTY() FString Kind;
	/** The track: its index from 0, its name, or master for the master chain's devices. */
	UPROPERTY() FString Track;
	/** track.add, track.set: the track's name. */
	UPROPERTY() FString Name;
	/** track.add: audio (plays SoundWaves), instrument (MIDI and trigger clips into a generator) or fx (a return). */
	UPROPERTY() FString Type;
	/** device.add: a catalog id such as gen.weapon or fx.verb. device.set, preset, remove: the device on the track, by id. */
	UPROPERTY() FString DeviceId;
	/** Which device on the track, from 0 in chain order; -1 picks the first with DeviceId. */
	UPROPERTY() int32 Device = -1;
	/** device.preset: a preset name from hyper_sound_inspect bPresets, without the device id. */
	UPROPERTY() FString Preset;
	/** device.set: parameter values. */
	UPROPERTY() TArray<FHyperAISoundParamValue> Params;
	/** clip.*: where the clip starts, in beats from the session's start. */
	UPROPERTY() double StartBeat = 0.0;
	/** clip.*: its length in beats; 0 takes a trigger's 1 beat, a MIDI clip's last note, or an audio clip's SoundWave. */
	UPROPERTY() double LengthBeats = 0.0;
	/** clip.midi: its notes. */
	UPROPERTY() TArray<FHyperAISoundNote> Notes;
	/**
	 * clip.audio: a SoundWave in the project, or a sound file on disk (WAV, FLAC, OGG, AIFF, such as a sound library's),
	 * imported into /Game/Audio/Library once and reused after. device.add, device.set: the same, as the sample gen.sampler
	 * plays across the keys (set its root param to the key the sound was recorded at) or fx.verb's impulse response.
	 */
	UPROPERTY() FString Sound;
	/** device.add, device.set on gen.sampler: one_shot plays the sample once per note, loop holds it (loopstart, loopend, xfadelen), granular clouds it. */
	UPROPERTY() FString SamplerMode;
	/**
	 * clip.audio: where in the sound the clip begins, and how much of it plays, in seconds; 0 plays to its end. Library
	 * files often hold several takes one after another: pick one this way.
	 */
	UPROPERTY() double SourceStartSeconds = 0.0;
	UPROPERTY() double SourceSeconds = 0.0;
	/** clip.audio: gain as a factor, and pitch in semitones. */
	UPROPERTY() float Gain = 1.f;
	UPROPERTY() float Pitch = 0.f;
	/** clip.audio: fades, in beats. */
	UPROPERTY() double FadeInBeats = 0.0;
	UPROPERTY() double FadeOutBeats = 0.0;
	/** track.set: volume as a factor, -1 leaves it; pan from -1 (left) to 1, -2 leaves it. */
	UPROPERTY() float Volume = -1.f;
	UPROPERTY() float Pan = -2.f;
	/** clip.remove: which clip on the track, from 0 in the order hyper_sound_inspect lists them. */
	UPROPERTY() int32 Clip = -1;
	/** session.set: tempo, and time signature; 0 leaves each. */
	UPROPERTY() int32 Bpm = 0;
	UPROPERTY() int32 TimeSigNumerator = 0;
	UPROPERTY() int32 TimeSigDenominator = 0;
	/** session.set: 1 on, 0 off, -1 leaves it. OneShot plays once and ends; Loop repeats LoopStartBeat to LoopEndBeat. */
	UPROPERTY() int32 OneShot = -1;
	UPROPERTY() int32 Loop = -1;
	UPROPERTY() double LoopStartBeat = -1.0;
	UPROPERTY() double LoopEndBeat = -1.0;
};

USTRUCT(BlueprintType)
struct FHyperAISoundPlanRequest
{
	GENERATED_BODY()

	/** The Decibel session, such as /Game/Audio/SFX/DS_RifleShot. */
	UPROPERTY() FString SessionPath;
	/** Make the session first; it must not exist yet. */
	UPROPERTY() bool bCreate = false;
	UPROPERTY() TArray<FHyperAISoundOp> Operations;
	/** Only check the plan against a copy: nothing changes. */
	UPROPERTY() bool bDryRun = false;
	/** Names the edit, for hyper_operation_wait when it waits for approval; made when empty. */
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAISoundPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, checked (a dry run), awaiting_user_approval, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString SessionPath;
	UPROPERTY() int32 Applied = 0;
	/** Values fitted into range, and anything else worth knowing. */
	UPROPERTY() TArray<FString> Notes;
	/** The session's tracks after the plan. */
	UPROPERTY() TArray<FHyperAISoundTrackInfo> Tracks;
};

USTRUCT(BlueprintType)
struct FHyperAISoundRenderRequest
{
	GENERATED_BODY()

	UPROPERTY() FString SessionPath;
	/** The SoundWave to make or replace, such as /Game/Audio/SFX/SW_RifleShot; beside the session, named SW_<session>, when empty. */
	UPROPERTY() FString OutputPath;
	/** Where to start, in seconds. */
	UPROPERTY() double StartSeconds = 0.0;
	/** How long; 0 renders to the end of the last clip plus TailSeconds, or the loop when the session loops. */
	UPROPERTY() double DurationSeconds = 0.0;
	/** Room for reverb and decay after the last clip, in seconds. */
	UPROPERTY() double TailSeconds = 1.0;
	UPROPERTY() int32 SampleRate = 48000;
	/** 1 or 2. */
	UPROPERTY() int32 Channels = 2;
	/** Also one SoundWave per track beside the mix, named <output>_<track>. */
	UPROPERTY() bool bStems = false;
	/** The SoundWave loops: ambiance and music beds. */
	UPROPERTY() bool bLoop = false;
	/** Also a MetaSound Source playing it, looping or one-shot as above, named MS_<name> beside it. */
	UPROPERTY() bool bMetaSound = false;
	/**
	 * Scale the finished render so its loudest sample lands here, in dBFS, such as -1; 0 leaves it. Decibel's limiter lets
	 * the fastest transients through, and a WAV clips anything over full scale; this makes a set of sounds sit at one peak.
	 */
	UPROPERTY() float NormalizePeakDb = 0.f;
	/**
	 * A seamless loop, for a looping bed: the render runs this many seconds past its end and that overrun is crossfaded over
	 * its start, so the last sample leads into the first and whatever still rings at the loop point wraps around. 0 = off.
	 */
	UPROPERTY() double LoopCrossfadeSeconds = 0.0;
	/**
	 * A music loop, whose notes end at the loop point: the render runs this many seconds past its end and adds that tail (the
	 * releases and reverb) onto its start, which plays at full level from its first beat. 0 = off; not with LoopCrossfadeSeconds.
	 */
	UPROPERTY() double LoopTailSeconds = 0.0;
	/** Only check the request: nothing is rendered. */
	UPROPERTY() bool bDryRun = false;
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAISoundRenderReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** completed, checked (a dry run), awaiting_user_approval, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString SoundWave;
	UPROPERTY() TArray<FString> Stems;
	UPROPERTY() FString MetaSound;
	/** The rendered file, relative to the project. */
	UPROPERTY() FString WavFile;
	UPROPERTY() double Seconds = 0.0;
	/** Loudest sample and the average level, in dBFS. */
	UPROPERTY() float PeakDb = -144.f;
	UPROPERTY() float RmsDb = -144.f;
	/** Silence before the first sound, in seconds. */
	UPROPERTY() double LeadingSilenceSeconds = 0.0;
	UPROPERTY() bool bSilent = false;
	UPROPERTY() bool bClipped = false;
	/** What a listener would hear wrong: silence, clipping, a late start, a loop that clicks. */
	UPROPERTY() TArray<FString> Issues;
};

/** Sound effects, ambiance and music built with Decibel, rendered into SoundWaves and MetaSounds. */
UCLASS()
class HYPERAISTUDIODECIBEL_API UHyperAIStudioDecibelToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Decibel's devices (bCatalog), one device's parameters and presets (DeviceId, bPresets), the project's sessions
	 * (bSessions), or one session's tempo, loop, tracks, device chains and clips (SessionPath). Read before a plan.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Sound")
	static FHyperAISoundInspectReport hyper_sound_inspect(const FHyperAISoundInspectRequest& Request);

	/**
	 * Builds or edits a Decibel session as one undo step, then saves it. A sound effect is an instrument track with a
	 * generator (gen.weapon, gen.impact, gen.engine, gen.synth...) set from a preset or by parameters, fired by
	 * clip.trigger; music is instrument tracks with clip.midi notes; ambiance is audio tracks of SoundWaves in loops,
	 * with effects (device.add fx.*) on tracks or on the master. Then hear it with hyper_sound_render.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Sound")
	static FHyperAISoundPlanReport hyper_sound_apply_plan(const FHyperAISoundPlanRequest& Request);

	/**
	 * Renders a Decibel session offline into a SoundWave (and a stem per track, and a MetaSound Source playing it when
	 * asked), and measures it: length, peak and average level, leading silence, clipping. The proof a sound works.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Sound")
	static FHyperAISoundRenderReport hyper_sound_render(const FHyperAISoundRenderRequest& Request);
};

class FHyperAIStudioDecibelContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("sound_design");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudiodecibeltoolset.v1");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_sound_inspect");
	static constexpr const TCHAR* PlanToolName = TEXT("hyper_sound_apply_plan");
	static constexpr const TCHAR* RenderToolName = TEXT("hyper_sound_render");
	static constexpr int32 MaxOperations = 256;
	static constexpr int32 MaxNotesPerClip = 4096;
	static constexpr double MaxRenderSeconds = 600.0;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAISoundInspectReport Inspect(const FHyperAISoundInspectRequest& Request);
	static FHyperAISoundPlanReport ApplyPlan(const FHyperAISoundPlanRequest& Request);
	static FHyperAISoundRenderReport Render(const FHyperAISoundRenderRequest& Request);
};

class FHyperAIStudioDecibelRegistration final
{
public:
	void Startup();
	void Shutdown();

private:
	void RegisterAfterEngineInit();

	FDelegateHandle PostEngineInitHandle;
	bool bStarted = false;
	bool bOwnsToolset = false;
};
