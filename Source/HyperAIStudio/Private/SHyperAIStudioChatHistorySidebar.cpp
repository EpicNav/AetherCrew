// Games by Hyper 2026.

#include "SHyperAIStudioChatHistorySidebar.h"

#include "Async/Async.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HyperAIStudioChatHistoryPrefs.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioChatHistorySidebar"

namespace HyperAIStudio::ChatHistorySidebar
{
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

	/** Heading for a chat last active at Utc, by the viewer's local calendar day, newest groups first. */
	FText DateGroup(const FDateTime& Utc)
	{
		const FTimespan LocalOffset = FDateTime::Now() - FDateTime::UtcNow();
		const FDateTime LocalDay = (Utc + LocalOffset).GetDate();
		const int32 DaysAgo = (FDateTime::Now().GetDate() - LocalDay).GetDays();
		if (DaysAgo <= 0)
		{
			return LOCTEXT("GroupToday", "Today");
		}
		if (DaysAgo == 1)
		{
			return LOCTEXT("GroupYesterday", "Yesterday");
		}
		if (DaysAgo < 7)
		{
			return LOCTEXT("GroupWeek", "Previous 7 Days");
		}
		if (DaysAgo < 30)
		{
			return LOCTEXT("GroupMonth", "Previous 30 Days");
		}
		return FText::FromString(LocalDay.ToString(TEXT("%B %Y")));
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
}

void SHyperAIStudioChatHistorySidebar::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::ChatHistorySidebar;
	OnResumeChat = InArgs._OnResumeChat;
	OnClose = InArgs._OnClose;
	OnNewChat = InArgs._OnNewChat;
	OnSetRemote = InArgs._OnSetRemote;
	OnStopChat = InArgs._OnStopChat;
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
					return Filter.IsEmpty()
						? LOCTEXT("Empty", "No Claude Code or Codex chats have run in this project yet.")
						: LOCTEXT("NoMatch", "No chats match.");
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
	// Sessions arrive newest first, so each date group is contiguous: emit a heading whenever the group changes.
	Filtered.Reset();
	FString CurrentGroup;
	TArray<FHyperAIStudioChatSession> Ordered = Sessions;
	Ordered.StableSort([](const FHyperAIStudioChatSession& A, const FHyperAIStudioChatSession& B)
	{
		return UHyperAIStudioChatHistoryPrefs::IsPinned(A.SessionId) && !UHyperAIStudioChatHistoryPrefs::IsPinned(B.SessionId);
	});
	for (const FHyperAIStudioChatSession& Session : Ordered)
	{
		const FString Renamed = UHyperAIStudioChatHistoryPrefs::GetTitle(Session.SessionId);
		const FString Shown = Renamed.IsEmpty() ? Session.Title : Renamed;
		if (!Filter.IsEmpty() && !Shown.Contains(Filter) && !Session.AgentName.Contains(Filter))
		{
			continue;
		}
		const FText Group = UHyperAIStudioChatHistoryPrefs::IsPinned(Session.SessionId)
			? LOCTEXT("GroupPinned", "Pinned")
			: HyperAIStudio::ChatHistorySidebar::DateGroup(Session.LastActiveUtc);
		if (Filtered.IsEmpty() || Group.ToString() != CurrentGroup)
		{
			CurrentGroup = Group.ToString();
			Filtered.Add(MakeShared<FRow>(FRow{Group, nullptr}));
		}
		Filtered.Add(MakeShared<FRow>(FRow{FText::GetEmpty(), MakeShared<FHyperAIStudioChatSession>(Session)}));
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SHyperAIStudioChatHistorySidebar::GenerateRow(FRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	using namespace HyperAIStudio::ChatHistorySidebar;
	if (!Row->Session.IsValid())
	{
		return SNew(STableRow<FRowPtr>, OwnerTable)
			.Style(FAppStyle::Get(), "TableView.NoHoverTableRow")
			.ShowSelection(false)
			.Padding(FMargin(6.0f, Filtered.Num() > 0 && Filtered[0] == Row ? 2.0f : 12.0f, 6.0f, 4.0f))
			[
				SNew(STextBlock)
				.Text(Row->Heading)
				.Font(FAppStyle::GetFontStyle("SmallFontBold"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
	}
	const TSharedPtr<FHyperAIStudioChatSession> Session = Row->Session;
	const FString SessionId = Session->SessionId;
	const bool bPinned = UHyperAIStudioChatHistoryPrefs::IsPinned(SessionId);
	const FString Renamed = UHyperAIStudioChatHistoryPrefs::GetTitle(SessionId);
	const FString Shown = Renamed.IsEmpty() ? Session->Title : Renamed;
	auto IsActive = [this, SessionId]() { return !SessionId.IsEmpty() && ActiveSessionId.Get(FString()) == SessionId; };

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

	return SNew(STableRow<FRowPtr>, OwnerTable)
		.Padding(FMargin(6.0f, 5.0f))
		.ToolTipText(Session->Live.IsBackground()
			? FText::Format(LOCTEXT("RowTooltipAttach", "{0}\nOpen the running {1} chat"), FText::FromString(Shown), FText::FromString(Session->AgentName))
			: FText::Format(LOCTEXT("RowTooltip", "{0}\nResume in {1}"), FText::FromString(Shown), FText::FromString(Session->AgentName)))
		[
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
			]
		];
}

TSharedPtr<SWidget> SHyperAIStudioChatHistorySidebar::BuildContextMenu()
{
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
	Menu.AddMenuEntry(
		LOCTEXT("MenuRename", "Rename"),
		LOCTEXT("MenuRenameTooltip", "Rename this chat here. The agent's own log is not touched."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([this, SessionId]()
		{
			RenamingSessionId = SessionId;
			RebuildFiltered();
		})));
	return Menu.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
