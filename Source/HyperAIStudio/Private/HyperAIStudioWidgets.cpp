// Games by Hyper 2026.

#include "HyperAIStudioWidgets.h"

#include "HyperAIStudioStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace HyperAIStudio::UI
{
	const ISlateStyle& Style()
	{
		return FHyperAIStudioStyle::Get();
	}

	const FSlateBrush* Icon(const FName Name)
	{
		return Style().GetBrush(Name);
	}

	FSlateColor StatusColor(const EStatus Status)
	{
		switch (Status)
		{
		case EStatus::Good:
			return FStyleColors::Success;
		case EStatus::Warning:
			return FStyleColors::Warning;
		case EStatus::Error:
			return FStyleColors::Error;
		case EStatus::Info:
		case EStatus::Busy:
			return FStyleColors::AccentBlue;
		default:
			// A theme colour, not the subdued foreground: chips tint their pill from it.
			return FStyleColors::Foreground;
		}
	}

	TSharedRef<SWidget> PageHeader(const FText& Title, const TAttribute<FText>& Description, const TSharedRef<SWidget>& Actions)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(Title)
					.TextStyle(&Style(), "HyperAIStudio.Text.PageTitle")
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(Description)
					.TextStyle(&Style(), "HyperAIStudio.Text.Subdued")
					.AutoWrapText(true)
					.Visibility_Lambda([Description]() { return Description.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(12.0f, 0.0f, 0.0f, 0.0f))
			[
				Actions
			];
	}

	TSharedRef<SWidget> Card(const TSharedRef<SWidget>& Content, const FText& Title, const TSharedRef<SWidget>& Actions, const FMargin Padding)
	{
		const bool bTitled = !Title.IsEmpty();
		return SNew(SBorder)
			.BorderImage(Style().GetBrush("HyperAIStudio.Card"))
			.Padding(Padding)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, bTitled ? 10.0f : 0.0f))
				[
					SNew(SHorizontalBox)
					.Visibility(bTitled ? EVisibility::Visible : EVisibility::Collapsed)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Title)
						.TextStyle(&Style(), "HyperAIStudio.Text.CardTitle")
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						Actions
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					Content
				]
			];
	}

	TSharedRef<SWidget> Chip(const TAttribute<FText>& Text, const TAttribute<FSlateColor>& Color)
	{
		return SNew(SBorder)
			.BorderImage(Style().GetBrush("HyperAIStudio.Chip"))
			.BorderBackgroundColor_Lambda([Color]() { return Color.Get().GetSpecifiedColor().CopyWithNewOpacity(0.18f); })
			.Padding(FMargin(8.0f, 1.0f))
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Text)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(Color)
			];
	}

	TSharedRef<SWidget> Dot(const TAttribute<FSlateColor>& Color)
	{
		return SNew(SImage)
			.Image(Style().GetBrush("HyperAIStudio.Dot"))
			.ColorAndOpacity(Color);
	}

	TSharedRef<SWidget> Count(const TAttribute<int32>& Value)
	{
		return SNew(SBorder)
			.BorderImage(Style().GetBrush("HyperAIStudio.Count"))
			.Padding(FMargin(6.0f, 0.0f))
			.Visibility_Lambda([Value]() { return Value.Get() > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(STextBlock)
				.Text_Lambda([Value]() { return FText::AsNumber(Value.Get()); })
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FStyleColors::ForegroundHover)
			];
	}

	TSharedRef<SWidget> EmptyState(const FSlateBrush* InIcon, const FText& Title, const FText& Body, const FText& ActionLabel, FOnClicked OnAction)
	{
		return SNew(SBox)
			.HAlign(HAlign_Center)
			.Padding(FMargin(24.0f, 36.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(SBox)
					.WidthOverride(32.0f)
					.HeightOverride(32.0f)
					.Visibility(InIcon ? EVisibility::Visible : EVisibility::Collapsed)
					[
						SNew(SImage)
						.Image(InIcon)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.0f, 12.0f, 0.0f, 4.0f))
				[
					SNew(STextBlock)
					.Text(Title)
					.TextStyle(&Style(), "HyperAIStudio.Text.CardTitle")
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(SBox)
					.MaxDesiredWidth(420.0f)
					[
						SNew(STextBlock)
						.Text(Body)
						.TextStyle(&Style(), "HyperAIStudio.Text.Subdued")
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.0f, 14.0f, 0.0f, 0.0f))
				[
					ActionLabel.IsEmpty() ? SNullWidget::NullWidget : Button(ActionLabel, OnAction, nullptr, /*bPrimary=*/true)
				]
			];
	}

	TSharedRef<SWidget> Button(const FText& Label, FOnClicked OnClicked, const FSlateBrush* InIcon, const bool bPrimary, const FText& ToolTip)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), bPrimary ? "PrimaryButton" : "Button")
			.ToolTipText(ToolTip)
			.OnClicked(OnClicked)
			.ContentPadding(FMargin(10.0f, 3.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, InIcon ? 6.0f : 0.0f, 0.0f))
				[
					SNew(SImage)
					.Image(InIcon)
					.Visibility(InIcon ? EVisibility::Visible : EVisibility::Collapsed)
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Label)
					.TextStyle(FAppStyle::Get(), "ButtonText")
				]
			];
	}

	TSharedRef<SWidget> IconButton(const FSlateBrush* InIcon, const FText& ToolTip, FOnClicked OnClicked)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(ToolTip)
			.OnClicked(OnClicked)
			.ContentPadding(FMargin(5.0f))
			[
				SNew(SImage)
				.Image(InIcon)
				.ColorAndOpacity(FSlateColor::UseForeground())
			];
	}

	TSharedRef<SWidget> SectionLabel(const FText& Text)
	{
		return SNew(STextBlock)
			.Text(Text.ToUpper())
			.TextStyle(&Style(), "HyperAIStudio.Text.Section");
	}

	TSharedRef<SWidget> Fact(const FText& Label, const TAttribute<FText>& Value)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SBox)
				.MinDesiredWidth(92.0f)
				[
					SNew(STextBlock)
					.Text(Label)
					.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(Value)
				.TextStyle(&Style(), "HyperAIStudio.Text.Body")
				.AutoWrapText(true)
			];
	}
}
