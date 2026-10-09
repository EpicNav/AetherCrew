// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * One line, shown only while auto-approval is on: what it covers, how long is left, and Stop. Activity and every chat
 * window carry one, so it is never on out of sight.
 */
class SHyperAIStudioAutoApprovalBanner : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioAutoApprovalBanner) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
};
