// Games by Hyper 2026.

#include "SHyperAIStudioQuickActionWindow.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "HyperAIStudioStyle.h"
#include "HyperAIStudioTerminalRawInput.h"
#include "ISettingsModule.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Dialog/SCustomDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HyperAIStudioChatHistoryPrefs.h"
#include "Misc/App.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "DragAndDrop/ActorDragDropOp.h"
#include "DragAndDrop/AssetDragDropOp.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Engine/Selection.h"
#include "IContentBrowserSingleton.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "HyperAIStudioApprovalGate.h"
#include "SHyperAIStudioActivitySidebar.h"
#include "SHyperAIStudioChatHistorySidebar.h"
#include "SHyperAIStudioMemoryPanel.h"
#include "HyperAIStudioAgents.h"
#include "HyperAIStudioNativeChat.h"
#include "SHyperAIStudioNativeChatView.h"
#include "HyperAIStudioSkills.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "STerminal.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "TerminalSettings.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SScrollBar.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioQuickActionWindow"

namespace HyperAIStudio::QuickAction
{
	// Theme-aware: fixed linear colours were illegible on the light editor theme.
	FSlateColor GoodColor()
	{
		return FStyleColors::Success;
	}

	FSlateColor WarningColor()
	{
		return FStyleColors::Warning;
	}

	FSlateColor ErrorColor()
	{
		return FStyleColors::Error;
	}

	FSlateColor MutedColor()
	{
		return FSlateColor::UseSubduedForeground();
	}

	void OpenSettingsPage(const UDeveloperSettings* Settings)
	{
		if (Settings)
		{
			FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer(
				Settings->GetContainerName(), Settings->GetCategoryName(), Settings->GetSectionName());
		}
	}

	constexpr float ChatSidebarWidth = 280.0f;

	/**
	 * The terminal paints its colour scheme's background; the frame around it takes the same colour so the padding
	 * blends in. The Terminal plugin keeps its parsed schemes private, so read the selected scheme's JSON directly.
	 */
	FLinearColor TerminalBackgroundColor()
	{
		static FString CachedSchemeName;
		static FColor CachedBackground(0x1E, 0x1E, 0x1E);
		const UTerminalSettings* Settings = GetDefault<UTerminalSettings>();
		const FString SchemeName = Settings ? Settings->ColorSchemeName : FString(TEXT("Default"));
		if (SchemeName != CachedSchemeName)
		{
			CachedSchemeName = SchemeName;
			CachedBackground = FColor(0x1E, 0x1E, 0x1E);
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Terminal"));
			const FString Directory = Plugin ? Plugin->GetBaseDir() / TEXT("Config") / TEXT("ColorSchemes") : FString();
			TArray<FString> Files;
			if (!Directory.IsEmpty() && SchemeName != TEXT("Default"))
			{
				IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.json")), true, false);
			}
			for (const FString& File : Files)
			{
				FString Json;
				TSharedPtr<FJsonObject> Scheme;
				FString Name;
				FString Hex;
				if (FFileHelper::LoadFileToString(Json, *(Directory / File))
					&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Scheme) && Scheme.IsValid()
					&& Scheme->TryGetStringField(TEXT("name"), Name) && Name == SchemeName
					&& Scheme->TryGetStringField(TEXT("defaultBackground"), Hex))
				{
					CachedBackground = FColor::FromHex(Hex);
					break;
				}
			}
		}
		return FLinearColor(CachedBackground);
	}

	FText InitialReadyMessage()
	{
		return LOCTEXT("InitialReadyMessage", "Type to the agent in the terminal: Enter sends, Shift+Enter adds a line. The Chats button resumes a past conversation. Copy Prompt is only for manual handoff/context fallback.");
	}

	FText InitialSetupMessage()
	{
		return LOCTEXT("InitialSetupMessage", "Setup is incomplete. Open Setup before using Chat / Terminal.");
	}

	FText RefreshingMessage()
	{
		return LOCTEXT("RefreshingMessage", "Refreshing HyperAIStudio readiness...");
	}

	FText MessageForStatus(const FHyperAIStudioStatus& InStatus)
	{
		return InStatus.IsReady()
			? InitialReadyMessage()
			: InitialSetupMessage();
	}

	bool IsDefaultStatusMessage(const FText& Message)
	{
		const FString Current = Message.ToString();
		return Current == InitialReadyMessage().ToString()
			|| Current == InitialSetupMessage().ToString()
			|| Current == RefreshingMessage().ToString();
	}

	struct FAgentRoute
	{
		FString Name;
		FString PrerequisiteId;
		FString ConfigPath;
		FString LaunchCommand;
	};

	struct FVisibleTerminalCommand
	{
		FString Command;
		FString Label;
	};

	TArray<FVisibleTerminalCommand>& QueuedVisibleTerminalCommands()
	{
		static TArray<FVisibleTerminalCommand> Commands;
		return Commands;
	}

	TWeakPtr<SHyperAIStudioQuickActionWindow>& ActiveQuickActionWindow()
	{
		static TWeakPtr<SHyperAIStudioQuickActionWindow> Window;
		return Window;
	}

	const TArray<FAgentRoute>& BuiltInAgentRoutes()
	{
		static const TArray<FAgentRoute> Routes = {
			{ TEXT("Codex"), TEXT("codex"), TEXT(".codex/config.toml"), TEXT("codex") },
			{ TEXT("Claude Code"), TEXT("claude"), TEXT(".mcp.json"), TEXT("claude") },
			{ TEXT("Gemini"), TEXT("gemini"), TEXT(".gemini/settings.json"), TEXT("gemini") }
		};
		return Routes;
	}

	FString EscapeForCmdEcho(FString Value)
	{
		Value.ReplaceInline(TEXT("&"), TEXT("^&"));
		Value.ReplaceInline(TEXT("|"), TEXT("^|"));
		Value.ReplaceInline(TEXT("<"), TEXT("^<"));
		Value.ReplaceInline(TEXT(">"), TEXT("^>"));
		return Value;
	}

	FString NoUsableAgentsLabel()
	{
		return TEXT("No usable agents");
	}

	bool IsPrerequisiteAvailable(const TArray<FHyperAIStudioPrerequisiteStatus>& Prerequisites, const FString& Id)
	{
		const FHyperAIStudioPrerequisiteStatus* Match = Prerequisites.FindByPredicate([&Id](const FHyperAIStudioPrerequisiteStatus& Entry)
		{
			return Entry.Id == Id;
		});
		return Match && Match->bAvailable;
	}

	const FAgentRoute* FindBuiltInRoute(const FString& AgentName)
	{
		return BuiltInAgentRoutes().FindByPredicate([&AgentName](const FAgentRoute& Route)
		{
			return Route.Name.Equals(AgentName, ESearchCase::IgnoreCase);
		});
	}

	TArray<TWeakPtr<SHyperAIStudioQuickActionWindow>>& LiveWindows()
	{
		static TArray<TWeakPtr<SHyperAIStudioQuickActionWindow>> Windows;
		Windows.RemoveAll([](const TWeakPtr<SHyperAIStudioQuickActionWindow>& Window) { return !Window.IsValid(); });
		return Windows;
	}

	struct FPendingRoute
	{
		FString Category;
		FHyperAIStudioRouteChoice Choice;
		FString Subagent;
	};

	/** Set just before a routed tab opens, and taken by the tab it opens. */
	TOptional<FPendingRoute>& PendingRoute()
	{
		static TOptional<FPendingRoute> Route;
		return Route;
	}

	/**
	 * MCP tool calls carry no caller identity, so credit goes to the only tab running an agent, or to the only one
	 * working when several run. ponytail: a guess from screen state; per-tab MCP endpoints would make it exact.
	 */
	bool ResolveWorkingAgent(FString& OutAgent, FString& OutModel)
	{
		TArray<TSharedPtr<SHyperAIStudioQuickActionWindow>> Running;
		for (const TWeakPtr<SHyperAIStudioQuickActionWindow>& Weak : LiveWindows())
		{
			FString Agent;
			FString Model;
			const TSharedPtr<SHyperAIStudioQuickActionWindow> Window = Weak.Pin();
			if (Window.IsValid() && Window->GetRunningAgent(Agent, Model))
			{
				Running.Add(Window);
			}
		}
		if (Running.Num() > 1)
		{
			Running.RemoveAll([](const TSharedPtr<SHyperAIStudioQuickActionWindow>& Window)
			{
				return Window->GetAgentState() != EHyperAIStudioAgentState::Working;
			});
		}
		return Running.Num() == 1 && Running[0]->GetRunningAgent(OutAgent, OutModel);
	}

	/** Remote Control and background sessions are Claude Code features. */
	bool IsClaudeAgent(const FString& AgentName)
	{
		return AgentName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase);
	}

	/** A Claude Code chat with Remote Control, or one running in the background, stays in the terminal, where those run. */
	bool NeedsTerminal(const FString& AgentName, const FString& SessionId, const FString& BackgroundId)
	{
		return IsClaudeAgent(AgentName) && (!BackgroundId.IsEmpty() || UHyperAIStudioChatHistoryPrefs::IsRemote(SessionId));
	}

	void Toast(const FText& Text)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 6.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

void SHyperAIStudioQuickActionWindow::Construct(const FArguments& InArgs)
{
	OnOpenWorkbench = InArgs._OnOpenWorkbench;
	OnNewAgentTab = InArgs._OnNewAgentTab;
	TabIndex = FMath::Max(1, InArgs._TabIndex);
	Service = MakeShared<FHyperAIStudioService>();
	Status = Service->GetStatusSync();
	LastMessage = HyperAIStudio::QuickAction::MessageForStatus(Status);
	RefreshAgentOptions();
	if (TOptional<HyperAIStudio::QuickAction::FPendingRoute>& Pending = HyperAIStudio::QuickAction::PendingRoute(); Pending.IsSet())
	{
		const HyperAIStudio::QuickAction::FPendingRoute Route = Pending.GetValue();
		Pending.Reset();
		RouteCategory = Route.Category;
		TabSubagent = Route.Subagent;
		for (const TSharedPtr<FString>& Agent : AgentOptions)
		{
			if (Agent.IsValid() && Agent->Equals(Route.Choice.Agent, ESearchCase::IgnoreCase))
			{
				SelectedAgent = Agent;
			}
		}
		if (!Route.Choice.Model.IsEmpty())
		{
			TabModel = Route.Choice.Model;
			TabModelAgent = Route.Choice.Agent;
		}
		LastMessage = FText::Format(LOCTEXT("RoutedTabOpened", "Opened for {0} with {1}. {2}"), FText::FromString(Route.Category),
			FText::FromString(Route.Choice.Model.IsEmpty() ? Route.Choice.Agent : FString::Printf(TEXT("%s (%s)"), *Route.Choice.Agent, *Route.Choice.Model)),
			FText::FromString(Route.Choice.Reason));
	}
	ResetTranscript();
	SAssignNew(TerminalScrollBar, SScrollBar)
		.Orientation(Orient_Vertical)
		.AlwaysShowScrollbar(false)
		.Thickness(FVector2D(5.0f, 5.0f));
	ChatSidebarCurve = FCurveSequence(0.0f, 0.2f, ECurveEaseFunction::CubicOut);
	ActivitySidebarCurve = FCurveSequence(0.0f, 0.2f, ECurveEaseFunction::CubicOut);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildHeader()
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					// Width and opacity follow the curve; the clip hides content while it slides in or out.
					SNew(SBox)
					.WidthOverride_Lambda([this]() { return HyperAIStudio::QuickAction::ChatSidebarWidth * ChatSidebarCurve.GetLerp(); })
					.Visibility_Lambda([this]() { return ChatSidebarCurve.GetLerp() > 0.0f ? EVisibility::Visible : EVisibility::Collapsed; })
					.Clipping(EWidgetClipping::ClipToBounds)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush(TEXT("NoBorder")))
						.Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
						.ColorAndOpacity_Lambda([this]() { return FLinearColor(1.0f, 1.0f, 1.0f, ChatSidebarCurve.GetLerp()); })
						[
							SAssignNew(ChatSidebar, SHyperAIStudioChatHistorySidebar)
							.OnResumeChat(this, &SHyperAIStudioQuickActionWindow::ResumeChat)
							.OnClose(this, &SHyperAIStudioQuickActionWindow::ToggleChatSidebar)
							.OnSetRemote_Lambda([this](const FHyperAIStudioChatSession& Session, const bool bRemote)
							{
								SetChatRemote(Session.SessionId, bRemote);
							})
							.OnStopChat_Lambda([this](const FHyperAIStudioChatSession& Session)
							{
								if (Session.Live.ShortId == ActiveBackgroundId)
								{
									ActiveBackgroundId.Reset();
								}
								StopBackgroundChat(Session.Live.ShortId, [this]()
								{
									if (ChatSidebar.IsValid())
									{
										ChatSidebar->Refresh();
									}
								});
							})
							.OnNewChat_Lambda([this]()
							{
								ResumeAgentName.Reset();
								ResumeSessionId.Reset();
								ResumeTitle.Reset();
								ResumeBackgroundId.Reset();
								ActiveChatSessionId.Reset();
								LastMessage = LOCTEXT("NewChatStarted", "Starting a new chat.");
								AddTranscriptLine(LastMessage.ToString());
								QueueTerminalStartup(true, true);
							})
							.ActiveSessionId_Lambda([this]() { return ActiveChatSessionId; })
						]
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						BuildStatusLine()
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						BuildAgentActions()
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(SBorder)
						.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.TerminalFrame"))
						.BorderBackgroundColor_Lambda([]() { return FSlateColor(HyperAIStudio::QuickAction::TerminalBackgroundColor()); })
						.Padding(FMargin(12.0f, 10.0f, 4.0f, 10.0f))
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								SAssignNew(TerminalHost, SBox)
								[
									BuildTerminalSessionWidget()
								]
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(4.0f, 0.0f, 0.0f, 0.0f)
							[
								TerminalScrollBar.ToSharedRef()
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						BuildSessionLog()
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return LastMessage; })
						.AutoWrapText(true)
						.ColorAndOpacity(HyperAIStudio::QuickAction::MutedColor())
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox)
					.WidthOverride_Lambda([this]() { return HyperAIStudio::QuickAction::ChatSidebarWidth * ActivitySidebarCurve.GetLerp(); })
					.Visibility_Lambda([this]() { return ActivitySidebarCurve.GetLerp() > 0.0f ? EVisibility::Visible : EVisibility::Collapsed; })
					.Clipping(EWidgetClipping::ClipToBounds)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush(TEXT("NoBorder")))
						.Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))
						.ColorAndOpacity_Lambda([this]() { return FLinearColor(1.0f, 1.0f, 1.0f, ActivitySidebarCurve.GetLerp()); })
						[
							SAssignNew(ActivitySidebar, SHyperAIStudioChatActivitySidebar)
							.OnClose(this, &SHyperAIStudioQuickActionWindow::ToggleActivitySidebar)
							.OnApprovalResolved_Lambda([this]()
							{
								Invalidate(EInvalidateWidgetReason::Paint);
							})
						]
					]
				]
			]
		]
	];

	HyperAIStudio::QuickAction::ActiveQuickActionWindow() = StaticCastSharedRef<SHyperAIStudioQuickActionWindow>(AsShared());
	HyperAIStudio::QuickAction::LiveWindows().Add(StaticCastSharedRef<SHyperAIStudioQuickActionWindow>(AsShared()));
	FHyperAIStudioAgentActivityLog::SetAgentResolver(&HyperAIStudio::QuickAction::ResolveWorkingAgent);
	if (HyperAIStudio::QuickAction::QueuedVisibleTerminalCommands().Num() > 0)
	{
		bTerminalStartupPaused = true;
		bTerminalStartupSent = false;
		ActiveTerminalAgentName.Reset();
	}
	RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunDeferredRefresh));
	// The MCP pill is only honest if it is re-probed; the probe is async and idles when nothing changed.
	RegisterActiveTimer(15.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunStatusHeartbeat));
	// Reading the agent's screen is cheap; half a second is fast enough to notice a prompt, slow enough to be free.
	RegisterActiveTimer(0.5f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunAgentStateHeartbeat));
	RegisterActiveTimer(0.35f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunDeferredTerminalStartup));
	RegisterActiveTimer(0.25f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunQueuedVisibleTerminalCommandPoll));
}

SHyperAIStudioQuickActionWindow::~SHyperAIStudioQuickActionWindow()
{
	if (HandoffTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HandoffTicker);
	}
}

void SHyperAIStudioQuickActionWindow::SelectAgentByName(const FString& AgentName)
{
	const FString TrimmedAgentName = AgentName.TrimStartAndEnd();
	if (TrimmedAgentName.IsEmpty())
	{
		return;
	}

	RefreshAgentOptions();
	for (const TSharedPtr<FString>& Agent : AgentOptions)
	{
		if (Agent.IsValid() && Agent->Equals(TrimmedAgentName, ESearchCase::IgnoreCase))
		{
			SelectedAgent = Agent;
			if (AgentComboBox.IsValid())
			{
				AgentComboBox->SetSelectedItem(SelectedAgent);
			}
			AddTranscriptLine(FString::Printf(TEXT("Agent selected: %s"), **Agent));
			QueueTerminalStartup(true, true);
			return;
		}
	}
}

void SHyperAIStudioQuickActionWindow::ResumeChat(const FHyperAIStudioChatSession& Session)
{
	if (HyperAIStudio::ChatHistory::BuildResumeArguments(Session.AgentName, Session.SessionId).IsEmpty())
	{
		return;
	}
	RefreshAgentOptions();
	const bool bAgentUsable = AgentOptions.ContainsByPredicate([&Session](const TSharedPtr<FString>& Agent)
	{
		return Agent.IsValid() && Agent->Equals(Session.AgentName, ESearchCase::IgnoreCase);
	});
	if (!bAgentUsable)
	{
		LastMessage = FText::Format(LOCTEXT("ResumeAgentUnavailable", "{0} is not set up for HyperAI Chat, so that chat cannot be resumed here."),
			FText::FromString(Session.AgentName));
		AddTranscriptLine(LastMessage.ToString());
		return;
	}

	ResumeAgentName = Session.AgentName;
	ResumeSessionId = Session.SessionId;
	const FString RenamedTitle = UHyperAIStudioChatHistoryPrefs::GetTitle(Session.SessionId);
	ResumeTitle = RenamedTitle.IsEmpty() ? Session.Title : RenamedTitle;
	ResumeBackgroundId = Session.Live.IsBackground() ? Session.Live.ShortId : FString();
	LastMessage = FText::Format(LOCTEXT("ResumingChat", "Resuming {0} chat: {1}"), FText::FromString(Session.AgentName), FText::FromString(Session.Title));
	AddTranscriptLine(LastMessage.ToString());
	if (GetSelectedAgentName().Equals(Session.AgentName, ESearchCase::IgnoreCase))
	{
		QueueTerminalStartup(true, true);
	}
	else
	{
		SelectAgentByName(Session.AgentName);
	}
}

void SHyperAIStudioQuickActionWindow::RunVisibleTerminalCommand(const FString& Command, const FString& Label)
{
	const FString TrimmedCommand = Command.TrimStartAndEnd();
	if (TrimmedCommand.IsEmpty())
	{
		return;
	}

	const FString TrimmedLabel = Label.TrimStartAndEnd();
	const FString RawStoppedAgentName = ActiveTerminalAgentName.IsEmpty() ? GetSelectedAgentName() : ActiveTerminalAgentName;
	const FString StoppedAgentName = RawStoppedAgentName.IsEmpty() ? FString(TEXT("embedded terminal")) : RawStoppedAgentName;
	const bool bHadActiveAgentTerminal = bTerminalStartupSent || !ActiveTerminalAgentName.IsEmpty();
	PendingVisibleTerminalCommand = TrimmedCommand;
	PendingVisibleTerminalLabel = TrimmedLabel.IsEmpty() ? FString(TEXT("HyperAIStudio command")) : TrimmedLabel;
	bVisibleTerminalCommandPending = true;
	bTerminalStartupPaused = true;
	bTerminalStartupSent = false;
	ActiveTerminalAgentName.Reset();
	RecreateTerminalSession();

	LastMessage = FText::FromString(FString::Printf(
		TEXT("%s opened in the embedded terminal after resetting any active agent session. Review the visible output, then return to HyperAIStudio and click Verify."),
		*PendingVisibleTerminalLabel));
	if (bHadActiveAgentTerminal)
	{
		AddTranscriptLine(FString::Printf(TEXT("Stopped active embedded agent terminal: %s"), *StoppedAgentName));
	}
	AddTranscriptLine(FString::Printf(TEXT("Visible terminal command queued: %s"), *PendingVisibleTerminalLabel));
	RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunDeferredVisibleTerminalCommand));
}

void SHyperAIStudioQuickActionWindow::EnqueueVisibleTerminalCommand(const FString& Command, const FString& Label)
{
	const FString TrimmedCommand = Command.TrimStartAndEnd();
	if (TrimmedCommand.IsEmpty())
	{
		return;
	}

	if (const TSharedPtr<SHyperAIStudioQuickActionWindow> ActiveWindow = HyperAIStudio::QuickAction::ActiveQuickActionWindow().Pin())
	{
		ActiveWindow->RunVisibleTerminalCommand(TrimmedCommand, Label);
		return;
	}

	HyperAIStudio::QuickAction::QueuedVisibleTerminalCommands().Add({ TrimmedCommand, Label.TrimStartAndEnd() });
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildTerminalSessionWidget()
{
	TSharedRef<STerminal> NewTerminal = SNew(STerminal)
		.ExternalScrollbar(TerminalScrollBar);
	TerminalWidget = NewTerminal;
	return NewTerminal;
}

FReply SHyperAIStudioQuickActionWindow::OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// STerminal translates Shift+Enter to a bare CR, which submits. Preview runs before the terminal sees the key,
	// so send ESC+CR instead: the meta-Return agent TUIs bind to "insert newline". Only while bracketed paste is on
	// (an agent TUI is live); at a shell prompt ESC would wipe the typed line, so plain Enter falls through.
	if (InKeyEvent.GetKey() == EKeys::Enter
		&& InKeyEvent.IsShiftDown() && !InKeyEvent.IsControlDown() && !InKeyEvent.IsAltDown()
		&& TerminalWidget.IsValid()
		&& FSlateApplication::Get().GetKeyboardFocusedWidget() == TerminalWidget
		&& HyperAIStudio::TerminalRawInput::IsBracketedPasteEnabled(*TerminalWidget)
		&& HyperAIStudio::TerminalRawInput::WriteRawBytes(*TerminalWidget, HyperAIStudio::TerminalRawInput::BuildNewlineInsertPayload()))
	{
		return FReply::Handled();
	}

	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildSessionLog()
{
	return SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.AreaTitle(LOCTEXT("SessionLogTitle", "Session Log"))
		.BodyContent()
		[
			SNew(SBox)
			.MaxDesiredHeight(180.0f)
			[
				// Read-only editable text so past entries can be selected and copied.
				SNew(SMultiLineEditableTextBox)
				.Text(this, &SHyperAIStudioQuickActionWindow::GetTranscriptText)
				.IsReadOnly(true)
				.AutoWrapText(true)
			]
		];
}

void SHyperAIStudioQuickActionWindow::ToggleActivitySidebar()
{
	bActivitySidebarOpen = !bActivitySidebarOpen;
	if (ActivitySidebarCurve.IsPlaying())
	{
		ActivitySidebarCurve.Reverse();
	}
	else if (bActivitySidebarOpen)
	{
		ActivitySidebarCurve.Play(AsShared());
	}
	else
	{
		ActivitySidebarCurve.PlayReverse(AsShared());
	}
}

void SHyperAIStudioQuickActionWindow::ToggleChatSidebar()
{
	bChatSidebarOpen = !bChatSidebarOpen;
	if (bChatSidebarOpen && ChatSidebar.IsValid())
	{
		ChatSidebar->Refresh();
	}
	if (ChatSidebarCurve.IsPlaying())
	{
		ChatSidebarCurve.Reverse();
	}
	else if (bChatSidebarOpen)
	{
		ChatSidebarCurve.Play(AsShared());
	}
	else
	{
		ChatSidebarCurve.PlayReverse(AsShared());
	}
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildHeader()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("ChatsToggleTooltip", "Show or hide this project's past agent chats."))
			.OnClicked_Lambda([this]()
			{
				ToggleChatSidebar();
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(TEXT("Icons.Recent")))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ChatsToggle", "Chats"))
				]
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			BuildAgentStateBadge()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			BuildRemoteToggle()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			BuildNativeToggle()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SComboButton)
			.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
			.ToolTipText(LOCTEXT("SkillsTooltip", "Run one of this agent's skills, or save what this chat did as a new skill."))
			.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildSkillsMenu)
			.ButtonContent()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SkillsButton", "Skills"))
			]
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SBox)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(LOCTEXT("ActivityToggleTooltip", "Plans waiting for your approval, work in progress, and what the agent changed."))
			.OnClicked_Lambda([this]()
			{
				ToggleActivitySidebar();
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(TEXT("Icons.Visibility")))
					.ColorAndOpacity_Lambda([]()
					{
						// Amber while something is waiting on the user.
						return SHyperAIStudioChatActivitySidebar::GetPendingApprovalCount() > 0
							? FSlateColor(FStyleColors::AccentYellow) : FSlateColor::UseForeground();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([]()
					{
						const int32 Pending = SHyperAIStudioChatActivitySidebar::GetPendingApprovalCount();
						return Pending > 0
							? FText::Format(LOCTEXT("ActivityTogglePending", "Activity ({0})"), Pending)
							: LOCTEXT("ActivityToggle", "Activity");
					})
				]
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 2.0f, 0.0f)
		[
			SNew(SComboButton)
			.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
			.HasDownArrow(false)
			.ToolTipText(LOCTEXT("NewAgentTabTooltip", "Open another chat tab, plain or for a kind of task. Each tab runs its own agent; dock them side by side to watch both."))
			.IsEnabled_Lambda([this]() { return OnNewAgentTab.IsBound(); })
			.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildNewTabMenu)
			.ButtonContent()
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.Plus")))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(2.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SComboButton)
			.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
			.HasDownArrow(false)
			.ToolTipText(LOCTEXT("ChatSettingsMoreTooltip", "Unreal MCP, checking again, setup, chat settings and terminal appearance."))
			.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildSettingsMenu)
			.ButtonContent()
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.Settings")))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildSettingsMenu()
{
	FMenuBuilder Menu(true, nullptr);
	Menu.AddWrapperSubMenu(
		GetMcpStatusText(),
		LOCTEXT("ChatMcpTooltip", "The Unreal MCP server this chat's agent uses: its state and what to do about it."),
		FOnGetContent::CreateSP(this, &SHyperAIStudioQuickActionWindow::BuildMcpStatusMenu),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.BulletPoint"));
	Menu.AddMenuEntry(
		LOCTEXT("ChatRefresh", "Check Again"),
		LOCTEXT("RefreshTooltip", "Re-check Unreal MCP readiness and installed agents."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]() { OnRefreshClicked(); })));
	Menu.AddSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("ChatOpenSetup", "Open Setup"),
		LOCTEXT("ChatOpenSetupStudioTooltip", "Open Settings in HyperAI Studio: setup, agents and MCP servers."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]() { OnOpenWorkbenchClicked(); })));
	Menu.AddMenuEntry(
		LOCTEXT("ChatSettingsEntry", "Chat Settings..."),
		LOCTEXT("ChatSettingsEntryTooltip", "Agent models and agent options."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([]() { HyperAIStudio::QuickAction::OpenSettingsPage(GetDefault<UHyperAIStudioSettings>()); })));
	Menu.AddMenuEntry(
		LOCTEXT("ChatRemember", "Remember..."),
		LOCTEXT("ChatRememberTooltip", "Add a fact to Project Memory: every agent gets it in every future chat."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
		FUIAction(FExecuteAction::CreateLambda([]() { SHyperAIStudioMemoryPanel::OpenFactDialog(); })));
	Menu.AddMenuEntry(
		LOCTEXT("TerminalAppearance", "Terminal Appearance..."),
		LOCTEXT("TerminalAppearanceTooltip", "Font, size, colour scheme and scrollback for the embedded terminal. Changes apply the next time the agent starts."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([]() { HyperAIStudio::QuickAction::OpenSettingsPage(GetDefault<UTerminalSettings>()); })));
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildStatusLine()
{
	// When ready the badge already says so; the hint only earns its space while something needs fixing.
	return SNew(SBox)
		.Visibility_Lambda([this]() { return StatusHint.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		.Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return StatusHint; })
			.AutoWrapText(true)
			.ColorAndOpacity(this, &SHyperAIStudioQuickActionWindow::GetReadinessColor)
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildAgentActions()
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			BuildAgentSelector()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			BuildModelSelector()
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			BuildContextSelector()
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SBox)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SComboButton)
			.ToolTipText(LOCTEXT("HandoffTooltip", "Plan here, build in another tab: this agent writes a plan file, the other tab's agent implements it and reports, then this one reviews the work."))
			.IsEnabled_Lambda([this]()
			{
				FString Agent;
				FString Model;
				return Handoff.IsSet() || GetRunningAgent(Agent, Model);
			})
			.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildHandoffMenu)
			.ButtonContent()
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return Handoff.IsSet() ? LOCTEXT("HandoffRunning", "Handoff Running") : LOCTEXT("HandoffButton", "Hand Off"); })
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("CopyPrompt", "Copy Prompt"))
			.ToolTipText(LOCTEXT("CopyPromptTooltip", "Copy a context prompt for pasting into an agent outside Unreal."))
			.IsEnabled_Lambda([this]() { return Status.IsReady() && HasUsableSelectedAgent(); })
			.OnClicked_Lambda([this]() { return OnPreparePromptClicked(GetSelectedAgentName()); })
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text_Lambda([this]()
			{
				return bTerminalStartupPaused ? LOCTEXT("StartAgent", "Start Agent") : LOCTEXT("StopAgent", "Stop Agent");
			})
			.ToolTipText_Lambda([this]()
			{
				return bTerminalStartupPaused
					? LOCTEXT("StartAgentTooltip", "Start the selected agent in a fresh embedded terminal session.")
					: LOCTEXT("StopAgentTooltip", "Stop the current embedded agent terminal session without changing the selected agent.");
			})
			.IsEnabled_Lambda([this]()
			{
				return TerminalWidget.IsValid() && (HasUsableSelectedAgent() || !ActiveTerminalAgentName.IsEmpty());
			})
			.OnClicked(this, &SHyperAIStudioQuickActionWindow::OnToggleTerminalAgentClicked)
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildContextSelector()
{
	return SNew(SComboButton)
		.ToolTipText(LOCTEXT("ContextTooltip", "Put what you have selected in Unreal into the agent's prompt. You can also drag assets or actors onto this panel."))
		.IsEnabled_Lambda([this]() { return TerminalWidget.IsValid() && TerminalWidget->IsSessionRunning(); })
		.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildContextMenu)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ContextButton", "Context"))
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildContextMenu()
{
	TArray<FAssetData> SelectedAssets;
	FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(SelectedAssets);
	TArray<FString> ActorPaths;
	if (GEditor)
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (const AActor* Actor = Cast<AActor>(*It))
			{
				ActorPaths.Add(Actor->GetPathName());
			}
		}
	}

	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(NAME_None, LOCTEXT("ContextMenuHeading", "Insert into the prompt"));
	Menu.AddMenuEntry(
		FText::Format(LOCTEXT("InsertAssets", "Selected assets ({0})"), SelectedAssets.Num()),
		LOCTEXT("InsertAssetsTooltip", "Paste the object path of each selected Content Browser asset."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Package"),
		FUIAction(
			FExecuteAction::CreateSPLambda(this, [this, SelectedAssets]()
			{
				TArray<FString> Paths;
				for (const FAssetData& Asset : SelectedAssets)
				{
					Paths.Add(Asset.GetSoftObjectPath().ToString());
				}
				InsertIntoAgentPrompt(FString::Join(Paths, TEXT("\n")), TEXT("selected assets"));
			}),
			FCanExecuteAction::CreateLambda([SelectedAssets]() { return SelectedAssets.Num() > 0; })));
	Menu.AddMenuEntry(
		FText::Format(LOCTEXT("InsertActors", "Selected actors ({0})"), ActorPaths.Num()),
		LOCTEXT("InsertActorsTooltip", "Paste the path of each selected level actor."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Level"),
		FUIAction(
			FExecuteAction::CreateSPLambda(this, [this, ActorPaths]()
			{
				InsertIntoAgentPrompt(FString::Join(ActorPaths, TEXT("\n")), TEXT("selected actors"));
			}),
			FCanExecuteAction::CreateLambda([ActorPaths]() { return ActorPaths.Num() > 0; })));
	Menu.AddMenuEntry(
		LOCTEXT("InsertSummary", "Context summary"),
		LOCTEXT("InsertSummaryTooltip", "Paste a sentence describing the current selection and map."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Info"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]()
		{
			if (Service.IsValid())
			{
				InsertIntoAgentPrompt(Service->GetCurrentContextSummary(), TEXT("context summary"));
			}
		})));
	Menu.EndSection();
	Menu.AddMenuSeparator();
	Menu.AddWidget(
		SNew(SBox)
		.WidthOverride(240.0f)
		.Padding(FMargin(12.0f, 4.0f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ContextDropHint", "Dragging assets or actors onto this panel does the same thing."))
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		],
		FText::GetEmpty());
	return Menu.MakeWidget();
}

bool SHyperAIStudioQuickActionWindow::InsertIntoAgentPrompt(const FString& Text, const FString& Label, const bool bFocusTerminal)
{
	const FString Trimmed = Text.TrimStartAndEnd();
	if (NativeView.IsValid() && !Trimmed.IsEmpty())
	{
		NativeView->InsertText(Trimmed);
		LastMessage = FText::Format(LOCTEXT("ContextInserted", "Inserted {0} into the prompt."), FText::FromString(Label));
		AddTranscriptLine(LastMessage.ToString());
		return true;
	}
	if (Trimmed.IsEmpty() || !TerminalWidget.IsValid() || !TerminalWidget->IsSessionRunning())
	{
		return false;
	}
	// Bracketed paste is on only while an agent TUI owns the prompt; at a shell prompt this would run as commands.
	if (!HyperAIStudio::TerminalRawInput::IsAvailable()
		|| !HyperAIStudio::TerminalRawInput::IsBracketedPasteEnabled(*TerminalWidget))
	{
		LastMessage = LOCTEXT("ContextNoAgentPrompt", "Nothing inserted: no agent prompt is accepting input. Start the agent first.");
		AddTranscriptLine(LastMessage.ToString());
		return false;
	}
	// Never sends: the text lands in the prompt for the user to finish writing around.
	if (!HyperAIStudio::TerminalRawInput::WriteText(*TerminalWidget, Trimmed, /*bAppendCarriageReturn=*/false))
	{
		LastMessage = LOCTEXT("ContextInsertFailed", "Nothing inserted: the terminal refused the paste.");
		AddTranscriptLine(LastMessage.ToString());
		return false;
	}
	LastMessage = FText::Format(LOCTEXT("ContextInserted", "Inserted {0} into the prompt."), FText::FromString(Label));
	AddTranscriptLine(LastMessage.ToString());
	if (bFocusTerminal)
	{
		FSlateApplication::Get().SetKeyboardFocus(TerminalWidget, EFocusCause::SetDirectly);
	}
	return true;
}

bool SHyperAIStudioQuickActionWindow::GetRunningAgent(FString& OutAgent, FString& OutModel) const
{
	if (NativeView.IsValid() && NativeView->GetChat().IsValid() && NativeView->GetChat()->IsRunning())
	{
		OutAgent = ActiveTerminalAgentName;
		OutModel = ActiveTerminalModel;
		return true;
	}
	if (ActiveTerminalAgentName.IsEmpty() || !TerminalWidget.IsValid() || !TerminalWidget->IsSessionRunning())
	{
		return false;
	}
	OutAgent = ActiveTerminalAgentName;
	OutModel = ActiveTerminalModel;
	return true;
}

FString SHyperAIStudioQuickActionWindow::GetTabDescription() const
{
	FString Agent;
	FString Model;
	if (!GetRunningAgent(Agent, Model))
	{
		Agent = GetSelectedAgentName();
		Model = GetModelForAgent(Agent);
	}
	return FString::Printf(TEXT("%s (%s%s)"), *GetTabTitle().ToString(), *Agent,
		Model.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", %s"), *Model));
}

TArray<FString> SHyperAIStudioQuickActionWindow::GetUsableAgentNames() const
{
	TArray<FString> Names;
	for (const TSharedPtr<FString>& Agent : AgentOptions)
	{
		if (Agent.IsValid() && *Agent != HyperAIStudio::QuickAction::NoUsableAgentsLabel())
		{
			Names.Add(*Agent);
		}
	}
	return Names;
}

FString SHyperAIStudioQuickActionWindow::GetModelForAgent(const FString& AgentName) const
{
	if (!TabModelAgent.IsEmpty() && TabModelAgent.Equals(AgentName, ESearchCase::IgnoreCase))
	{
		return TabModel;
	}
	const FHyperAIStudioAgentModelRoute* Route = GetDefault<UHyperAIStudioSettings>()->FindAgentModelRoute(AgentName);
	return Route ? Route->SelectedModel : FString();
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildNewTabMenu()
{
	if (UHyperAIStudioSettings* MutableSettings = GetMutableDefault<UHyperAIStudioSettings>(); MutableSettings->EnsureDefaultTaskRoutes())
	{
		MutableSettings->SaveConfig();
	}
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const TArray<FString> Usable = GetUsableAgentNames();

	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(
		LOCTEXT("NewPlainTab", "New Chat Tab"),
		LOCTEXT("NewPlainTabTooltip", "Another tab, starting with the preferred agent."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]() { OnNewAgentTab.ExecuteIfBound(); })));
	Menu.BeginSection(NAME_None, LOCTEXT("NewTabForHeading", "New tab for"));
	for (const FHyperAIStudioAgentTaskRoute& Route : Settings->AgentTaskRoutes)
	{
		if (Route.Category.IsEmpty())
		{
			continue;
		}
		const FHyperAIStudioRouteChoice Choice = FHyperAIStudioAgentScoreboard::ChooseAgent(
			Route.AgentName, Route.ModelId, Route.Packs, Usable, Settings->PreferredAgent);
		const FHyperAIStudioAgentScore Record = FHyperAIStudioAgentScoreboard::Summarize(Choice.Agent, Choice.Model, Route.Packs);
		FString ToolTip = Choice.Reason;
		if (Record.Total() > 0)
		{
			ToolTip += FString::Printf(TEXT("\nIts record on these tasks: %d completed, %d failed, %d rejected by you."),
				Record.Completed, Record.Failed, Record.Rejected);
		}
		const FString AgentText = Choice.Model.IsEmpty() ? Choice.Agent : FString::Printf(TEXT("%s (%s)"), *Choice.Agent, *Choice.Model);
		Menu.AddMenuEntry(
			FText::Format(LOCTEXT("RoutedTabEntry", "{0}: {1}"), FText::FromString(Route.Category),
				Choice.Agent.IsEmpty() ? LOCTEXT("RoutedNoAgent", "no agent set up") : FText::FromString(AgentText)),
			FText::FromString(ToolTip),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSPLambda(this, [this, Category = Route.Category, Choice, Subagent = Route.Subagent]()
				{
					const bool bClaude = HyperAIStudio::QuickAction::IsClaudeAgent(Choice.Agent) && HyperAIStudio::Skills::IsValidName(Subagent);
					OpenRoutedTab(Category, Choice, bClaude ? Subagent : FString());
				}),
				FCanExecuteAction::CreateLambda([bHasAgent = !Choice.Agent.IsEmpty()]() { return bHasAgent; })));
	}
	Menu.EndSection();
	// A subagent becomes the whole chat: its instructions, tools and model replace the defaults (claude --agent).
	const FString* Claude = Usable.FindByPredicate([](const FString& Agent) { return HyperAIStudio::QuickAction::IsClaudeAgent(Agent); });
	const TArray<FHyperAIStudioAgentDefinition> Subagents = Claude
		? HyperAIStudio::Agents::List(FHyperAIStudioService::GetProjectRoot()) : TArray<FHyperAIStudioAgentDefinition>();
	if (!Subagents.IsEmpty())
	{
		Menu.BeginSection(NAME_None, LOCTEXT("SubagentTabHeading", "Claude Code as a subagent"));
		for (const FHyperAIStudioAgentDefinition& Subagent : Subagents)
		{
			FHyperAIStudioRouteChoice Choice;
			Choice.Agent = *Claude;
			Choice.Reason = FString::Printf(TEXT("Started as the subagent %s."), *Subagent.Name);
			Menu.AddMenuEntry(
				FText::FromString(Subagent.Name),
				FText::FromString(Subagent.Description),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateSPLambda(this, [this, Name = Subagent.Name, Choice]() { OpenRoutedTab(Name, Choice, Name); })));
		}
		Menu.EndSection();
	}
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("EditRouting", "Edit Task Routing..."),
		LOCTEXT("EditRoutingTooltip", "Pick an agent and model per kind of task, or leave the agent empty to choose by track record."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([]() { HyperAIStudio::QuickAction::OpenSettingsPage(GetDefault<UHyperAIStudioSettings>()); })));
	return Menu.MakeWidget();
}

void SHyperAIStudioQuickActionWindow::OpenRoutedTab(const FString& Category, const FHyperAIStudioRouteChoice& Choice, const FString& Subagent)
{
	TOptional<HyperAIStudio::QuickAction::FPendingRoute>& Pending = HyperAIStudio::QuickAction::PendingRoute();
	Pending = HyperAIStudio::QuickAction::FPendingRoute{Category, Choice, Subagent};
	OnNewAgentTab.ExecuteIfBound();
	if (Pending.IsSet())
	{
		// No new tab took it: every chat tab is already open.
		Pending.Reset();
		LastMessage = LOCTEXT("RoutedTabNoRoom", "Every chat tab is already open; close one to start another agent.");
		AddTranscriptLine(LastMessage.ToString());
	}
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildHandoffMenu()
{
	FMenuBuilder Menu(true, nullptr);
	if (Handoff.IsSet())
	{
		Menu.AddMenuEntry(
			FText::Format(LOCTEXT("StopHandoff", "Stop Handoff to {0}"), FText::FromString(Handoff->BuilderDescription)),
			FText::Format(LOCTEXT("StopHandoffTooltip", "Stop watching {0}. Prompts already sent stay sent."), FText::FromString(Handoff->PlanPath)),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.X"),
			FUIAction(FExecuteAction::CreateSPLambda(this, [this]()
			{
				Handoff.Reset();
				AddTranscriptLine(TEXT("Handoff stopped."));
			})));
		return Menu.MakeWidget();
	}
	Menu.BeginSection(NAME_None, LOCTEXT("HandoffHeading", "Plan here, build in"));
	int32 Targets = 0;
	for (const TWeakPtr<SHyperAIStudioQuickActionWindow>& Weak : HyperAIStudio::QuickAction::LiveWindows())
	{
		const TSharedPtr<SHyperAIStudioQuickActionWindow> Window = Weak.Pin();
		FString Agent;
		FString Model;
		if (!Window.IsValid() || Window.Get() == this || !Window->GetRunningAgent(Agent, Model))
		{
			continue;
		}
		++Targets;
		Menu.AddMenuEntry(
			FText::FromString(Window->GetTabDescription()),
			LOCTEXT("HandoffTargetTooltip", "This agent writes a plan file, that tab's agent implements it and writes a report, then this agent reviews the work."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSPLambda(this, [this, Weak]() { StartHandoff(Weak); })));
	}
	if (Targets == 0)
	{
		Menu.AddWidget(
			SNew(SBox)
			.WidthOverride(240.0f)
			.Padding(FMargin(12.0f, 4.0f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("HandoffNoTargets", "Open another chat tab with + and start its agent first."))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			],
			FText::GetEmpty());
	}
	Menu.EndSection();
	return Menu.MakeWidget();
}

void SHyperAIStudioQuickActionWindow::StartHandoff(TWeakPtr<SHyperAIStudioQuickActionWindow> Builder)
{
	const TSharedPtr<SHyperAIStudioQuickActionWindow> Target = Builder.Pin();
	if (!Target.IsValid())
	{
		return;
	}
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Handoffs"));
	IFileManager::Get().MakeDirectory(*Folder, true);
	const FString Stem = Folder / FString::Printf(TEXT("%s-chat%d-to-chat%d"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")), TabIndex, Target->TabIndex);
	FHandoff Next;
	Next.PlanPath = Stem + TEXT("-plan.md");
	Next.ResultPath = Stem + TEXT("-result.md");
	Next.Builder = Builder;
	Next.BuilderDescription = Target->GetTabDescription();
	Next.StartedTime = FPlatformTime::Seconds();
	const FString Prompt = FString::Printf(
		TEXT("Plan the current task for another agent, %s, to implement. Write the complete plan as Markdown to %s: the goal, ")
		TEXT("the assets and HyperAI tools involved, ordered steps, and how to check the result. Do not implement it yourself; stop once the file is written."),
		*Next.BuilderDescription, *Next.PlanPath);
	if (!DeliverHandoffPrompt(Prompt, TEXT("handoff plan request")))
	{
		return;
	}
	Handoff = MoveTemp(Next);
	if (!HandoffTicker.IsValid())
	{
		HandoffTicker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::TickHandoff), 1.0f);
	}
	AddTranscriptLine(FString::Printf(TEXT("Handoff started: plan here, build in %s. Waiting for %s."), *Handoff->BuilderDescription, *Handoff->PlanPath));
}

bool SHyperAIStudioQuickActionWindow::TickHandoff(float DeltaTime)
{
	auto Stop = [this](const FString& Why)
	{
		if (!Why.IsEmpty())
		{
			AddTranscriptLine(Why);
		}
		Handoff.Reset();
		HandoffTicker.Reset();
		return false;
	};
	if (!Handoff.IsSet())
	{
		return Stop(FString());
	}
	FHandoff& Current = Handoff.GetValue();
	const TSharedPtr<SHyperAIStudioQuickActionWindow> Builder = Current.Builder.Pin();
	FString BuilderAgent;
	FString BuilderModel;
	constexpr double MaxHandoffSeconds = 3.0 * 60.0 * 60.0;
	if (!Builder.IsValid() || !Builder->GetRunningAgent(BuilderAgent, BuilderModel)
		|| FPlatformTime::Seconds() - Current.StartedTime > MaxHandoffSeconds)
	{
		return Stop(FString::Printf(TEXT("Handoff stopped: %s closed, stopped its agent, or three hours passed."), *Current.BuilderDescription));
	}

	// A file counts as written once it exists and its size held for a tick; agents write it in one go.
	const int64 Size = IFileManager::Get().FileSize(Current.bBuilding ? *Current.ResultPath : *Current.PlanPath);
	const bool bWritten = Size > 0 && Size == Current.LastSize;
	Current.LastSize = Size;
	if (!bWritten)
	{
		return true;
	}
	if (!Current.bBuilding)
	{
		const FString Prompt = FString::Printf(
			TEXT("Implement the plan in %s, written by %s. Follow its steps with the HyperAI tools (dry run, then apply). ")
			TEXT("When you are done, write what you changed, and anything you could not do, to %s."),
			*Current.PlanPath, *GetTabDescription(), *Current.ResultPath);
		if (!Builder->DeliverHandoffPrompt(Prompt, TEXT("handoff plan")))
		{
			return Stop(FString::Printf(TEXT("Handoff stopped: %s is not at its prompt. The plan is in %s."), *Current.BuilderDescription, *Current.PlanPath));
		}
		Current.bBuilding = true;
		Current.LastSize = -1;
		AddTranscriptLine(FString::Printf(TEXT("Plan written; %s is implementing it."), *Current.BuilderDescription));
		return true;
	}
	const FString Prompt = FString::Printf(
		TEXT("%s finished your plan in %s and reported in %s. Review the work: inspect the assets it changed, compare them with the plan, and report anything missing or wrong."),
		*Current.BuilderDescription, *Current.PlanPath, *Current.ResultPath);
	DeliverHandoffPrompt(Prompt, TEXT("review request"));
	return Stop(FString::Printf(TEXT("%s finished; review requested here."), *Current.BuilderDescription));
}

bool SHyperAIStudioQuickActionWindow::DeliverHandoffPrompt(const FString& Text, const FString& Label)
{
	// A hidden tab's state is from when it was last on screen; an agent does not start work on its own, so an
	// idle reading stays true until something is sent to it.
	const bool bSend = GetDefault<UHyperAIStudioSettings>()->bAutoSendHandoffPrompts && AgentState.State == EHyperAIStudioAgentState::Idle;
	if (!InsertIntoAgentPrompt(Text, Label, /*bFocusTerminal=*/false))
	{
		return false;
	}
	if (bSend)
	{
		// Enter goes a beat after the paste so the agent has taken the text first. The core ticker runs even
		// while this tab is hidden, when its widget timers would not.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSPLambda(this, [this](float)
		{
			const uint8 Enter = 0x0D;
			if (TerminalWidget.IsValid() && TerminalWidget->IsSessionRunning())
			{
				HyperAIStudio::TerminalRawInput::WriteRawBytes(*TerminalWidget, TConstArrayView<uint8>(&Enter, 1));
			}
			return false;
		}), 0.4f);
		AddTranscriptLine(FString::Printf(TEXT("Sent the %s."), *Label));
	}
	else
	{
		AddTranscriptLine(FString::Printf(TEXT("The %s is typed into the prompt; press Enter to send it."), *Label));
	}
	if (const TSharedPtr<SDockTab> Tab = OwnerTab.Pin())
	{
		FGlobalTabmanager::Get()->DrawAttention(Tab.ToSharedRef());
	}
	return true;
}

void SHyperAIStudioQuickActionWindow::OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	SCompoundWidget::OnDragEnter(MyGeometry, DragDropEvent);
}

FReply SHyperAIStudioQuickActionWindow::OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	const bool bUnderstood = DragDropEvent.GetOperationAs<FAssetDragDropOp>().IsValid()
		|| DragDropEvent.GetOperationAs<FActorDragDropOp>().IsValid();
	return bUnderstood ? FReply::Handled() : FReply::Unhandled();
}

FReply SHyperAIStudioQuickActionWindow::OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent)
{
	TArray<FString> Paths;
	FString Label;
	if (const TSharedPtr<FAssetDragDropOp> AssetDrop = DragDropEvent.GetOperationAs<FAssetDragDropOp>())
	{
		for (const FAssetData& Asset : AssetDrop->GetAssets())
		{
			Paths.Add(Asset.GetSoftObjectPath().ToString());
		}
		Label = TEXT("dropped assets");
	}
	else if (const TSharedPtr<FActorDragDropOp> ActorDrop = DragDropEvent.GetOperationAs<FActorDragDropOp>())
	{
		for (const TWeakObjectPtr<AActor>& Actor : ActorDrop->Actors)
		{
			if (Actor.IsValid())
			{
				Paths.Add(Actor->GetPathName());
			}
		}
		Label = TEXT("dropped actors");
	}
	if (Paths.IsEmpty())
	{
		return FReply::Unhandled();
	}
	InsertIntoAgentPrompt(FString::Join(Paths, TEXT("\n")), Label);
	return FReply::Handled();
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildMcpStatusMenu()
{
	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(NAME_None, LOCTEXT("McpMenuHeading", "Unreal MCP"));
	Menu.AddMenuEntry(
		LOCTEXT("McpReconnect", "Reconnect"),
		LOCTEXT("McpReconnectTooltip", "Restart the Unreal MCP server, then re-probe it. Running agents reconnect on their next call."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]()
		{
			if (!Service.IsValid())
			{
				return;
			}
			FText Message;
			const bool bRestarted = Service->RestartUnrealMCP(Message);
			LastMessage = Message.IsEmpty()
				? (bRestarted ? LOCTEXT("McpRestarted", "Unreal MCP restarted.") : LOCTEXT("McpRestartFailed", "Unreal MCP did not restart."))
				: Message;
			AddTranscriptLine(LastMessage.ToString());
			RefreshStatus();
		})));
	Menu.AddMenuEntry(
		LOCTEXT("McpReprobe", "Re-probe"),
		LOCTEXT("McpReprobeTooltip", "Check the endpoint again without restarting anything."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]() { RefreshStatus(); })));
	Menu.AddMenuEntry(
		LOCTEXT("McpCopyEndpoint", "Copy endpoint"),
		LOCTEXT("McpCopyEndpointTooltip", "Copy the MCP endpoint URL to the clipboard."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Duplicate"),
		FUIAction(
			FExecuteAction::CreateSPLambda(this, [this]() { FPlatformApplicationMisc::ClipboardCopy(*Status.Endpoint); }),
			FCanExecuteAction::CreateLambda([this]() { return !Status.Endpoint.IsEmpty(); })));
	Menu.EndSection();
	return Menu.MakeWidget();
}

FText SHyperAIStudioQuickActionWindow::GetMcpStatusText() const
{
	if (!Status.bUnrealMCPModuleAvailable)
	{
		return LOCTEXT("McpMissing", "MCP: unavailable");
	}
	if (!Status.bServerRunning)
	{
		return LOCTEXT("McpStopped", "MCP: stopped");
	}
	if (!Status.bPortListening)
	{
		return FText::Format(LOCTEXT("McpNotListening", "MCP: port {0} closed"), FText::AsNumber(Status.ActivePort, &FNumberFormattingOptions::DefaultNoGrouping()));
	}
	if (!Status.bToolsListReachable)
	{
		return LOCTEXT("McpUnreachable", "MCP: no tool list");
	}
	return FText::Format(LOCTEXT("McpReady", "MCP: {0} tools"), Status.RegisteredToolCount);
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildModelSelector()
{
	return SNew(SComboButton)
		.IsEnabled_Lambda([this]() { return Status.IsReady() && HasUsableSelectedAgent(); })
		.ToolTipText(LOCTEXT("ModelTooltip", "Model the agent starts with. Changing it restarts a running agent."))
		.OnGetMenuContent(this, &SHyperAIStudioQuickActionWindow::BuildModelMenu)
		.ButtonContent()
		[
			SNew(STextBlock)
			.Text(this, &SHyperAIStudioQuickActionWindow::GetModelButtonText)
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildModelMenu()
{
	const FString AgentName = GetSelectedAgentName();
	const FHyperAIStudioAgentModelRoute* Route = GetDefault<UHyperAIStudioSettings>()->FindAgentModelRoute(AgentName);

	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection(NAME_None, FText::Format(LOCTEXT("ModelMenuHeading", "{0} Model"), FText::FromString(AgentName)));
	auto AddChoice = [this, &Menu](const FString& ModelId, const FText& Label, const FText& ToolTip)
	{
		Menu.AddMenuEntry(
			Label,
			ToolTip,
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SHyperAIStudioQuickActionWindow::SelectModel, ModelId),
				FCanExecuteAction(),
				FIsActionChecked::CreateSP(this, &SHyperAIStudioQuickActionWindow::IsModelSelected, ModelId)),
			NAME_None,
			EUserInterfaceActionType::RadioButton);
	};
	AddChoice(FString(), LOCTEXT("ModelDefaultEntry", "Default"), LOCTEXT("ModelDefaultEntryTooltip", "Pass no model flag; the agent uses its own configured model."));
	if (UHyperAIStudioSettings::IsSmartModelAgent(AgentName))
	{
		const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
		AddChoice(UHyperAIStudioSettings::SmartModelId,
			FText::Format(LOCTEXT("ModelSmartEntry", "Smart: {0} plans, {1} builds"),
				FText::FromString(Settings->SmartPlanModel), FText::FromString(Settings->SmartBuildModel)),
			LOCTEXT("ModelSmartEntryTooltip",
				"Plans in plan mode with the plan model, then builds with the build model once you accept the plan. "
				"Inside this session `opus` means the plan model and `sonnet` the build model, and the prompt cache "
				"starts over once when building begins. Change either model, or plan auto-accept, in Settings."));
	}
	if (Route)
	{
		for (const FString& Model : Route->Models)
		{
			AddChoice(Model, FText::FromString(Model), FText::GetEmpty());
		}
	}
	Menu.EndSection();

	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("EditModelList", "Edit Model List..."),
		LOCTEXT("EditModelListTooltip", "Add the model ids your account can use, spelled exactly as the agent CLI accepts them."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Edit"),
		FUIAction(FExecuteAction::CreateLambda([]() { HyperAIStudio::QuickAction::OpenSettingsPage(GetDefault<UHyperAIStudioSettings>()); })));
	return Menu.MakeWidget();
}

void SHyperAIStudioQuickActionWindow::SelectModel(FString ModelId)
{
	const FString AgentName = GetSelectedAgentName();
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	FHyperAIStudioAgentModelRoute* Route = Settings->FindAgentModelRoute(AgentName);
	if (!Route || GetModelForAgent(AgentName) == ModelId)
	{
		return;
	}

	// This tab starts with it; the saved choice is what new tabs start with.
	TabModel = ModelId;
	TabModelAgent = AgentName;
	Route->SelectedModel = ModelId;
	Settings->SaveConfig();

	// The flag only applies at process start: restart a running agent, otherwise it applies on the next start.
	const FText ModelLabel = ModelId.IsEmpty() ? LOCTEXT("ModelDefaultLabel", "its default model")
		: ModelId == UHyperAIStudioSettings::SmartModelId ? LOCTEXT("ModelSmartLabel", "Smart models")
		: FText::FromString(ModelId);
	if (!bTerminalStartupPaused)
	{
		LastMessage = FText::Format(LOCTEXT("ModelRestarting", "Restarting {0} with {1}."), FText::FromString(AgentName), ModelLabel);
		QueueTerminalStartup(true, true);
	}
	else
	{
		LastMessage = FText::Format(LOCTEXT("ModelSaved", "{0} will start with {1}."), FText::FromString(AgentName), ModelLabel);
	}
	AddTranscriptLine(LastMessage.ToString());
}

bool SHyperAIStudioQuickActionWindow::IsModelSelected(FString ModelId) const
{
	return GetModelForAgent(GetSelectedAgentName()) == ModelId;
}

FText SHyperAIStudioQuickActionWindow::GetModelButtonText() const
{
	const FString Model = GetModelForAgent(GetSelectedAgentName());
	return FText::Format(LOCTEXT("ModelButton", "Model: {0}"),
		Model.IsEmpty() ? LOCTEXT("ModelDefault", "Default")
		: Model == UHyperAIStudioSettings::SmartModelId ? LOCTEXT("ModelSmart", "Smart")
		: FText::FromString(Model));
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildAgentSelector()
{
	return SAssignNew(AgentComboBox, SComboBox<TSharedPtr<FString>>)
		.OptionsSource(&AgentOptions)
		.InitiallySelectedItem(SelectedAgent)
		.IsEnabled_Lambda([this]() { return Status.IsReady() && HasUsableSelectedAgent(); })
		.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
		{
			return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
		})
			.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo)
			{
				if (Item.IsValid())
				{
					SelectedAgent = Item;
					if (SelectInfo == ESelectInfo::Direct)
					{
						return;
					}

					LastMessage = FText::Format(
						LOCTEXT("TerminalAgentSwitching", "Switching embedded terminal to {0}."),
						FText::FromString(*Item));
					AddTranscriptLine(LastMessage.ToString());
					QueueTerminalStartup(true, true);
				}
			})
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return FText::FromString(FString::Printf(TEXT("Agent: %s"), SelectedAgent.IsValid() ? **SelectedAgent : TEXT("None")));
			})
		];
}

FReply SHyperAIStudioQuickActionWindow::OnRefreshClicked()
{
	LastMessage = HyperAIStudio::QuickAction::RefreshingMessage();
	AddTranscriptLine(TEXT("Refreshing readiness probe..."));
	RefreshStatus();
	return FReply::Handled();
}

FReply SHyperAIStudioQuickActionWindow::OnPreparePromptClicked(FString AgentName)
{
	if (!HasUsableSelectedAgent())
	{
		LastMessage = LOCTEXT("NoUsableAgentCopy", "No usable agent is selected. Open Agents to install or configure an agent first.");
		AddTranscriptLine(LastMessage.ToString());
		return FReply::Handled();
	}

	FText Message;
	const bool bOk = Service->CreateQuickConnectPromptPackage(AgentName, LastPromptPackage, Message);
	LastMessage = bOk ? Message : FText::Format(LOCTEXT("PromptCopyFailed", "Prompt copy failed: {0}"), Message);
	AddTranscriptLine(LastMessage.ToString());
	return FReply::Handled();
}

FReply SHyperAIStudioQuickActionWindow::OnToggleTerminalAgentClicked()
{
	if (bTerminalStartupPaused)
	{
		const FString AgentName = GetSelectedAgentName();
		const FString AgentLabel = AgentName.IsEmpty() ? TEXT("the selected agent") : AgentName;
		LastMessage = FText::Format(
			LOCTEXT("TerminalAgentStarting", "Starting {0} in a fresh embedded terminal."),
			FText::FromString(AgentLabel));
		AddTranscriptLine(LastMessage.ToString());
		QueueTerminalStartup(true, true);
		return FReply::Handled();
	}

	const FString StoppedAgentName = ActiveTerminalAgentName.IsEmpty() ? GetSelectedAgentName() : ActiveTerminalAgentName;
	bTerminalStartupPaused = true;
	bTerminalStartupSent = false;
	ActiveTerminalAgentName.Reset();
	RecreateTerminalSession();
	const FString StoppedAgentLabel = StoppedAgentName.IsEmpty() ? TEXT("the embedded agent") : StoppedAgentName;
	LastMessage = FText::Format(
		LOCTEXT("TerminalAgentStopped", "Stopped {0}. Use Start Agent or choose another agent to launch a fresh terminal session."),
		FText::FromString(StoppedAgentLabel));
	AddTranscriptLine(LastMessage.ToString());
	return FReply::Handled();
}

FReply SHyperAIStudioQuickActionWindow::OnOpenWorkbenchClicked()
{
	if (OnOpenWorkbench.IsBound())
	{
		OnOpenWorkbench.Execute();
	}
	return FReply::Handled();
}

void SHyperAIStudioQuickActionWindow::RefreshStatus(const bool bSilent)
{
	if (!Service.IsValid())
	{
		return;
	}

	bRefreshing = true;
	// A silent re-probe keeps the last settled status on screen: no Checking badge, no greyed-out
	// agent buttons, no "Status:" log line every heartbeat. Only a settled change shows.
	if (!bSilent)
	{
		Status = Service->GetStatusSync();
	}
	TWeakPtr<SHyperAIStudioQuickActionWindow> WeakThis = StaticCastSharedRef<SHyperAIStudioQuickActionWindow>(AsShared());
	Service->RefreshStatusAsync([WeakThis, bSilent](const FHyperAIStudioStatus& NewStatus)
	{
		if (bSilent && NewStatus.bProbeInProgress)
		{
			return;
		}
		if (const TSharedPtr<SHyperAIStudioQuickActionWindow> Pinned = WeakThis.Pin())
		{
			const bool bShouldUpdateDefaultMessage = HyperAIStudio::QuickAction::IsDefaultStatusMessage(Pinned->LastMessage);
			const FString PreviousReadiness = Pinned->GetReadinessText().ToString();
			Pinned->Status = NewStatus;
			Pinned->bRefreshing = NewStatus.bProbeInProgress;
			if (!NewStatus.bProbeInProgress)
			{
				Pinned->StatusHint = NewStatus.IsReady() ? FText::GetEmpty() : GetNextActionTextForStatus(NewStatus, false);
			}
			if (bShouldUpdateDefaultMessage)
			{
				Pinned->LastMessage = HyperAIStudio::QuickAction::MessageForStatus(NewStatus);
			}
			Pinned->RefreshAgentOptions();
			const FString NewReadiness = Pinned->GetReadinessText().ToString();
			if (NewReadiness != PreviousReadiness)
			{
				Pinned->AddTranscriptLine(FString::Printf(TEXT("Status: %s"), *NewReadiness));
			}
			Pinned->QueueTerminalStartup(false);
		}
	});
}

void SHyperAIStudioQuickActionWindow::RefreshAgentOptions()
{
	if (!Service.IsValid())
	{
		return;
	}

	const FString PreviousAgent = GetSelectedAgentName();
	if (UHyperAIStudioSettings* MutableSettings = GetMutableDefault<UHyperAIStudioSettings>())
	{
		const bool bSeededModels = MutableSettings->EnsureDefaultAgentModelRoutes();
		const bool bSeededTasks = MutableSettings->EnsureDefaultTaskRoutes();
		if (bSeededModels || bSeededTasks)
		{
			MutableSettings->SaveConfig();
		}
	}
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const FString PreferredAgent = Settings ? Settings->PreferredAgent : FString();
	const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	CachedPrerequisites = Service->GetPrerequisites();

	AgentOptions.Reset();
	auto AddAgent = [this](const FString& AgentName)
	{
		if (!AgentOptions.ContainsByPredicate([&AgentName](const TSharedPtr<FString>& Existing)
		{
			return Existing.IsValid() && Existing->Equals(AgentName, ESearchCase::IgnoreCase);
		}))
		{
			AgentOptions.Add(MakeShared<FString>(AgentName));
		}
	};

	for (const HyperAIStudio::QuickAction::FAgentRoute& Route : HyperAIStudio::QuickAction::BuiltInAgentRoutes())
	{
		const bool bConfigured = FPaths::FileExists(FPaths::Combine(ProjectRoot, Route.ConfigPath));
		const bool bInstalled = HyperAIStudio::QuickAction::IsPrerequisiteAvailable(CachedPrerequisites, Route.PrerequisiteId);
		const bool bEnabled = !Settings || Settings->IsBuiltInAgentEnabled(Route.Name);
		if (bEnabled && bConfigured && bInstalled)
		{
			AddAgent(Route.Name);
		}
	}

	if (AgentOptions.Num() == 0)
	{
		AgentOptions.Add(MakeShared<FString>(HyperAIStudio::QuickAction::NoUsableAgentsLabel()));
	}

	const FString DesiredAgent = !PreviousAgent.IsEmpty() ? PreviousAgent : PreferredAgent;
	SelectedAgent = AgentOptions[0];
	for (const TSharedPtr<FString>& Agent : AgentOptions)
	{
		if (Agent.IsValid() && Agent->Equals(DesiredAgent, ESearchCase::IgnoreCase))
		{
			SelectedAgent = Agent;
			break;
		}
	}

	if (AgentComboBox.IsValid())
	{
		AgentComboBox->RefreshOptions();
		AgentComboBox->SetSelectedItem(SelectedAgent);
	}
}

void SHyperAIStudioQuickActionWindow::ResetTranscript()
{
	TranscriptLines.Reset();
	AddTranscriptLine(TEXT("HyperAIStudio Chat"));
	AddTranscriptLine(FString::Printf(TEXT("Status: %s"), *GetReadinessText().ToString()));
	AddTranscriptLine(FString::Printf(TEXT("Endpoint: %s"), Status.Endpoint.IsEmpty() ? TEXT("<not configured>") : *Status.Endpoint));

	if (Status.bProbeInProgress || bRefreshing)
	{
		AddTranscriptLine(TEXT("Unreal MCP: checking initialize and tools/list..."));
	}
	else if (Status.bToolsListReachable)
	{
		if (Status.ProbeToolCount > 0 && Status.ProbeToolCount <= 3)
		{
			AddTranscriptLine(TEXT("Unreal MCP: reachable; tools/list returned tool-search meta-tools. HyperAIStudio will not expand them automatically."));
		}
		else if (Status.ProbeToolCount > 0)
		{
			AddTranscriptLine(FString::Printf(TEXT("Unreal MCP: reachable, tools/list returned %d tools."), Status.ProbeToolCount));
		}
		else
		{
			AddTranscriptLine(TEXT("Unreal MCP: reachable."));
		}
	}
	else if (Status.bPortListening)
	{
		AddTranscriptLine(TEXT("Unreal MCP: port is listening, but tools/list is not reachable yet."));
	}
	else
	{
		AddTranscriptLine(TEXT("Unreal MCP: not reachable yet."));
	}

	if (!Status.ProbeMessage.IsEmpty())
	{
		AddTranscriptLine(FString::Printf(TEXT("Probe: %s"), *Status.ProbeMessage));
	}

	AddTranscriptLine(GetSelectedAgentStatusLine());
	AddTranscriptLine(TEXT("Context: manual. Use the Context button, or drag assets and actors onto this panel, to put paths in the prompt."));
	AddTranscriptLine(FString::Printf(TEXT("Last action: %s"), *LastMessage.ToString()));
}

void SHyperAIStudioQuickActionWindow::AddTranscriptLine(const FString& Line)
{
	if (Line.IsEmpty())
	{
		return;
	}

	TranscriptLines.Add(Line);
	constexpr int32 MaxTranscriptLines = 500;
	if (TranscriptLines.Num() > MaxTranscriptLines)
	{
		TranscriptLines.RemoveAt(0, TranscriptLines.Num() - MaxTranscriptLines);
	}

	Invalidate(EInvalidateWidgetReason::Paint);
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunDeferredRefresh(double CurrentTime, float DeltaTime)
{
	RefreshStatus();
	return EActiveTimerReturnType::Stop;
}

void SHyperAIStudioQuickActionWindow::SetOwnerTab(const TSharedRef<SDockTab>& InTab)
{
	OwnerTab = InTab;
	InTab->SetLabel(GetTabLabel());
}

FText SHyperAIStudioQuickActionWindow::GetTabTitle() const
{
	const FText Base = TabIndex <= 1
		? LOCTEXT("TabLabelFirst", "HyperAI Chat")
		: FText::Format(LOCTEXT("TabLabelNumbered", "HyperAI Chat {0}"), TabIndex);
	return RouteCategory.IsEmpty() ? Base : FText::Format(LOCTEXT("TabTitleRouted", "{0}: {1}"), Base, FText::FromString(RouteCategory));
}

FText SHyperAIStudioQuickActionWindow::GetTabLabel() const
{
	const FText Base = GetTabTitle();
	// The tab label is what you read when the panel is not on screen, so the state belongs in it.
	if (AgentState.State == EHyperAIStudioAgentState::Stopped || AgentState.Label.IsEmpty())
	{
		return Base;
	}
	return FText::Format(LOCTEXT("TabLabelWithState", "{0} - {1}"), Base, AgentState.Label);
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunAgentStateHeartbeat(double CurrentTime, float DeltaTime)
{
	FHyperAIStudioAgentStateInputs Inputs;
	Inputs.bSessionRunning = TerminalWidget.IsValid() && TerminalWidget->IsSessionRunning();
	Inputs.bStartupPending = Inputs.bSessionRunning && !bTerminalStartupSent && !bTerminalStartupPaused;
	if (const TSharedPtr<FHyperAIStudioNativeChat> Native = NativeView.IsValid() ? NativeView->GetChat() : nullptr)
	{
		// The native chat says what it is doing; there is no screen to read.
		Inputs.bSessionRunning = Native->IsRunning();
		Inputs.bStartupPending = Native->IsRunning() && !Native->IsReady();
		Inputs.bApprovalPending = Native->HasOpenApproval();
		Inputs.SecondsSinceOutput = Native->IsBusy() ? 0.0 : 3600.0;
	}
	else if (Inputs.bSessionRunning && HyperAIStudio::TerminalRawInput::IsAvailable())
	{
		Inputs.Tail = HyperAIStudio::TerminalRawInput::ReadVisibleTail(
			*TerminalWidget, FHyperAIStudioAgentStateEvaluator::TailRows);
		const double LastOutput = HyperAIStudio::TerminalRawInput::GetLastOutputTime(*TerminalWidget);
		Inputs.SecondsSinceOutput = LastOutput > 0.0 ? FPlatformTime::Seconds() - LastOutput : -1.0;
		AutoAcceptSmartPlan(Inputs.Tail, LastOutput);
	}

	const FHyperAIStudioAgentStateSnapshot Previous = AgentState;
	AgentState = FHyperAIStudioAgentStateEvaluator::Evaluate(Inputs);
	if (AgentState.State == Previous.State)
	{
		return EActiveTimerReturnType::Continue;
	}

	if (const TSharedPtr<SDockTab> Tab = OwnerTab.Pin())
	{
		Tab->SetLabel(GetTabLabel());
		// Flash the tab only on the way into a state that needs a person, so a background tab still gets noticed.
		if (FHyperAIStudioAgentStateEvaluator::WantsAttention(AgentState.State)
			&& !FHyperAIStudioAgentStateEvaluator::WantsAttention(Previous.State))
		{
			FGlobalTabmanager::Get()->DrawAttention(Tab.ToSharedRef());
		}
	}
	AddTranscriptLine(FString::Printf(TEXT("Agent is %s%s"),
		FHyperAIStudioAgentStateEvaluator::LexToString(AgentState.State),
		AgentState.Evidence.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *AgentState.Evidence)));
	Invalidate(EInvalidateWidgetReason::Paint);

	// A tab you are not looking at says when its agent needs you, or when a long turn ends.
	const TSharedPtr<SDockTab> ShownTab = OwnerTab.Pin();
	if (AgentState.State == EHyperAIStudioAgentState::Working)
	{
		WorkingSince = FPlatformTime::Seconds();
	}
	else if (Previous.State == EHyperAIStudioAgentState::Working && ShownTab.IsValid() && !ShownTab->IsForeground())
	{
		if (FHyperAIStudioAgentStateEvaluator::WantsAttention(AgentState.State))
		{
			HyperAIStudio::QuickAction::Toast(FText::Format(LOCTEXT("AgentNeedsYouToast", "{0}: the agent needs you."), GetTabTitle()));
		}
		else if (AgentState.State == EHyperAIStudioAgentState::Idle && FPlatformTime::Seconds() - WorkingSince >= 20.0)
		{
			HyperAIStudio::QuickAction::Toast(FText::Format(LOCTEXT("AgentFinishedToast", "{0}: the agent finished."), GetTabTitle()));
		}
	}
	if (bRemoteRelaunchPending && AgentState.State == EHyperAIStudioAgentState::Idle)
	{
		RelaunchActiveChat();
	}
	return EActiveTimerReturnType::Continue;
}

void SHyperAIStudioQuickActionWindow::AutoAcceptSmartPlan(const FString& Tail, const double LastOutputTime)
{
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	// The model this terminal launched with, not the saved choice, which another tab may have changed since.
	const bool bSmart = Settings->bAutoAcceptPlansInSmartMode
		&& UHyperAIStudioSettings::IsSmartModelAgent(ActiveTerminalAgentName)
		&& ActiveTerminalModel == UHyperAIStudioSettings::SmartModelId;
	const FHyperAIStudioPlanApprovalChoice Choice = bSmart
		? FHyperAIStudioAgentStateEvaluator::FindPlanAutoApprovalChoice(Tail)
		: FHyperAIStudioPlanApprovalChoice();
	if (Choice.Digit == INDEX_NONE)
	{
		bPlanAutoAcceptSent = false;
		bPlanAutoAcceptConfirmed = false;
		return;
	}

	if (!bPlanAutoAcceptSent)
	{
		const uint8 Key = static_cast<uint8>('0' + Choice.Digit);
		if (!HyperAIStudio::TerminalRawInput::WriteRawBytes(*TerminalWidget, TConstArrayView<uint8>(&Key, 1)))
		{
			return;
		}
		bPlanAutoAcceptSent = true;
		PlanAutoAcceptSentTime = FPlatformTime::Seconds();
		const FString Detail = FString::Printf(TEXT("Smart mode chose \"%s\"; %s builds from here."), *Choice.Line, *Settings->SmartBuildModel);
		AddTranscriptLine(TEXT("Plan auto-accepted. ") + Detail);
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = EHyperAIStudioActivityKind::ApprovalApproved;
		Entry.ToolName = TEXT("claude_code_plan");
		Entry.StatusCode = TEXT("plan_auto_accepted");
		Entry.Detail = Detail;
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		return;
	}

	// If the digit only moved the cursor, confirm once the screen has redrawn with it on the choice. A redraw
	// is required first, so a frame from before the keypress can never trigger a second key.
	if (!bPlanAutoAcceptConfirmed && Choice.bCursorOnChoice && LastOutputTime > PlanAutoAcceptSentTime)
	{
		const uint8 Enter = 0x0D;
		bPlanAutoAcceptConfirmed = HyperAIStudio::TerminalRawInput::WriteRawBytes(*TerminalWidget, TConstArrayView<uint8>(&Enter, 1));
	}
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildAgentStateBadge()
{
	return SNew(SHorizontalBox)
		.ToolTipText_Lambda([this]()
		{
			return AgentState.Evidence.IsEmpty()
				? FText::Format(LOCTEXT("AgentStateTooltip", "This tab's agent is {0}."), AgentState.Label)
				: FText::Format(LOCTEXT("AgentStateTooltipEvidence", "This tab's agent is {0}.\nFrom its screen: {1}"),
					AgentState.Label, FText::FromString(AgentState.Evidence));
		})
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush(TEXT("Icons.BulletPoint")))
			.ColorAndOpacity(this, &SHyperAIStudioQuickActionWindow::GetAgentStateColor)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(3.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return AgentState.Label; })
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
}

FSlateColor SHyperAIStudioQuickActionWindow::GetAgentStateColor() const
{
	switch (AgentState.State)
	{
	case EHyperAIStudioAgentState::NeedsInput:
	case EHyperAIStudioAgentState::PlanReview: return FSlateColor(FStyleColors::AccentYellow);
	case EHyperAIStudioAgentState::Blocked: return FSlateColor(FStyleColors::AccentRed);
	case EHyperAIStudioAgentState::Working: return FSlateColor(FStyleColors::AccentBlue);
	case EHyperAIStudioAgentState::Idle: return FSlateColor(FStyleColors::AccentGreen);
	default: return FSlateColor::UseSubduedForeground();
	}
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunStatusHeartbeat(double CurrentTime, float DeltaTime)
{
	// Only re-probe when the previous one finished, so a slow endpoint cannot queue probes on top of each other.
	if (!bRefreshing && !Status.bProbeInProgress)
	{
		RefreshStatus(/*bSilent=*/true);
	}
	return EActiveTimerReturnType::Continue;
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunDeferredTerminalStartup(double CurrentTime, float DeltaTime)
{
	if (bTerminalStartupPaused)
	{
		return EActiveTimerReturnType::Stop;
	}

	if (bTerminalStartupSent)
	{
		return EActiveTimerReturnType::Stop;
	}

	if (!TerminalWidget.IsValid() || !TerminalWidget->IsSessionRunning())
	{
		return EActiveTimerReturnType::Continue;
	}

	if (!Status.IsReady())
	{
		return EActiveTimerReturnType::Stop;
	}

	const FString AgentName = GetSelectedAgentName();
	FString LaunchModel = GetModelForAgent(AgentName);
	if (const FHyperAIStudioAgentModelRoute* SavedRoute = GetDefault<UHyperAIStudioSettings>()->FindAgentModelRoute(AgentName))
	{
		FHyperAIStudioAgentModelRoute ModelRoute = *SavedRoute;
		ModelRoute.SelectedModel = LaunchModel;
		FString ModelPrefix;
		FString ModelArgument;
		FString ModelError;
		if (!GetDefault<UHyperAIStudioSettings>()->TryResolveModelLaunch(ModelRoute, ModelPrefix, ModelArgument, ModelError))
		{
			LaunchModel.Reset();
			LastMessage = FText::FromString(ModelError + TEXT(" Starting with the agent's default model."));
			AddTranscriptLine(LastMessage.ToString());
		}
	}
	bTerminalStartupSent = true;
	ActiveTerminalAgentName = AgentName;
	ActiveTerminalModel = LaunchModel;
	LaunchChat(AgentName);
	Invalidate(EInvalidateWidgetReason::Paint);
	return EActiveTimerReturnType::Stop;
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunDeferredVisibleTerminalCommand(double CurrentTime, float DeltaTime)
{
	if (!bVisibleTerminalCommandPending)
	{
		return EActiveTimerReturnType::Stop;
	}

	if (!TerminalWidget.IsValid() || !TerminalWidget->IsSessionRunning())
	{
		return EActiveTimerReturnType::Continue;
	}

	TerminalWidget->ExecuteCommand(PendingVisibleTerminalCommand);
	AddTranscriptLine(FString::Printf(TEXT("Visible terminal command started: %s"), *PendingVisibleTerminalLabel));
	PendingVisibleTerminalCommand.Empty();
	PendingVisibleTerminalLabel.Empty();
	bVisibleTerminalCommandPending = false;
	Invalidate(EInvalidateWidgetReason::Paint);
	return EActiveTimerReturnType::Stop;
}

EActiveTimerReturnType SHyperAIStudioQuickActionWindow::RunQueuedVisibleTerminalCommandPoll(double CurrentTime, float DeltaTime)
{
	if (!bVisibleTerminalCommandPending)
	{
		TArray<HyperAIStudio::QuickAction::FVisibleTerminalCommand>& Queue = HyperAIStudio::QuickAction::QueuedVisibleTerminalCommands();
		if (Queue.Num() > 0)
		{
			const HyperAIStudio::QuickAction::FVisibleTerminalCommand Command = Queue[0];
			Queue.RemoveAt(0);
			RunVisibleTerminalCommand(Command.Command, Command.Label);
		}
	}

	return EActiveTimerReturnType::Continue;
}

void SHyperAIStudioQuickActionWindow::QueueTerminalStartup(bool bForce, bool bResetTerminal)
{
	const FString AgentName = GetSelectedAgentName();
	if (NativeView.IsValid())
	{
		if (!bForce && ActiveTerminalAgentName.Equals(AgentName, ESearchCase::IgnoreCase))
		{
			return;
		}
		const bool bResuming = ResumeAgentName.Equals(AgentName, ESearchCase::IgnoreCase);
		const FString Resume = bResuming ? ResumeSessionId : FString();
		if (FHyperAIStudioNativeChat::IsSupported(AgentName)
			&& !HyperAIStudio::QuickAction::NeedsTerminal(AgentName, Resume, bResuming ? ResumeBackgroundId : FString()))
		{
			ResumeAgentName.Reset();
			ResumeSessionId.Reset();
			ResumeTitle.Reset();
			ResumeBackgroundId.Reset();
			StopNativeChat();
			StartNativeChat(Resume);
			return;
		}
		// A terminal-only agent, or a Claude Code chat with Remote Control, takes the tab back to the terminal.
		bResetTerminal = true;
	}
	if (!bForce && bTerminalStartupPaused)
	{
		return;
	}

	if (!bForce && bTerminalStartupSent && ActiveTerminalAgentName.Equals(AgentName, ESearchCase::IgnoreCase))
	{
		return;
	}

	if (bResetTerminal)
	{
		RecreateTerminalSession();
	}

	bTerminalStartupPaused = false;
	bTerminalStartupSent = false;
	ActiveTerminalAgentName.Reset();
	RegisterActiveTimer(0.15f, FWidgetActiveTimerDelegate::CreateSP(this, &SHyperAIStudioQuickActionWindow::RunDeferredTerminalStartup));
}

void SHyperAIStudioQuickActionWindow::RecreateTerminalSession()
{
	StopNativeChat();
	if (TerminalHost.IsValid())
	{
		TerminalHost->SetContent(BuildTerminalSessionWidget());
	}
	else
	{
		TerminalWidget.Reset();
	}

	if (TerminalScrollBar.IsValid())
	{
		TerminalScrollBar->SetState(0.0f, 1.0f);
	}

	Invalidate(EInvalidateWidgetReason::LayoutAndVolatility);
}

FString SHyperAIStudioQuickActionWindow::GetTerminalLaunchCommandForAgent(const FString& AgentName, const FString& ChatArguments,
	FString* OutModelPrefix, FString* OutModelArgument) const
{
	if (const HyperAIStudio::QuickAction::FAgentRoute* Route = HyperAIStudio::QuickAction::FindBuiltInRoute(AgentName))
	{
		FString LaunchCommand = FHyperAIStudioService::GetAgentTerminalLaunchCommand(Route->Name);
		if (!LaunchCommand.IsEmpty() && !ChatArguments.IsEmpty())
		{
			LaunchCommand += TEXT(" ") + ChatArguments;
		}
		const FHyperAIStudioAgentModelRoute* SavedRoute = GetDefault<UHyperAIStudioSettings>()->FindAgentModelRoute(Route->Name);
		FHyperAIStudioAgentModelRoute ModelRoute = SavedRoute ? *SavedRoute : FHyperAIStudioAgentModelRoute();
		ModelRoute.SelectedModel = GetModelForAgent(Route->Name);
		FString ModelPrefix;
		FString ModelArgument;
		FString ModelError;
		if (!LaunchCommand.IsEmpty() && SavedRoute
			&& GetDefault<UHyperAIStudioSettings>()->TryResolveModelLaunch(ModelRoute, ModelPrefix, ModelArgument, ModelError)
			&& !ModelArgument.IsEmpty())
		{
			if (OutModelPrefix)
			{
				*OutModelPrefix = ModelPrefix;
			}
			if (OutModelArgument)
			{
				*OutModelArgument = ModelArgument;
			}
			LaunchCommand = ModelPrefix + LaunchCommand + TEXT(" ") + ModelArgument;
		}
		return LaunchCommand;
	}

	return FString();
}

void SHyperAIStudioQuickActionWindow::RunBootstrap(const FString& AgentName, const FString& LaunchCommand)
{
	if (TerminalWidget.IsValid())
	{
		TerminalWidget->ExecuteCommand(BuildTerminalBootstrapCommandForStatus(Status, AgentName, FHyperAIStudioService::GetProjectRoot(), LaunchCommand));
	}
}

void SHyperAIStudioQuickActionWindow::LaunchChat(const FString& AgentName)
{
	using namespace HyperAIStudio::ChatHistory;
	const bool bResuming = !ResumeAgentName.IsEmpty() && ResumeAgentName.Equals(AgentName, ESearchCase::IgnoreCase);
	FString SessionId = bResuming ? ResumeSessionId : FString();
	const FString AttachId = bResuming ? ResumeBackgroundId : FString();
	ActiveChatTitle = bResuming ? ResumeTitle : FString();
	ResumeAgentName.Reset();
	ResumeSessionId.Reset();
	ResumeTitle.Reset();
	ResumeBackgroundId.Reset();
	ActiveBackgroundId.Reset();
	bRemoteRelaunchPending = false;
	bBackgroundStarting = false;
	const int32 Generation = ++LaunchGeneration;

	// A native chat that cannot start falls back to the terminal.
	if (GetDefault<UHyperAIStudioSettings>()->bUseNativeChatView && FHyperAIStudioNativeChat::IsSupported(AgentName)
		&& !HyperAIStudio::QuickAction::NeedsTerminal(AgentName, SessionId, AttachId) && StartNativeChat(SessionId))
	{
		return;
	}
	if (!HyperAIStudio::QuickAction::IsClaudeAgent(AgentName))
	{
		ActiveChatSessionId = SessionId;
		RunBootstrap(AgentName, GetTerminalLaunchCommandForAgent(AgentName, BuildResumeArguments(AgentName, SessionId)));
		return;
	}

	const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	const FString Claude = FHyperAIStudioService::GetAgentTerminalLaunchCommand(AgentName);
	if (!AttachId.IsEmpty())
	{
		// Already running in the background: open it here instead of starting a second copy.
		ActiveChatSessionId = SessionId;
		ActiveBackgroundId = AttachId;
		RunBootstrap(AgentName, Claude + TEXT(" attach ") + AttachId);
		return;
	}

	// A new chat gets its id up front, so Remote can be switched on before Claude writes anything. A chat Claude
	// never wrote down (nothing was sent yet) cannot be resumed, so it starts again under a fresh id.
	const bool bRemote = UHyperAIStudioChatHistoryPrefs::IsRemote(SessionId);
	const bool bExists = ClaudeSessionExists(ProjectRoot, SessionId);
	if (!bExists)
	{
		const FString FreshId = NewClaudeSessionId();
		if (bRemote)
		{
			UHyperAIStudioChatHistoryPrefs::SetRemote(SessionId, false);
			UHyperAIStudioChatHistoryPrefs::SetRemote(FreshId, true);
		}
		SessionId = FreshId;
	}
	ActiveChatSessionId = SessionId;
	const FString RemoteArguments = bRemote ? BuildClaudeRemoteArguments(FApp::GetProjectName(), ActiveChatTitle) : FString();
	FString ForegroundArguments = (bExists ? TEXT("--resume ") : TEXT("--session-id ")) + SessionId;
	// A resumed chat keeps what it started as; a new one in a subagent tab starts as that subagent.
	const FString SubagentArgument = !bExists && HyperAIStudio::Skills::IsValidName(TabSubagent) ? TEXT(" --agent ") + TabSubagent : FString();
	ForegroundArguments += SubagentArgument;
	if (bRemote)
	{
		ForegroundArguments += TEXT(" ") + RemoteArguments;
	}
	if (!bRemote || !GetDefault<UHyperAIStudioSettings>()->bKeepRemoteChatsRunning)
	{
		RunBootstrap(AgentName, GetTerminalLaunchCommandForAgent(AgentName, ForegroundArguments));
		return;
	}

	// Remote chats run as background sessions, so they outlive this tab and the editor; the tab attaches to one.
	FString ModelPrefix;
	FString ModelArgument;
	GetTerminalLaunchCommandForAgent(AgentName, FString(), &ModelPrefix, &ModelArgument);
	FString BackgroundArguments = (bExists ? TEXT("--resume ") + SessionId + TEXT(" ") + RemoteArguments : RemoteArguments) + SubagentArgument;
	if (!ModelArgument.IsEmpty())
	{
		BackgroundArguments += TEXT(" ") + ModelArgument;
	}
	bBackgroundStarting = true;
	LastMessage = LOCTEXT("StartingBackgroundChat", "Starting this remote chat in the background...");
	AddTranscriptLine(LastMessage.ToString());
	const TWeakPtr<SHyperAIStudioQuickActionWindow> WeakThis = SharedThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Generation, AgentName, Claude, ProjectRoot, ModelPrefix, BackgroundArguments,
		SessionId, ForegroundArguments]()
	{
		FString Error;
		const FString ShortId = StartClaudeBackgroundChat(Claude, ProjectRoot, ModelPrefix, BackgroundArguments, Error);
		// --bg picks a new chat's id itself, and starts a copy when the old process is still closing: follow the real id.
		FString StartedSessionId = SessionId;
		if (!ShortId.IsEmpty())
		{
			for (const FHyperAIStudioLiveChat& Chat : ListLiveClaudeChats(Claude, ProjectRoot))
			{
				if (Chat.ShortId == ShortId)
				{
					StartedSessionId = Chat.SessionId;
				}
			}
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Generation, AgentName, Claude, ShortId, Error, SessionId, StartedSessionId,
			ForegroundArguments]()
		{
			const TSharedPtr<SHyperAIStudioQuickActionWindow> This = WeakThis.Pin();
			if (!This.IsValid() || This->LaunchGeneration != Generation)
			{
				// The tab closed or moved on meanwhile; nothing will attach, so do not leave it running.
				if (!ShortId.IsEmpty())
				{
					Async(EAsyncExecution::ThreadPool, [Claude, ShortId]() { StopClaudeBackgroundChat(Claude, ShortId); });
				}
				return;
			}
			This->bBackgroundStarting = false;
			if (ShortId.IsEmpty())
			{
				This->LastMessage = FText::Format(LOCTEXT("BackgroundChatFailed",
					"The background session did not start, so this remote chat runs in the tab and stops with it. {0}"), FText::FromString(Error));
				This->AddTranscriptLine(This->LastMessage.ToString());
				This->RunBootstrap(AgentName, This->GetTerminalLaunchCommandForAgent(AgentName, ForegroundArguments));
				return;
			}
			if (StartedSessionId != SessionId)
			{
				UHyperAIStudioChatHistoryPrefs::SetRemote(SessionId, false);
				UHyperAIStudioChatHistoryPrefs::SetRemote(StartedSessionId, true);
				This->ActiveChatSessionId = StartedSessionId;
			}
			This->ActiveBackgroundId = ShortId;
			This->RunBootstrap(AgentName, Claude + TEXT(" attach ") + ShortId);
		});
	});
}

bool SHyperAIStudioQuickActionWindow::IsActiveChatRemote() const
{
	return UHyperAIStudioChatHistoryPrefs::IsRemote(ActiveChatSessionId);
}

bool SHyperAIStudioQuickActionWindow::IsTurnActive() const
{
	return AgentState.State == EHyperAIStudioAgentState::Working || AgentState.State == EHyperAIStudioAgentState::NeedsInput
		|| AgentState.State == EHyperAIStudioAgentState::PlanReview;
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildNativeToggle()
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.Visibility_Lambda([this]() { return FHyperAIStudioNativeChat::IsSupported(GetSelectedAgentName()) ? EVisibility::Visible : EVisibility::Collapsed; })
		.IsChecked_Lambda([this]() { return NativeView.IsValid() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.ToolTipText(LOCTEXT("NativeViewAgentsTooltip", "Show this chat in HyperAI's own view instead of the terminal: a transcript with tool calls, approvals and the plan, and a message box. New chats open the way you leave it. Switching keeps a Claude Code conversation, and a Codex one opened from the history or started in this view; a Gemini chat starts fresh. A Claude Code chat with Remote Control stays in the terminal."))
		.OnCheckStateChanged_Lambda([this](const ECheckBoxState State)
		{
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			Settings->bUseNativeChatView = State == ECheckBoxState::Checked;
			Settings->SaveConfig();
			SetNativeView(Settings->bUseNativeChatView);
		})
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NativeView", "Native View"))
			.Margin(FMargin(6.0f, 1.0f))
		];
}

bool SHyperAIStudioQuickActionWindow::StartNativeChat(const FString& ResumeId)
{
	const FString Agent = GetSelectedAgentName();
	const FString ChatModel = GetModelForAgent(Agent);
	// A new chat in a subagent tab starts as that subagent, as it does in the terminal.
	const TSharedPtr<FHyperAIStudioNativeChat> Chat = FHyperAIStudioNativeChat::Create(Agent, FHyperAIStudioService::GetProjectRoot(), ChatModel, ResumeId,
		HyperAIStudio::Skills::IsValidName(TabSubagent) ? TabSubagent : FString());
	FString Error;
	if (!Chat.IsValid() || !Chat->Start(Error))
	{
		LastMessage = FText::FromString(Error.IsEmpty() ? FString::Printf(TEXT("%s has no native view."), *Agent) : Error);
		AddTranscriptLine(LastMessage.ToString());
		return false;
	}
	// Replacing the terminal widget ends its session; the native chat carries the conversation from here.
	TerminalWidget.Reset();
	bTerminalStartupSent = true;
	ActiveTerminalAgentName = Agent;
	ActiveTerminalModel = ChatModel;
	// Claude Code's id is known from the start; the others' arrive with their handshake.
	ActiveChatSessionId = Chat->GetSessionId().IsEmpty() ? ResumeId : Chat->GetSessionId();
	if (TerminalHost.IsValid())
	{
		TerminalHost->SetContent(SAssignNew(NativeView, SHyperAIStudioNativeChatView).Chat(Chat));
		NativeView->FocusComposer();
	}
	LastMessage = FText::Format(LOCTEXT("NativeStarted", "{0} is running in the native view."), FText::FromString(Agent));
	AddTranscriptLine(LastMessage.ToString());
	return true;
}

void SHyperAIStudioQuickActionWindow::StopNativeChat()
{
	if (!NativeView.IsValid())
	{
		return;
	}
	if (const TSharedPtr<FHyperAIStudioNativeChat> Chat = NativeView->GetChat())
	{
		ActiveChatSessionId = Chat->GetSessionId();
		Chat->Stop();
	}
	NativeView.Reset();
}

void SHyperAIStudioQuickActionWindow::SetNativeView(const bool bNative)
{
	if (bNative == NativeView.IsValid())
	{
		return;
	}
	if (bNative)
	{
		if (HyperAIStudio::QuickAction::NeedsTerminal(ActiveTerminalAgentName, ActiveChatSessionId, ActiveBackgroundId))
		{
			LastMessage = LOCTEXT("NativeNeedsTerminal", "This chat has Remote Control on, so it stays in the terminal. New chats open in the native view.");
			AddTranscriptLine(LastMessage.ToString());
		}
		else if (FHyperAIStudioNativeChat::IsSupported(GetSelectedAgentName()))
		{
			StartNativeChat(ActiveChatSessionId);
		}
		return;
	}
	// Back to the terminal, where the same conversation resumes.
	const TSharedPtr<FHyperAIStudioNativeChat> Chat = NativeView->GetChat();
	ResumeAgentName = GetSelectedAgentName();
	ResumeSessionId = Chat.IsValid() ? Chat->GetSessionId() : FString();
	ResumeTitle.Reset();
	ResumeBackgroundId.Reset();
	StopNativeChat();
	QueueTerminalStartup(true, true);
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildRemoteToggle()
{
	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.IsEnabled_Lambda([this]()
		{
			return HyperAIStudio::QuickAction::IsClaudeAgent(ActiveTerminalAgentName) && bTerminalStartupSent
				&& !ActiveChatSessionId.IsEmpty() && !bBackgroundStarting;
		})
		.ToolTipText_Lambda([this]() { return GetRemoteToolTip(); })
		.OnClicked_Lambda([this]()
		{
			SetChatRemote(ActiveChatSessionId, !IsActiveChatRemote());
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.World")))
				.ColorAndOpacity_Lambda([this]()
				{
					return IsActiveChatRemote() ? FSlateColor(FStyleColors::AccentGreen) : FSlateColor::UseSubduedForeground();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					if (bRemoteRelaunchPending || bBackgroundStarting)
					{
						return LOCTEXT("RemoteSwitching", "Remote...");
					}
					return IsActiveChatRemote() ? LOCTEXT("RemoteOn", "Remote on") : LOCTEXT("RemoteOff", "Remote");
				})
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioQuickActionWindow::BuildSkillsMenu()
{
	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	const FString Agent = ActiveTerminalAgentName.IsEmpty() ? GetSelectedAgentName() : ActiveTerminalAgentName;
	const FString Prefix = HyperAIStudio::Skills::InvocationPrefix(Agent);
	if (Prefix.IsEmpty())
	{
		Menu.AddMenuEntry(LOCTEXT("SkillsUnavailable", "Skills work with Claude Code and Codex"), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
		return Menu.MakeWidget();
	}
	const FString Root = FHyperAIStudioService::GetProjectRoot();
	// Claude Code reads its own copies; keep them current before offering them.
	HyperAIStudio::Skills::SyncClaudeMirror(Root);
	const TArray<FHyperAIStudioSkill> Skills = HyperAIStudio::Skills::ListForAgent(Root, Agent);
	auto AddSkills = [this, &Menu, &Skills, &Prefix](const bool bPersonal)
	{
		for (const FHyperAIStudioSkill& Skill : Skills)
		{
			if (Skill.Scope.StartsWith(TEXT("personal")) != bPersonal)
			{
				continue;
			}
			Menu.AddMenuEntry(
				FText::FromString(Prefix + Skill.Name),
				FText::FromString(Skill.Description),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateSPLambda(this, [this, Command = Prefix + Skill.Name + TEXT(" ")]()
				{
					InsertIntoAgentPrompt(Command, TEXT("skill"));
				})));
		}
	};
	Menu.BeginSection(NAME_None, LOCTEXT("ProjectSkills", "Project"));
	AddSkills(false);
	Menu.EndSection();
	Menu.BeginSection(NAME_None, LOCTEXT("PersonalSkills", "Personal"));
	AddSkills(true);
	Menu.EndSection();
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(
		LOCTEXT("SaveChatAsSkill", "Save Chat as Skill..."),
		LOCTEXT("SaveChatAsSkillTooltip", "Types a request for the agent to turn what this chat did into a reusable project skill. Edit it, then send."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this, Prefix]()
		{
			InsertIntoAgentPrompt(Prefix + TEXT("make-skill Turn the workflow we just did in this chat into a reusable project skill named "),
				TEXT("save as skill"));
		})));
	Menu.AddMenuEntry(
		LOCTEXT("ManageSkills", "Manage Skills..."),
		LOCTEXT("ManageSkillsTooltip", "Import, export and review skills in HyperAIStudio > Agents."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]() { OnOpenWorkbenchClicked(); })));
	return Menu.MakeWidget();
}

FText SHyperAIStudioQuickActionWindow::GetRemoteToolTip() const
{
	if (ActiveTerminalAgentName.Equals(TEXT("Codex"), ESearchCase::IgnoreCase))
	{
		return LOCTEXT("RemoteCodexTooltip", "Codex has no per-chat Remote Control: its remote control opens every Codex chat on this PC to your paired devices. To use it anyway, run `codex remote-control start` in a terminal and pair from the ChatGPT app.");
	}
	if (!HyperAIStudio::QuickAction::IsClaudeAgent(ActiveTerminalAgentName))
	{
		return LOCTEXT("RemoteUnavailableTooltip", "Remote Control is available for Claude Code chats.");
	}
	if (bRemoteRelaunchPending)
	{
		return LOCTEXT("RemotePendingTooltip", "Remote Control switches as soon as the agent finishes its current turn. Click again to cancel.");
	}
	if (IsActiveChatRemote())
	{
		return GetDefault<UHyperAIStudioSettings>()->bKeepRemoteChatsRunning
			? LOCTEXT("RemoteOnBackgroundTooltip", "Remote Control is on: continue this chat at claude.ai/code or in the Claude app. It runs in the background, so it keeps going after this tab or the editor closes; reopen it from Chats. Click to turn it off.")
			: LOCTEXT("RemoteOnTooltip", "Remote Control is on: continue this chat at claude.ai/code or in the Claude app while this tab is open. Click to turn it off.");
	}
	if (NativeView.IsValid())
	{
		return LOCTEXT("RemoteOffNativeTooltip", "Turn on Remote Control for this chat, to continue it at claude.ai/code or in the Claude app. The chat moves to the terminal view, on the same conversation. Needs a Claude subscription login; on Team and Enterprise plans an admin must allow Remote Control.");
	}
	return LOCTEXT("RemoteOffTooltip", "Turn on Remote Control for this chat, to continue it at claude.ai/code or in the Claude app. The chat restarts on the same conversation. Needs a Claude subscription login; on Team and Enterprise plans an admin must allow Remote Control.");
}

void SHyperAIStudioQuickActionWindow::SetChatRemote(const FString& SessionId, const bool bRemote)
{
	if (SessionId.IsEmpty())
	{
		return;
	}
	UHyperAIStudioChatHistoryPrefs::SetRemote(SessionId, bRemote);
	if (ChatSidebar.IsValid())
	{
		ChatSidebar->RebuildFiltered();
	}
	if (SessionId != ActiveChatSessionId || !bTerminalStartupSent)
	{
		LastMessage = bRemote
			? LOCTEXT("RemoteSetLater", "Remote Control is on for that chat from the next time it starts.")
			: LOCTEXT("RemoteClearedLater", "Remote Control is off for that chat from the next time it starts.");
		AddTranscriptLine(LastMessage.ToString());
		return;
	}
	if (bRemoteRelaunchPending)
	{
		// Switched back before the restart happened: the running chat already matches.
		bRemoteRelaunchPending = false;
		LastMessage = LOCTEXT("RemoteSwitchCancelled", "Remote Control change cancelled.");
		AddTranscriptLine(LastMessage.ToString());
		return;
	}
	HyperAIStudio::QuickAction::Toast(bRemote
		? LOCTEXT("RemoteOnToast", "Remote Control on: this chat appears at claude.ai/code and in the Claude app.")
		: LOCTEXT("RemoteOffToast", "Remote Control off for this chat."));
	if (IsTurnActive())
	{
		bRemoteRelaunchPending = true;
		LastMessage = LOCTEXT("RemoteWaitsForTurn", "Remote Control switches when the agent finishes its current turn.");
		AddTranscriptLine(LastMessage.ToString());
		return;
	}
	RelaunchActiveChat();
}

void SHyperAIStudioQuickActionWindow::RelaunchActiveChat()
{
	bRemoteRelaunchPending = false;
	ResumeAgentName = ActiveTerminalAgentName;
	ResumeSessionId = ActiveChatSessionId;
	ResumeTitle = ActiveChatTitle;
	ResumeBackgroundId.Reset();
	const FString BackgroundId = ActiveBackgroundId;
	if (BackgroundId.IsEmpty())
	{
		QueueTerminalStartup(true, true);
		return;
	}
	// The background session has to end before the same conversation starts again in its new mode.
	ActiveBackgroundId.Reset();
	bBackgroundStarting = true;
	StopBackgroundChat(BackgroundId, [this]() { QueueTerminalStartup(true, true); });
}

void SHyperAIStudioQuickActionWindow::StopBackgroundChat(const FString& ShortId, TFunction<void()> Then)
{
	const TWeakPtr<SHyperAIStudioQuickActionWindow> WeakThis = SharedThis(this);
	const FString Claude = FHyperAIStudioService::GetAgentTerminalLaunchCommand(TEXT("Claude Code"));
	Async(EAsyncExecution::ThreadPool, [WeakThis, ShortId, Claude, Then = MoveTemp(Then)]() mutable
	{
		HyperAIStudio::ChatHistory::StopClaudeBackgroundChat(Claude, ShortId);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Then = MoveTemp(Then)]()
		{
			if (WeakThis.IsValid() && Then)
			{
				Then();
			}
		});
	});
}

bool SHyperAIStudioQuickActionWindow::CanCloseEditor()
{
	if (FApp::IsUnattended())
	{
		return true;
	}
	int32 Stopping = 0;
	TArray<FString> Background;
	for (const TWeakPtr<SHyperAIStudioQuickActionWindow>& Weak : HyperAIStudio::QuickAction::LiveWindows())
	{
		const TSharedPtr<SHyperAIStudioQuickActionWindow> Window = Weak.Pin();
		if (!Window.IsValid() || !Window->IsTurnActive())
		{
			continue;
		}
		if (Window->ActiveBackgroundId.IsEmpty())
		{
			++Stopping;
		}
		else
		{
			Background.Add(Window->ActiveBackgroundId);
		}
	}
	if (Stopping == 0 && Background.IsEmpty())
	{
		return true;
	}

	TArray<FText> Lines;
	if (Stopping > 0)
	{
		Lines.Add(FText::Format(LOCTEXT("CloseStopsChats", "{0} {0}|plural(one=chat is,other=chats are) still working and will stop when the editor closes."), Stopping));
	}
	if (!Background.IsEmpty())
	{
		Lines.Add(FText::Format(LOCTEXT("CloseKeepsRemoteChats", "{0} remote {0}|plural(one=chat is,other=chats are) working in the background and will keep going after the editor closes. Reopen them from Chats."), Background.Num()));
	}
	TArray<SCustomDialog::FButton> Buttons;
	Buttons.Add(SCustomDialog::FButton(Stopping > 0 ? LOCTEXT("CloseAnyway", "Close Anyway") : LOCTEXT("CloseKeepRunning", "Keep Running")));
	if (!Background.IsEmpty())
	{
		Buttons.Add(SCustomDialog::FButton(LOCTEXT("CloseStopRemote", "Stop Them and Close")));
	}
	Buttons.Add(SCustomDialog::FButton(LOCTEXT("CloseCancel", "Cancel")));
	const TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
		.Title(LOCTEXT("CloseChatsTitle", "Agent Chats Are Working"))
		.Content()
		[
			SNew(STextBlock)
			.Text(FText::Join(FText::FromString(TEXT("\n\n")), Lines))
			.AutoWrapText(true)
		]
		.Buttons(Buttons);
	const int32 Choice = Dialog->ShowModal();
	if (Choice == 0)
	{
		return true;
	}
	if (!Background.IsEmpty() && Choice == 1)
	{
		const FString Claude = FHyperAIStudioService::GetAgentTerminalLaunchCommand(TEXT("Claude Code"));
		for (const FString& ShortId : Background)
		{
			HyperAIStudio::ChatHistory::StopClaudeBackgroundChat(Claude, ShortId);
		}
		return true;
	}
	return false;
}

FString SHyperAIStudioQuickActionWindow::BuildTerminalBootstrapCommandForStatus(const FHyperAIStudioStatus& InStatus, const FString& AgentName, const FString& ProjectRoot, const FString& LaunchCommand)
{
	const FString Endpoint = InStatus.Endpoint.IsEmpty() ? TEXT("<not configured>") : InStatus.Endpoint;
	const FString ResolvedProjectRoot = ProjectRoot.IsEmpty() ? FHyperAIStudioService::GetProjectRoot() : ProjectRoot;
	TArray<FString> Commands;
	Commands.Add(TEXT("set \"TERM=xterm-256color\""));
	Commands.Add(FString::Printf(TEXT("cd /d \"%s\""), *ResolvedProjectRoot));
	Commands.Add(TEXT("echo HyperAIStudio Terminal"));
	Commands.Add(FString::Printf(TEXT("echo Status: %s"), *HyperAIStudio::QuickAction::EscapeForCmdEcho(GetReadinessTextForStatus(InStatus, InStatus.bProbeInProgress).ToString())));
	Commands.Add(FString::Printf(TEXT("echo Endpoint: %s"), *HyperAIStudio::QuickAction::EscapeForCmdEcho(Endpoint)));
	if (AgentName.IsEmpty() || LaunchCommand.IsEmpty())
	{
		Commands.Add(TEXT("echo Agent: none usable. Open Setup or Agents, install a CLI agent, then reopen HyperAI Chat."));
	}
	else if (!InStatus.IsReady())
	{
		Commands.Add(FString::Printf(TEXT("echo Agent: %s"), *HyperAIStudio::QuickAction::EscapeForCmdEcho(AgentName)));
		Commands.Add(TEXT("echo Setup is not ready yet. Complete setup before starting the agent CLI."));
	}
	else
	{
		Commands.Add(FString::Printf(TEXT("echo Agent: %s"), *HyperAIStudio::QuickAction::EscapeForCmdEcho(AgentName)));
		Commands.Add(TEXT("echo Starting agent CLI from the Unreal project root..."));
		Commands.Add(FString::Printf(TEXT("( %s ) & echo Agent CLI exited. If no agent prompt is visible, fix the CLI error above or use Copy Prompt."), *LaunchCommand));
	}

	return FString::Printf(TEXT("cmd /d /s /k \"%s\""), *FString::Join(Commands, TEXT(" && ")));
}

FString SHyperAIStudioQuickActionWindow::GetSelectedAgentName() const
{
	return SelectedAgent.IsValid() && *SelectedAgent != HyperAIStudio::QuickAction::NoUsableAgentsLabel()
		? *SelectedAgent
		: FString();
}

bool SHyperAIStudioQuickActionWindow::HasUsableSelectedAgent() const
{
	return !GetSelectedAgentName().IsEmpty();
}

FString SHyperAIStudioQuickActionWindow::GetSelectedAgentStatusLine() const
{
	const FString AgentName = GetSelectedAgentName();
	if (AgentName.IsEmpty())
	{
		return TEXT("Agent: none usable. Open Agents to install or configure Codex, VS Code, Claude Code, Gemini, Cursor, or a custom route.");
	}

	const HyperAIStudio::QuickAction::FAgentRoute* Route = HyperAIStudio::QuickAction::FindBuiltInRoute(AgentName);
	if (!Route)
	{
		return FString::Printf(TEXT("Agent: %s (custom/manual route)."), *AgentName);
	}

	const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	const bool bConfigured = FPaths::FileExists(FPaths::Combine(ProjectRoot, Route->ConfigPath));
	const bool bInstalled = HyperAIStudio::QuickAction::IsPrerequisiteAvailable(CachedPrerequisites, Route->PrerequisiteId);
	return FString::Printf(TEXT("Agent: %s, app %s, config %s (%s)."),
		*AgentName,
		bInstalled ? TEXT("installed") : TEXT("missing"),
		bConfigured ? TEXT("configured") : TEXT("missing"),
		*Route->ConfigPath);
}

FText SHyperAIStudioQuickActionWindow::GetTranscriptText() const
{
	return FText::FromString(FString::Join(TranscriptLines, LINE_TERMINATOR));
}

FText SHyperAIStudioQuickActionWindow::GetReadinessText() const
{
	return GetReadinessTextForStatus(Status, bRefreshing);
}

FText SHyperAIStudioQuickActionWindow::GetReadinessTextForStatus(const FHyperAIStudioStatus& InStatus, bool bIsRefreshing)
{
	if (InStatus.IsReady())
	{
		return LOCTEXT("ReadyBadge", "Ready");
	}
	if (bIsRefreshing || InStatus.bProbeInProgress)
	{
		return LOCTEXT("CheckingBadge", "Checking");
	}
	return LOCTEXT("SetupBadge", "Setup needed");
}

FSlateColor SHyperAIStudioQuickActionWindow::GetReadinessColor() const
{
	if (Status.IsReady())
	{
		return HyperAIStudio::QuickAction::GoodColor();
	}
	if (bRefreshing || Status.bProbeInProgress)
	{
		return HyperAIStudio::QuickAction::WarningColor();
	}
	return HyperAIStudio::QuickAction::ErrorColor();
}

FText SHyperAIStudioQuickActionWindow::GetNextActionTextForStatus(const FHyperAIStudioStatus& InStatus, bool bIsRefreshing)
{
	if (InStatus.IsReady())
	{
		return LOCTEXT("ReadyNextAction", "Next useful action: type to the embedded agent terminal. Copy Prompt is only for manual handoff fallback.");
	}
	if (bIsRefreshing || InStatus.bProbeInProgress)
	{
		return LOCTEXT("CheckingNextAction", "Next useful action: wait for the MCP readiness probe to finish, then choose a handoff action.");
	}
	if (!InStatus.bUnrealMCPModuleAvailable)
	{
		return LOCTEXT("ModuleNextAction", "Next useful action: enable the required Unreal MCP plugins from the full HyperAIStudio workbench.");
	}
	if (!InStatus.bAgentFilesReady || !InStatus.bUnrealMCPSettingsConfigured)
	{
		return LOCTEXT("SetupNextAction", "Next useful action: start setup to configure Unreal MCP and generate agent files.");
	}
	if (InStatus.ConfiguredAgentCount <= 0)
	{
		return LOCTEXT("AgentConfigNextAction", "Next useful action: enable one installed agent with generated config before using Chat / Terminal.");
	}
	return LOCTEXT("ServerNextAction", "Next useful action: start or refresh Unreal MCP from the full workbench.");
}

#undef LOCTEXT_NAMESPACE
