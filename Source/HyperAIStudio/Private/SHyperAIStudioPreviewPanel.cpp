// Games by Hyper 2026.

#include "SHyperAIStudioPreviewPanel.h"

#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioShell.h"
#include "IDesktopPlatform.h"
#include "Input/DragAndDrop.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioPreviewPanel"

namespace HyperAIStudio::Preview::Private
{
	const FLinearColor PaneBackground(0.045f, 0.045f, 0.05f, 1.0f);
	const FLinearColor DividerColor(1.0f, 1.0f, 1.0f, 0.85f);
	const FLinearColor LabelBackground(0.0f, 0.0f, 0.0f, 0.55f);
	const FLinearColor HistogramA(0.35f, 0.62f, 1.0f, 0.55f);
	const FLinearColor HistogramB(1.0f, 0.70f, 0.25f, 0.55f);
	constexpr float DividerGrabPixels = 6.0f;

	const FSlateBrush* WhiteBrush()
	{
		return FCoreStyle::Get().GetBrush("GenericWhiteBox");
	}

	/** Relight this view, which the image pack registers when it loads: a reference made from the level itself. */
	TOptional<FHyperAIStudioAction> RelightAction()
	{
		for (const FHyperAIStudioAction& Action : FHyperAIStudioShell::GetActions())
		{
			if (Action.Id == FName(TEXT("RelightView")))
			{
				return Action;
			}
		}
		return {};
	}

	FVector2f ImageSizeOf(const FSlateBrush* Brush)
	{
		const FVector2f Size = Brush ? FVector2f(Brush->ImageSize.X, Brush->ImageSize.Y) : FVector2f::ZeroVector;
		return FVector2f(FMath::Max(Size.X, 1.0f), FMath::Max(Size.Y, 1.0f));
	}

	/** Where an image lands inside a pane: fitted to it, then zoomed and panned. */
	void ComputePlacement(const FVector2f& Image, const FVector2f& PaneOffset, const FVector2f& PaneSize,
		const float Zoom, const FVector2f& Pan, FVector2f& OutOffset, FVector2f& OutSize)
	{
		const float Fit = FMath::Min(PaneSize.X / Image.X, PaneSize.Y / Image.Y);
		OutSize = Image * Fit * Zoom;
		OutOffset = PaneOffset + (PaneSize - OutSize) * 0.5f + Pan * PaneSize;
	}

	void DrawImage(FSlateWindowElementList& Out, const int32 Layer, const FGeometry& Geometry, const FSlateBrush* Brush,
		const FVector2f& PaneOffset, const FVector2f& PaneSize, const float Zoom, const FVector2f& Pan)
	{
		if (!Brush)
		{
			return;
		}
		FVector2f Offset = FVector2f::ZeroVector;
		FVector2f Size = FVector2f::ZeroVector;
		ComputePlacement(ImageSizeOf(Brush), PaneOffset, PaneSize, Zoom, Pan, Offset, Size);
		FSlateDrawElement::MakeBox(Out, Layer, Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(Offset)), Brush);
	}

	void DrawLabel(FSlateWindowElementList& Out, const int32 Layer, const FGeometry& Geometry, const FText& Text,
		const FVector2f& At)
	{
		if (Text.IsEmpty())
		{
			return;
		}
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 9);
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		const FVector2f Extent(Measure->Measure(Text, Font));
		FSlateDrawElement::MakeBox(Out, Layer,
			Geometry.ToPaintGeometry(Extent + FVector2f(10.0f, 4.0f), FSlateLayoutTransform(At - FVector2f(5.0f, 2.0f))),
			WhiteBrush(), ESlateDrawEffect::None, LabelBackground);
		FSlateDrawElement::MakeText(Out, Layer + 1,
			Geometry.ToPaintGeometry(Extent, FSlateLayoutTransform(At)), Text, Font);
	}

	FText FormatMatched(const FHyperAIStudioPreviewComparison& Comparison)
	{
		for (const FHyperAIStudioPreviewStat& Stat : Comparison.Stats)
		{
			if (Stat.Tone != EHyperAIStudioPreviewTone::Neutral)
			{
				return FText::Format(LOCTEXT("StatSummary", "{0} {1}"), Stat.Label, Stat.Value);
			}
		}
		return FText::GetEmpty();
	}
}

void SHyperAIStudioComparisonView::Construct(const FArguments& InArgs)
{
	BrushA = InArgs._BrushA;
	BrushB = InArgs._BrushB;
	LabelA = InArgs._LabelA;
	LabelB = InArgs._LabelB;
	Mode = InArgs._Mode;
	EmptyMessage = InArgs._EmptyMessage;
}

FVector2D SHyperAIStudioComparisonView::ComputeDesiredSize(float) const
{
	return FVector2D(640.0, 320.0);
}

void SHyperAIStudioComparisonView::ResetView()
{
	Zoom = 1.0f;
	Pan = FVector2f::ZeroVector;
	Divider = 0.5f;
}

bool SHyperAIStudioComparisonView::IsOverDivider(const FGeometry& Geometry, const FPointerEvent& Event) const
{
	using namespace HyperAIStudio::Preview::Private;
	if (Mode.Get(EHyperAIStudioCompareMode::SideBySide) != EHyperAIStudioCompareMode::Wipe)
	{
		return false;
	}
	const FVector2f Local = FVector2f(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()));
	return FMath::Abs(Local.X - Geometry.GetLocalSize().X * Divider) <= DividerGrabPixels;
}

int32 SHyperAIStudioComparisonView::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace HyperAIStudio::Preview::Private;
	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), WhiteBrush(),
		ESlateDrawEffect::None, PaneBackground);
	++LayerId;

	const FSlateBrush* A = BrushA.Get(nullptr);
	const FSlateBrush* B = BrushB.Get(nullptr);
	if (!A && !B)
	{
		const FText Message = EmptyMessage.Get(FText::GetEmpty());
		if (!Message.IsEmpty())
		{
			const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Regular", 10);
			const FVector2f Extent(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Message, Font));
			FSlateDrawElement::MakeText(OutDrawElements, LayerId,
				AllottedGeometry.ToPaintGeometry(Extent, FSlateLayoutTransform((Size - Extent) * 0.5f)), Message, Font,
				ESlateDrawEffect::None, FLinearColor(0.6f, 0.6f, 0.62f));
		}
		return LayerId;
	}

	if (Mode.Get(EHyperAIStudioCompareMode::SideBySide) == EHyperAIStudioCompareMode::SideBySide)
	{
		const FVector2f PaneSize(Size.X * 0.5f - 2.0f, Size.Y);
		DrawImage(OutDrawElements, LayerId, AllottedGeometry, A, FVector2f::ZeroVector, PaneSize, Zoom, Pan);
		DrawImage(OutDrawElements, LayerId, AllottedGeometry, B, FVector2f(Size.X * 0.5f + 2.0f, 0.0f), PaneSize, Zoom, Pan);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(FVector2f(2.0f, Size.Y), FSlateLayoutTransform(FVector2f(Size.X * 0.5f - 1.0f, 0.0f))),
			WhiteBrush(), ESlateDrawEffect::None, FLinearColor(0.0f, 0.0f, 0.0f, 0.6f));
		DrawLabel(OutDrawElements, LayerId + 2, AllottedGeometry, LabelA.Get(FText::GetEmpty()), FVector2f(10.0f, Size.Y - 22.0f));
		DrawLabel(OutDrawElements, LayerId + 2, AllottedGeometry, LabelB.Get(FText::GetEmpty()), FVector2f(Size.X * 0.5f + 10.0f, Size.Y - 22.0f));
		return LayerId + 4;
	}

	// Wipe: both images fill the widget, each clipped to its side of the divider.
	const FSlateRect Bounds = AllottedGeometry.GetLayoutBoundingRect();
	const float SplitX = Bounds.Left + Bounds.GetSize().X * Divider;
	OutDrawElements.PushClip(FSlateClippingZone(FSlateRect(Bounds.Left, Bounds.Top, SplitX, Bounds.Bottom)));
	DrawImage(OutDrawElements, LayerId, AllottedGeometry, A, FVector2f::ZeroVector, Size, Zoom, Pan);
	DrawLabel(OutDrawElements, LayerId + 1, AllottedGeometry, LabelA.Get(FText::GetEmpty()), FVector2f(10.0f, Size.Y - 22.0f));
	OutDrawElements.PopClip();

	OutDrawElements.PushClip(FSlateClippingZone(FSlateRect(SplitX, Bounds.Top, Bounds.Right, Bounds.Bottom)));
	DrawImage(OutDrawElements, LayerId, AllottedGeometry, B, FVector2f::ZeroVector, Size, Zoom, Pan);
	DrawLabel(OutDrawElements, LayerId + 1, AllottedGeometry, LabelB.Get(FText::GetEmpty()),
		FVector2f(FMath::Min(Size.X * Divider + 10.0f, Size.X - 120.0f), Size.Y - 22.0f));
	OutDrawElements.PopClip();

	FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 2,
		AllottedGeometry.ToPaintGeometry(FVector2f(2.0f, Size.Y), FSlateLayoutTransform(FVector2f(Size.X * Divider - 1.0f, 0.0f))),
		WhiteBrush(), ESlateDrawEffect::None, DividerColor);
	return LayerId + 3;
}

FReply SHyperAIStudioComparisonView::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (Event.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	DragStart = FVector2f(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()));
	if (IsOverDivider(Geometry, Event))
	{
		bDraggingDivider = true;
	}
	else
	{
		bPanning = true;
		PanAtDragStart = Pan;
	}
	return FReply::Handled().CaptureMouse(SharedThis(this));
}

FReply SHyperAIStudioComparisonView::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (Event.GetEffectingButton() != EKeys::LeftMouseButton || (!bPanning && !bDraggingDivider))
	{
		return FReply::Unhandled();
	}
	bPanning = false;
	bDraggingDivider = false;
	return FReply::Handled().ReleaseMouseCapture();
}

FReply SHyperAIStudioComparisonView::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
	if (!bPanning && !bDraggingDivider)
	{
		return FReply::Unhandled();
	}
	const FVector2f Local = FVector2f(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()));
	const FVector2f Size = FVector2f(Geometry.GetLocalSize());
	if (bDraggingDivider)
	{
		Divider = FMath::Clamp(Local.X / FMath::Max(Size.X, 1.0f), 0.02f, 0.98f);
	}
	else
	{
		Pan = PanAtDragStart + (Local - DragStart) / FVector2f(FMath::Max(Size.X, 1.0f), FMath::Max(Size.Y, 1.0f));
	}
	return FReply::Handled();
}

FReply SHyperAIStudioComparisonView::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event)
{
	Zoom = FMath::Clamp(Zoom * (1.0f + Event.GetWheelDelta() * 0.1f), 0.25f, 8.0f);
	return FReply::Handled();
}

FReply SHyperAIStudioComparisonView::OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& Event)
{
	ResetView();
	return FReply::Handled();
}

FCursorReply SHyperAIStudioComparisonView::OnCursorQuery(const FGeometry& Geometry, const FPointerEvent& Event) const
{
	if (bPanning)
	{
		return FCursorReply::Cursor(EMouseCursor::GrabHandClosed);
	}
	return IsOverDivider(Geometry, Event)
		? FCursorReply::Cursor(EMouseCursor::ResizeLeftRight)
		: FCursorReply::Cursor(EMouseCursor::GrabHand);
}

void SHyperAIStudioHistogramView::Construct(const FArguments& InArgs)
{
	HistogramA = InArgs._HistogramA;
	HistogramB = InArgs._HistogramB;
}

FVector2D SHyperAIStudioHistogramView::ComputeDesiredSize(float) const
{
	return FVector2D(220.0, 64.0);
}

int32 SHyperAIStudioHistogramView::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace HyperAIStudio::Preview::Private;
	const TArray<float> A = HistogramA.Get(TArray<float>());
	const TArray<float> B = HistogramB.Get(TArray<float>());
	const int32 Bins = FMath::Max(A.Num(), B.Num());
	if (Bins <= 0)
	{
		return LayerId;
	}
	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), WhiteBrush(),
		ESlateDrawEffect::None, FLinearColor(0.08f, 0.08f, 0.09f, 1.0f));
	float Peak = KINDA_SMALL_NUMBER;
	for (const float Value : A) Peak = FMath::Max(Peak, Value);
	for (const float Value : B) Peak = FMath::Max(Peak, Value);

	const float BinWidth = Size.X / static_cast<float>(Bins);
	auto DrawBar = [&](const int32 Index, const float Value, const FLinearColor& Color, const int32 Layer)
	{
		const float Height = FMath::Clamp(Value / Peak, 0.0f, 1.0f) * (Size.Y - 2.0f);
		if (Height <= 0.0f)
		{
			return;
		}
		FSlateDrawElement::MakeBox(OutDrawElements, Layer,
			AllottedGeometry.ToPaintGeometry(FVector2f(FMath::Max(BinWidth - 1.0f, 1.0f), Height),
				FSlateLayoutTransform(FVector2f(Index * BinWidth, Size.Y - Height - 1.0f))),
			WhiteBrush(), ESlateDrawEffect::None, Color);
	};
	// The shorter bar goes on top, so neither series hides behind the other.
	for (int32 Index = 0; Index < Bins; ++Index)
	{
		const float ValueA = A.IsValidIndex(Index) ? A[Index] : 0.0f;
		const float ValueB = B.IsValidIndex(Index) ? B[Index] : 0.0f;
		const bool bTallerIsA = ValueA >= ValueB;
		const FLinearColor& ColorA = HyperAIStudio::Preview::Private::HistogramA;
		const FLinearColor& ColorB = HyperAIStudio::Preview::Private::HistogramB;
		DrawBar(Index, bTallerIsA ? ValueA : ValueB, bTallerIsA ? ColorA : ColorB, LayerId + 1);
		DrawBar(Index, bTallerIsA ? ValueB : ValueA, bTallerIsA ? ColorB : ColorA, LayerId + 2);
	}
	return LayerId + 3;
}

void SHyperAIStudioPreviewPanel::Construct(const FArguments& InArgs)
{
	RebuildSourceOptions();
	if (const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>())
	{
		if (!Settings->RecentPreviewReferences.IsEmpty())
		{
			ReferencePath = Settings->RecentPreviewReferences[0];
		}
	}

	SourcesChangedHandle = FHyperAIStudioPreviewPanel::OnSourcesChanged().AddLambda([this]()
	{
		RebuildSourceOptions();
		Refresh();
	});
	OpenRequestedHandle = FHyperAIStudioPreviewPanel::OnOpenRequested().AddRaw(this, &SHyperAIStudioPreviewPanel::HandleOpenRequested);
	// A job's round ending is when its best attempt changes.
	JobsChangedHandle = FHyperAIStudioAgentJobs::OnChanged().AddLambda([this]() { Refresh(); });

	ChildSlot
	[
		SNew(SVerticalBox)

		// Auto exposure silently cancels the change you are trying to see, so it gets a bar, not a footnote.
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 4.0f, 4.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(FLinearColor(0.75f, 0.45f, 0.1f, 1.0f))
			.Visibility_Lambda([this]() { return Comparison.Warning.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(6.0f, 4.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this]() { return Comparison.Warning; })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f)
				[
					SNew(SButton)
					.Text_Lambda([this]() { return Comparison.WarningActionLabel; })
					.Visibility_Lambda([this]()
					{
						return Comparison.WarningAction.IsBound() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked(this, &SHyperAIStudioPreviewPanel::OnWarningActionClicked)
				]
			]
		]

		// The reference never collapses out of sight: knowing which one is loaded is the point.
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 4.0f, 4.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Visibility_Lambda([this]()
			{
				const FHyperAIStudioPreviewPanel::FSource* Source = CurrentSource();
				return Source && Source->bNeedsReference ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f)
					[
						SNew(SBox).WidthOverride(56.0f).HeightOverride(36.0f)
						[
							SNew(SImage).Image_Lambda([this]() { return Thumbnail.Get(ReferencePath); })
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(6.0f, 2.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
							.Text_Lambda([this]()
							{
								return ReferencePath.IsEmpty()
									? LOCTEXT("NoReference", "No reference image")
									: FText::FromString(FPaths::GetCleanFilename(ReferencePath));
							})
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
							.ColorAndOpacity_Lambda([this]()
							{
								return bStatusIsError ? FSlateColor(FLinearColor(1.0f, 0.5f, 0.4f)) : FSlateColor::UseSubduedForeground();
							})
							.Text_Lambda([this]()
							{
								if (!Status.IsEmpty()) return FText::FromString(Status);
								return HyperAIStudio::Preview::Private::FormatMatched(Comparison);
							})
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f)
					[
						SNew(SComboButton)
						.ToolTipText(LOCTEXT("RecentsTip", "Reference images you used before."))
						.OnGetMenuContent(this, &SHyperAIStudioPreviewPanel::BuildRecentsMenu)
						.ButtonContent()[SNew(STextBlock).Text(LOCTEXT("Recents", "Recent"))]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Browse", "Browse..."))
						.ToolTipText(LOCTEXT("BrowseTip", "Pick a PNG, JPG, BMP or TGA from inside the project."))
						.OnClicked(this, &SHyperAIStudioPreviewPanel::OnBrowseClicked)
					]
					// No reference at all: one made from the level itself, relit as the user describes.
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("RelightView", "Relight this view..."))
						.ToolTipText(LOCTEXT("RelightViewTip", "No reference? Make one from the level itself. The Images page takes the view through HyperAI_MatchCamera, an image model relights it as you describe, and your preferred agent then matches the level's lighting to that picture."))
						.Visibility_Lambda([]() { return HyperAIStudio::Preview::Private::RelightAction().IsSet() ? EVisibility::Visible : EVisibility::Collapsed; })
						.OnClicked_Lambda([]()
						{
							if (const TOptional<FHyperAIStudioAction> Action = HyperAIStudio::Preview::Private::RelightAction(); Action.IsSet() && Action->Run)
							{
								Action->Run();
							}
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f)
					[
						SNew(SButton)
						.ToolTipText(LOCTEXT("ExpandTip", "Show the full path and where else a reference can come from."))
						.OnClicked_Lambda([this]()
						{
							bReferenceExpanded = !bReferenceExpanded;
							return FReply::Handled();
						})
						.Content()[SNew(STextBlock).Text_Lambda([this]()
						{
							return bReferenceExpanded ? LOCTEXT("Less", "▲") : LOCTEXT("More", "▼");
						})]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 2.0f)
				[
					SNew(SVerticalBox)
					.Visibility_Lambda([this]() { return bReferenceExpanded ? EVisibility::Visible : EVisibility::Collapsed; })
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text_Lambda([this]() { return FText::FromString(ReferencePath); })
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity_Lambda([this]()
						{
							return bDropAccepted ? FSlateColor(FLinearColor(0.4f, 0.9f, 0.5f)) : FSlateColor::UseSubduedForeground();
						})
						.Text(LOCTEXT("DropHint", "Or drop an image from the project folder anywhere on this panel."))
					]
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SComboButton)
				.OnGetMenuContent(this, &SHyperAIStudioPreviewPanel::BuildSourceMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						const FHyperAIStudioPreviewPanel::FSource* Source = CurrentSource();
						return Source ? Source->Label : LOCTEXT("NoSource", "No preview source");
					})
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "Refresh"))
				.ToolTipText(LOCTEXT("RefreshTip", "Capture and measure again."))
				.OnClicked(this, &SHyperAIStudioPreviewPanel::OnRefreshClicked)
			]
			// The numbers say how far off it is; Match it hands the reference to an agent that closes the gap.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SComboButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.ToolTipText(LOCTEXT("MatchItTip", "Hand the reference to your preferred agent. It changes the level until the match score reaches the target, and HyperAI sends it back to work after each round until it does."))
				.Visibility_Lambda([this]()
				{
					const FHyperAIStudioPreviewPanel::FSource* Source = CurrentSource();
					return Source && !Source->AgentActions.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.IsEnabled_Lambda([this]()
				{
					FString Why;
					const TOptional<FHyperAIStudioAgentJobInfo> Job = CurrentJob();
					return FHyperAIStudioPreviewPanel::IsSupportedProjectImage(ReferencePath, Why) && !(Job.IsSet() && Job->IsRunning());
				})
				.OnGetMenuContent(this, &SHyperAIStudioPreviewPanel::BuildMatchItMenu)
				.ButtonContent()
				[
					SNew(STextBlock).Text(LOCTEXT("MatchIt", "Match it"))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SSegmentedControl<EHyperAIStudioCompareMode>)
				.Value_Lambda([this]() { return Mode; })
				.OnValueChanged_Lambda([this](const EHyperAIStudioCompareMode In) { Mode = In; })
				+ SSegmentedControl<EHyperAIStudioCompareMode>::Slot(EHyperAIStudioCompareMode::SideBySide)
					.Text(LOCTEXT("SideBySide", "Side by side"))
				+ SSegmentedControl<EHyperAIStudioCompareMode>::Slot(EHyperAIStudioCompareMode::Wipe)
					.Text(LOCTEXT("Wipe", "Wipe"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)[SNullWidget::NullWidget]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("ResetView", "Fit"))
				.ToolTipText(LOCTEXT("ResetViewTip", "Back to fitting both images. Drag to pan, wheel to zoom, double-click to fit."))
				.OnClicked_Lambda([this]()
				{
					if (ComparisonView.IsValid()) ComparisonView->ResetView();
					return FReply::Handled();
				})
			]
		]

		// The job working toward this reference: who has it, how it stands, and a way to it or to stop it.
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 0.0f, 4.0f, 4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Visibility_Lambda([this]() { return CurrentJob().IsSet() ? EVisibility::Visible : EVisibility::Collapsed; })
			.BorderBackgroundColor_Lambda([this]()
			{
				const TOptional<FHyperAIStudioAgentJobInfo> Job = CurrentJob();
				const EHyperAIStudioAgentJobState State = Job.IsSet() ? Job->State : EHyperAIStudioAgentJobState::Stopped;
				return State == EHyperAIStudioAgentJobState::Succeeded ? FLinearColor(0.20f, 0.45f, 0.25f)
					: Job.IsSet() && Job->IsRunning() ? FLinearColor(0.16f, 0.30f, 0.55f) : FLinearColor(0.50f, 0.34f, 0.10f);
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(6.0f, 4.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this]() { return GetJobText(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ShowJobChat", "Show chat"))
					.ToolTipText(LOCTEXT("ShowJobChatTip", "Bring the agent's chat forward."))
					.OnClicked_Lambda([this]()
					{
						if (const TOptional<FHyperAIStudioAgentJobInfo> Job = CurrentJob()) FHyperAIStudioAgentJobs::ShowChat(Job->Id);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 2.0f, 4.0f, 2.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("StopJob", "Stop"))
					.ToolTipText(LOCTEXT("StopJobTip", "Stop the agent's turn and stop sending it back to work. What it changed so far stays, unsaved."))
					.Visibility_Lambda([this]()
					{
						const TOptional<FHyperAIStudioAgentJobInfo> Job = CurrentJob();
						return Job.IsSet() && Job->IsRunning() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([this]()
					{
						if (const TOptional<FHyperAIStudioAgentJobInfo> Job = CurrentJob()) FHyperAIStudioAgentJobs::Stop(Job->Id);
						return FReply::Handled();
					})
				]
			]
		]

		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(4.0f, 0.0f)
		[
			SAssignNew(ComparisonView, SHyperAIStudioComparisonView)
			.BrushA_Lambda([this]() { return ImageA.Get(Comparison.ImagePathA); })
			.BrushB_Lambda([this]() { return ImageB.Get(Comparison.ImagePathB); })
			.LabelA_Lambda([this]() { return Comparison.LabelA; })
			.LabelB_Lambda([this]() { return Comparison.LabelB; })
			.Mode_Lambda([this]() { return Mode; })
			.EmptyMessage_Lambda([this]()
			{
				if (!Status.IsEmpty())
				{
					return FText::FromString(Status);
				}
				return HyperAIStudio::Preview::Private::RelightAction().IsSet()
					? LOCTEXT("PickOrRelight", "Pick a reference, then Refresh. No reference? Relight this view makes one from the level itself.")
					: LOCTEXT("PickThenRefresh", "Pick a reference, then Refresh.");
			})
		]

		// Earlier attempts, when the source keeps any: pick one to put it beside the reference.
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 4.0f, 4.0f, 0.0f)
		[
			SNew(SScrollBox)
			.Orientation(Orient_Horizontal)
			.Visibility_Lambda([this]() { return Comparison.Frames.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			+ SScrollBox::Slot()
			[
				SAssignNew(FrameStrip, SHorizontalBox)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SAssignNew(StatsBox, SWrapBox).UseAllottedSize(true)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(220.0f)
				.HeightOverride(64.0f)
				.Visibility_Lambda([this]()
				{
					return Comparison.HistogramA.IsEmpty() && Comparison.HistogramB.IsEmpty()
						? EVisibility::Collapsed : EVisibility::Visible;
				})
				[
					SNew(SHyperAIStudioHistogramView)
					.HistogramA_Lambda([this]() { return Comparison.HistogramA; })
					.HistogramB_Lambda([this]() { return Comparison.HistogramB; })
				]
			]
		]
	];

	Refresh();
}

SHyperAIStudioPreviewPanel::~SHyperAIStudioPreviewPanel()
{
	FHyperAIStudioPreviewPanel::OnSourcesChanged().Remove(SourcesChangedHandle);
	FHyperAIStudioPreviewPanel::OnOpenRequested().Remove(OpenRequestedHandle);
	FHyperAIStudioAgentJobs::OnChanged().Remove(JobsChangedHandle);
}

TOptional<FHyperAIStudioAgentJobInfo> SHyperAIStudioPreviewPanel::CurrentJob() const
{
	return ReferencePath.IsEmpty() ? TOptional<FHyperAIStudioAgentJobInfo>() : FHyperAIStudioAgentJobs::FindByKey(ReferencePath);
}

FText SHyperAIStudioPreviewPanel::GetJobText() const
{
	const TOptional<FHyperAIStudioAgentJobInfo> Job = CurrentJob();
	if (!Job.IsSet())
	{
		return FText::GetEmpty();
	}
	const FText State = Job->State == EHyperAIStudioAgentJobState::Starting ? LOCTEXT("JobStarting", "starting")
		: Job->State == EHyperAIStudioAgentJobState::Working ? LOCTEXT("JobWorking", "working")
		: Job->State == EHyperAIStudioAgentJobState::Succeeded ? LOCTEXT("JobDone", "done")
		: Job->State == EHyperAIStudioAgentJobState::Failed ? LOCTEXT("JobFailed", "failed") : LOCTEXT("JobStopped", "stopped");
	return FText::Format(LOCTEXT("JobLine", "{0}. {1} in chat {2}, {3}, round {4}. {5}"), FText::FromString(Job->Title), FText::FromString(Job->Agent),
		FText::AsNumber(Job->TabIndex), State, FText::AsNumber(Job->Turns + (Job->IsRunning() ? 1 : 0)), FText::FromString(Job->Status));
}

TSharedRef<SWidget> SHyperAIStudioPreviewPanel::BuildMatchItMenu()
{
	FMenuBuilder Builder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	if (const FHyperAIStudioPreviewPanel::FSource* Source = CurrentSource())
	{
		for (const FHyperAIStudioPreviewPanel::FAgentAction& Action : Source->AgentActions)
		{
			Builder.AddMenuEntry(Action.Label, Action.Tooltip, FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this, SourceId = Source->Id, ActionId = Action.Id]()
				{
					FString Error;
					if (!FHyperAIStudioPreviewPanel::StartAgentAction(SourceId, ActionId, ReferencePath, Error))
					{
						Status = Error;
						bStatusIsError = true;
					}
				})));
		}
	}
	return Builder.MakeWidget();
}

void SHyperAIStudioPreviewPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	if (InCurrentTime < NextJobRefresh)
	{
		return;
	}
	NextJobRefresh = InCurrentTime + 4.0;
	const TOptional<FHyperAIStudioAgentJobInfo> Job = CurrentJob();
	const FHyperAIStudioPreviewPanel::FSource* Source = CurrentSource();
	if (Job.IsSet() && Job->IsRunning() && Source && Source->bRefreshWhileAgentWorks)
	{
		Refresh();
	}
}

const FHyperAIStudioPreviewPanel::FSource* SHyperAIStudioPreviewPanel::CurrentSource() const
{
	return FHyperAIStudioPreviewPanel::FindSource(SourceId);
}

void SHyperAIStudioPreviewPanel::RebuildSourceOptions()
{
	const TArray<FHyperAIStudioPreviewPanel::FSource> Sources = FHyperAIStudioPreviewPanel::GetSources();
	if (!Sources.ContainsByPredicate([this](const FHyperAIStudioPreviewPanel::FSource& Source) { return Source.Id == SourceId; }))
	{
		SourceId = Sources.IsEmpty() ? NAME_None : Sources[0].Id;
	}
}

void SHyperAIStudioPreviewPanel::HandleOpenRequested(const FName InSourceId, const FString& ReferenceImagePath)
{
	if (!InSourceId.IsNone() && FHyperAIStudioPreviewPanel::FindSource(InSourceId))
	{
		SourceId = InSourceId;
	}
	if (!ReferenceImagePath.IsEmpty())
	{
		SetReference(ReferenceImagePath, /*bRemember=*/true);
		return;
	}
	Refresh();
}

void SHyperAIStudioPreviewPanel::SetReference(const FString& Path, const bool bRemember)
{
	FString Why;
	if (!FHyperAIStudioPreviewPanel::IsSupportedProjectImage(Path, Why))
	{
		Status = Why;
		bStatusIsError = true;
		return;
	}
	ReferencePath = FPaths::ConvertRelativePathToFull(Path);
	Thumbnail.Forget();
	if (bRemember)
	{
		if (UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>())
		{
			Settings->RememberPreviewReference(ReferencePath);
		}
	}
	Refresh();
}

void SHyperAIStudioPreviewPanel::Refresh()
{
	Comparison = FHyperAIStudioPreviewComparison();
	Status.Reset();
	bStatusIsError = false;
	const FHyperAIStudioPreviewPanel::FSource* Source = CurrentSource();
	if (!Source)
	{
		Status = TEXT("No preview source is available; the packs that provide one are not loaded.");
		bStatusIsError = true;
		RebuildStats();
		return;
	}
	if (Source->bNeedsReference)
	{
		FString Why;
		if (!FHyperAIStudioPreviewPanel::IsSupportedProjectImage(ReferencePath, Why))
		{
			Status = Why;
			bStatusIsError = ReferencePath.Len() > 0;
			RebuildStats();
			return;
		}
	}
	FString Error;
	if (!Source->Refresh.IsBound() || !Source->Refresh.Execute(ReferencePath, Comparison, Error))
	{
		Comparison = FHyperAIStudioPreviewComparison();
		Status = Error.IsEmpty() ? TEXT("That source could not produce a comparison.") : Error;
		bStatusIsError = true;
	}
	RebuildStats();
}

void SHyperAIStudioPreviewPanel::RebuildFrames()
{
	if (!FrameStrip.IsValid())
	{
		return;
	}
	FrameStrip->ClearChildren();
	FrameImages.Reset();
	for (const FHyperAIStudioPreviewFrame& Frame : Comparison.Frames)
	{
		const TSharedPtr<FHyperAIStudioPreviewImage> Image = FrameImages.Add_GetRef(MakeShared<FHyperAIStudioPreviewImage>());
		const FString Path = Frame.ImagePath;
		const FText Label = Frame.Label;
		const FLinearColor Tint = Frame.Tone == EHyperAIStudioPreviewTone::Good ? FLinearColor(0.20f, 0.45f, 0.25f)
			: Frame.Tone == EHyperAIStudioPreviewTone::Warning ? FLinearColor(0.50f, 0.34f, 0.10f) : FLinearColor(0.16f, 0.16f, 0.18f);
		FrameStrip->AddSlot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(Frame.Tooltip.IsEmpty() ? FText::FromString(Path) : Frame.Tooltip)
			.OnClicked_Lambda([this, Path, Label]()
			{
				Comparison.ImagePathB = Path;
				Comparison.LabelB = Label;
				return FReply::Handled();
			})
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.BorderBackgroundColor_Lambda([this, Path, Tint]() { return Comparison.ImagePathB == Path ? FLinearColor(0.85f, 0.55f, 0.1f) : Tint; })
					.Padding(2.0f)
					[
						SNew(SBox).WidthOverride(112.0f).HeightOverride(66.0f)
						[
							SNew(SScaleBox)
							.Stretch(EStretch::ScaleToFit)
							[
								SNew(SImage).Image_Lambda([Image, Path]() { return Image->Get(Path); })
							]
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular", 8)).Text(Label)
				]
			]
		];
	}
}

void SHyperAIStudioPreviewPanel::RebuildStats()
{
	RebuildFrames();
	if (!StatsBox.IsValid())
	{
		return;
	}
	StatsBox->ClearChildren();
	for (const FHyperAIStudioPreviewStat& Stat : Comparison.Stats)
	{
		const FLinearColor Tint = Stat.Tone == EHyperAIStudioPreviewTone::Good
			? FLinearColor(0.20f, 0.45f, 0.25f)
			: Stat.Tone == EHyperAIStudioPreviewTone::Warning
				? FLinearColor(0.50f, 0.34f, 0.10f)
				: FLinearColor(0.16f, 0.16f, 0.18f);
		StatsBox->AddSlot().Padding(0.0f, 0.0f, 4.0f, 4.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(Tint)
			.Padding(FMargin(6.0f, 3.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(Stat.Label)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.Text(Stat.Value)
				]
			]
		];
	}
}

FReply SHyperAIStudioPreviewPanel::OnRefreshClicked()
{
	Refresh();
	return FReply::Handled();
}

FReply SHyperAIStudioPreviewPanel::OnWarningActionClicked()
{
	if (Comparison.WarningAction.IsBound())
	{
		Comparison.WarningAction.Execute();
		Refresh();
	}
	return FReply::Handled();
}

FReply SHyperAIStudioPreviewPanel::OnBrowseClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}
	const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
	TArray<FString> Chosen;
	// The image tools only read inside the project, so start there rather than let a pick fail later.
	const FString Start = ReferencePath.IsEmpty()
		? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())
		: FPaths::GetPath(ReferencePath);
	if (DesktopPlatform->OpenFileDialog(ParentWindow, LOCTEXT("PickReference", "Pick a reference image").ToString(),
		Start, FString(), TEXT("Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga)|*.png;*.jpg;*.jpeg;*.bmp;*.tga"),
		EFileDialogFlags::None, Chosen) && Chosen.Num() > 0)
	{
		SetReference(Chosen[0], /*bRemember=*/true);
	}
	return FReply::Handled();
}

TSharedRef<SWidget> SHyperAIStudioPreviewPanel::BuildRecentsMenu()
{
	FMenuBuilder Builder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const TArray<FString> Recents = Settings ? Settings->RecentPreviewReferences : TArray<FString>();
	if (Recents.IsEmpty())
	{
		Builder.AddWidget(SNew(STextBlock).Text(LOCTEXT("NoRecents", "Nothing yet")), FText::GetEmpty());
		return Builder.MakeWidget();
	}
	for (const FString& Path : Recents)
	{
		Builder.AddMenuEntry(
			FText::FromString(FPaths::GetCleanFilename(Path)),
			FText::FromString(Path),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, Path]() { SetReference(Path, /*bRemember=*/true); })));
	}
	return Builder.MakeWidget();
}

TSharedRef<SWidget> SHyperAIStudioPreviewPanel::BuildSourceMenu()
{
	FMenuBuilder Builder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	for (const FHyperAIStudioPreviewPanel::FSource& Source : FHyperAIStudioPreviewPanel::GetSources())
	{
		const FName Id = Source.Id;
		Builder.AddMenuEntry(Source.Label, Source.Tooltip, FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([this, Id]()
			{
				SourceId = Id;
				Refresh();
			})));
	}
	return Builder.MakeWidget();
}

FString SHyperAIStudioPreviewPanel::DroppedImagePath(const FDragDropEvent& Event) const
{
	const TSharedPtr<FExternalDragOperation> Operation = Event.GetOperationAs<FExternalDragOperation>();
	if (!Operation.IsValid() || !Operation->HasFiles())
	{
		return FString();
	}
	FString Ignored;
	for (const FString& File : Operation->GetFiles())
	{
		if (FHyperAIStudioPreviewPanel::IsSupportedProjectImage(File, Ignored))
		{
			return File;
		}
	}
	return FString();
}

void SHyperAIStudioPreviewPanel::OnDragEnter(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	bDropAccepted = !DroppedImagePath(Event).IsEmpty();
	bReferenceExpanded = bReferenceExpanded || Event.GetOperationAs<FExternalDragOperation>().IsValid();
}

void SHyperAIStudioPreviewPanel::OnDragLeave(const FDragDropEvent& Event)
{
	bDropAccepted = false;
}

FReply SHyperAIStudioPreviewPanel::OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	return bDropAccepted ? FReply::Handled() : FReply::Unhandled();
}

FReply SHyperAIStudioPreviewPanel::OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	bDropAccepted = false;
	const FString Path = DroppedImagePath(Event);
	if (Path.IsEmpty())
	{
		const TSharedPtr<FExternalDragOperation> Operation = Event.GetOperationAs<FExternalDragOperation>();
		if (Operation.IsValid() && Operation->HasFiles())
		{
			Status = TEXT("That file is not a project image; copy a PNG or JPG into the project folder first.");
			bStatusIsError = true;
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}
	SetReference(Path, /*bRemember=*/true);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
