// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioSkills.h"

/** A custom agent, defined once in .hyperai/agents/<name>.md for Claude Code and Codex. */
struct FHyperAIStudioAgentDefinition
{
	FString Name;
	FString Description;
	/** "read" (no file writes or shell) or "edit". Unreal MCP calls are open to both. */
	FString Tools = TEXT("edit");
	/** Empty keeps the chat's model. */
	FString ModelClaude;
	FString ModelCodex;
	TArray<FString> Skills;
	FString Instructions;
	/** Absolute path of the definition, or of a Claude-only agent's file. */
	FString File;
	/** project (.hyperai/agents) or claude (a .claude/agents file HyperAIStudio did not write). */
	FString Scope;
	/** Installed with the gameplay team and not edited since. */
	bool bBuiltIn = false;
	/** Project agents, per CLI: synced, stale, missing, edited (changed in that CLI's folder) or taken (another agent has the name). */
	FString ClaudeState;
	FString CodexState;
};

/**
 * Claude Code reads subagents from .claude/agents/<name>.md and Codex from .codex/agents/<name>.toml. HyperAIStudio
 * keeps one definition per agent in .hyperai/agents and writes both, ending each generated file with a marker that
 * holds a hash of what it wrote. A file without the marker, or edited since, is never overwritten or deleted.
 */
namespace HyperAIStudio::Agents
{
	FString CanonicalDirectory(const FString& ProjectRoot);
	FString ClaudeDirectory(const FString& ProjectRoot);
	FString CodexDirectory(const FString& ProjectRoot);
	FString WorkflowDirectory(const FString& ProjectRoot);
	/** The plugin's Resources/AgentTeam: agents/, skills/ and workflows/. */
	FString TeamDirectory();

	bool Parse(const FString& Text, FHyperAIStudioAgentDefinition& OutAgent, FString& OutError);
	FString Format(const FHyperAIStudioAgentDefinition& Agent);
	FString ToClaude(const FHyperAIStudioAgentDefinition& Agent);
	FString ToCodex(const FHyperAIStudioAgentDefinition& Agent);
	/** A TOML basic string, quotes included. */
	FString TomlString(const FString& Value);

	/** Text with a last line naming Tag, Source and the hash of Text, inside the file type's comment. */
	FString WithMarker(const FString& Text, const FString& Tag, const FString& Source, const TCHAR* CommentStart, const TCHAR* CommentEnd);
	/** "clean" when the Tag marker's hash still matches, "edited" when not, "foreign" without one. */
	FString MarkerState(const FString& Text, const FString& Tag);

	TArray<FHyperAIStudioAgentDefinition> List(const FString& ProjectRoot);
	/** Brings .claude/agents and .codex/agents in line with .hyperai/agents, within the rules above. */
	Skills::FSyncReport Sync(const FString& ProjectRoot);
	/**
	 * HyperAI's team: its agents (director, programmer, level designer, lighting artist, technical artist and QA
	 * tester), the team skill and Claude Code's team workflow, then a sync. Members, skills and workflows the team
	 * no longer has are removed where the project did not edit them. With bUpdateOnly, refreshes an installed team,
	 * the retired one included, and adds nothing to a project without one.
	 */
	Skills::FSyncReport InstallTeam(const FString& ProjectRoot, bool bUpdateOnly);
	/** Every member of the team is in the project, with the team skill. */
	bool IsTeamInstalled(const FString& ProjectRoot);
	/**
	 * Message as a request for the team, in the agent's own way: its team skill for Claude Code ("/team") and Codex
	 * ("$team"), and the skill's file for an agent without skills or a chat that started before the team was added.
	 */
	FString TeamRequest(const FString& AgentName, const FString& Message);
	/** Writes the definition and syncs. PreviousName renames an existing agent. */
	bool Save(const FString& ProjectRoot, const FHyperAIStudioAgentDefinition& Agent, const FString& PreviousName, FString& OutError);
	bool Delete(const FString& ProjectRoot, const FString& Name, FString& OutError);
	/** Makes a Claude-only agent a project agent, so Codex gets it too. */
	bool Promote(const FString& ProjectRoot, const FString& Name, FString& OutError);
}
