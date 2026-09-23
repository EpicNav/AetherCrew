// Games by Hyper 2026.

#include "SHyperAIStudioNativeChatView.h"

#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioNativeChatView"

namespace HyperAIStudio::NativeChatView
{
	using EKind = FHyperAIStudioChatItem::EKind;

	FSlateFontInfo Mono()
	{
		return FCoreStyle::GetDefaultFontStyle("Mono", 9);
	}

	/** Editable text takes its colour from a style it keeps a pointer to, so the four variants live for good. */
	const FTextBlockStyle& LiveStyle(const bool bMono, const bool bMuted)
	{
		static FTextBlockStyle Styles[4];
		static bool bMade = false;
		if (!bMade)
		{
			bMade = true;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Styles[Index] = FAppStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");
				Styles[Index].SetFont((Index & 1) ? Mono() : FCoreStyle::GetDefaultFontStyle("Regular", 10));
				Styles[Index].SetColorAndOpacity((Index & 2) ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground());
			}
		}
		return Styles[(bMono ? 1 : 0) + (bMuted ? 2 : 0)];
	}

	/** Selectable, wrapping text that follows the item as it streams in. */
	TSharedRef<SWidget> LiveText(const TSharedPtr<FHyperAIStudioChatItem>& Item, const bool bMono = false, const bool bMuted = false)
	{
		return SNew(SMultiLineEditableText)
			.IsReadOnly(true)
			.AutoWrapText(true)
			.TextStyle(&LiveStyle(bMono, bMuted))
			.Text_Lambda([Item]() { return FText::FromString(Item->Text); });
	}

	FSlateColor StatusColor(const FString& Status)
	{
		if (Status == TEXT("completed"))
		{
			return FSlateColor(FStyleColors::AccentGreen);
		}
		if (Status == TEXT("failed") || Status == TEXT("declined") || Status.StartsWith(TEXT("exit code")))
		{
			return FSlateColor(FStyleColors::AccentRed);
		}
		return FSlateColor::UseSubduedForeground();
	}

	FText KindLabel(const EKind Kind)
	{
		switch (Kind)
		{
		case EKind::Command: return LOCTEXT("KindCommand", "Command");
		case EKind::FileChange: return LOCTEXT("KindFiles", "Files");
		case EKind::Reasoning: return LOCTEXT("KindThinking", "Thinking");
		default: return LOCTEXT("KindTool", "Tool");
		}
	}
}

void SHyperAIStudioNativeChatView::Construct(const FArguments& InArgs)
{
	Chat = InArgs._Chat;
	check(Chat.IsValid());
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					if (!Chat->IsRunning())
					{
						return Chat->IsReady() ? LOCTEXT("StateReady", "Ready") : LOCTEXT("StateStopped", "Stopped");
					}
					if (!Chat->IsReady())
					{
						return LOCTEXT("StateStarting", "Starting...");
					}
					if (Chat->IsBusy())
					{
						return Chat->NumQueued() > 0
							? FText::Format(LOCTEXT("StateWorkingQueued", "Working... ({0} queued)"), Chat->NumQueued())
							: LOCTEXT("StateWorking", "Working...");
					}
					return LOCTEXT("StateReady", "Ready");
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]() { return FText::FromString(Chat->GetUsage()); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Stop", "Stop"))
				.ToolTipText(LOCTEXT("StopTooltip", "Stop the agent's current turn and drop queued messages."))
				.Visibility_Lambda([this]() { return Chat->IsBusy() ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked_Lambda([this]()
				{
					Chat->Interrupt();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(PlanBox, SVerticalBox)
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(List, SListView<FItemPtr>)
			.ListItemsSource(&Rows)
			.SelectionMode(ESelectionMode::None)
			.OnGenerateRow(this, &SHyperAIStudioNativeChatView::GenerateRow)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SBox)
				.MaxDesiredHeight(160.0f)
				[
					SAssignNew(Composer, SMultiLineEditableTextBox)
					.AutoWrapText(true)
					.HintText(FText::Format(LOCTEXT("ComposerHint", "Message {0}. Enter sends; Shift+Enter starts a new line."), FText::FromString(Chat->GetAgentName())))
					.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent)
					{
						if (KeyEvent.GetKey() == EKeys::Enter && !KeyEvent.IsShiftDown() && !KeyEvent.IsControlDown() && !KeyEvent.IsAltDown())
						{
							Submit();
							return FReply::Handled();
						}
						return FReply::Unhandled();
					})
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text_Lambda([this]() { return Chat->IsBusy() || !Chat->IsReady() ? LOCTEXT("Queue", "Queue") : LOCTEXT("Send", "Send"); })
				.ToolTipText(LOCTEXT("SendTooltip", "Send now, or after the running turn."))
				.OnClicked_Lambda([this]()
				{
					Submit();
					return FReply::Handled();
				})
			]
		]
	];
	ChangedHandle = Chat->OnChanged.AddSP(this, &SHyperAIStudioNativeChatView::Refresh);
	Refresh();
}

SHyperAIStudioNativeChatView::~SHyperAIStudioNativeChatView()
{
	if (Chat.IsValid())
	{
		Chat->OnChanged.Remove(ChangedHandle);
	}
}

void SHyperAIStudioNativeChatView::InsertText(const FString& Text)
{
	if (Composer.IsValid())
	{
		const FString Current = Composer->GetText().ToString();
		Composer->SetText(FText::FromString(Current.IsEmpty() ? Text : Current + TEXT(" ") + Text));
		FocusComposer();
	}
}

void SHyperAIStudioNativeChatView::FocusComposer()
{
	if (Composer.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(Composer, EFocusCause::SetDirectly);
	}
}

void SHyperAIStudioNativeChatView::Submit()
{
	const FString Text = Composer.IsValid() ? Composer->GetText().ToString().TrimStartAndEnd() : FString();
	if (Text.IsEmpty())
	{
		return;
	}
	Composer->SetText(FText::GetEmpty());
	Chat->Send(Text);
}

void SHyperAIStudioNativeChatView::Refresh()
{
	const int32 Previous = Rows.Num();
	Rows = Chat->GetItems();
	if (List.IsValid())
	{
		List->RequestListRefresh();
		if (Rows.Num() != Previous)
		{
			List->ScrollToBottom();
		}
	}
	// The plan is small and changes rarely; rebuilding it whole is simpler than diffing it.
	if (PlanBox.IsValid())
	{
		PlanBox->ClearChildren();
		const TArray<FHyperAIStudioChatPlanStep>& Plan = Chat->GetPlan();
		if (!Plan.IsEmpty())
		{
			TSharedRef<SVerticalBox> Steps = SNew(SVerticalBox);
			for (const FHyperAIStudioChatPlanStep& Step : Plan)
			{
				const bool bDone = Step.Status == TEXT("completed");
				const bool bNow = Step.Status == TEXT("inProgress");
				Steps->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%s %s"), bDone ? TEXT("[x]") : bNow ? TEXT("[>]") : TEXT("[ ]"), *Step.Text)))
					.ColorAndOpacity(bNow ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				];
			}
			PlanBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(FMargin(8.0f, 6.0f))
				[
					Steps
				]
			];
		}
	}
}

TSharedRef<ITableRow> SHyperAIStudioNativeChatView::GenerateRow(FItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	using namespace HyperAIStudio::NativeChatView;
	TSharedRef<SWidget> Content = SNullWidget::NullWidget;
	const FString Id = Item->Id;
	switch (Item->Kind)
	{
	case EKind::User:
		Content = SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("You", "You")).Font(FAppStyle::GetFontStyle("SmallFontBold"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					LiveText(Item)
				]
			];
		break;
	case EKind::Agent:
		Content = LiveText(Item);
		break;
	case EKind::Reasoning:
	case EKind::Tool:
	case EKind::Command:
	case EKind::FileChange:
	{
		const bool bReasoning = Item->Kind == EKind::Reasoning;
		Content = SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("ToggleDetails", "Show or hide the details."))
				.OnClicked_Lambda([this, Id]()
				{
					if (Open.Remove(Id) == 0)
					{
						Open.Add(Id);
					}
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this, Id]() { return FText::FromString(Open.Contains(Id) ? TEXT("v") : TEXT(">")); })
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(KindLabel(Item->Kind))
						.Font(FAppStyle::GetFontStyle("SmallFontBold"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(bReasoning ? FCoreStyle::GetDefaultFontStyle("Italic", 9) : Mono())
						.ColorAndOpacity(bReasoning ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground())
						.Text_Lambda([Item, bReasoning]()
						{
							const FString& Source = bReasoning ? Item->Text : Item->Title;
							int32 Newline = INDEX_NONE;
							return FText::FromString((Source.FindChar(TEXT('\n'), Newline) ? Source.Left(Newline) : Source).Left(160));
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("SmallFont"))
						.Text_Lambda([Item]() { return FText::FromString(Item->Status); })
						.ColorAndOpacity_Lambda([Item]() { return StatusColor(Item->Status); })
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(18.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.MaxDesiredHeight(260.0f)
				.Visibility_Lambda([this, Id]() { return Open.Contains(Id) ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						LiveText(Item, !bReasoning, bReasoning)
					]
				]
			];
		break;
	}
	case EKind::Approval:
	{
		TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
		for (int32 Index = 0; Index < Item->Choices.Num(); ++Index)
		{
			Buttons->AddSlot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton)
				.Text(FText::FromString(Item->Choices[Index]))
				.IsEnabled_Lambda([Item]() { return !Item->bAnswered; })
				.OnClicked_Lambda([this, Id, Index]()
				{
					Chat->Answer(Id, Index);
					return FReply::Handled();
				})
			];
		}
		Content = SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.BorderBackgroundColor(FSlateColor(FStyleColors::AccentYellow))
			.Padding(FMargin(10.0f, 8.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(FText::FromString(Item->Title)).Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
				[
					LiveText(Item, /*bMono=*/true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					Buttons
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Visibility_Lambda([Item]() { return Item->bAnswered ? EVisibility::Visible : EVisibility::Collapsed; })
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([Item]() { return FText::Format(LOCTEXT("Answered", "Answered: {0}"), FText::FromString(Item->Status)); })
				]
			];
		break;
	}
	case EKind::Error:
		Content = SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FStyleColors::AccentRed))
			.Text_Lambda([Item]() { return FText::FromString(Item->Text); });
		break;
	default:
		Content = SNew(STextBlock)
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([Item]() { return FText::FromString(Item->Text); });
		break;
	}
	return SNew(STableRow<FItemPtr>, OwnerTable)
		.ShowSelection(false)
		.Padding(FMargin(2.0f, 4.0f))
		[
			Content
		];
}

#undef LOCTEXT_NAMESPACE
