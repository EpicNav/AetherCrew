// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAutoApproval.h"
#include "Widgets/SCompoundWidget.h"

class SButton;
class SWindow;

/**
 * The warning auto-approval turns on from, and the only way it does: what each category covers and risks, how long it
 * lasts, and what already waits. Cancel is the default; Turn On asks once more in a native box, which an MCP-only
 * client's synthetic Slate input cannot reach. (A program with OS-level input, such as an agent with a shell, could
 * answer it; its own client's permission prompts govern that.)
 */
class SHyperAIStudioAutoApprovalDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioAutoApprovalDialog) {}
		/** The modal window it lives in, closed once it turns on or is cancelled. */
		SLATE_ARGUMENT(TSharedPtr<SWindow>, Window)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	/** What its window focuses as it opens, so Enter presses it: Cancel. */
	TSharedPtr<SWidget> FocusOnOpen() const;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend struct FHyperAIStudioAutoApprovalTestAccess;
#endif
	static constexpr int32 CategoryCount = 5;

	TArray<EHyperAIStudioApprovalCategory> GetChosen() const;
	/** The requests waiting as it opened that Turn On approves too: those in a ticked category, while that box is ticked. */
	TSet<FString> GetChosenWaiting() const;
	/** Whether a request waiting as it opened is in a ticked category. */
	bool IsCoveredByTicks(const FHyperAIStudioPendingApproval& Pending) const;
	bool CanTurnOn() const;
	FReply OnTurnOnClicked();
	FReply OnCancelClicked();
	void Close();
	FText BuildConfirmation() const;
	TSharedRef<SWidget> BuildCategory(EHyperAIStudioApprovalCategory Category, const FText& Label, const FText& Body,
		const TSharedRef<SWidget>& Note);
	TSharedRef<SWidget> BuildDuration(EHyperAIStudioAutoApprovalDuration Choice, const FText& Label);
	/** What waits now, each with its category, and whether Turn On approves those in the ticked categories too. */
	TSharedRef<SWidget> BuildWaiting();
	/** MessageBoxW, Cancel by default; false wherever nobody can answer it. */
	static bool ShowNativeConfirmation(const TSharedPtr<SWindow>& Owner, const FText& Caption, const FText& Body);

	TWeakPtr<SWindow> Window;
	/** Every activation starts from these: Edits only. */
	bool bTicked[CategoryCount] = {true, false, false, false, false};
	/** "Also approve the requests waiting now", ticked by default: turning it on with requests waiting approves them. */
	bool bAlsoApproveWaiting = true;
	/** Sandbox Mode was on as it opened, so its held saves are offered. */
	bool bOffersSandbox = false;
	/** How many files Sandbox Mode held as it opened. */
	int32 SandboxHeldAtOpen = 0;
	EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour;
	TArray<FHyperAIStudioPendingApproval> PendingAtOpen;
	TSharedPtr<SButton> CancelButton;
	/** The second confirmation; only the tests' access replaces it. */
	TFunction<bool(const FText& Caption, const FText& Body)> ConfirmNatively;
	int32 NativeConfirmations = 0;
	bool bClosed = false;
#if WITH_DEV_AUTOMATION_TESTS
	/** Set once a test answers the confirmation: what its activation may cover. */
	TOptional<TSet<FString>> TestScope;
#endif
};
