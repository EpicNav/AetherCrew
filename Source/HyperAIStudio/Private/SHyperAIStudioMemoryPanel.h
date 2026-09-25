// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioProjectMemory.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

/** Project Memory in the workbench: every note agents see, the ones held for the user first, and the tasks chats finished. */
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
	using FTaskPtr = TSharedPtr<FHyperAIStudioMemoryTask>;

	void Reload();
	void RebuildFiltered();
	TSharedRef<ITableRow> GenerateRow(FFactPtr Fact, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> GenerateTaskRow(FTaskPtr Task, const TSharedRef<STableViewBase>& OwnerTable);
	/** A held note, or one the setting keeps from chats until the user confirms it. */
	static bool NeedsUser(const FHyperAIStudioMemoryFact& Fact);
	TSharedRef<SWidget> BuildModeMenu();

	TArray<FHyperAIStudioMemoryFact> AllFacts;
	TArray<FFactPtr> Filtered;
	FString Filter;
	TSharedPtr<SListView<FFactPtr>> List;
	/** The page shows notes, or the tasks chats finished on this machine. */
	bool bShowTasks = false;
	TArray<FTaskPtr> Tasks;
	TSharedPtr<SListView<FTaskPtr>> TaskList;
	FDelegateHandle ChangedHandle;
};
