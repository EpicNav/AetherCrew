// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

DECLARE_DELEGATE_OneParam(FHyperAIStudioOnResumeChat, const FHyperAIStudioChatSession& /*Session*/);
DECLARE_DELEGATE_TwoParams(FHyperAIStudioOnSetChatRemote, const FHyperAIStudioChatSession& /*Session*/, bool /*bRemote*/);

/**
 * This project's past agent chats, newest first, in sections that fold: Pinned, the user's own groups, then by date.
 * A switch shows every agent's chats or one agent's. Clicking a chat resumes it.
 */
class SHyperAIStudioChatHistorySidebar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioChatHistorySidebar) {}
		SLATE_EVENT(FHyperAIStudioOnResumeChat, OnResumeChat)
		SLATE_EVENT(FSimpleDelegate, OnClose)
		/** Start a fresh conversation with the selected agent. */
		SLATE_EVENT(FSimpleDelegate, OnNewChat)
		/** Turn Claude Code Remote Control on or off for a chat. */
		SLATE_EVENT(FHyperAIStudioOnSetChatRemote, OnSetRemote)
		/** End a chat that runs in the background. */
		SLATE_EVENT(FHyperAIStudioOnResumeChat, OnStopChat)
		/** Delete a chat the user confirmed. */
		SLATE_EVENT(FHyperAIStudioOnResumeChat, OnDeleteChat)
		/** Session id of the chat running in the terminal, highlighted in the list. */
		SLATE_ATTRIBUTE(FString, ActiveSessionId)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	/** Re-read the agents' session stores, and which chats are running, off the game thread. */
	void Refresh();
	/** Redraw from what is loaded, after a pin, rename, group or Remote change. */
	void RebuildFiltered();
	/** Still reading the agents' stores after a Refresh. */
	bool IsLoading() const { return bLoading; }

private:
	/** A section heading when Session is null, otherwise a chat. */
	struct FRow
	{
		/** The section: "pinned", "group:<name>", or a date such as "today" or "m:2026-09". */
		FString SectionKey;
		FText Heading;
		int32 Count = 0;
		bool bCustomGroup = false;
		bool bCollapsed = false;
		TSharedPtr<FHyperAIStudioChatSession> Session;
	};
	using FRowPtr = TSharedPtr<FRow>;

	TSharedRef<ITableRow> GenerateRow(FRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> GenerateHeading(const FRowPtr& Row, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedPtr<SWidget> BuildContextMenu();
	/** Rename and delete for one of the user's groups. */
	void OpenGroupMenu(const FString& Group);
	/** Asks for a group name in a small box at the cursor; OnName gets what was typed when Enter is pressed. */
	void AskGroupName(const FText& Label, const FString& Initial, TFunction<void(const FString&)> OnName);
	void ConfirmDelete(const FHyperAIStudioChatSession& Session);

	FHyperAIStudioOnResumeChat OnResumeChat;
	FHyperAIStudioOnSetChatRemote OnSetRemote;
	FHyperAIStudioOnResumeChat OnStopChat;
	FHyperAIStudioOnResumeChat OnDeleteChat;
	FSimpleDelegate OnClose;
	FSimpleDelegate OnNewChat;
	/** Session whose title is being edited inline, if any. */
	FString RenamingSessionId;
	TAttribute<FString> ActiveSessionId;

	TSharedPtr<SListView<FRowPtr>> List;
	TArray<FHyperAIStudioChatSession> Sessions;
	TArray<FRowPtr> Filtered;
	FString Filter;
	bool bLoading = false;
};
