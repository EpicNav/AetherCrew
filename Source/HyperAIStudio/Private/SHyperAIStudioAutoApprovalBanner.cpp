// Games by Hyper 2026.

#include "SHyperAIStudioAutoApprovalBanner.h"

#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioAutoApproval"

void SHyperAIStudioAutoApprovalBanner::Construct(const FArguments& InArgs)
{
	SetVisibility(TAttribute<EVisibility>::CreateLambda([]()
	{
		return FHyperAIStudioAutoApproval::IsActive() ? EVisibility::Visible : EVisibility::Collapsed;
	}));
	ChildSlot
	[
		SNew(SBorder)
		// A white rounded brush tinted orange, so it stands out from every panel it sits on.
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.TerminalFrame"))
		.BorderBackgroundColor_Lambda([]() { return FSlateColor(FStyleColors::AccentOrange.GetSpecifiedColor().CopyWithNewOpacity(0.3f)); })
		.Padding(FMargin(8.0f, 4.0f))
		.ToolTipText_Lambda([]() { return FHyperAIStudioAutoApproval::DescribeBannerTooltip(); })
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.WarningWithColor")))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([]() { return FHyperAIStudioAutoApproval::DescribeBanner(); })
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SAssignNew(StopButton, SButton)
				.Text(LOCTEXT("Stop", "Stop"))
				.ToolTipText(GetStopTooltip())
				.OnClicked_Lambda([]()
				{
					FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::Stopped);
					return FReply::Handled();
				})
			]
		]
	];
}

FText SHyperAIStudioAutoApprovalBanner::GetStopTooltip()
{
	return LOCTEXT("StopUnstartedTooltip",
		"Turn auto-approval off now. Requests wait for your approval again, and so does what it approved that has not started yet; work already running goes on.");
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioAutoApprovalTestAccess::PressStop(SHyperAIStudioAutoApprovalBanner& Banner)
{
	if (Banner.StopButton.IsValid())
	{
		Banner.StopButton->SimulateClick();
	}
}
#endif

#undef LOCTEXT_NAMESPACE
