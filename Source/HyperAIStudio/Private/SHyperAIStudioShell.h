// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioService.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FTabManager;
class SDockTab;
class SMenuAnchor;
class SSearchBox;
class SVerticalBox;
class SWindow;

/**
 * The HyperAI Studio window's content.
 * - Header: the logo, the current page, search and status.
 * - Navigation rail: pages grouped, folding to icons when the window is narrow or when asked.
 * - Pages: tabs in the window's own tab manager, so they split, dock and float like any editor panel.
 * - Status bar: the MCP server, work waiting on the user, and the level.
 */
class SHyperAIStudioShell : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioShell) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<SDockTab>& InOwnerTab, const TSharedPtr<SWindow>& OwnerWindow);
	virtual ~SHyperAIStudioShell() override;

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	/** Opens a page's tab, or brings it forward. */
	TSharedPtr<SDockTab> OpenPage(FName PageId);
	TSharedPtr<SDockTab> FindPageTab(FName PageId) const;
	FName GetCurrentPage() const { return CurrentPage; }

	/** Whether the rail shows icons only: asked for, or the window is narrow. */
	bool IsRailCompact() const;
	/** Auto follows the window's width; the others are what the user asked for. */
	enum class ERail : uint8
	{
		Auto,
		Expanded,
		Compact
	};
	void SetRail(ERail InRail);

	/** Runs the search box's query, as typing it would; the results are what the list shows. */
	void Search(const FString& Query);
	int32 GetSearchResultCount() const { return Results.Num(); }

private:
	struct FResult
	{
		FText Label;
		FText Detail;
		FName Icon;
		TFunction<void()> Run;
	};

	void RegisterSpawner(const struct FHyperAIStudioPage& Page);
	TSharedRef<SDockTab> SpawnPage(const class FSpawnTabArgs& Args, FName PageId);
	TSharedRef<SWidget> BuildHeader();
	TSharedRef<SWidget> BuildRail();
	TSharedRef<SWidget> BuildNavItem(const struct FHyperAIStudioPage& Page);
	TSharedRef<SWidget> BuildStatusBar();
	TSharedRef<SWidget> BuildSearch();
	TSharedRef<ITableRow> GenerateResult(TSharedPtr<FResult> Result, const TSharedRef<class STableViewBase>& Owner);
	FReply OnSearchKey(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	void RunResult(const TSharedPtr<FResult>& Result);
	void RebuildNav();
	void OnPagesChanged();
	void OnActiveTabChanged(TSharedPtr<SDockTab> Previous, TSharedPtr<SDockTab> Active);
	void PersistLayout();
	void RefreshStatus();
	FText GetMcpText() const;
	FSlateColor GetMcpColor() const;

	TSharedPtr<FTabManager> TabManager;
	TWeakPtr<SDockTab> OwnerTab;
	TSharedPtr<SVerticalBox> NavList;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SMenuAnchor> SearchAnchor;
	TSharedPtr<SListView<TSharedPtr<FResult>>> ResultList;
	TArray<TSharedPtr<FResult>> Results;

	FName CurrentPage;
	ERail Rail = ERail::Auto;
	float Width = 1280.0f;

	TSharedPtr<FHyperAIStudioService> Service;
	FHyperAIStudioStatus Mcp;
	bool bHasStatus = false;
	double NextStatusAt = 0.0;
	int32 Approvals = 0;
	int32 Running = 0;

	FDelegateHandle ActiveTabHandle;
	FDelegateHandle ForegroundHandle;
	FDelegateHandle ChangedHandle;
};
