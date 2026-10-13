// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioMentions.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class ITableRow;
class STableViewBase;

DECLARE_DELEGATE_OneParam(FOnHyperAIStudioMentionChosen, const FHyperAIStudioMention& /*Mention*/);

/**
 * The list the chat composer's @ opens: assets, level actors, project files, skills and recent HyperAI operations that
 * match what follows the @. Typing stays in the composer, which moves the selection (Up, Down) and picks (Enter, Tab);
 * a click picks too. The footer says what the asset index holds and that it stays on this computer.
 */
class SHyperAIStudioMentionPicker : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioMentionPicker) {}
		SLATE_EVENT(FOnHyperAIStudioMentionChosen, OnChosen)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Shows Items, the first one selected. */
	void SetItems(const TArray<FHyperAIStudioMention>& Items);
	int32 Num() const { return Items.Num(); }
	/** Moves the selection by Delta, wrapping. */
	void MoveSelection(int32 Delta);
	/** Picks the selected item, as Enter does; false when the list is empty. */
	bool ChooseSelected();
	const FHyperAIStudioMention* GetSelected() const;

private:
	TSharedRef<ITableRow> MakeRow(TSharedPtr<FHyperAIStudioMention> Item, const TSharedRef<STableViewBase>& Owner);

	TArray<TSharedPtr<FHyperAIStudioMention>> Items;
	TSharedPtr<SListView<TSharedPtr<FHyperAIStudioMention>>> List;
	FOnHyperAIStudioMentionChosen OnChosen;
	int32 Selected = 0;
};
