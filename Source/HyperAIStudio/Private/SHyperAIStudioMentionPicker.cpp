// Games by Hyper 2026.

#include "SHyperAIStudioMentionPicker.h"

#include "HyperAIStudioStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioMentionPicker"

namespace HyperAIStudio::MentionPicker
{
	const FSlateBrush* KindIcon(const EHyperAIStudioMentionKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioMentionKind::Asset: return FAppStyle::GetBrush("Icons.Package");
		case EHyperAIStudioMentionKind::Actor: return FAppStyle::GetBrush("Icons.Level");
		case EHyperAIStudioMentionKind::File: return FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Icon.File");
		case EHyperAIStudioMentionKind::Skill: return FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Icon.Skills");
		default: return FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Icon.Activity");
		}
	}

	FText KindLabel(const EHyperAIStudioMentionKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioMentionKind::Asset: return LOCTEXT("KindAsset", "Asset");
		case EHyperAIStudioMentionKind::Actor: return LOCTEXT("KindActor", "Actor");
		case EHyperAIStudioMentionKind::File: return LOCTEXT("KindFile", "File");
		case EHyperAIStudioMentionKind::Skill: return LOCTEXT("KindSkill", "Skill");
		default: return LOCTEXT("KindOperation", "Operation");
		}
	}
}

void SHyperAIStudioMentionPicker::Construct(const FArguments& InArgs)
{
	OnChosen = InArgs._OnChosen;
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Menu.Background"))
		.Padding(4.0f)
		[
			SNew(SBox)
			.WidthOverride(560.0f)
			.MaxDesiredHeight(340.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SAssignNew(List, SListView<TSharedPtr<FHyperAIStudioMention>>)
					.ListItemsSource(&Items)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SHyperAIStudioMentionPicker::MakeRow)
					.OnMouseButtonClick_Lambda([this](const TSharedPtr<FHyperAIStudioMention> Item)
					{
						if (Item.IsValid())
						{
							OnChosen.ExecuteIfBound(*Item);
						}
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]() { return Items.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text(LOCTEXT("NothingFound", "Nothing matches. Keep typing, or press Esc."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(2.0f, 4.0f, 2.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
					.Text_Lambda([]()
					{
						return FText::Format(LOCTEXT("PickerFooter", "Up and Down move, Enter or Tab inserts, Esc closes. {0}"),
							FText::FromString(HyperAIStudio::Mentions::DescribeIndex()));
					})
				]
			]
		]
	];
}

TSharedRef<ITableRow> SHyperAIStudioMentionPicker::MakeRow(TSharedPtr<FHyperAIStudioMention> Item, const TSharedRef<STableViewBase>& Owner)
{
	using namespace HyperAIStudio::MentionPicker;
	return SNew(STableRow<TSharedPtr<FHyperAIStudioMention>>, Owner)
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SImage)
				.Image(KindIcon(Item->Kind))
				.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
				.ToolTipText(KindLabel(Item->Kind))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(FText::FromString(Item->Label))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Detail))
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.ToolTipText(FText::FromString(Item->Reference))
			]
		];
}

void SHyperAIStudioMentionPicker::SetItems(const TArray<FHyperAIStudioMention>& InItems)
{
	Items.Reset();
	for (const FHyperAIStudioMention& Item : InItems)
	{
		Items.Add(MakeShared<FHyperAIStudioMention>(Item));
	}
	Selected = 0;
	if (List.IsValid())
	{
		List->RequestListRefresh();
		if (!Items.IsEmpty())
		{
			List->SetSelection(Items[0], ESelectInfo::Direct);
			List->RequestScrollIntoView(Items[0]);
		}
	}
}

void SHyperAIStudioMentionPicker::MoveSelection(const int32 Delta)
{
	if (Items.IsEmpty())
	{
		return;
	}
	Selected = (Selected + Delta % Items.Num() + Items.Num()) % Items.Num();
	if (List.IsValid())
	{
		List->SetSelection(Items[Selected], ESelectInfo::Direct);
		List->RequestScrollIntoView(Items[Selected]);
	}
}

const FHyperAIStudioMention* SHyperAIStudioMentionPicker::GetSelected() const
{
	return Items.IsValidIndex(Selected) ? Items[Selected].Get() : nullptr;
}

bool SHyperAIStudioMentionPicker::ChooseSelected()
{
	if (const FHyperAIStudioMention* Item = GetSelected())
	{
		const FHyperAIStudioMention Chosen = *Item;
		OnChosen.ExecuteIfBound(Chosen);
		return true;
	}
	return false;
}

#undef LOCTEXT_NAMESPACE
