// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAudioAuthoringToolset.h"

class USoundAttenuation;
class USoundCue;
class USoundNode;
class USoundWave;

/** Sound Cue graphs as plans: the node kinds, a plan's check and layout, building it into a cue, and reading one back. */
namespace HyperAIStudio::AudioAuthoring::CueGraph
{
	/** A plan's tree, checked: the order nodes are built in (root first), their parents and where the layout puts them. */
	struct FCheckedGraph
	{
		TArray<FHyperAISoundCueNode> Nodes;
		int32 Root = INDEX_NONE;
		/** Depth-first from the root; every node is in it once. */
		TArray<int32> Order;
		TArray<FIntPoint> Positions;
		/** Per node: its wave (wave_player) and attenuation asset (attenuation), loaded at check. */
		TArray<TWeakObjectPtr<USoundWave>> Waves;
		TArray<TWeakObjectPtr<USoundAttenuation>> Attenuations;
	};

	/** The kinds a plan may use. */
	const TArray<FString>& Kinds();

	/** The node kind of a sound node's class; empty for a kind plans do not build (such as a dialogue player). */
	FString KindOf(const USoundNode& Node);

	/** Checks a plan's nodes: kinds, ids, a tree from Root with no node left out, each kind's inputs and fields, and that every asset loads. */
	bool Check(const TArray<FHyperAISoundCueNode>& Nodes, const FString& Root, FCheckedGraph& Out, FString& OutError);

	/** The plan's nodes as the report shows them, with their layout positions. */
	TArray<FHyperAISoundCueNodeInfo> Describe(const FCheckedGraph& Graph);

	/**
	 * Replaces Cue's graph with the checked one: the old nodes are removed as the Sound Cue editor removes them, the new
	 * ones built, linked and laid out. Call inside a transaction, after Cue->Modify().
	 */
	bool Build(USoundCue& Cue, const FCheckedGraph& Graph, FString& OutError);

	/** The cue's graph as plan nodes, ids n0, n1... depth-first from the root; nodes the root cannot reach follow. */
	void Read(const USoundCue& Cue, TArray<FHyperAISoundCueNode>& OutNodes, FString& OutRoot);

	/** What is wrong with a cue's graph: missing waves, dangling nodes, empty inputs, a graph out of step with its nodes. */
	TArray<FHyperAISoundIssue> Validate(const USoundCue& Cue);

	/** The cue's duration as Unreal computes it; -1 when it loops forever. */
	float DurationOf(USoundCue& Cue);

	/** Loads an asset of class T by package or object path; null when it is not there or is something else. */
	UObject* LoadAsset(const FString& Path, const UClass& Class, FString* OutReason = nullptr);

	/** /Game/A/B or /Game/A/B.B as the package /Game/A/B. */
	FString PackageOf(const FString& Path);
}
