// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** What a chat mode lets HyperAI's tools do. */
enum class EHyperAIStudioModeApproval : uint8
{
	/** The user's own approval settings. */
	Normal,
	/** Every edit plan waits for a click, whatever Preferences and auto-approval say. */
	Ask,
	/** Edit plans are refused, so the agent can only read. */
	ReadOnly
};

/**
 * A named preset for a chat: what the agent is told, the packs, tools and skills its messages go with, how far HyperAI's
 * tools may go, and the agent and model it prefers. Built-in modes are in code; a project's are JSON files in
 * .hyperai/modes/<id>.json, shared through version control, and never hold a key.
 */
struct FHyperAIStudioChatMode
{
	/** Lower case letters, digits and hyphens: the file's name. */
	FString Id;
	FString Name;
	FString Description;
	/** Added to what the agent is told when the chat enters the mode. */
	FString Instructions;
	/** Catalog pack ids, hyper_* tool names and skill names its messages go with. */
	TArray<FString> Packs;
	TArray<FString> Tools;
	TArray<FString> Skills;
	EHyperAIStudioModeApproval Approval = EHyperAIStudioModeApproval::Normal;
	/** The agent ("Claude Code", "Codex", ...) and model it prefers; empty keeps the chat's. */
	FString Agent;
	FString Model;
	/** One of HyperAI's own modes, as shipped. */
	bool bBuiltIn = false;
	/** The project file that defines it (a built-in one the project changed has one too); empty for a built-in as shipped. */
	FString File;

	bool IsReadOnly() const { return Approval == EHyperAIStudioModeApproval::ReadOnly; }
};

/** Game thread only. */
namespace HyperAIStudio::ChatModes
{
	/** .hyperai/modes under the project. */
	FString Directory(const FString& ProjectRoot);
	/** Plan, Build, Review, Art pass and QA. */
	TArray<FHyperAIStudioChatMode> BuiltIns();
	/** The built-ins (or the project's version of one), then the project's own, by name. */
	TArray<FHyperAIStudioChatMode> List(const FString& ProjectRoot);
	bool Find(const FString& ProjectRoot, const FString& Id, FHyperAIStudioChatMode& OutMode);
	/** The mode a chat starts in. */
	FString DefaultId();

	/** "normal", "ask" or "read-only". */
	FString ApprovalName(EHyperAIStudioModeApproval Approval);
	bool ParseApproval(const FString& Name, EHyperAIStudioModeApproval& OutApproval);
	/** A string that looks like an API key or token, which a mode file must never hold. */
	bool LooksLikeSecret(const FString& Text);

	FString ToJson(const FHyperAIStudioChatMode& Mode);
	/** Reads and checks a mode: a valid id, a name, a known approval, lists of names, and nothing that looks like a key. */
	bool FromJson(const FString& Json, FHyperAIStudioChatMode& OutMode, FString& OutError);

	/** Writes .hyperai/modes/<id>.json; PreviousId renames a project mode. */
	bool Save(const FString& ProjectRoot, const FHyperAIStudioChatMode& Mode, const FString& PreviousId, FString& OutError);
	/** Removes the project's file; a built-in mode goes back to how it shipped. */
	bool Delete(const FString& ProjectRoot, const FString& Id, FString& OutError);
	/** Copies a mode file into the project after checking it; OutId is its id. */
	bool Import(const FString& ProjectRoot, const FString& SourceFile, FString& OutId, FString& OutError);
	bool Export(const FString& ProjectRoot, const FString& Id, const FString& DestinationFile, FString& OutError);

	/** What the agent is told on entering Mode, as a block that goes with the next message. */
	FString InstructionsBlock(const FHyperAIStudioChatMode& Mode);

	/**
	 * The modes open chats are in. Owner is the chat; Label names it for messages ("HyperAI Chat 2"). A null Mode, or a
	 * chat that ends, takes it out.
	 */
	void SetActive(const void* Owner, const FHyperAIStudioChatMode* Mode, const FString& Label);
	void ClearActive(const void* Owner);
	/** The first open chat in a read-only mode, as "<Label> is in <Name> mode", or empty. */
	FString ReadOnlyHolder();
	/** An open chat is in a mode that asks for every edit. */
	bool IsAskActive();
	/** What an edit plan is told while a chat is read-only; empty when none is. */
	FString ReadOnlyRefusal();
#if WITH_DEV_AUTOMATION_TESTS
	void ClearAllActiveForTests();
#endif
}
