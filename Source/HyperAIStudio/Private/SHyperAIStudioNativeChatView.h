// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioNativeChat.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SMultiLineEditableTextBox;

/** The chat tab's native view of a Codex or Gemini chat: transcript, plan, approvals and a composer. */
class SHyperAIStudioNativeChatView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioNativeChatView) {}
		SLATE_ARGUMENT(TSharedPtr<FHyperAIStudioNativeChat>, Chat)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioNativeChatView() override;

	TSharedPtr<FHyperAIStudioNativeChat> GetChat() const { return Chat; }
	/** Puts text into the composer for the user to finish; never sends it. */
	void InsertText(const FString& Text);
	void FocusComposer();

private:
	using FItemPtr = TSharedPtr<FHyperAIStudioChatItem>;

	void Refresh();
	void Submit();
	TSharedRef<ITableRow> GenerateRow(FItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<SWidget> BuildPlan();

	TSharedPtr<FHyperAIStudioNativeChat> Chat;
	TArray<FItemPtr> Rows;
	TSharedPtr<SListView<FItemPtr>> List;
	TSharedPtr<SMultiLineEditableTextBox> Composer;
	TSharedPtr<class SVerticalBox> PlanBox;
	/** Items whose details are open. */
	TSet<FString> Open;
	FDelegateHandle ChangedHandle;
	int32 PlanRevision = 0;
};
