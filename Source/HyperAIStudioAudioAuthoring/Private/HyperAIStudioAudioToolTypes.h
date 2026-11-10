// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

#include "HyperAIStudioAudioToolTypes.generated.h"

/** A setting by its Unreal property name, such as Volume, FalloffDistance or MaxCount, and its value as the Details panel would take it. */
USTRUCT(BlueprintType)
struct FHyperAIAudioSetting
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Value;
};

/** One pin of a MetaSound node class. */
USTRUCT(BlueprintType)
struct FHyperAIMetaSoundPin
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** Audio, Float, Int32, Bool, Time (seconds), Trigger, String, WaveAsset, an enum (Enum:...) or an array (Float:Array, WaveAsset:Array...). */
	UPROPERTY() FString Type;
	/** Inputs: the class default, written the way a plan's Defaults take a value. */
	UPROPERTY() FString Default;
	UPROPERTY() FString Description;
	/** Set once when the sound starts: a constructor output, a graph input or a default feeds it, never a running signal. */
	UPROPERTY() bool bConstructor = false;
	/** Enum pins: Name=value pairs; a plan may give either. */
	UPROPERTY() TArray<FString> EnumValues;
};

/** A node class in the MetaSound registry. */
USTRUCT(BlueprintType)
struct FHyperAIMetaSoundNodeClass
{
	GENERATED_BODY()

	/** Namespace.Name.Variant, as a graph plan's Class takes it: UE.Wave Player.Mono, UE.Sine.Audio, Array.Random Get.WaveAsset:Array. */
	UPROPERTY() FString Class;
	UPROPERTY() FString DisplayName;
	/** Its menu path, such as Generators > Oscillators. */
	UPROPERTY() FString Category;
	UPROPERTY() FString Description;
	UPROPERTY() int32 MajorVersion = 1;
	UPROPERTY() int32 MinorVersion = 0;
	UPROPERTY() bool bDeprecated = false;
	UPROPERTY() TArray<FHyperAIMetaSoundPin> Inputs;
	UPROPERTY() TArray<FHyperAIMetaSoundPin> Outputs;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundNodeSearchRequest
{
	GENERATED_BODY()

	/** Words, all of which must appear in the class name, display name, category, keywords or description: "wave player", "random get", "band pass filter", "envelope". Empty matches every node. */
	UPROPERTY() FString Query;
	/** Only nodes under this category, such as Filters, Envelopes, Generators, Triggers, Array, Math or Delays. */
	UPROPERTY() FString Category;
	/** Only nodes with an input pin of this data type (Audio, Float, Trigger, WaveAsset, Time...). */
	UPROPERTY() FString InputType;
	/** Only nodes with an output pin of this data type. */
	UPROPERTY() FString OutputType;
	UPROPERTY() bool bIncludeDeprecated = false;
	/** 1-100; 25 when 0. */
	UPROPERTY() int32 MaxResults = 25;
	/** For the next page: the number of matches already read. */
	UPROPERTY() int32 FirstResult = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundNodeSearchReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() int32 TotalMatches = 0;
	/** Best match first, with each pin's name and type; hyper_metasound_node_describe gives defaults, descriptions and enum values. */
	UPROPERTY() TArray<FHyperAIMetaSoundNodeClass> Nodes;
	UPROPERTY() bool bTruncated = false;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundNodeDescribeRequest
{
	GENERATED_BODY()

	/** Up to 16 classes as Namespace.Name.Variant, such as UE.Wave Player.Mono or UE.AD Envelope.Audio (from hyper_metasound_node_search). */
	UPROPERTY() TArray<FString> Classes;
	/** Also list every registered data type a graph input, output or pin may have. */
	UPROPERTY() bool bListDataTypes = false;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundNodeDescribeReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAIMetaSoundNodeClass> Nodes;
	UPROPERTY() TArray<FString> DataTypes;
};

/** A node of a MetaSound graph plan. */
USTRUCT(BlueprintType)
struct FHyperAIMetaSoundGraphNode
{
	GENERATED_BODY()

	/** The node's id in the plan, such as player or env; connections name nodes by it. To use a node already in the MetaSound, give its NodeId GUID (from hyper_sound_asset_inspect) and leave Class empty. */
	UPROPERTY() FString Id;
	/** Namespace.Name.Variant from hyper_metasound_node_search, such as UE.Wave Player.Mono. */
	UPROPERTY() FString Class;
	/** 0 takes the highest registered version. */
	UPROPERTY() int32 MajorVersion = 0;
	/** Input pins' values by pin name: numbers, true/false, an enum's name or value, an asset path for WaveAsset, a comma list for arrays. */
	UPROPERTY() TArray<FHyperAIAudioSetting> Defaults;
	/** Where it goes, in columns left to right and rows; -1 lays it out from the connections. */
	UPROPERTY() int32 Column = -1;
	UPROPERTY() int32 Row = -1;
};

/** A graph input (a parameter a Blueprint or the Details panel sets) or a graph output. */
USTRUCT(BlueprintType)
struct FHyperAIMetaSoundGraphMember
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** A registered data type: Float, Int32, Bool, Time, Trigger, String, WaveAsset, WaveAsset:Array, Audio (outputs)... */
	UPROPERTY() FString Type;
	/** Inputs: the default, as node Defaults take values. */
	UPROPERTY() FString Default;
};

/** One wire. Node "graph" is the graph itself: as From, FromPin is a graph input or On Play; as To, ToPin is a graph output, Out Mono, Out Left, Out Right or On Finished. */
USTRUCT(BlueprintType)
struct FHyperAIMetaSoundConnection
{
	GENERATED_BODY()

	UPROPERTY() FString FromNode;
	/** An output pin of FromNode, such as Out, Out Mono, On Finished, Value. */
	UPROPERTY() FString FromPin;
	UPROPERTY() FString ToNode;
	/** An input pin of ToNode, such as In, Play, Trigger, Wave Asset. */
	UPROPERTY() FString ToPin;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundGraphPlanRequest
{
	GENERATED_BODY()

	/** The MetaSound Source, such as /Game/Audio/MS_Footsteps. Made when nothing is there; an existing one gets the plan's nodes, inputs and wires added. */
	UPROPERTY() FString Path;
	/** New sources: mono or stereo. */
	UPROPERTY() FString Format = TEXT("mono");
	/** New sources: one-shot (it has On Finished; wire it, or the sound plays forever) or a loop. */
	UPROPERTY() bool bOneShot = true;
	UPROPERTY() TArray<FHyperAIMetaSoundGraphMember> Inputs;
	UPROPERTY() TArray<FHyperAIMetaSoundGraphMember> Outputs;
	UPROPERTY() TArray<FHyperAIMetaSoundGraphNode> Nodes;
	UPROPERTY() TArray<FHyperAIMetaSoundConnection> Connections;
	/** Only check the plan against the registry and lay it out: nothing is changed. */
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bSave = true;
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundBuiltNode
{
	GENERATED_BODY()

	UPROPERTY() FString Id;
	UPROPERTY() FString Class;
	/** After it runs: the node's GUID in the MetaSound. */
	UPROPERTY() FString NodeId;
	UPROPERTY() int32 Column = 0;
	UPROPERTY() int32 Row = 0;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundGraphPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, awaiting_user_approval, completed, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Path;
	UPROPERTY() bool bCreates = false;
	UPROPERTY() TArray<FHyperAIMetaSoundBuiltNode> Nodes;
	UPROPERTY() int32 ConnectionCount = 0;
	/** What the check found that does not stop the plan, such as an input pin nothing feeds. */
	UPROPERTY() TArray<FString> Warnings;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioAnalyzeRequest
{
	GENERATED_BODY()

	/** A SoundWave or a MetaSound Source, such as /Game/Audio/MS_Footsteps. */
	UPROPERTY() FString Sound;
	/** MetaSounds: how many times to render it (1-16), each a fresh play, to hear its variation. */
	UPROPERTY() int32 Plays = 1;
	/** MetaSounds: the longest render in seconds (0.1-30); a one-shot stops at On Finished. */
	UPROPERTY() float MaxSeconds = 5.f;
	/** MetaSounds: 8000-96000. */
	UPROPERTY() int32 SampleRate = 48000;
	/** MetaSounds: graph input values for the render, by name, such as Pitch Jitter 2. */
	UPROPERTY() TArray<FHyperAIAudioSetting> Parameters;
	/** Also the level every 20 ms (dBFS), for a look at the envelope. */
	UPROPERTY() bool bEnvelope = false;
};

/** One render's measurements. Levels are dBFS; loudness is ITU-R BS.1770 (K-weighted, gated). */
USTRUCT(BlueprintType)
struct FHyperAIAudioMeasure
{
	GENERATED_BODY()

	UPROPERTY() float DurationSeconds = 0.f;
	/** Integrated loudness in LUFS; for a sound shorter than 400 ms, its whole length as one block. */
	UPROPERTY() float IntegratedLufs = -144.f;
	/** The loudest 400 ms block, LUFS. */
	UPROPERTY() float MaxMomentaryLufs = -144.f;
	UPROPERTY() float PeakDb = -144.f;
	UPROPERTY() float RmsDb = -144.f;
	/** When the sound starts: the level first within 30 dB of its peak, ms from the start. */
	UPROPERTY() float OnsetMs = 0.f;
	/** The time of the peak, ms. */
	UPROPERTY() float PeakMs = 0.f;
	/** The energy-weighted spectral centroid, Hz: low for thuds, high for hiss and clicks. */
	UPROPERTY() float SpectralCentroidHz = 0.f;
	/** From the peak until the level stays 60 dB below it, seconds. */
	UPROPERTY() float TailSeconds = 0.f;
	/** Samples at or over full scale (|x| >= 0.999). */
	UPROPERTY() int32 ClippedSamples = 0;
	UPROPERTY() bool bSilent = false;
	/** MetaSound one-shots: it finished (On Finished) before MaxSeconds. */
	UPROPERTY() bool bFinished = false;
	/** The jump between the last and the first sample, dB below full scale: a loop clicks at its seam when this is high (above -30). */
	UPROPERTY() float LoopSeamDb = -144.f;
	UPROPERTY() TArray<float> EnvelopeDb;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioAnalyzeReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString Sound;
	/** sound_wave or metasound_source. */
	UPROPERTY() FString Kind;
	/** How it was heard: sound_wave_pcm (the imported samples) or metasound_offline (its generator run without an audio device). */
	UPROPERTY() FString Method;
	UPROPERTY() int32 SampleRate = 0;
	UPROPERTY() int32 NumChannels = 0;
	UPROPERTY() TArray<FHyperAIAudioMeasure> Plays;
	/** Across plays: the loudest minus the quietest, LU, and the spread of the centroid, Hz. Footsteps want a little (1-3 LU). */
	UPROPERTY() float LoudnessSpreadLu = 0.f;
	UPROPERTY() float CentroidSpreadHz = 0.f;
	/** silent, clipping, too_quiet, late_onset, never_finishes, clicks_at_seam... with what to do. */
	UPROPERTY() TArray<FString> Issues;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioPreviewRequest
{
	GENERATED_BODY()

	/**
	 * play (Sound, from StartTime, with Parameters), stop, set (Parameters, VolumeMultiplier, PitchMultiplier on what is
	 * playing), status (what plays, the active sounds and mixes, solos and mutes), solo or mute (Target, bEnable),
	 * clear (every solo and mute).
	 */
	UPROPERTY() FString Action = TEXT("status");
	/** play: a SoundWave, Sound Cue or MetaSound Source. */
	UPROPERTY() FString Sound;
	UPROPERTY() float StartTime = 0.f;
	/** Parameters by name. A number is a float (or an int when the sound's parameter is one), true/false a bool, trigger fires a trigger. */
	UPROPERTY() TArray<FHyperAIAudioSetting> Parameters;
	/** 0 leaves them. */
	UPROPERTY() float VolumeMultiplier = 0.f;
	UPROPERTY() float PitchMultiplier = 0.f;
	/** solo, mute: a Sound Class, Submix, SoundWave or Sound Cue. */
	UPROPERTY() FString Target;
	UPROPERTY() bool bEnable = true;
};

USTRUCT(BlueprintType)
struct FHyperAIAudioPreviewReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	/** The editor has an audio device; without one nothing is heard and only status works. */
	UPROPERTY() bool bAudioAvailable = false;
	UPROPERTY() bool bPreviewing = false;
	UPROPERTY() FString PreviewSound;
	/** Each as sound path, time played, volume and owner. */
	UPROPERTY() TArray<FString> ActiveSounds;
	/** Each as mix path, push count and fade. */
	UPROPERTY() TArray<FString> ActiveMixes;
	UPROPERTY() TArray<FString> Soloed;
	UPROPERTY() TArray<FString> Muted;
};
