// Games by Hyper 2026.

#include "SHyperAIStudioChatHistorySidebar.h"

#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HyperAIStudioChatHistoryPrefs.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioStyle.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/STextEntryPopup.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioChatHistorySidebar"

namespace HyperAIStudio::ChatHistorySidebar
{
	const TCHAR* const PinnedSection = TEXT("pinned");
	const TCHAR* const GroupPrefix = TEXT("group:");

	FText RelativeTime(const FDateTime& Utc)
	{
		const FTimespan Age = FDateTime::UtcNow() - Utc;
		if (Age.GetTotalMinutes() < 60.0)
		{
			return FText::Format(LOCTEXT("MinutesAgo", "{0}m ago"), FMath::Max(1, FMath::FloorToInt(Age.GetTotalMinutes())));
		}
		if (Age.GetTotalHours() < 24.0)
		{
			return FText::Format(LOCTEXT("HoursAgo", "{0}h ago"), FMath::FloorToInt(Age.GetTotalHours()));
		}
		return FText::Format(LOCTEXT("DaysAgo", "{0}d ago"), FMath::FloorToInt(Age.GetTotalDays()));
	}

	/** The date section for a chat last active at Utc, by the viewer's local calendar day: its key and heading. */
	TPair<FString, FText> DateSection(const FDateTime& Utc)
	{
		const FTimespan LocalOffset = FDateTime::Now() - FDateTime::UtcNow();
		const FDateTime LocalDay = (Utc + LocalOffset).GetDate();
		const int32 DaysAgo = (FDateTime::Now().GetDate() - LocalDay).GetDays();
		if (DaysAgo <= 0)
		{
			return {TEXT("today"), LOCTEXT("GroupToday", "Today")};
		}
		if (DaysAgo == 1)
		{
			return {TEXT("yesterday"), LOCTEXT("GroupYesterday", "Yesterday")};
		}
		if (DaysAgo < 7)
		{
			return {TEXT("week"), LOCTEXT("GroupWeek", "Previous 7 Days")};
		}
		if (DaysAgo < 30)
		{
			return {TEXT("month"), LOCTEXT("GroupMonth", "Previous 30 Days")};
		}
		// FDateTime::ToString has no month names; the culture's own come from ICU. The day is already local, so no zone shift.
		return {TEXT("m:") + LocalDay.ToString(TEXT("%Y-%m")), FText::AsDateTime(LocalDay, TEXT("MMMM yyyy"), TEXT("UTC"))};
	}

	TSharedRef<SWidget> IconButton(const FName Icon, const FText& ToolTip, FSimpleDelegate OnClicked)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(ToolTip)
			.OnClicked_Lambda([OnClicked]()
			{
				OnClicked.ExecuteIfBound();
				return FReply::Handled();
			})
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(Icon))
				.ColorAndOpacity(FSlateColor::UseForeground())
			];
	}

	void Toast(const FText& Text)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 5.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	FString ShownTitle(const FHyperAIStudioChatSession& Session)
	{
		const FString Renamed = UHyperAIStudioChatHistoryPrefs::GetTitle(Session.SessionId);
		return Renamed.IsEmpty() ? Session.Title : Renamed;
	}
}

void SHyperAIStudioChatHistorySidebar::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::ChatHistorySidebar;
	OnResumeChat = InArgs._OnResumeChat;
	OnClose = InArgs._OnClose;
	OnNewChat = InArgs._OnNewChat;
	OnSetRemote = InArgs._OnSetRemote;
	OnStopChat = InArgs._OnStopChat;
	OnDeleteChat = InArgs._OnDeleteChat;
	ActiveSessionId = InArgs._ActiveSessionId;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Panel"))
		.Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "Chats"))
					.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.Title")
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					IconButton("Icons.Plus", LOCTEXT("NewChatTooltip", "Start a new chat with the selected agent"), OnNewChat)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					IconButton("Icons.FolderClosed", LOCTEXT("NewGroupTooltip", "New group, to keep chats together. Move a chat into it from its right-click menu."),
						FSimpleDelegate::CreateLambda([this]()
						{
							AskGroupName(LOCTEXT("NewGroupLabel", "New group"), FString(), [this](const FString& Name)
							{
								if (!UHyperAIStudioChatHistoryPrefs::AddGroup(Name))
								{
									Toast(LOCTEXT("GroupNameTaken", "That group name is empty, too long or already used."));
								}
								RebuildFiltered();
							});
						}))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					IconButton("Icons.Refresh", LOCTEXT("RefreshTooltip", "Re-read chat history"),
						FSimpleDelegate::CreateSP(this, &SHyperAIStudioChatHistorySidebar::Refresh))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					IconButton("Icons.X", LOCTEXT("CloseTooltip", "Hide chat history"), OnClose)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 6.0f)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Search chats"))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					Filter = Text.ToString();
					RebuildFiltered();
				})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SSegmentedControl<FString>)
				.Value_Lambda([]() { return UHyperAIStudioChatHistoryPrefs::GetAgentFilter(); })
				.OnValueChanged_Lambda([this](const FString& Agent)
				{
					UHyperAIStudioChatHistoryPrefs::SetAgentFilter(Agent);
					RebuildFiltered();
				})
				+ SSegmentedControl<FString>::Slot(FString())
				.Text(LOCTEXT("AgentAll", "All"))
				.ToolTip(LOCTEXT("AgentAllTooltip", "Chats with every agent"))
				+ SSegmentedControl<FString>::Slot(TEXT("Claude Code"))
				.Text(LOCTEXT("AgentClaude", "Claude Code"))
				.ToolTip(LOCTEXT("AgentClaudeTooltip", "Only Claude Code chats"))
				+ SSegmentedControl<FString>::Slot(TEXT("Codex"))
				.Text(LOCTEXT("AgentCodex", "Codex"))
				.ToolTip(LOCTEXT("AgentCodexTooltip", "Only Codex chats"))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]() { return Filtered.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text_Lambda([this]()
				{
					if (bLoading)
					{
						return LOCTEXT("Loading", "Reading chat history...");
					}
					if (!Filter.IsEmpty())
					{
						return LOCTEXT("NoMatch", "No chats match.");
					}
					const FString Agent = UHyperAIStudioChatHistoryPrefs::GetAgentFilter();
					return Agent.IsEmpty()
						? LOCTEXT("Empty", "No Claude Code or Codex chats have run in this project yet.")
						: FText::Format(LOCTEXT("EmptyAgent", "No {0} chats have run in this project yet."), FText::FromString(Agent));
				})
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(List, SListView<FRowPtr>)
				.ListItemsSource(&Filtered)
				// Rows only report clicks when selectable: STableRow takes mouse capture, and so fires the click, only then.
				.SelectionMode(ESelectionMode::Single)
				.OnIsSelectableOrNavigable_Lambda([](FRowPtr Row) { return Row.IsValid() && Row->Session.IsValid(); })
				.OnGenerateRow(this, &SHyperAIStudioChatHistorySidebar::GenerateRow)
				.OnContextMenuOpening(this, &SHyperAIStudioChatHistorySidebar::BuildContextMenu)
				.OnMouseButtonClick_Lambda([this](FRowPtr Row)
				{
					if (Row.IsValid() && Row->Session.IsValid())
					{
						OnResumeChat.ExecuteIfBound(*Row->Session);
					}
				})
			]
		]
	];
}

void SHyperAIStudioChatHistorySidebar::Refresh()
{
	if (bLoading)
	{
		return;
	}
	bLoading = true;
	// Session files can be tens of megabytes; read them off the game thread.
	TWeakPtr<SHyperAIStudioChatHistorySidebar> WeakThis = SharedThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Root = FHyperAIStudioService::GetProjectRoot(),
		Claude = FHyperAIStudioService::GetAgentTerminalLaunchCommand(TEXT("Claude Code"))]()
	{
		TArray<FHyperAIStudioChatSession> Loaded = HyperAIStudio::ChatHistory::ListSessions(Root);
		HyperAIStudio::ChatHistory::MergeLiveChats(Loaded, HyperAIStudio::ChatHistory::ListLiveClaudeChats(Claude, Root));
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Loaded = MoveTemp(Loaded)]() mutable
		{
			if (const TSharedPtr<SHyperAIStudioChatHistorySidebar> Pinned = WeakThis.Pin())
			{
				Pinned->Sessions = MoveTemp(Loaded);
				Pinned->bLoading = false;
				Pinned->RebuildFiltered();
			}
		});
	});
}

void SHyperAIStudioChatHistorySidebar::RebuildFiltered()
{
	using namespace HyperAIStudio::ChatHistorySidebar;
	Filtered.Reset();
	const FString Agent = UHyperAIStudioChatHistoryPrefs::GetAgentFilter();
	const bool bSearching = !Filter.IsEmpty();

	// Sections in the order they show: Pinned, the user's groups, then dates, newest first.
	TArray<FString> Order = {PinnedSection};
	TMap<FString, FText> Headings = {{PinnedSection, LOCTEXT("GroupPinned", "Pinned")}};
	TSet<FString> CustomGroups;
	for (const FString& Group : UHyperAIStudioChatHistoryPrefs::GetGroups())
	{
		Order.Add(GroupPrefix + Group);
		Headings.Add(GroupPrefix + Group, FText::FromString(Group));
		CustomGroups.Add(GroupPrefix + Group);
	}
	// Sessions arrive newest first, so date sections are met newest first too.
	TMap<FString, TArray<TSharedPtr<FHyperAIStudioChatSession>>> Members;
	for (const FHyperAIStudioChatSession& Session : Sessions)
	{
		if (!Agent.IsEmpty() && Session.AgentName != Agent)
		{
			continue;
		}
		const FString Shown = ShownTitle(Session);
		if (bSearching && !Shown.Contains(Filter) && !Session.AgentName.Contains(Filter))
		{
			continue;
		}
		FString Section;
		const FString Group = UHyperAIStudioChatHistoryPrefs::GetGroup(Session.SessionId);
		if (UHyperAIStudioChatHistoryPrefs::IsPinned(Session.SessionId))
		{
			Section = PinnedSection;
		}
		else if (!Group.IsEmpty())
		{
			Section = GroupPrefix + Group;
		}
		else
		{
			const TPair<FString, FText> Date = DateSection(Session.LastActiveUtc);
			Section = Date.Key;
			if (!Headings.Contains(Section))
			{
				Order.Add(Section);
				Headings.Add(Section, Date.Value);
			}
		}
		Members.FindOrAdd(Section).Add(MakeShared<FHyperAIStudioChatSession>(Session));
	}

	for (const FString& Section : Order)
	{
		const TArray<TSharedPtr<FHyperAIStudioChatSession>>* Chats = Members.Find(Section);
		const bool bCustom = CustomGroups.Contains(Section);
		// An empty group still shows, so chats can be moved into it, unless a search or the agent switch narrows the list.
		if (!Chats && (!bCustom || bSearching || !Agent.IsEmpty()))
		{
			continue;
		}
		// A search looks through every section, folded or not.
		const bool bCollapsed = !bSearching && UHyperAIStudioChatHistoryPrefs::IsCollapsed(Section);
		const TSharedRef<FRow> Heading = MakeShared<FRow>();
		Heading->SectionKey = Section;
		Heading->Heading = Headings[Section];
		Heading->Count = Chats ? Chats->Num() : 0;
		Heading->bCustomGroup = bCustom;
		Heading->bCollapsed = bCollapsed;
		Filtered.Add(Heading);
		if (Chats && !bCollapsed)
		{
			for (const TSharedPtr<FHyperAIStudioChatSession>& Chat : *Chats)
			{
				const TSharedRef<FRow> Row = MakeShared<FRow>();
				Row->SectionKey = Section;
				Row->Session = Chat;
				Filtered.Add(Row);
			}
		}
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SHyperAIStudioChatHistorySidebar::GenerateHeading(const FRowPtr& Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	using namespace HyperAIStudio::ChatHistorySidebar;
	const FString Section = Row->SectionKey;
	const bool bCustom = Row->bCustomGroup;
	const FString Group = bCustom ? Section.RightChop(FCString::Strlen(GroupPrefix)) : FString();
	TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush(Row->bCollapsed ? "Icons.ChevronRight" : "Icons.ChevronDown"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	if (bCustom)
	{
		Line->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush(Row->bCollapsed ? "Icons.FolderClosed" : "Icons.FolderOpen"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}
	Line->AddSlot()
	.FillWidth(1.0f)
	.VAlign(VAlign_Center)
	.Padding(4.0f, 0.0f, 0.0f, 0.0f)
	[
		SNew(STextBlock)
		.Text(Row->Heading)
		.Font(FAppStyle::GetFontStyle("SmallFontBold"))
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
	];
	Line->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(6.0f, 0.0f, 2.0f, 0.0f)
	[
		SNew(STextBlock)
		.Text(FText::AsNumber(Row->Count))
		.Font(FAppStyle::GetFontStyle("SmallFont"))
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
	];

	return SNew(STableRow<FRowPtr>, OwnerTable)
		.Style(FAppStyle::Get(), "TableView.NoHoverTableRow")
		.ShowSelection(false)
		.Padding(FMargin(2.0f, Filtered.Num() > 0 && Filtered[0] == Row ? 0.0f : 8.0f, 2.0f, 2.0f))
		[
			// Right-click reaches the border: the button inside answers only the left button.
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("NoBorder"))
			.Padding(0.0f)
			.OnMouseButtonUp_Lambda([this, bCustom, Group](const FGeometry&, const FPointerEvent& Event)
			{
				if (!bCustom || Event.GetEffectingButton() != EKeys::RightMouseButton)
				{
					return FReply::Unhandled();
				}
				OpenGroupMenu(Group);
				return FReply::Handled();
			})
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(4.0f, 2.0f))
				.ToolTipText(bCustom
					? LOCTEXT("GroupHeadingTooltip", "Click to fold or open this group. Right-click to rename or delete it.")
					: LOCTEXT("SectionHeadingTooltip", "Click to fold or open this section."))
				.OnClicked_Lambda([this, Section]()
				{
					UHyperAIStudioChatHistoryPrefs::SetCollapsed(Section, !UHyperAIStudioChatHistoryPrefs::IsCollapsed(Section));
					RebuildFiltered();
					return FReply::Handled();
				})
				[
					Line
				]
			]
		];
}

TSharedRef<ITableRow> SHyperAIStudioChatHistorySidebar::GenerateRow(FRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	using namespace HyperAIStudio::ChatHistorySidebar;
	if (!Row->Session.IsValid())
	{
		return GenerateHeading(Row, OwnerTable);
	}
	const TSharedPtr<FHyperAIStudioChatSession> Session = Row->Session;
	const FString SessionId = Session->SessionId;
	const bool bPinned = UHyperAIStudioChatHistoryPrefs::IsPinned(SessionId);
	const FString Shown = ShownTitle(*Session);
	auto IsActive = [this, SessionId]() { return !SessionId.IsEmpty() && ActiveSessionId.Get(FString()) == SessionId; };

	const TSharedRef<STableRow<FRowPtr>> TableRow = SNew(STableRow<FRowPtr>, OwnerTable)
		.Padding(FMargin(6.0f, 5.0f))
		.ToolTipText(Session->Live.IsBackground()
			? FText::Format(LOCTEXT("RowTooltipAttach", "{0}\nOpen the running {1} chat"), FText::FromString(Shown), FText::FromString(Session->AgentName))
			: FText::Format(LOCTEXT("RowTooltip", "{0}\nResume in {1}"), FText::FromString(Shown), FText::FromString(Session->AgentName)));
	const TWeakPtr<STableRow<FRowPtr>> WeakRow = TableRow;

	TSharedRef<SHorizontalBox> TitleLine = SNew(SHorizontalBox);
	if (RenamingSessionId == SessionId)
	{
		TitleLine->AddSlot()
		.FillWidth(1.0f)
		[
			SNew(SEditableTextBox)
			.Text(FText::FromString(Shown))
			.SelectAllTextWhenFocused(true)
			.OnTextCommitted_Lambda([this, SessionId](const FText& NewTitle, ETextCommit::Type CommitType)
			{
				if (CommitType == ETextCommit::OnEnter)
				{
					UHyperAIStudioChatHistoryPrefs::SetTitle(SessionId, NewTitle.ToString());
				}
				RenamingSessionId.Reset();
				RebuildFiltered();
			})
		];
	}
	else
	{
		TitleLine->AddSlot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Shown))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.ColorAndOpacity_Lambda([IsActive]() { return IsActive() ? FSlateColor(FStyleColors::AccentBlue) : FSlateColor::UseForeground(); })
		];
		TitleLine->AddSlot()
		.AutoWidth()
		[
			IconButton(bPinned ? "Icons.Pinned" : "Icons.Unpinned",
				bPinned ? LOCTEXT("UnpinTooltip", "Unpin this chat") : LOCTEXT("PinTooltip", "Pin this chat to the top"),
				FSimpleDelegate::CreateLambda([this, SessionId]()
				{
					UHyperAIStudioChatHistoryPrefs::TogglePinned(SessionId);
					RebuildFiltered();
				}))
		];
		TitleLine->AddSlot()
		.AutoWidth()
		[
			IconButton("Icons.Edit", LOCTEXT("RenameTooltip", "Rename this chat here. The agent's own log is not touched."),
				FSimpleDelegate::CreateLambda([this, SessionId]()
				{
					RenamingSessionId = SessionId;
					RebuildFiltered();
				}))
		];
		// Shown on the row under the pointer, so the list stays quiet.
		TitleLine->AddSlot()
		.AutoWidth()
		[
			SNew(SBox)
			.Visibility_Lambda([WeakRow]()
			{
				const TSharedPtr<STableRow<FRowPtr>> Owner = WeakRow.Pin();
				return Owner.IsValid() && Owner->IsHovered() ? EVisibility::Visible : EVisibility::Hidden;
			})
			[
				IconButton("Icons.Delete", LOCTEXT("DeleteTooltip", "Delete this chat. Its log goes to the Recycle Bin."),
					FSimpleDelegate::CreateLambda([this, Session]() { ConfirmDelete(*Session); }))
			]
		];
	}

	TSharedRef<SHorizontalBox> MetaLine = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("RowMeta", "{0} - {1}"),
				FText::FromString(Session->AgentName), HyperAIStudio::ChatHistorySidebar::RelativeTime(Session->LastActiveUtc)))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	auto AddBadge = [&MetaLine](const FText& Label, const FText& ToolTip, const FSlateColor& Color)
	{
		MetaLine->AddSlot()
		.AutoWidth()
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Badge"))
			.Padding(FMargin(4.0f, 0.0f))
			.ToolTipText(ToolTip)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(Color)
			]
		];
	};
	if (Session->bUsedUnrealMcp)
	{
		AddBadge(LOCTEXT("UnrealBadge", "Unreal"), LOCTEXT("UnrealBadgeTooltip", "This chat called Unreal MCP tools."),
			FSlateColor(FStyleColors::AccentGreen));
	}
	if (UHyperAIStudioChatHistoryPrefs::IsRemote(SessionId))
	{
		AddBadge(LOCTEXT("RemoteBadge", "Remote"),
			LOCTEXT("RemoteBadgeTooltip", "Remote Control is on: while it runs, this chat is at claude.ai/code and in the Claude app."),
			FSlateColor(FStyleColors::AccentBlue));
	}
	if (Session->Live.IsRunning())
	{
		AddBadge(Session->Live.IsBusy() ? LOCTEXT("WorkingBadge", "Working") : LOCTEXT("RunningBadge", "Running"),
			Session->Live.IsBackground()
				? LOCTEXT("BackgroundBadgeTooltip", "Running in the background. Click to open it here; right-click to stop it.")
				: LOCTEXT("InteractiveBadgeTooltip", "Open in a terminal right now."),
			FSlateColor(FStyleColors::AccentYellow));
	}

	TableRow->SetContent(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			TitleLine
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 2.0f, 0.0f, 0.0f)
		[
			MetaLine
		]);
	return TableRow;
}

TSharedPtr<SWidget> SHyperAIStudioChatHistorySidebar::BuildContextMenu()
{
	using namespace HyperAIStudio::ChatHistorySidebar;
	const TArray<FRowPtr> Selected = List.IsValid() ? List->GetSelectedItems() : TArray<FRowPtr>();
	if (Selected.Num() != 1 || !Selected[0].IsValid() || !Selected[0]->Session.IsValid())
	{
		return nullptr;
	}
	const FHyperAIStudioChatSession Session = *Selected[0]->Session;
	const FString SessionId = Session.SessionId;
	const bool bClaude = Session.AgentName == TEXT("Claude Code");
	const bool bRemote = UHyperAIStudioChatHistoryPrefs::IsRemote(SessionId);
	const bool bPinned = UHyperAIStudioChatHistoryPrefs::IsPinned(SessionId);
	const FString CurrentGroup = UHyperAIStudioChatHistoryPrefs::GetGroup(SessionId);

	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	Menu.AddMenuEntry(
		Session.Live.IsBackground() ? LOCTEXT("MenuAttach", "Open Running Chat") : LOCTEXT("MenuResume", "Resume"),
		FText::GetEmpty(),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([this, Session]() { OnResumeChat.ExecuteIfBound(Session); })));
	Menu.AddMenuEntry(
		LOCTEXT("MenuRemote", "Remote Control"),
		bClaude
			? LOCTEXT("MenuRemoteTooltip", "Continue this chat at claude.ai/code or in the Claude app. Remote chats run in the background, so they keep going after the editor closes.")
			: LOCTEXT("MenuRemoteCodex", "Per-chat Remote Control is a Claude Code feature. Codex's remote control opens every Codex chat on this PC at once, so it is not offered per chat."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.World"),
		FUIAction(
			FExecuteAction::CreateLambda([this, Session, bRemote]() { OnSetRemote.ExecuteIfBound(Session, !bRemote); }),
			FCanExecuteAction::CreateLambda([bClaude]() { return bClaude; }),
			FIsActionChecked::CreateLambda([bRemote]() { return bRemote; })),
		NAME_None,
		EUserInterfaceActionType::ToggleButton);
	if (Session.Live.IsBackground())
	{
		Menu.AddMenuEntry(
			LOCTEXT("MenuStop", "Stop Background Chat"),
			LOCTEXT("MenuStopTooltip", "End the background session. The conversation is kept and can be resumed."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Toolbar.Stop"),
			FUIAction(FExecuteAction::CreateLambda([this, Session]() { OnStopChat.ExecuteIfBound(Session); })));
	}
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		bPinned ? LOCTEXT("MenuUnpin", "Unpin") : LOCTEXT("MenuPin", "Pin to Top"),
		FText::GetEmpty(),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), bPinned ? "Icons.Unpinned" : "Icons.Pinned"),
		FUIAction(FExecuteAction::CreateLambda([this, SessionId]()
		{
			UHyperAIStudioChatHistoryPrefs::TogglePinned(SessionId);
			RebuildFiltered();
		})));
	Menu.AddSubMenu(
		LOCTEXT("MenuMoveToGroup", "Move to Group"),
		LOCTEXT("MenuMoveToGroupTooltip", "Keep this chat in one of your groups instead of under its date."),
		FNewMenuDelegate::CreateLambda([this, SessionId, CurrentGroup](FMenuBuilder& Groups)
		{
			for (const FString& Group : UHyperAIStudioChatHistoryPrefs::GetGroups())
			{
				Groups.AddMenuEntry(
					FText::FromString(Group),
					FText::GetEmpty(),
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderClosed"),
					FUIAction(
						FExecuteAction::CreateLambda([this, SessionId, Group]()
						{
							UHyperAIStudioChatHistoryPrefs::SetGroup(SessionId, Group);
							RebuildFiltered();
						}),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda([CurrentGroup, Group]() { return CurrentGroup == Group; })),
					NAME_None,
					EUserInterfaceActionType::RadioButton);
			}
			Groups.AddMenuSeparator();
			Groups.AddMenuEntry(
				LOCTEXT("MenuNewGroup", "New Group..."),
				LOCTEXT("MenuNewGroupTooltip", "Make a group and move this chat into it."),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
				FUIAction(FExecuteAction::CreateLambda([this, SessionId]()
				{
					AskGroupName(LOCTEXT("NewGroupLabel", "New group"), FString(), [this, SessionId](const FString& Name)
					{
						if (UHyperAIStudioChatHistoryPrefs::AddGroup(Name))
						{
							UHyperAIStudioChatHistoryPrefs::SetGroup(SessionId, Name.TrimStartAndEnd());
						}
						else
						{
							HyperAIStudio::ChatHistorySidebar::Toast(LOCTEXT("GroupNameTaken", "That group name is empty, too long or already used."));
						}
						RebuildFiltered();
					});
				})));
			if (!CurrentGroup.IsEmpty())
			{
				Groups.AddMenuEntry(
					LOCTEXT("MenuUngroup", "Remove from Group"),
					LOCTEXT("MenuUngroupTooltip", "Show this chat under its date again."),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([this, SessionId]()
					{
						UHyperAIStudioChatHistoryPrefs::SetGroup(SessionId, FString());
						RebuildFiltered();
					})));
			}
		}),
		false,
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderClosed"));
	Menu.AddMenuEntry(
		LOCTEXT("MenuRename", "Rename"),
		LOCTEXT("MenuRenameTooltip", "Rename this chat here. The agent's own log is not touched."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([this, SessionId]()
		{
			RenamingSessionId = SessionId;
			RebuildFiltered();
		})));
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("MenuDelete", "Delete Chat..."),
		Session.Live.IsRunning()
			? LOCTEXT("MenuDeleteRunningTooltip", "This chat is running. Stop it first, then delete it.")
			: LOCTEXT("MenuDeleteTooltip", "Delete this chat. Its log goes to the Recycle Bin, where it can be restored."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
		FUIAction(
			FExecuteAction::CreateLambda([this, Session]() { ConfirmDelete(Session); }),
			FCanExecuteAction::CreateLambda([bRunning = Session.Live.IsRunning()]() { return !bRunning; })));
	return Menu.MakeWidget();
}

void SHyperAIStudioChatHistorySidebar::OpenGroupMenu(const FString& Group)
{
	FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	Menu.AddMenuEntry(
		LOCTEXT("MenuRenameGroup", "Rename Group..."),
		FText::GetEmpty(),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([this, Group]()
		{
			AskGroupName(LOCTEXT("RenameGroupLabel", "Group name"), Group, [this, Group](const FString& Name)
			{
				if (!UHyperAIStudioChatHistoryPrefs::RenameGroup(Group, Name))
				{
					HyperAIStudio::ChatHistorySidebar::Toast(LOCTEXT("GroupNameTaken", "That group name is empty, too long or already used."));
				}
				RebuildFiltered();
			});
		})));
	Menu.AddMenuEntry(
		LOCTEXT("MenuDeleteGroup", "Delete Group"),
		LOCTEXT("MenuDeleteGroupTooltip", "Its chats go back under their dates. No chat is deleted."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
		FUIAction(FExecuteAction::CreateLambda([this, Group]()
		{
			UHyperAIStudioChatHistoryPrefs::RemoveGroup(Group);
			RebuildFiltered();
		})));
	FSlateApplication::Get().PushMenu(AsShared(), FWidgetPath(), Menu.MakeWidget(), FSlateApplication::Get().GetCursorPos(),
		FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
}

void SHyperAIStudioChatHistorySidebar::AskGroupName(const FText& Label, const FString& Initial, TFunction<void(const FString&)> OnName)
{
	const TSharedRef<STextEntryPopup> Popup = SNew(STextEntryPopup)
		.Label(Label)
		.DefaultText(FText::FromString(Initial))
		.SelectAllTextWhenFocused(true)
		.OnTextCommitted_Lambda([OnName](const FText& Text, const ETextCommit::Type Commit)
		{
			FSlateApplication::Get().DismissAllMenus();
			if (Commit == ETextCommit::OnEnter)
			{
				OnName(Text.ToString());
			}
		});
	FSlateApplication::Get().PushMenu(AsShared(), FWidgetPath(), Popup, FSlateApplication::Get().GetCursorPos(),
		FPopupTransitionEffect(FPopupTransitionEffect::TypeInPopup));
	Popup->FocusDefaultWidget();
}

void SHyperAIStudioChatHistorySidebar::ConfirmDelete(const FHyperAIStudioChatSession& Session)
{
	using namespace HyperAIStudio::ChatHistorySidebar;
	if (Session.Live.IsRunning())
	{
		Toast(LOCTEXT("DeleteRunning", "This chat is running. Stop it first, then delete it."));
		return;
	}
	const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
		FText::Format(LOCTEXT("DeleteConfirm", "Delete \"{0}\"?\n\nIts {1} log goes to the Recycle Bin, so it can be restored from there. Nothing in the project changes."),
			FText::FromString(ShownTitle(Session)), FText::FromString(Session.AgentName)),
		LOCTEXT("DeleteConfirmTitle", "Delete Chat"));
	if (Answer == EAppReturnType::Yes)
	{
		OnDeleteChat.ExecuteIfBound(Session);
	}
}

#undef LOCTEXT_NAMESPACE
