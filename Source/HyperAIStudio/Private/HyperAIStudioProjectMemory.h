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
	/**
	 * Why HyperAI keeps an agent's fact out of every chat until the user allows it: it reads like instructions to
	 * agents rather than a note about the project. Empty for almost every fact.
	 */
	FString Held;
	/** YYYY-MM-DD. */
	FString Updated;
	FString Text;
};

/** One finished chat turn: what was asked and how it ended, so a later chat can find what was done before. */
struct FHyperAIStudioMemoryTask
{
	FDateTime Utc;
	/** The agent, and its model when one was picked. */
	FString Agent;
	FString Request;
	/** The agent's last reply in the turn. */
	FString Outcome;
	/** The tools it called and the files it edited, as their chat cards name them. */
	TArray<FString> Tools;
};

/**
 * Project memory. Facts live in .hyperai/memory, shared by every agent and, through Git, every teammate; AGENTS.md
 * lists their titles, and each message a chat sends carries the few that match it. What agents changed, and every
 * task a chat finished, are local logs in the gitignored .hyperai/runtime/memory: they would churn a shared file on
 * every edit. Game thread only.
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
	 * Writes a fact and rebuilds the index. An empty Name adds a new fact, unless an agent saves a title it saved
	 * before, which updates that fact; a set Name replaces that fact, which an agent may only do to a fact an agent
	 * wrote. Text shaped like a key is refused. Returns the fact's name, or empty with OutError set; OutSaved gets the
	 * fact as stored.
	 */
	FString Remember(const FString& ProjectRoot, FHyperAIStudioMemoryFact Fact, FString& OutError, FHyperAIStudioMemoryFact* OutSaved = nullptr);
	/** Deletes a fact. Agents may only delete facts agents wrote. */
	bool Forget(const FString& ProjectRoot, const FString& Name, bool bByAgent, FString& OutError);
	/** Marks an agent-written fact as reviewed by the user, which also releases a held one. */
	bool Confirm(const FString& ProjectRoot, const FString& Name);
	/** Best matches (every visible fact when Query is empty, newest first), hiding what the memory setting hides. */
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
	/** Appends a finished task to the month's history, keys in it removed first. Returns its id, as ContextFor's Given holds it. */
	FString RecordTask(const FString& ProjectRoot, const FHyperAIStudioMemoryTask& Task);
	/** Best matches from the last two months of tasks; newest first when Query is empty. */
	TArray<FHyperAIStudioMemoryTask> RecallTasks(const FString& ProjectRoot, const FString& Query, int32 Limit);
	/**
	 * What memory knows about Message, to send with it: the few facts and past tasks that match it well, as a short
	 * block, or empty when nothing does. Given holds what the chat already got, so nothing is sent twice.
	 */
	FString ContextFor(const FString& ProjectRoot, const FString& Message, TSet<FString>& Given, EHyperAIStudioAgentMemoryMode Mode);
	/** The Project Memory block for AGENTS.md: how memory works, and the facts' titles. */
	FString BuildInstructionSection(const FString& ProjectRoot, EHyperAIStudioAgentMemoryMode Mode);
	/** Fired after the facts change on disk, so AGENTS.md can follow. The change log and task history do not fire it. */
	FSimpleMulticastDelegate& OnChanged();

	// Pure pieces, exposed for tests.
	FString Slugify(const FString& Title);
	/** Keeps text safe to store and to paste into AGENTS.md: no control characters, no links to URLs, no HTML comment markers. */
	FString Sanitize(const FString& Text, int32 MaxChars);
	/** Replaces anything shaped like an API key or access token with a marker. */
	FString RedactKeys(const FString& Text);
	/** Why Text reads like instructions to agents rather than a note about the project, or empty. */
	FString LooksLikeInstructions(const FString& Text);
	bool ParseFact(const FString& FileText, FHyperAIStudioMemoryFact& OutFact);
	FString FormatFact(const FHyperAIStudioMemoryFact& Fact);
	FString FormatChange(const FHyperAIStudioActivityEntry& Entry);
	/** One line for recall: when, which agent, what was asked, how it ended. */
	FString FormatTask(const FHyperAIStudioMemoryTask& Task);
	FString TaskToJson(const FHyperAIStudioMemoryTask& Task);
	bool ParseTask(const FString& JsonLine, FHyperAIStudioMemoryTask& OutTask);
	/** Search words: lowercase runs of letters and digits, two or more long, less the commonest English words, in order. */
	TArray<FString> SearchWords(const FString& Text);

	struct FRank
	{
		double Score = 0.0;
		/** How many distinct query words the document holds. */
		int32 Matched = 0;
		/** How many of those few documents hold: at most two, or one in twenty. */
		int32 MatchedRare = 0;
	};
	/** BM25 of each document (its search words) against the query words; a word matches its other forms (light, lights, lighting). */
	TArray<FRank> Rank(const TArray<FString>& Query, const TArray<TArray<FString>>& Documents);
	FString BuildInstructionSectionFrom(const TArray<FHyperAIStudioMemoryFact>& Facts, EHyperAIStudioAgentMemoryMode Mode);
}
