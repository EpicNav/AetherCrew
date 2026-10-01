// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Animation/CurveSequence.h"
#include "Containers/Ticker.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAgentState.h"
#include "HyperAIStudioService.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

struct FHyperAIStudioRestartChat;
class FHyperAIStudioNativeChat;

class SHyperAIStudioQuickActionWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioQuickActionWindow) {}
		SLATE_EVENT(FSimpleDelegate, OnOpenWorkbench)
		/** Opens another chat tab, so a second agent can work alongside this one. */
		SLATE_EVENT(FSimpleDelegate, OnNewAgentTab)
		/** 1 for the first chat tab; shown in the tab label so tabs can be told apart. */
		SLATE_ARGUMENT(int32, TabIndex)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioQuickActionWindow() override;
	/** The dock tab hosting this panel, so its label can carry the agent's state. */
	void SetOwnerTab(const TSharedRef<class SDockTab>& InTab);
	EHyperAIStudioAgentState GetAgentState() const { return AgentState.State; }
	/** The agent and model this tab's terminal launched, while that session runs. */
	bool GetRunningAgent(FString& OutAgent, FString& OutModel) const;
	/** "HyperAI Chat 2: VFX (Codex)": which tab and agent, for handoff menus and messages. */
	FString GetTabDescription() const;
	void SelectAgentByName(const FString& AgentName);
	/** Restart the terminal on that chat's agent, resuming the conversation. */
	void ResumeChat(const struct FHyperAIStudioChatSession& Session);
	void RunVisibleTerminalCommand(const FString& Command, const FString& Label);
	static void EnqueueVisibleTerminalCommand(const FString& Command, const FString& Label);
	static FText GetReadinessTextForStatus(const FHyperAIStudioStatus& InStatus, bool bIsRefreshing);
	static FText GetNextActionTextForStatus(const FHyperAIStudioStatus& InStatus, bool bIsRefreshing);
	static FString BuildTerminalBootstrapCommandForStatus(const FHyperAIStudioStatus& InStatus, const FString& AgentName, const FString& ProjectRoot, const FString& LaunchCommand);
	/** Editor close check: chats still working get a say, and remote chats working in the background can be stopped. */
	static bool CanCloseEditor();
	/** The chat pages' ids: Chat for the first tab, then Chat2, Chat3... */
	static FName GetPageId(int32 TabIndex);
	/** Every tab running an agent, as an editor restart reopens it. */
	static TArray<FHyperAIStudioRestartChat> GetRestartChats();
	/**
	 * Sends Text to a chat tab as the user, opening the tab if needed (HyperAIStudio.ChatSend). Images go with it in
	 * HyperAI's chat view; a terminal gets a long or multi-line message as one line pointing at a file that holds it.
	 */
	static bool SendToChat(int32 TabIndex, const FString& Text, FString& OutMessage, const TArray<FString>& Images = {});
	/** Chats open at once. */
	static constexpr int32 MaxChatTabs = 4;
	/**
	 * Opens the lowest free chat tab for a job, on the agent its task route names or else the preferred agent, and gives
	 * the agent Prompt (with Images) as it starts. Returns the tab's index, or 0 with OutError set.
	 */
	static int32 OpenJobChat(const FString& Category, const FString& Prompt, const TArray<FString>& Images, FString& OutAgent, FString& OutError);
	/**
	 * False when the tab is not open. Working: starting, a turn running, a message queued, or a question waiting.
	 * Started: its agent was launched, after which the tab may go behind another page (only the launch needs it in front).
	 */
	static bool GetTabWork(int32 TabIndex, bool& bOutWorking, bool* bOutStarted = nullptr);
	/** Interrupts the turn running in a tab's HyperAI chat view; a terminal chat is left to finish. */
	static void InterruptTab(int32 TabIndex);
	/** The chat a tab's HyperAI chat view runs; null for a closed tab or a terminal one. */
	static TSharedPtr<FHyperAIStudioNativeChat> GetTabChat(int32 TabIndex);

	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	/** Assets and actors dropped on the panel become object paths in the agent's prompt. */
	virtual void OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

private:
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildStatusLine();
	TSharedRef<SWidget> BuildSettingsMenu();
	TSharedRef<SWidget> BuildAgentActions();
	TSharedRef<SWidget> BuildAgentSelector();
	TSharedRef<SWidget> BuildModelSelector();
	TSharedRef<SWidget> BuildModelMenu();
	TSharedRef<SWidget> BuildContextSelector();
	TSharedRef<SWidget> BuildContextMenu();
	TSharedRef<SWidget> BuildMcpStatusMenu();
	/** Paste text into the running agent's prompt without sending it. */
	bool InsertIntoAgentPrompt(const FString& Text, const FString& Label, bool bFocusTerminal = true);
	/** Handoff prompts: sent when the agent is idle and auto-send is on, otherwise typed in for the user to send. */
	bool DeliverHandoffPrompt(const FString& Text, const FString& Label);
	TSharedRef<SWidget> BuildNewTabMenu();
	TSharedRef<SWidget> BuildHandoffMenu();
	/** Subagent, when set, starts a Claude Code tab as that subagent. */
	void OpenRoutedTab(const FString& Category, const FHyperAIStudioRouteChoice& Choice, const FString& Subagent = FString());
	void StartHandoff(TWeakPtr<SHyperAIStudioQuickActionWindow> Builder);
	/**
	 * Moves a running handoff along: plan file written, then result file written, then review asked for. Runs on
	 * the core ticker and watches files, because Slate paints a hidden tab's terminal, and its timers, not at all.
	 */
	bool TickHandoff(float DeltaTime);
	TArray<FString> GetUsableAgentNames() const;
	static TSharedPtr<SHyperAIStudioQuickActionWindow> FindTab(int32 TabIndex);
	/** The model this tab launches AgentName with: the tab's own choice, else the agent's saved one. */
	FString GetModelForAgent(const FString& AgentName) const;
	FText GetTabTitle() const;
	FText GetMcpStatusText() const;
	void SelectModel(FString ModelId);
	bool IsModelSelected(FString ModelId) const;
	FText GetModelButtonText() const;
	TSharedRef<SWidget> BuildTerminalSessionWidget();
	TSharedRef<SWidget> BuildSessionLog();
	void ToggleChatSidebar();
	void ToggleActivitySidebar();

	FReply OnRefreshClicked();
	FReply OnPreparePromptClicked(FString AgentName);
	FReply OnToggleTerminalAgentClicked();
	FReply OnOpenWorkbenchClicked();
	/** bSilent keeps the current status on screen until the probe settles (heartbeat re-probes). */
	void RefreshStatus(bool bSilent = false);
	void RefreshAgentOptions();
	void ResetTranscript();
	void AddTranscriptLine(const FString& Line);
	EActiveTimerReturnType RunDeferredRefresh(double CurrentTime, float DeltaTime);
	EActiveTimerReturnType RunStatusHeartbeat(double CurrentTime, float DeltaTime);
	/** Re-reads what this tab's agent is showing and updates the tab label when it changes. */
	EActiveTimerReturnType RunAgentStateHeartbeat(double CurrentTime, float DeltaTime);
	/** Smart mode: answers Claude's plan prompt with its auto-mode option, once per prompt. */
	void AutoAcceptSmartPlan(const FString& Tail, double LastOutputTime);
	TSharedRef<SWidget> BuildAgentStateBadge();
	FSlateColor GetAgentStateColor() const;
	FText GetTabLabel() const;
	EActiveTimerReturnType RunDeferredTerminalStartup(double CurrentTime, float DeltaTime);
	EActiveTimerReturnType RunDeferredVisibleTerminalCommand(double CurrentTime, float DeltaTime);
	EActiveTimerReturnType RunQueuedVisibleTerminalCommandPoll(double CurrentTime, float DeltaTime);
	void QueueTerminalStartup(bool bForce, bool bResetTerminal = false);
	void RecreateTerminalSession();
	/** Starts this tab's chat: picks its session id, adds Remote Control, and runs remote chats in the background. */
	void LaunchChat(const FString& AgentName);
	void RunBootstrap(const FString& AgentName, const FString& LaunchCommand);
	/** Launcher, then ChatArguments (session, remote), then the model. Also returns the model's cmd prefix and argument. */
	FString GetTerminalLaunchCommandForAgent(const FString& AgentName, const FString& ChatArguments,
		FString* OutModelPrefix = nullptr, FString* OutModelArgument = nullptr) const;
	/** Remote Control for a chat. The chat in this tab restarts on the same conversation now, or once the turn ends. */
	void SetChatRemote(const FString& SessionId, bool bRemote);
	void RelaunchActiveChat();
	void StopBackgroundChat(const FString& ShortId, TFunction<void()> Then);
	/** Deletes a chat the user confirmed, unless a tab has it open; every tab's chat list then shows it gone. */
	void DeleteChat(const struct FHyperAIStudioChatSession& Session);
	bool IsActiveChatRemote() const;
	/** The agent is producing output, or is stopped at a question or a plan. */
	bool IsTurnActive() const;
	TSharedRef<SWidget> BuildRemoteToggle();
	/** Auto Mode for Claude Code and Codex; switching it restarts this tab's chat on the same conversation. */
	TSharedRef<SWidget> BuildAutoToggle();
	void SetAutoMode(bool bAuto);
	/** This agent's skills: picking one types its command into the prompt. */
	TSharedRef<SWidget> BuildSkillsMenu();
	FText GetRemoteToolTip() const;

	FString GetSelectedAgentName() const;
	bool HasUsableSelectedAgent() const;
	FString GetSelectedAgentStatusLine() const;
	FText GetTranscriptText() const;
	FText GetReadinessText() const;
	FSlateColor GetReadinessColor() const;

	TSharedPtr<FHyperAIStudioService> Service;
	FHyperAIStudioStatus Status;
	FHyperAIStudioPromptPackage LastPromptPackage;
	TArray<FHyperAIStudioPrerequisiteStatus> CachedPrerequisites;
	TArray<TSharedPtr<FString>> AgentOptions;
	TSharedPtr<FString> SelectedAgent;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> AgentComboBox;
	TSharedPtr<class SBox> TerminalHost;
	TSharedPtr<class STerminal> TerminalWidget;
	TSharedPtr<class SScrollBar> TerminalScrollBar;
	TSharedPtr<class SHyperAIStudioChatHistorySidebar> ChatSidebar;
	TSharedPtr<class SHyperAIStudioChatActivitySidebar> ActivitySidebar;
	FCurveSequence ActivitySidebarCurve;
	bool bActivitySidebarOpen = false;
	FCurveSequence ChatSidebarCurve;
	bool bChatSidebarOpen = false;
	/** Chat running in the terminal: resumed from history, or a new Claude Code chat's own id. */
	FString ActiveChatSessionId;
	TArray<FString> TranscriptLines;
	FSimpleDelegate OnOpenWorkbench;
	FText LastMessage;
	bool bRefreshing = false;
	/**
	 * Hint under the header, set only when a probe settles. Every re-probe briefly reports "not ready",
	 * and binding the line to live status made it expand and collapse on each heartbeat.
	 */
	FText StatusHint;
	bool bTerminalStartupSent = false;
	bool bTerminalStartupPaused = false;
	bool bVisibleTerminalCommandPending = false;
	FString PendingVisibleTerminalCommand;
	FString PendingVisibleTerminalLabel;
	FString ActiveTerminalAgentName;
	/** Chat the next agent startup resumes; cleared once that startup is sent. */
	FString ResumeAgentName;
	FString ResumeSessionId;
	FString ResumeTitle;
	/** Set when that chat already runs in the background: the startup attaches instead of starting a copy. */
	FString ResumeBackgroundId;
	/** Title of the chat in this tab when it came from Chats; it names the remote session. */
	FString ActiveChatTitle;
	/** Short id of the background session this tab's terminal is attached to; empty when the chat runs in the tab. */
	FString ActiveBackgroundId;
	/** Remote was switched mid-turn: restart on it once the agent is idle. */
	bool bRemoteRelaunchPending = false;
	/** A background session is starting or stopping; the tab attaches when that is done. */
	bool bBackgroundStarting = false;
	/** Bumped per launch, so a background start that finishes late never attaches to a newer chat. */
	int32 LaunchGeneration = 0;
	double WorkingSince = 0.0;
	FSimpleDelegate OnNewAgentTab;
	TWeakPtr<class SDockTab> OwnerTab;
	int32 TabIndex = 1;
	FHyperAIStudioAgentStateSnapshot AgentState;
	/** The option digit went to the plan prompt now showing; cleared once that prompt leaves the screen. */
	bool bPlanAutoAcceptSent = false;
	bool bPlanAutoAcceptConfirmed = false;
	double PlanAutoAcceptSentTime = 0.0;
	/** Kind of task this tab was opened for from the new-tab menu, or its chat's first message routed it to; shown on the tab. */
	FString RouteCategory;
	/** RouteCategory came from the first message, so a new chat in this tab is routed again. */
	bool bAutoRouted = false;
	/** You picked this tab's agent or model: a task never moves it to another. */
	bool bAgentChosenHere = false;
	/** The native chat running here continues an earlier conversation. */
	bool bChatResumed = false;
	/** Said in the next native chat as it starts, such as which agent a task was routed to. */
	FString PendingNotice;
	/** Claude Code subagent this tab's new chats start as (claude --agent). */
	FString TabSubagent;
	/** After an editor restart: what this tab's chat is told when it runs again. */
	FString ContinueMessage;
	/** What the agent's start last waited on, so the log says it once. */
	FString LastStartupWait;
	/** A prompt sent before the agent started (HyperAIStudio.ChatSend, a job): the agent gets it as it starts. */
	FString PendingPrompt;
	TArray<FString> PendingImages;
	/** Codex, Gemini and Claude Code can run in HyperAI's own chat view instead of the terminal. */
	TSharedPtr<class SHyperAIStudioNativeChatView> NativeView;
	TSharedRef<SWidget> BuildNativeToggle();
	/** Switches this tab between the terminal and the native view, resuming the conversation when its id is known. */
	void SetNativeView(bool bNative);
	/** Replaces the terminal with a native chat for the selected agent, resuming ResumeId when set. */
	bool StartNativeChat(const FString& ResumeId);
	void StopNativeChat();
	/**
	 * A new chat's first message, before it is sent: the task route it fits starts its agent here, the chat starting
	 * again on that agent with the message when it is another. True when it took the message.
	 */
	bool RouteFirstMessage(const FString& Message, const TArray<FString>& Images);
	/** Model chosen in this tab (or by its route) for TabModelAgent; other tabs keep their own. */
	FString TabModel;
	FString TabModelAgent;
	FString ActiveTerminalModel;

	/** Plan here, build in another tab, then review here. */
	struct FHandoff
	{
		FString PlanPath;
		/** The builder writes what it changed here when it is done. */
		FString ResultPath;
		TWeakPtr<SHyperAIStudioQuickActionWindow> Builder;
		FString BuilderDescription;
		bool bBuilding = false;
		/** Size of the awaited file at the last tick; a file counts as written once its size holds for a tick. */
		int64 LastSize = -1;
		double StartedTime = 0.0;
	};
	TOptional<FHandoff> Handoff;
	FTSTicker::FDelegateHandle HandoffTicker;
};
