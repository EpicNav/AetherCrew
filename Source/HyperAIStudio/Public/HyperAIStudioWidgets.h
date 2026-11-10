// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Framework/SlateDelegates.h"
#include "Input/Reply.h"
#include "Misc/Attribute.h"
#include "Layout/Margin.h"
#include "Styling/SlateColor.h"
#include "Widgets/SNullWidget.h"

class ISlateStyle;
class SWidget;
struct FSlateBrush;

/**
 * The pieces every HyperAI Studio page is built from, so pages from any pack look and read alike: a page header, cards,
 * status chips and dots, empty states, buttons and section labels. Colours come from the editor theme.
 */
namespace HyperAIStudio::UI
{
	enum class EStatus : uint8
	{
		Good,
		Warning,
		Error,
		Info,
		Busy,
		Neutral
	};

	/** The plugin's style set: the HyperAIStudio.* brushes and text styles, falling back to the editor's. */
	HYPERAISTUDIO_API const ISlateStyle& Style();
	HYPERAISTUDIO_API const FSlateBrush* Icon(FName Name);
	HYPERAISTUDIO_API FSlateColor StatusColor(EStatus Status);

	/** A page's title, one line on what it is for, and actions on the right. */
	HYPERAISTUDIO_API TSharedRef<SWidget> PageHeader(const FText& Title, const TAttribute<FText>& Description, const TSharedRef<SWidget>& Actions = SNullWidget::NullWidget);

	/** A rounded panel, with a title row when Title is set. */
	HYPERAISTUDIO_API TSharedRef<SWidget> Card(const TSharedRef<SWidget>& Content, const FText& Title = FText::GetEmpty(),
		const TSharedRef<SWidget>& Actions = SNullWidget::NullWidget, FMargin Padding = FMargin(14.0f, 12.0f));

	/** A state as a small coloured pill. */
	HYPERAISTUDIO_API TSharedRef<SWidget> Chip(const TAttribute<FText>& Text, const TAttribute<FSlateColor>& Color);

	/** A state as a coloured dot. */
	HYPERAISTUDIO_API TSharedRef<SWidget> Dot(const TAttribute<FSlateColor>& Color);

	/** A number in an accent pill, such as approvals waiting; hidden at 0. */
	HYPERAISTUDIO_API TSharedRef<SWidget> Count(const TAttribute<int32>& Value);

	/** What an empty view says: an icon, why it is empty, and the action that fills it. */
	HYPERAISTUDIO_API TSharedRef<SWidget> EmptyState(const FSlateBrush* Icon, const FText& Title, const FText& Body,
		const FText& ActionLabel = FText::GetEmpty(), FOnClicked OnAction = FOnClicked());

	/** A button with an optional icon; a primary one is the page's main action, drawn in the accent colour. */
	HYPERAISTUDIO_API TSharedRef<SWidget> Button(const FText& Label, FOnClicked OnClicked, const FSlateBrush* Icon = nullptr,
		bool bPrimary = false, const FText& ToolTip = FText::GetEmpty());

	/** An icon-only button, for toolbars. */
	HYPERAISTUDIO_API TSharedRef<SWidget> IconButton(const FSlateBrush* Icon, const FText& ToolTip, FOnClicked OnClicked);

	/** A small upper-case label over a group, like the editor's section headings. */
	HYPERAISTUDIO_API TSharedRef<SWidget> SectionLabel(const FText& Text);

	/** A label and a value on one row, for a card's facts. */
	HYPERAISTUDIO_API TSharedRef<SWidget> Fact(const FText& Label, const TAttribute<FText>& Value);
}
