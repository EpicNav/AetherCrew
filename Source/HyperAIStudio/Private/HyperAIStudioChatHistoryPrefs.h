// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "HyperAIStudioChatHistoryPrefs.generated.h"

/**
 * Per-user names, pins and groups for agent chats, keyed by session id, and how the chat list is shown.
 *
 * Kept here rather than written into the agents' own session files: those belong to the CLI, and a
 * rename must never risk corrupting a conversation. EditorPerProjectUserSettings, so it stays local.
 */
UCLASS(Config = EditorPerProjectUserSettings)
class UHyperAIStudioChatHistoryPrefs : public UObject
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxTitleChars = 90;
	static constexpr int32 MaxTrackedSessions = 200;
	static constexpr int32 MaxGroups = 40;
	static constexpr int32 MaxGroupNameChars = 40;
	static constexpr int32 MaxGroupedSessions = 1000;

	UPROPERTY(Config)
	TMap<FString, FString> Titles;

	UPROPERTY(Config)
	TArray<FString> Pinned;

	/** Chats that run with Claude Code Remote Control, so claude.ai and the Claude app can reach them. */
	UPROPERTY(Config)
	TArray<FString> Remote;

	/** Repeated workflows already offered as skills, so each is suggested once. */
	UPROPERTY(Config)
	TArray<FString> SuggestedWorkflows;

	/** The user's own chat groups, in the order the list shows them. */
	UPROPERTY(Config)
	TArray<FString> Groups;

	/** The group each chat was moved into. */
	UPROPERTY(Config)
	TMap<FString, FString> ChatGroups;

	/** Sections of the chat list folded shut: Pinned, a date, or a group (see SectionKey in the sidebar). */
	UPROPERTY(Config)
	TArray<FString> Collapsed;

	/** Whose chats the list shows: empty for all, else an agent name. */
	UPROPERTY(Config)
	FString AgentFilter;

	static FString GetTitle(const FString& SessionId);
	static bool IsPinned(const FString& SessionId);
	static bool IsRemote(const FString& SessionId);
	static void SetRemote(const FString& SessionId, bool bRemote);
	/** An empty or blank name clears the rename and restores the agent's own title. */
	static void SetTitle(const FString& SessionId, const FString& Title);
	static void TogglePinned(const FString& SessionId);

	static TArray<FString> GetGroups();
	/** False, with nothing changed, for a blank, overlong or duplicate name, or one group too many. */
	static bool AddGroup(const FString& Name);
	/** Its chats follow it. False for the same reasons as AddGroup. */
	static bool RenameGroup(const FString& From, const FString& To);
	/** Its chats go back to their dates. */
	static void RemoveGroup(const FString& Name);
	/** The group a chat was moved into, or empty. */
	static FString GetGroup(const FString& SessionId);
	/** An empty group takes the chat out of its group. */
	static void SetGroup(const FString& SessionId, const FString& Group);
	static bool IsCollapsed(const FString& Section);
	static void SetCollapsed(const FString& Section, bool bCollapsed);
	static FString GetAgentFilter();
	static void SetAgentFilter(const FString& AgentName);
	/** Drops everything kept about a chat that was deleted. */
	static void Forget(const FString& SessionId);
};
