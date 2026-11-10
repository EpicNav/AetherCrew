// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioChatModes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

/** Chat modes: the built-in ones and the project's, made, edited, imported and exported here. */
class SHyperAIStudioModesPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioModesPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The panel in a window of its own, as the composer's mode menu opens it. */
	static void OpenWindow();
	/** Edits a mode (a new one when Existing is null) until it saves or the user cancels; true when it saved. */
	static bool EditMode(const FHyperAIStudioChatMode* Existing);

private:
	using FModePtr = TSharedPtr<FHyperAIStudioChatMode>;

	void Reload();
	TSharedRef<ITableRow> GenerateRow(FModePtr Mode, const TSharedRef<STableViewBase>& OwnerTable);

	TArray<FModePtr> Modes;
	TSharedPtr<SListView<FModePtr>> List;
};
