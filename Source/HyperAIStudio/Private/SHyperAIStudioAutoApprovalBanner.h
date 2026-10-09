// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SButton;

/**
 * One line, shown only while auto-approval is on: what it covers, how long is left, and Stop. Activity and every chat
 * window carry one, and the Studio's status bar a short one, so it is never on out of sight.
 */
class SHyperAIStudioAutoApprovalBanner : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioAutoApprovalBanner) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Stop, with what it does: one tooltip for every Stop button. */
	static FText GetStopTooltip();

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend struct FHyperAIStudioAutoApprovalTestAccess;
#endif
	TSharedPtr<SButton> StopButton;
};
