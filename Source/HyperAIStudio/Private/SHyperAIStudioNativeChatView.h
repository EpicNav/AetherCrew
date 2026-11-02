// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioMentions.h"
#include "HyperAIStudioNativeChat.h"
#include "HyperAIStudioPreviewImage.h"
#include "HyperAIStudioPromptPresets.h"
#include "Widgets/SCompoundWidget.h"

class SComboButton;
class SMenuAnchor;
class SHyperAIStudioMentionPicker;
class SMultiLineEditableTextBox;
class SHyperAIStudioPromptPicker;

/**
 * Sees a typed message before it is sent; true when it took the message, as a chat that starts again on another agent
 * does. Hints are what the message's preset names, if it came from one.
 */
DECLARE_DELEGATE_RetVal_ThreeParams(bool, FOnHyperAIStudioRouteMessage, const FString& /*Message*/, const TArray<FString>& /*Images*/,
	const FHyperAIStudioMessageHints& /*Hints*/);

/** A mode picked in the composer, after the chat has taken it. */
DECLARE_DELEGATE_OneParam(FOnHyperAIStudioModeChosen, const FHyperAIStudioChatMode& /*Mode*/);

/** The chat tab's native view of a Codex, Gemini or Claude Code chat: transcript, plan, approvals and a composer. */
class SHyperAIStudioNativeChatView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioNativeChatView) {}
		SLATE_ARGUMENT(TSharedPtr<FHyperAIStudioNativeChat>, Chat)
		SLATE_EVENT(FOnHyperAIStudioRouteMessage, OnRoute)
		SLATE_EVENT(FOnHyperAIStudioModeChosen, OnModeChosen)
		/** Names the chat in a read-only mode's refusals, such as "HyperAI Chat 2". */
		SLATE_ARGUMENT(FString, ChatLabel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioNativeChatView() override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	TSharedPtr<FHyperAIStudioNativeChat> GetChat() const { return Chat; }
	/** Puts text into the composer for the user to finish; never sends it. */
	void InsertText(const FString& Text);
	/**
	 * Puts a prompt preset's filled text in the composer (after a blank line when it holds something), selects its first
	 * blank and says which are left; never sends it. The next message goes with the preset's packs, skills and agent, and
	 * a preset for the team turns Team on (sent with Team off, it goes to this agent alone). Typing the composer empty
	 * forgets the preset, with the Team it turned on and its line under the composer.
	 */
	void InsertPreset(const FHyperAIStudioPromptPreset& Preset, const FString& Text, const TArray<FString>& Blanks = {});
	/** Puts the chat in a mode, as picking it in the composer does. */
	void ChooseMode(const FHyperAIStudioChatMode& Mode);
	/** Opens the prompt picker by the composer, as Prompts and Ctrl+Space do. */
	void OpenPrompts();
	void FocusComposer();
	/** What the composer holds now. */
	FString GetComposerText() const;
	/** The line under the composer: what a capture or save said, or the blanks a preset left. */
	const FString& GetNotice() const { return Notice; }
	bool IsTeam() const { return bTeam; }
	/** What the next message goes with: the inserted preset's packs, skills and agent; empty when none is pending. */
	FHyperAIStudioMessageHints GetPendingHints() const;
	/** Adds a picture to the next message; false when it is not an image file this chat can send. */
	bool Attach(const FString& Path);
	/**
	 * Adds @-mention chips (assets, actors, files, skills, operations), each one's "@Label" token typed in at the cursor.
	 * The message goes with an exact reference for every chip whose token is still in it. Drops, pastes and the @ picker
	 * all come here.
	 */
	void AddMentions(const TArray<FHyperAIStudioMention>& InMentions);
	const TArray<FHyperAIStudioMention>& GetMentions() const { return Mentions; }
	/** What Submit would send now: the composer's text and the references of its chips. */
	FString GetMessageWithMentions() const;
	const TArray<FString>& GetAttached() const { return Attached; }

	virtual FReply OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event) override;
	virtual FReply OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event) override;

	/** The level viewport as a PNG under the chat's image folder, for attaching; empty when there is none to read. */
	static FString CaptureViewport(FString& OutError);
	/** Copies a chat picture into the project's generated-images folder with a sidecar; the project-relative path. */
	static bool SaveToProject(const FHyperAIStudioChatItem& Item, const FString& AgentName, FString& OutProjectPath, FString& OutError);

	/** What a tool, command, file edit or thought shows when opened: its result as a report, then what it was asked. */
	static FString StepMarkdown(const FHyperAIStudioChatItem& Item);
	/** Whether the transcript keeps the newest text in view: until the user scrolls up to read, and again at the end. */
	bool IsFollowing() const { return bFollow; }
	/** Whether the transcript is scrolled to its end, as last laid out. */
	bool IsAtEnd() const;
	/** Opens or closes a step's details, as clicking its row does. */
	void SetOpen(const FString& ItemId, bool bOpen);
#if WITH_DEV_AUTOMATION_TESTS
	/** Types Text into the composer and sends it as the Send button does. */
	void SubmitForTest(const FString& Text);
#endif

private:
	using FItemPtr = TSharedPtr<FHyperAIStudioChatItem>;

	void Refresh();
	void Submit();
	/**
	 * The composer holds the auto-approval command: runs it here, so no agent ever sees it, and empties the composer if it
	 * turned on. bFromEnterKey is false for the Send button, which the command does not work from.
	 */
	void RunAutoApprovalCommand(bool bFromEnterKey);
	/** The composer's text with one character per line end, and the cursor as an index into it. */
	FString GetComposerTextAndCursor(int32& OutCursor) const;
	void SetComposerText(const FString& Text, int32 Cursor);
	/** Opens, updates or closes the @ picker for the query the cursor is in. */
	void RefreshMentionPicker();
	void CloseMentionPicker();
	void ChooseMention(const FHyperAIStudioMention& Mention);
	void RebuildMentions();
	void RebuildAttachments();
	TSharedRef<SWidget> BuildPromptPicker();
	TSharedRef<SWidget> BuildModeMenu();
	/** A picture scaled to fit MaxSize; Path is read as it paints, and the file reloads when it changes. */
	TSharedRef<SWidget> Picture(TFunction<FString()> Path, const FVector2D& MaxSize);
	TSharedRef<SWidget> ImageActions(const FItemPtr& Item);
	TSharedRef<SWidget> MakeRow(const FItemPtr& Item);
	/** A tool call, command, file edit or thought: one line with its state, opening to the details. */
	TSharedRef<SWidget> MakeStep(const FItemPtr& Item);
	TSharedRef<SWidget> BuildPlan();

	TSharedPtr<FHyperAIStudioNativeChat> Chat;
	FOnHyperAIStudioRouteMessage OnRoute;
	FOnHyperAIStudioModeChosen OnModeChosen;
	FString ChatLabel;
	TArray<FItemPtr> Rows;
	TSharedPtr<class SScrollBox> Scroll;
	TSharedPtr<class SVerticalBox> Transcript;
	bool bFollow = true;
	TSharedPtr<SMultiLineEditableTextBox> Composer;
	TSharedPtr<class SVerticalBox> PlanBox;
	TSharedPtr<class SWrapBox> AttachmentBox;
	TArray<FString> Attached;
	/** @-mention chips in the composer, in the order they were added. */
	TArray<FHyperAIStudioMention> Mentions;
	TSharedPtr<class SWrapBox> MentionBox;
	TSharedPtr<SMenuAnchor> MentionAnchor;
	TSharedPtr<SHyperAIStudioMentionPicker> MentionPicker;
	/** Typing changed the composer: the picker is updated on the next tick, once the cursor has moved. */
	bool bMentionRefreshPending = false;
	/** What the last Save to project or capture said, shown under the composer. */
	FString Notice;
	/** Messages go to HyperAI's team, not to this agent alone. */
	bool bTeam = false;
	TSharedPtr<SComboButton> PromptsButton;
	TWeakPtr<SHyperAIStudioPromptPicker> Picker;
	/** The preset whose text is in the composer, until it is sent or typed away. */
	TOptional<FHyperAIStudioPromptPreset> PendingPreset;
	/** Its category's task route, for a new chat's first message. */
	FString PendingRoute;
	/** The blanks it left, as "[Label]" in the text. */
	TArray<FString> PendingBlanks;
	/** The first send with blanks left only says so; the next one sends. */
	bool bBlanksNoted = false;
	/** Team is on because the pending preset turned it on, not the user; typing the preset away turns it off again. */
	bool bTeamFromPreset = false;
	/** The line the pending preset put under the composer, which leaves with it. */
	FString PresetNotice;
	/** Items whose details are open. */
	TSet<FString> Open;
	FDelegateHandle ChangedHandle;
	int32 PlanRevision = 0;
};
