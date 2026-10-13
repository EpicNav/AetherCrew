// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

#include "HyperAIStudioAudioAuthoringToolset.generated.h"

/** A setting by its Unreal property name, such as Volume, FalloffDistance or MaxCount, and its value as the Details panel would take it. */
USTRUCT(BlueprintType)
struct FHyperAIAudioSetting
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() FString Value;
};

USTRUCT(BlueprintType)
struct FHyperAISoundIssue
{
	GENERATED_BODY()

	/** error or warning. */
	UPROPERTY() FString Severity;
	/** missing_wave, dangling_node, empty_input, too_few_inputs, no_root, missing_asset, graph_out_of_sync. */
	UPROPERTY() FString Code;
	UPROPERTY() FString Message;
};

/** param_crossfade: where one input fades in and out along the parameter's value. */
USTRUCT(BlueprintType)
struct FHyperAISoundCueFade
{
	GENERATED_BODY()

	UPROPERTY() float FadeInStart = 0.f;
	UPROPERTY() float FadeInEnd = 0.f;
	UPROPERTY() float FadeOutStart = 1.f;
	UPROPERTY() float FadeOutEnd = 1.f;
	UPROPERTY() float Volume = 1.f;
};

/** One node of a Sound Cue's graph. A plan's nodes form a tree from Root: each node feeds one input of one parent. */
USTRUCT(BlueprintType)
struct FHyperAISoundCueNode
{
	GENERATED_BODY()

	/** The node's id in the plan, such as steps or mod; Children name nodes by it. */
	UPROPERTY() FString Id;
	/**
	 * wave_player, random, modulator, mixer, concatenator, looping, delay, attenuation, enveloper, branch (a bool
	 * parameter), switch (an int parameter), param_crossfade (a float parameter), continuous_modulator (float parameters
	 * drive volume and pitch) or wave_param (a SoundWave parameter).
	 */
	UPROPERTY() FString Kind;
	/**
	 * The ids feeding this node, in input order. branch: true, false, then unset (3). switch: unset, then 0, 1, 2 and on.
	 * wave_player has none; modulator, looping, delay, attenuation, enveloper, continuous_modulator and wave_param one.
	 */
	UPROPERTY() TArray<FString> Children;
	/** wave_player: the SoundWave, such as /Game/Audio/SW_Step_01. */
	UPROPERTY() FString Wave;
	/** wave_player: loop the wave itself. */
	UPROPERTY() bool bLooping = false;
	/** random: one weight per child. mixer, concatenator: one input volume per child. All 1 when empty. */
	UPROPERTY() TArray<float> Weights;
	/** random: play every child once before any repeats (Unreal's Randomize Without Replacement); the way a Sound Cue avoids repeats. */
	UPROPERTY() bool bNoRepeat = false;
	/** random: pick this many children when the level loads and play only those (0 = all), to save memory. */
	UPROPERTY() int32 PreselectAtLevelLoad = 0;
	/** modulator, enveloper: picked between on each play (1 = unchanged). continuous_modulator: the outputs its parameters map 0-1 onto. */
	UPROPERTY() float PitchMin = 1.f;
	UPROPERTY() float PitchMax = 1.f;
	UPROPERTY() float VolumeMin = 1.f;
	UPROPERTY() float VolumeMax = 1.f;
	/** looping, enveloper: how many times it plays (0 = forever). */
	UPROPERTY() int32 LoopCount = 0;
	/** delay: seconds before it plays, picked between. */
	UPROPERTY() float DelayMin = 0.f;
	UPROPERTY() float DelayMax = 0.f;
	/** attenuation: a Sound Attenuation asset. */
	UPROPERTY() FString Attenuation;
	/**
	 * branch: the bool parameter. switch: the int parameter. param_crossfade: the float parameter. wave_param: the
	 * SoundWave parameter. continuous_modulator: the float parameter (0-1) that drives volume.
	 */
	UPROPERTY() FString Parameter;
	/** continuous_modulator: the float parameter (0-1) that drives pitch. */
	UPROPERTY() FString PitchParameter;
	/** param_crossfade: one per child. */
	UPROPERTY() TArray<FHyperAISoundCueFade> Fades;
	/** enveloper: volume and pitch over time, as (seconds, value) points. */
	UPROPERTY() TArray<FVector2D> VolumeCurve;
	UPROPERTY() TArray<FVector2D> PitchCurve;
	/** enveloper: the loop's start and end, and how long it plays after the last loop, in seconds. */
	UPROPERTY() float LoopStart = 0.f;
	UPROPERTY() float LoopEnd = 0.f;
	UPROPERTY() float DurationAfterLoop = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAISoundCuePlanRequest
{
	GENERATED_BODY()

	/** The Sound Cue, such as /Game/Audio/SC_Footsteps. Made when nothing is there. */
	UPROPERTY() FString Cue;
	/** The graph, as a tree from Root. Empty leaves an existing cue's graph and only changes its settings. */
	UPROPERTY() TArray<FHyperAISoundCueNode> Nodes;
	/** The id of the node the cue plays; the first node when empty. */
	UPROPERTY() FString Root;
	/** An existing cue's graph is replaced only when this is true. */
	UPROPERTY() bool bReplaceGraph = false;
	/** A Sound Class for the cue; none clears it, empty leaves it. */
	UPROPERTY() FString SoundClass;
	/** A Sound Attenuation for the cue; none clears it, empty leaves it. */
	UPROPERTY() FString Attenuation;
	/** Sound Concurrency assets; they replace the cue's set. [none] clears it, empty leaves it. */
	UPROPERTY() TArray<FString> Concurrency;
	/** 0 leaves them. */
	UPROPERTY() float VolumeMultiplier = 0.f;
	UPROPERTY() float PitchMultiplier = 0.f;
	/** Only check and lay out the plan: nothing is changed. */
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bSave = true;
	UPROPERTY() FString OperationId;
};

/** A node as built, with where the layout put it. */
USTRUCT(BlueprintType)
struct FHyperAISoundCueNodeInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Id;
	UPROPERTY() FString Kind;
	UPROPERTY() TArray<FString> Children;
	/** Its settings in a line, such as "pitch 0.95-1.05, volume 0.9-1". */
	UPROPERTY() FString Detail;
	UPROPERTY() int32 X = 0;
	UPROPERTY() int32 Y = 0;
};

USTRUCT(BlueprintType)
struct FHyperAISoundCuePlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	/** checked, awaiting_user_approval, completed, or why it was refused. */
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Cue;
	UPROPERTY() bool bCreates = false;
	UPROPERTY() TArray<FHyperAISoundCueNodeInfo> Nodes;
	/** The cue's settings after the plan: class, attenuation, concurrency, volume and pitch. */
	UPROPERTY() TArray<FString> Settings;
	/** What the built graph's validation found; the plan stops on an error. */
	UPROPERTY() TArray<FHyperAISoundIssue> Issues;
	/** The cue's length in seconds; -1 when it loops forever. */
	UPROPERTY() float DurationSeconds = 0.f;
	UPROPERTY() TArray<FString> Saved;
};

/** sound_mix: one class's adjustment while the mix is active. */
USTRUCT(BlueprintType)
struct FHyperAISoundMixAdjuster
{
	GENERATED_BODY()

	UPROPERTY() FString SoundClass;
	UPROPERTY() float Volume = 1.f;
	UPROPERTY() float Pitch = 1.f;
	/** Hz; 20000 leaves it open. */
	UPROPERTY() float LowPassFilterFrequency = 20000.f;
	UPROPERTY() bool bApplyToChildren = true;
};

/** sound_mix: one EQ band. Gain is linear (1 = flat), bandwidth in octaves. */
USTRUCT(BlueprintType)
struct FHyperAISoundEQBand
{
	GENERATED_BODY()

	UPROPERTY() float Frequency = 1000.f;
	UPROPERTY() float Gain = 1.f;
	UPROPERTY() float Bandwidth = 1.f;
};

/** submix: one effect of its chain, a preset asset made (or reused) with these settings. */
USTRUCT(BlueprintType)
struct FHyperAISubmixEffect
{
	GENERATED_BODY()

	/** reverb, eq or compressor. */
	UPROPERTY() FString Kind;
	/** The preset asset; beside the submix, named after it and the kind, when empty. An existing preset is reused and set. */
	UPROPERTY() FString Preset;
	/** The preset's settings by property name: reverb DecayTime, Density, WetLevel...; compressor ThresholdDb, Ratio...; eq takes Bands. */
	UPROPERTY() TArray<FHyperAIAudioSetting> Settings;
	/** eq: its bands. Gain in dB here. */
	UPROPERTY() TArray<FHyperAISoundEQBand> Bands;
};

/** One mix asset to make or change. */
USTRUCT(BlueprintType)
struct FHyperAISoundMixAsset
{
	GENERATED_BODY()

	/** sound_class, sound_mix, attenuation, concurrency or submix. */
	UPROPERTY() FString Kind;
	/** The asset, such as /Game/Audio/Mix/SCL_Footsteps. Made when nothing is there. */
	UPROPERTY() FString Path;
	/** sound_class, submix: the parent; none detaches it, empty leaves it. A parent made earlier in the plan may be named. */
	UPROPERTY() FString Parent;
	/**
	 * By Unreal property name. sound_class: its Properties (Volume, Pitch, LowPassFilterFrequency, AttenuationDistanceScale,
	 * bIsUISound, bIsMusic, bAlwaysPlay, bApplyEffects, bReverb, Default2DReverbSendAmount...). sound_mix: InitialDelay,
	 * FadeInTime, Duration (-1 forever), FadeOutTime, EQPriority, bApplyEQ. attenuation: its settings (FalloffDistance,
	 * AttenuationShape, AttenuationShapeExtents, DistanceAlgorithm, bSpatialize, bAttenuateWithLPF, LPFRadiusMin...).
	 * concurrency: MaxCount, bLimitToOwner, ResolutionRule, RetriggerTime, VolumeScale... submix: its own (OutputVolumeModulation...).
	 */
	UPROPERTY() TArray<FHyperAIAudioSetting> Settings;
	/** sound_mix: the class adjusters; they replace the mix's list when given. */
	UPROPERTY() TArray<FHyperAISoundMixAdjuster> Adjusters;
	/** sound_mix: up to 4 EQ bands; they replace its EQ and turn it on. */
	UPROPERTY() TArray<FHyperAISoundEQBand> EQBands;
	/** submix: its effect chain; replaces the chain when given. */
	UPROPERTY() TArray<FHyperAISubmixEffect> Effects;
};

USTRUCT(BlueprintType)
struct FHyperAISoundMixPlanRequest
{
	GENERATED_BODY()

	/** Applied in order, in one undo step. */
	UPROPERTY() TArray<FHyperAISoundMixAsset> Assets;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() bool bSave = true;
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAISoundMixPlanReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	/** What each asset gets, one line per change. */
	UPROPERTY() TArray<FString> Changes;
	UPROPERTY() TArray<FString> Created;
	UPROPERTY() TArray<FString> Saved;
};

USTRUCT(BlueprintType)
struct FHyperAISoundAssetInspectRequest
{
	GENERATED_BODY()

	/** Assets by path: Sound Cues, Sound Classes, Sound Mixes, attenuation, concurrency, submixes, SoundWaves and MetaSound Sources. */
	UPROPERTY() TArray<FString> Paths;
	/** Or every such asset in a folder, such as /Game/Audio. */
	UPROPERTY() FString Folder;
	UPROPERTY() bool bRecursive = true;
	/** Every editable setting rather than the common ones. */
	UPROPERTY() bool bAllSettings = false;
	UPROPERTY() int32 MaxAssets = 50;
};

USTRUCT(BlueprintType)
struct FHyperAISoundAssetInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Path;
	/** sound_cue, sound_class, sound_mix, attenuation, concurrency, submix, sound_wave or metasound_source. */
	UPROPERTY() FString Kind;
	UPROPERTY() TArray<FHyperAIAudioSetting> Settings;
	UPROPERTY() FString Parent;
	UPROPERTY() TArray<FString> Children;
	/** sound_cue: its graph as a plan's nodes (ids n0, n1... from the root), ready to change and send back with bReplaceGraph. */
	UPROPERTY() TArray<FHyperAISoundCueNode> CueNodes;
	UPROPERTY() FString CueRoot;
	/** sound_mix: its class adjusters and EQ. submix: its effects. */
	UPROPERTY() TArray<FString> Details;
	UPROPERTY() TArray<FHyperAISoundIssue> Issues;
	/** sound_cue, sound_wave, metasound_source: seconds; -1 loops forever. */
	UPROPERTY() float DurationSeconds = 0.f;
};

USTRUCT(BlueprintType)
struct FHyperAISoundAssetInspectReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() TArray<FHyperAISoundAssetInfo> Assets;
	UPROPERTY() bool bTruncated = false;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundRecipeRequest
{
	GENERATED_BODY()

	/** wind_loop, ui_blip, ui_confirm, engine_hum, rain_bed or laser_zap. Empty lists the recipes and their inputs. */
	UPROPERTY() FString Recipe;
	/** The MetaSound Source to make, such as /Game/Audio/MS_Wind. */
	UPROPERTY() FString Path;
	/** Defaults for the recipe's inputs, by name, such as RPM 2400. */
	UPROPERTY() TArray<FHyperAIAudioSetting> Inputs;
	UPROPERTY() bool bDryRun = true;
	UPROPERTY() FString OperationId;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundInputInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	/** Float or Time (seconds). */
	UPROPERTY() FString Type;
	UPROPERTY() float Default = 0.f;
	UPROPERTY() float Min = 0.f;
	UPROPERTY() float Max = 0.f;
	UPROPERTY() FString Description;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundRecipeInfo
{
	GENERATED_BODY()

	UPROPERTY() FString Recipe;
	UPROPERTY() FString Description;
	UPROPERTY() bool bOneShot = false;
	UPROPERTY() TArray<FHyperAIMetaSoundInputInfo> Inputs;
};

USTRUCT(BlueprintType)
struct FHyperAIMetaSoundRecipeReport
{
	GENERATED_BODY()

	UPROPERTY() bool bOk = false;
	UPROPERTY() FString Status;
	UPROPERTY() FString Diagnostic;
	UPROPERTY() FString OperationId;
	UPROPERTY() FString Path;
	/** The recipes are experimental: the sound is a starting point to tune by ear. */
	UPROPERTY() bool bExperimental = true;
	/** The recipe asked for, or every recipe when none was. */
	UPROPERTY() TArray<FHyperAIMetaSoundRecipeInfo> Recipes;
	UPROPERTY() int32 NodeCount = 0;
	UPROPERTY() TArray<FString> Saved;
};

/** Sound Cues, the mix (Sound Classes, Sound Mixes, attenuation, concurrency, submixes) and procedural MetaSound recipes. */
UCLASS()
class UHyperAIStudioAudioAuthoringToolset final : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	virtual FString GetToolsetVersion() const override { return TEXT("1.0.0"); }

	/**
	 * Makes or rebuilds a Sound Cue from a tree of nodes (wave_player, random, modulator, mixer, concatenator, looping,
	 * delay, attenuation, enveloper, branch, switch, param_crossfade, continuous_modulator, wave_param) in one undo step,
	 * lays the graph out, checks every wave exists and nothing dangles, and sets its Sound Class, attenuation, concurrency,
	 * volume and pitch. Dry-run first (the default); an existing cue's graph is replaced only with bReplaceGraph.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static FHyperAISoundCuePlanReport hyper_sound_cue_plan(const FHyperAISoundCuePlanRequest& Request);

	/**
	 * Makes or changes the mix in one undo step: Sound Classes (parent and properties), Sound Mixes (class adjusters,
	 * EQ, fades), Sound Attenuation, Sound Concurrency and Submixes with reverb, EQ and compressor effects. Settings go by
	 * Unreal property name; hyper_sound_asset_inspect shows them. Dry-run first (the default).
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static FHyperAISoundMixPlanReport hyper_sound_mix_plan(const FHyperAISoundMixPlanRequest& Request);

	/**
	 * Reads Sound Cues (their graph as plan nodes, and what validation finds), Sound Classes, Sound Mixes, attenuation,
	 * concurrency, submixes, SoundWaves and MetaSound Sources, by path or by folder. Changes nothing.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static FHyperAISoundAssetInspectReport hyper_sound_asset_inspect(const FHyperAISoundAssetInspectRequest& Request);

	/**
	 * Experimental: builds a procedural MetaSound Source from a recipe of oscillators, noise, envelopes, filters and LFOs
	 * (wind_loop, ui_blip, ui_confirm, engine_hum with an RPM input, rain_bed, laser_zap), with its controls exposed as
	 * MetaSound inputs. An empty Recipe lists them. Dry-run first (the default); a recipe is a starting point to tune by ear.
	 */
	UFUNCTION(meta = (AICallable), Category = "HyperAI|Audio")
	static FHyperAIMetaSoundRecipeReport hyper_metasound_recipe(const FHyperAIMetaSoundRecipeRequest& Request);
};

class FHyperAIStudioAudioAuthoringContracts final
{
public:
	static constexpr const TCHAR* PackId = TEXT("audio_metasound");
	static constexpr const TCHAR* AtomicCohortId = TEXT("cohort.source.hyperaistudioaudioauthoringtoolset.v1");
	static constexpr const TCHAR* CueToolName = TEXT("hyper_sound_cue_plan");
	static constexpr const TCHAR* MixToolName = TEXT("hyper_sound_mix_plan");
	static constexpr const TCHAR* InspectToolName = TEXT("hyper_sound_asset_inspect");
	static constexpr const TCHAR* RecipeToolName = TEXT("hyper_metasound_recipe");
	static constexpr int32 MaxCueNodes = 128;
	static constexpr int32 MaxMixAssets = 32;
	static constexpr int32 MaxSettings = 64;

	static FString GetQualifiedToolsetName();
	static TArray<FString> GetToolNames();
	static bool IsRegistrationAllowed(bool bAllowSourceCandidateForDev);
	static FHyperAISoundCuePlanReport CuePlan(const FHyperAISoundCuePlanRequest& Request);
	static FHyperAISoundMixPlanReport MixPlan(const FHyperAISoundMixPlanRequest& Request);
	static FHyperAISoundAssetInspectReport Inspect(const FHyperAISoundAssetInspectRequest& Request);
	static FHyperAIMetaSoundRecipeReport Recipe(const FHyperAIMetaSoundRecipeRequest& Request);
};

class FHyperAIStudioAudioAuthoringRegistration final
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
