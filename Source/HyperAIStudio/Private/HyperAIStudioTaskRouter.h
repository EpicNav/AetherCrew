// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAgentActivity.h"

struct FHyperAIStudioAgentTaskRoute;
struct FHyperAIStudioRuntimeToolBinding;

/**
 * What kind of task a message is, from its words: the capability packs it needs, the task route its chat should start
 * on, and the skills and tools made for that work. A search, not a model: the words HyperAI's memory search uses,
 * against what each pack is for and how people ask for it. Game thread only.
 */
namespace HyperAIStudio::TaskRouter
{
	struct FPackFit
	{
		FString PackId;
		double Score = 0.0;
	};

	/** One pack's words: how people ask for its kind of work, and its tools' names and descriptions. */
	struct FPackDocument
	{
		FString PackId;
		TArray<FString> Words;
	};

	struct FRouteDecision
	{
		/** Into the routes it was decided from. */
		int32 RouteIndex = INDEX_NONE;
		/** The route's own agent, else the best record on its packs, else the preferred agent; and why. */
		FHyperAIStudioRouteChoice Choice;
	};

	/** The packs a message's task needs, best first: at most Limit, each a clear fit. None for a message about nothing in particular. */
	TArray<FPackFit> PacksFor(const FString& Message, int32 Limit = 2);

	/** The task route a message fits, by its category's name and its packs, or INDEX_NONE when none fits clearly. */
	int32 RouteFor(const FString& Message, const TArray<FHyperAIStudioAgentTaskRoute>& Routes);

	/** The route a new chat's first message fits and the agent it starts with; unset when no route fits or no agent is set up. */
	TOptional<FRouteDecision> Decide(const FString& Message, const TArray<FHyperAIStudioAgentTaskRoute>& Routes, const TArray<FString>& UsableAgents,
		const FString& PreferredAgent);

	/** The bundled skills made for these packs' kind of work, best pack first. */
	TArray<FString> SkillsForPacks(const TArray<FPackFit>& Packs);

	/**
	 * The HyperAI tools of these packs that are enabled now, as a block to send with the message: each with the
	 * toolset_name call_tool takes and what it does. A pack this chat was already pointed at (Given) is left out.
	 */
	FString ToolHintFor(const TArray<FPackFit>& Packs, TSet<FString>& Given);

	// Pure pieces, exposed for tests.
	/** How people ask for a pack's kind of work, beyond its tools' own words; empty for a pack with none. */
	FString PackWords(const FString& PackId);
	/** Every pack's words, from the generated catalog and the tools registered now. */
	TArray<FPackDocument> PackDocuments();
	TArray<FPackFit> RankPacks(const FString& Message, const TArray<FPackDocument>& Documents, int32 Limit);
	int32 RankRoutes(const FString& Message, const TArray<FHyperAIStudioAgentTaskRoute>& Routes, const TArray<FPackDocument>& Documents);
	FString ToolHintFrom(const TArray<FPackFit>& Packs, const TArray<FHyperAIStudioRuntimeToolBinding>& Bindings, const TMap<FString, FString>& PackOfTool,
		TSet<FString>& Given);
}
