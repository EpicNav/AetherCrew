// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAgentActivity.h"

struct FHyperAIStudioAgentDefinition;
struct FHyperAIStudioAgentTaskRoute;
struct FHyperAIStudioRuntimeToolBinding;

/**
 * What kind of task a message is, from its words: the capability packs it needs, the project agent it is for, the task
 * route its chat should start on, and the skills and tools made for that work. A search, not a model: the words
 * HyperAI's memory search uses, against what each pack and agent is for and how people ask for it. Game thread only.
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

	/** The project agent (.hyperai/agents) a task is for. */
	struct FAgentPick
	{
		/** The agent to hand the task to; empty when the team takes it. */
		FString Agent;
		FString Description;
		/** Its parts are for several roles, so HyperAI's team runs it: the director splits it into parts. */
		bool bTeam = false;
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

	/**
	 * The project agent a message's task is for: by what the agent says it does and the work of its role's packs, or of
	 * its skills' for an agent of the project's own. A request that asks first to test or to plan goes to the QA tester
	 * or the director whatever it is about; one with parts for two builders that fit about as well, neither able to do
	 * the other's, goes to the team when bTeam (the director and the team skill are in the project). Unset when no agent
	 * clearly fits: then the chat's agent does the work itself.
	 */
	TOptional<FAgentPick> AgentFor(const FString& Message, const TArray<FHyperAIStudioAgentDefinition>& ProjectAgents, bool bTeam,
		const TArray<FPackDocument>& Documents);
	/**
	 * The project agent for a message's task, as a block to send with it: hand the task to that agent in ChatAgent's own
	 * way (Claude Code's Agent tool, Codex's multi-agent tools, or the agent's file for any other). Nothing in a chat that
	 * runs as a project agent already (StartedAs), for a request to the team, or for an agent this chat was already
	 * pointed at (Given holds "agent:<name>", or "agent:team").
	 */
	FString AgentHintFor(const FString& Message, const FString& ProjectRoot, const FString& ChatAgent, const FString& StartedAs, TSet<FString>& Given);

	// Pure pieces, exposed for tests.
	/** How people ask for a pack's kind of work, beyond its tools' own words; empty for a pack with none. */
	FString PackWords(const FString& PackId);
	/** Every pack's words, from the generated catalog and, with bToolDescriptions, what the tools registered now say they do. */
	TArray<FPackDocument> PackDocuments(bool bToolDescriptions = true);
	TArray<FPackFit> RankPacks(const FString& Message, const TArray<FPackDocument>& Documents, int32 Limit);
	int32 RankRoutes(const FString& Message, const TArray<FHyperAIStudioAgentTaskRoute>& Routes, const TArray<FPackDocument>& Documents);
	FString ToolHintFrom(const TArray<FPackFit>& Packs, const TArray<FHyperAIStudioRuntimeToolBinding>& Bindings, const TMap<FString, FString>& PackOfTool,
		TSet<FString>& Given);
	/** The block AgentHintFor sends for a pick, with the agent's definition from ProjectAgents. */
	FString AgentHintFrom(const FAgentPick& Pick, const TArray<FHyperAIStudioAgentDefinition>& ProjectAgents, const FString& ProjectRoot,
		const FString& ChatAgent, TSet<FString>& Given);
}
