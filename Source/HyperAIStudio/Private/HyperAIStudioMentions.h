// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FAssetData;
class AActor;

/** What an @-mention in the chat composer points at. */
enum class EHyperAIStudioMentionKind : uint8
{
	Asset,
	Actor,
	File,
	Skill,
	Operation
};

/** One @-mention: shown as a chip, sent to the agent as a precise reference. */
struct FHyperAIStudioMention
{
	EHyperAIStudioMentionKind Kind = EHyperAIStudioMentionKind::Asset;
	/** What the chip and the "@Label" token in the message say: an asset or file name, an actor's label, a skill's name. */
	FString Label;
	/** The precise reference: an asset's object path, an actor's path, a file's absolute path, a skill's SKILL.md, an operation id. */
	FString Reference;
	/** Shown beside it and sent with it: an asset's class, an actor's class, a file's project-relative path, an operation's tool. */
	FString Detail;

	bool operator==(const FHyperAIStudioMention& Other) const { return Kind == Other.Kind && Reference == Other.Reference; }
};

/**
 * The composer's @ picker and its chips. Pure where it can be: fuzzy filtering, the active @ query, the token a chip
 * leaves in the text and the block the agent gets with the message are all testable without Slate. Gathering candidates
 * reads the asset index, the editor world, the project folder, its skills and the activity log, on the game thread.
 */
namespace HyperAIStudio::Mentions
{
	constexpr int32 MaxPickerResults = 40;
	/** Project files the picker scans for, at most; Source, Config, .hyperai, Docs and the project's root files. */
	constexpr int32 MaxProjectFiles = 6000;

	const TCHAR* KindName(EHyperAIStudioMentionKind Kind);

	/**
	 * How well Query fuzzily matches Candidate: every query character in order, with bonuses for a match at the start, at
	 * a word start (after _ - . / space or a lower-to-upper step) and for runs. -1 when it does not match; an empty query
	 * scores 0. Case-insensitive.
	 */
	int32 FuzzyScore(FStringView Query, FStringView Candidate);

	/**
	 * The candidates Query matches, best first: by fuzzy score on the label (the reference counts at half), then kind
	 * order (assets, actors, files, skills, operations), then label. At most Max.
	 */
	TArray<FHyperAIStudioMention> Filter(const TArray<FHyperAIStudioMention>& Candidates, const FString& Query, int32 Max = MaxPickerResults);

	/**
	 * The @ query the cursor is in: an "@" at the start or after whitespace, then non-space characters up to the cursor.
	 * OutStart is the "@"'s index. False when the cursor is not in one.
	 */
	bool FindActiveQuery(const FString& Text, int32 Cursor, int32& OutStart, FString& OutQuery);

	/** The token a chip leaves in the text: "@" and the label with spaces made underscores. */
	FString Token(const FHyperAIStudioMention& Mention);

	/** Text with the query at [Start, Cursor) replaced by Mention's token and a space; OutCursor is just after it. */
	FString ReplaceQuery(const FString& Text, int32 Start, int32 Cursor, const FHyperAIStudioMention& Mention, int32& OutCursor);

	/** The mentions whose token is still in Text, in order, once each; a chip whose token was typed away is dropped. */
	TArray<FHyperAIStudioMention> StillReferenced(const FString& Text, const TArray<FHyperAIStudioMention>& Mentions);

	/**
	 * The message as the agent gets it: Text, then a "Referenced with @:" block with one line per mention still in the text,
	 * "- @Label: asset /Game/Env/SM_Rock.SM_Rock (StaticMesh)". Text alone when there are none.
	 */
	FString Serialize(const FString& Text, const TArray<FHyperAIStudioMention>& Mentions);

	/** Chips for Content Browser assets (a drop, a paste, the context menu). */
	TArray<FHyperAIStudioMention> FromAssets(TConstArrayView<FAssetData> Assets);
	FHyperAIStudioMention FromActor(const AActor& Actor);

	/**
	 * Chips for pasted text when every non-empty line names an asset the registry knows ("/Game/A/B.B", or a Content
	 * Browser copy such as "/Script/Engine.StaticMesh'/Game/A/B.B'") or a loaded actor's path; empty otherwise, so
	 * ordinary text pastes as text. Game thread.
	 */
	TArray<FHyperAIStudioMention> FromPastedText(const FString& Text);

	/**
	 * The picker's candidates for Query, from every source, already filtered and ranked: assets from the project index
	 * (by fuzzy name, then by meaning for a longer query), actors in the editor world, project files, project skills and
	 * the session's recent HyperAI operations. Game thread.
	 */
	TArray<FHyperAIStudioMention> Gather(const FString& Query, int32 Max = MaxPickerResults);

	/** One line for the picker's footer: the asset index's state and size, and that it stays on this computer. */
	FString DescribeIndex();

#if WITH_DEV_AUTOMATION_TESTS
	/** Tests: the project files the picker offers, instead of scanning the project; unset scans again. */
	void SetProjectFilesForTests(const TOptional<TArray<FString>>& Files);
#endif
}
