// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDialogueToolset.h"

class UHyperAIStudioVoice;
class USoundWave;

/** The only code that talks to the speech service or writes audio; the toolset handles contracts and execution. */
namespace HyperAIStudio::Dialogue::Gate
{
	/**
	 * The speech model's prompt: <speaker description>, "<text>" <Direction,> "<more text>".
	 *
	 * Only text inside the quotes is spoken. A direction is an unspoken third-person cue; the first one reads
	 * as part of the description sentence and stays lowercase, later ones are their own pacing sentences and
	 * take a capital. Ported from Earlark's build_prompt, whose rules were established against the model.
	 */
	FString BuildPrompt(const FString& SpeakerDescription, const TArray<FHyperAIDialogueLine>& Segments);

	UHyperAIStudioVoice* ResolveVoice(const FString& Path);
	/** Every voice asset in the project. */
	TArray<FHyperAIDialogueVoiceRecord> ReadVoices();
	/** Clip files sitting in the voice reference folder on this machine. */
	TArray<FString> ReadAvailableClips();
	/** True when the voice's clip is on this machine; OutServicePath is what the service is told. */
	bool ResolveReferenceClip(const UHyperAIStudioVoice& Voice, FString& OutServicePath, FString& OutError);

	/** Problems the service would refuse, or silently perform wrong. */
	TArray<FHyperAIDialogueIssue> CheckLines(const TArray<FHyperAIDialogueLine>& Lines);
	/** Roughly how long a batch takes, from its spoken characters. */
	float EstimateRenderSeconds(const TArray<FHyperAIDialogueLine>& Lines);

	/** Starts rendering; returns at once. The job renders one line at a time, because the model owns the card. */
	bool StartRender(const FString& SceneName, const TArray<FHyperAIDialogueLine>& Lines, FString& OutRenderId, FString& OutFolder, FString& OutError);
	TArray<FHyperAIDialogueRenderRecord> ReadRenders();
	bool FindRender(const FString& RenderId, FHyperAIDialogueRenderRecord& OutRecord);
	/** Name, size and time of every wav in the render, so a changed folder is caught before an import. */
	FString ComputeRenderKey(const FString& RenderId);

	/** int16 little-endian mono PCM into a RIFF wav. */
	bool WriteWav(const FString& Path, TConstArrayView<uint8> Pcm, int32 SampleRate, FString& OutError);

	/** Imports every wav of a finished render into TargetFolder; the caller holds the transaction. */
	bool ImportRender(const FString& RenderId, const FString& TargetFolder, bool bCreateDialogueWaves,
		TArray<UObject*>& OutCreated, FString& OutError);
	/** Object paths the import would write, so a plan can say what it touches before it runs. */
	TArray<FString> PlanImportPaths(const FString& RenderId, const FString& TargetFolder, bool bCreateDialogueWaves);
	/** Packages holding the created assets. */
	TArray<UPackage*> PackagesOf(const TArray<UObject*>& Objects);

	void Shutdown();
}
