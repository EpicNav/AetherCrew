// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioToolInventoryViewModel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SBox;

/**
 * Every tool agents can call, from HyperAI's packs and Epic's toolsets, as a searchable list. Picking one opens it as
 * a form beside the list: fill its request, run it the way an agent's MCP call would, and read its answer.
 */
class SHyperAIStudioToolsPage : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioToolsPage) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	using FItem = TSharedPtr<FHyperAIStudioToolInventoryRow>;

	void SetQuery(const FString& InQuery);
	void SetSource(TOptional<EHyperAIStudioToolInventorySource> InSource);
	/** Opens a tool's form, as clicking it in the list does; false when no tool has that name. */
	bool SelectTool(const FString& Name);
	int32 GetShownCount() const { return Shown.Num(); }
	int32 GetToolCount() const { return All.Num(); }
	bool IsRefreshing() const { return bRefreshing; }
	/** Asks the MCP server for its tools again. */
	void Refresh();

private:
	void SetView(FHyperAIStudioToolInventoryView InView);
	void Filter();
	void ShowTool(const FItem& Item);
	TSharedRef<ITableRow> GenerateRow(FItem Item, const TSharedRef<class STableViewBase>& Owner);
	TSharedRef<SWidget> SourceButton(const FText& Label, TOptional<EHyperAIStudioToolInventorySource> InSource);

	FString Endpoint;
	FHyperAIStudioToolInventoryView View;
	TArray<FItem> All;
	TArray<FItem> Shown;
	FString Query;
	TOptional<EHyperAIStudioToolInventorySource> Source;
	bool bRefreshing = false;
	FItem Selected;
	TSharedPtr<SListView<FItem>> List;
	TSharedPtr<SBox> FormHost;
	/** The refresh answers later; the page may be gone by then. */
	TSharedRef<bool> bAlive = MakeShared<bool>(true);

public:
	virtual ~SHyperAIStudioToolsPage() override { *bAlive = false; }
};
