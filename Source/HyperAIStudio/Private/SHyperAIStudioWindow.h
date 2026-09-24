// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Framework/SlateDelegates.h"
#include "HyperAIStudioService.h"
#include "Widgets/SCompoundWidget.h"

enum class EHyperAIStudioNativeExecutionMode : uint8;
enum class EHyperAIStudioNativeToolChannel : uint8;

struct FHyperAIStudioWorkbenchSurfaceSmoke
{
	TArray<FString> NavigationTabs;
	TArray<FString> FirstRunLabels;
	TArray<FString> AgentTargetActions;
	TArray<FString> HandoffActions;
	TArray<FString> AgentTestActions;
	TArray<FString> AdvancedLabels;
	TArray<FString> AgentConfigLabels;
	TArray<FString> MCPServerActions;
	TArray<FString> BannedSubstrings;
};

class SHyperAIStudioWindow : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_OneParam(FOnOpenSection, int32 /*Section*/);

	SLATE_BEGIN_ARGS(SHyperAIStudioWindow)
		: _Section(0)
	{}
		/** The section this shows, as a page of HyperAI Studio's Settings: 0 setup, 1 agents, 2 MCP servers, 3 documentation. */
		SLATE_ARGUMENT(int32, Section)
		/** Asked to show another section, as Configure Agents does from Setup. */
		SLATE_EVENT(FOnOpenSection, OnOpenSection)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	/** Closes the dialogs this section opened: their buttons call back into it. */
	virtual ~SHyperAIStudioWindow() override;
	static int32 GetInitialTabIndexForStatus(const FHyperAIStudioStatus& InStatus);
	static int32 GetPostRefreshTabIndex(int32 CurrentTabIndex, const FHyperAIStudioStatus& NewStatus, bool bUserSelectedTab, bool bAutoRoutePending);
	static FText GetTerminalPanelHintText();
	static FString GetTerminalStartupRecipeText(const FString& ProjectRoot);
	static FHyperAIStudioWorkbenchSurfaceSmoke GetWorkbenchSurfaceSmoke();
	static bool IsRetiredExactOwnedMCPServerEntryForWorkbench(const FHyperAIStudioMCPServerEntry& Entry);
	static FString BuildPrerequisiteInstallTerminalCommandForSmoke(const FString& PrerequisiteId, const FString& DisplayName, const FString& InstallCommand, const FString& ProjectRoot);

private:
	TSharedRef<SWidget> BuildIconTextButton(const FText& Label, const FName IconName, const FText& ToolTip, FOnClicked OnClicked) const;
	TSharedRef<SWidget> BuildSetupTab();
	TSharedRef<SWidget> BuildAgentsTab();
	TSharedRef<SWidget> BuildMCPServersTab();
	TSharedRef<SWidget> BuildNativeToolSettingsBar();
	TSharedRef<SWidget> BuildHelpTab();
	TSharedRef<SWidget> BuildSetupSummaryPanel();
	TSharedRef<SWidget> BuildSetupSummaryRow(const FText& Label, TFunction<FText()> ValueGetter, TFunction<FSlateColor()> ColorGetter, const FText& ActionLabel, FOnClicked Action) const;
	TSharedRef<SWidget> BuildAgentTableHeader() const;
	TSharedRef<SWidget> BuildAgentRow(const FString& PrerequisiteId, const FString& AgentName, const FString& ConfigRelativePath);
	TSharedRef<SWidget> BuildDocsShortcutRow(const FText& Title, const FText& Body, const FText& PrimaryLabel, FOnClicked PrimaryAction, const FText& SecondaryLabel, FOnClicked SecondaryAction) const;
	TSharedRef<SWidget> BuildMCPStaticRow(const FText& Name, const FText& Type, TFunction<FText()> StatusGetter, TFunction<FSlateColor()> ColorGetter, const FText& Priority, TFunction<FText()> ToolsGetter, const FText& PrimaryLabel, FOnClicked PrimaryAction, const FText& SecondaryLabel, FOnClicked SecondaryAction) const;
	void ResetContentScroll();

	FReply OnSetupClicked();
	FReply OnEnablePluginsClicked();
	FReply OnRefreshClicked();
	FReply OnEditMCPServerClicked(int32 ServerIndex);
	FReply OnOpenChatClicked();
	FReply OnConfigureAgentsClicked();
	FReply OnViewPrerequisitesClicked();
	FReply OnOpenAgentFilesDialogClicked();
	FReply OnOpenAgentFilesForAgentClicked(FString AgentName);
	FReply OnCreateAgentFileClicked(FString RelativePath);
	FReply OnRegenerateAgentFilesClicked();
	FReply OnGenerateAgentConfigClicked(FString AgentName);
	FReply OnUseAgentInChatClicked(FString AgentName);
	FReply OnPromptInAgentClicked(FString AgentName);
	FReply OnOpenMCPToolsClicked(FString ServerName, FString ToolsSummary);
	FReply OnSetNativeExecutionModeClicked(EHyperAIStudioNativeExecutionMode Mode);
	FReply OnSetNativeToolChannelClicked(EHyperAIStudioNativeToolChannel Channel);
	FReply OnOpenProjectPathClicked(FString RelativePath);
	FReply OnRevealProjectPathClicked(FString RelativePath);
	FReply OnInstallPrerequisiteClicked(FString PrerequisiteId, FString DisplayName, FString InstallCommand);
	FReply OnInstallMissingPrerequisitesClicked();
	FReply OnCopyExampleMCPProfileClicked();
	FReply OnImportMCPProfileClicked();
	FReply OnTestMCPServerClicked(int32 ServerIndex);
	void SaveMCPServerChange(const FString& ChangeSummary);
	FReply OnCopyInspectorCommandClicked();
	FReply OnOpenAgentsClicked();
	FReply OnOpenIncludedDocsClicked();
	FReply OnCopyFirstRunPromptClicked();
	FReply OnOpenOfficialMCPDocsClicked();
	FReply OnOpenProjectFolderClicked();
	FReply OnOpenPrerequisiteDocsClicked(FString DisplayName, FString DocumentationUrl);
	bool OpenChatTerminalInstallCommand(const FString& TerminalCommand, const FString& Label, FText& OutMessage);
	void RefreshPrerequisitesCache();
	const FHyperAIStudioPrerequisiteStatus* FindCachedPrerequisite(const FString& PrerequisiteId) const;
	bool HasCachedMissingInstallablePrerequisites() const;
	void RefreshStatus();
	EActiveTimerReturnType RunDeferredRefresh(double CurrentTime, float DeltaTime);
	EActiveTimerReturnType RunReadinessRetry(double CurrentTime, float DeltaTime);
	void AddSessionLogEntry(const FString& Entry);

	FSlateColor BoolColor(bool bValue) const;

	TSharedPtr<FHyperAIStudioService> Service;
	FHyperAIStudioStatus Status;
	FHyperAIStudioPromptPackage LastPromptPackage;
	TArray<FHyperAIStudioPrerequisiteStatus> CachedPrerequisites;
	TSharedPtr<class SScrollBox> MainScrollBox;
	FOnOpenSection OnOpenSection;
	/** Dialogs this section has open, closed with it. */
	TArray<TWeakPtr<SWindow>> OpenDialogs;
	/** Opens Dialog in front of the window this section is in, as its child, so it closes with that window too. */
	void ShowDialog(const TSharedRef<SWindow>& Dialog);
	FText LastMessage;
	TArray<FString> SessionLog;
	FString ActiveInstallingPrerequisiteId;
	int32 ActiveTabIndex = 0;
	bool bRefreshing = false;
	bool bReportNextRefreshResult = false;
	bool bUserSelectedTab = false;
	bool bAutoRouteAfterFirstRefresh = true;
	int32 PendingReadinessRetryCount = 0;
	int32 CachedLiveEpicToolCount = INDEX_NONE;
	int32 CachedLiveHyperToolCount = INDEX_NONE;
};
