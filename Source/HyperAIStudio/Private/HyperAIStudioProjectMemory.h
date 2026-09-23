// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FHyperAIStudioActivityEntry;
enum class EHyperAIStudioAgentMemoryMode : uint8;

/** One remembered fact: .hyperai/memory/facts/<Name>.md. */
struct FHyperAIStudioMemoryFact
{
	/** File name without extension: lowercase words joined by dashes. */
	FString Name;
	/** One line saying what the fact is about. */
	FString Title;
	/** convention, decision, fact or gotcha. */
	FString Type = TEXT("fact");
	/** user or agent. */
	FString Source = TEXT("user");
	/** A fact an agent saved stays unconfirmed until the user confirms it; the memory setting decides what that hides. */
	bool bConfirmed = true;
	/** YYYY-MM-DD. */
	FString Updated;
	FString Text;
};

/**
 * Project memory. Facts live in .hyperai/memory, shared by every agent and, through Git, every teammate; their
 * own block in AGENTS.md carries them into every chat without a tool call. What agents changed is a local log
 * in the gitignored .hyperai/runtime/memory, one line per completed operation or agent save: it would churn
 * a shared file on every edit. Game thread only.
 */
namespace HyperAIStudio::Memory
{
	constexpr int32 MaxTitleChars = 120;
	constexpr int32 MaxTextChars = 4000;
	constexpr int32 MaxFacts = 500;

	FString MemoryDirectory(const FString& ProjectRoot);
	/** Newest first; malformed files are skipped. */
	TArray<FHyperAIStudioMemoryFact> LoadFacts(const FString& ProjectRoot);
	/**
	 * Writes a fact and rebuilds the index. An empty Name adds a new fact; a set Name replaces that fact, which
	 * an agent may only do to a fact an agent wrote. Returns the fact's name, or empty with OutError set.
	 */
	FString Remember(const FString& ProjectRoot, FHyperAIStudioMemoryFact Fact, FString& OutError);
	/** Deletes a fact. Agents may only delete facts agents wrote. */
	bool Forget(const FString& ProjectRoot, const FString& Name, bool bByAgent, FString& OutError);
	/** Marks an agent-written fact as reviewed by the user. */
	bool Confirm(const FString& ProjectRoot, const FString& Name);
	/** Best keyword matches (every visible fact when Query is empty), hiding what the memory setting hides. */
	TArray<FHyperAIStudioMemoryFact> Recall(const FString& ProjectRoot, const FString& Query, int32 Limit, EHyperAIStudioAgentMemoryMode Mode);
	/** Appends one line to the month's change log and rewrites recent-changes.md. */
	void RecordChange(const FString& ProjectRoot, const FHyperAIStudioActivityEntry& Entry);
	/**
	 * A package was saved. While a chat agent is working, that is an agent change even when it came through
	 * Epic's tools, which never reach the activity log. One line per package per 10 minutes.
	 */
	void NotePackageSaved(const FString& ProjectRoot, const FString& PackageName);
	/** Newest first, from this month's and last month's logs; only lines naming Target when it is set. */
	TArray<FString> RecentChanges(const FString& ProjectRoot, int32 Limit, const FString& Target = FString());
	/** The Project Memory block for AGENTS.md: the facts, and where recent changes are. */
	FString BuildInstructionSection(const FString& ProjectRoot, EHyperAIStudioAgentMemoryMode Mode);
	/** Fired after the facts change on disk, so AGENTS.md can follow. The change log does not fire it. */
	FSimpleMulticastDelegate& OnChanged();

	// Pure pieces, exposed for tests.
	FString Slugify(const FString& Title);
	/** Keeps text safe to store and to paste into AGENTS.md: no control characters, no links to URLs, no HTML comment markers. */
	FString Sanitize(const FString& Text, int32 MaxChars);
	bool ParseFact(const FString& FileText, FHyperAIStudioMemoryFact& OutFact);
	FString FormatFact(const FHyperAIStudioMemoryFact& Fact);
	FString FormatChange(const FHyperAIStudioActivityEntry& Entry);
	FString BuildInstructionSectionFrom(const TArray<FHyperAIStudioMemoryFact>& Facts, EHyperAIStudioAgentMemoryMode Mode);
}
