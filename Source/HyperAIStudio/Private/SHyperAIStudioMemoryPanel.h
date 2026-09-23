// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioProjectMemory.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

/** Project Memory in the workbench: every fact agents see, with review for the ones agents saved. */
class SHyperAIStudioMemoryPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioMemoryPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioMemoryPanel() override;

	/** Adds a fact, or edits Existing when it is set. What the user writes applies in full at once. */
	static void OpenFactDialog(const FHyperAIStudioMemoryFact* Existing = nullptr);
	/** Copies chosen Claude Code memory files for this project into project memory, each shown first. */
	static void OpenClaudeImportDialog();

private:
	using FFactPtr = TSharedPtr<FHyperAIStudioMemoryFact>;

	void Reload();
	void RebuildFiltered();
	TSharedRef<ITableRow> GenerateRow(FFactPtr Fact, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<SWidget> BuildModeMenu();

	TArray<FHyperAIStudioMemoryFact> AllFacts;
	TArray<FFactPtr> Filtered;
	FString Filter;
	TSharedPtr<SListView<FFactPtr>> List;
	FDelegateHandle ChangedHandle;
};
