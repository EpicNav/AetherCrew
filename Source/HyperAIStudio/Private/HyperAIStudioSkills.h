// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** One skill folder with a SKILL.md, as the Skills panel and the chat's Skills menu show it. */
struct FHyperAIStudioSkill
{
	FString Name;
	FString Description;
	/** Absolute path of the skill's folder. */
	FString Folder;
	/** project (.agents/skills), claude (.claude/skills only), personal-claude (~/.claude/skills), personal-agents (~/.agents or ~/.codex). */
	FString Scope;
	/** Only the user starts it: disable-model-invocation for Claude Code, the agents/openai.yaml policy for Codex. */
	bool bMenuOnly = false;
	/** Installed from the plugin and not edited since. */
	bool bBuiltIn = false;
	/** Project skills only: synced, stale, missing, edited (its Claude copy was changed there) or taken (a Claude-only skill has the name). */
	FString MirrorState;
	/** Why its SKILL.md is not a valid skill, so agents cannot use it; empty for a valid one. */
	FString Problem;
};

/**
 * Claude Code and Codex read the same SKILL.md format from different folders. The project's skills live once in
 * .agents/skills (Codex's folder); HyperAIStudio copies each into .claude/skills and marks the copy with a hash of
 * what it wrote, so it can tell a copy edited in place from one it may refresh. Nothing HyperAIStudio did not write,
 * and nothing edited since, is ever overwritten or deleted. Game thread for anything that touches UObjects.
 */
namespace HyperAIStudio::Skills
{
	FString CanonicalDirectory(const FString& ProjectRoot);
	FString ClaudeDirectory(const FString& ProjectRoot);
	FString PersonalClaudeDirectory();
	TArray<FString> PersonalAgentsDirectories();
	/** The plugin's Resources/Skills. */
	FString BuiltInDirectory();

	bool IsValidName(const FString& Name);
	bool ParseSkill(const FString& SkillText, FString& OutName, FString& OutDescription, bool& bOutMenuOnly);
	/**
	 * Checks a SKILL.md the way Claude Code and Codex read it: frontmatter between --- lines with a name (lowercase
	 * letters, digits and hyphens, up to 64, not naming Anthropic or Claude, and the folder's name when FolderName is
	 * given) and a description (up to 1024 characters), neither holding XML tags. OutError says what is wrong.
	 */
	bool ValidateSkill(const FString& SkillText, const FString& FolderName, FString& OutName, FString& OutDescription, FString& OutError);
	/** A new project skill in .agents/skills from Name and Description, with a body to fill in; synced to Claude Code. */
	bool Create(const FString& ProjectRoot, const FString& Name, const FString& Description, FString& OutError);
	/** Hash of a skill folder's files and relative paths, leaving out HyperAIStudio's marker files. */
	FString FolderHash(const FString& Folder);

	/** bIncludeInvalid adds folders whose SKILL.md is not valid, with Problem set, for the Skills page to show. */
	TArray<FHyperAIStudioSkill> ListProjectSkills(const FString& ProjectRoot, bool bIncludeInvalid = false);
	TArray<FHyperAIStudioSkill> ListPersonalSkills();
	/** What the chat's Skills menu offers Claude Code or Codex; nothing for other agents. */
	TArray<FHyperAIStudioSkill> ListForAgent(const FString& ProjectRoot, const FString& AgentName);
	/** "/" for Claude Code, "$" for Codex, empty for agents without skills. */
	FString InvocationPrefix(const FString& AgentName);
	/**
	 * The project skills that fit Message and the chat was not pointed at yet, as a short block to send with it, or
	 * empty. Only skills an agent may start on its own; Given holds what the chat already got. Preferred names the
	 * skills made for the task's kind of work, which come first when the project has them.
	 */
	FString HintFor(const FString& ProjectRoot, const FString& Message, TSet<FString>& Given, const TArray<FString>& Preferred = {});

	struct FSyncReport
	{
		int32 Changed = 0;
		TArray<FString> Notes;
	};
	/** Installs the plugin's built-in skills, and updates the ones nobody edited. */
	FSyncReport InstallBuiltIns(const FString& ProjectRoot, const FString& SourceDirectory);
	/** Brings .claude/skills in line with .agents/skills, within the rules above. */
	FSyncReport SyncClaudeMirror(const FString& ProjectRoot);
	/** Makes a Claude-only or edited Claude copy the project's skill. */
	bool Promote(const FString& ProjectRoot, const FString& Name, FString& OutError);
	bool SetMenuOnly(const FString& SkillFolder, bool bMenuOnly, FString& OutError);
	/** Copies a skill folder, or a zip holding one, into the project's skills. Returns its name. */
	FString Import(const FString& ProjectRoot, const FString& Source, FString& OutError);
	bool Export(const FString& SkillFolder, const FString& DestinationDirectory, FString& OutError);
	bool Delete(const FString& ProjectRoot, const FString& Name, FString& OutError);
	/** One-way copy of Epic's AgentSkill classes (Description and Instructions) into project skills. */
	int32 ImportEpicSkills(const FString& ProjectRoot, TArray<FString>& OutNotes);

	/** Unreal MCP tool calls in a Claude or Codex transcript, in order, repeats collapsed: "BlueprintTools.create". */
	TArray<FString> ToolSequence(const FString& TranscriptText);
	/** Runs of three calls, at least one of them a change, found in at least MinSessions chats; most common first. */
	TArray<FString> RepeatedWorkflows(const TArray<TArray<FString>>& Sessions, int32 MinSessions = 3);
}
