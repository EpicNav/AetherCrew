// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioPreviewImage.h"
#include "HyperAIStudioPreviewPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"

class SEditableTextBox;

enum class EHyperAIStudioCompareMode : uint8
{
	/** Both images in full, one beside the other. */
	SideBySide,
	/** One over the other, split by a divider you drag across them. */
	Wipe
};

/**
 * Two images, side by side or under a wipe divider, sharing one zoom and pan so the same pixels line up.
 * Painted directly rather than composed from SImage, because the two halves have to stay in step.
 */
class SHyperAIStudioComparisonView final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioComparisonView) {}
		SLATE_ATTRIBUTE(const FSlateBrush*, BrushA)
		SLATE_ATTRIBUTE(const FSlateBrush*, BrushB)
		SLATE_ATTRIBUTE(FText, LabelA)
		SLATE_ATTRIBUTE(FText, LabelB)
		SLATE_ATTRIBUTE(EHyperAIStudioCompareMode, Mode)
		SLATE_ATTRIBUTE(FText, EmptyMessage)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override;
	virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& Geometry, const FPointerEvent& Event) override;
	virtual FCursorReply OnCursorQuery(const FGeometry& Geometry, const FPointerEvent& Event) const override;

	/** Back to fitting both images in their panes. */
	void ResetView();

private:
	bool IsOverDivider(const FGeometry& Geometry, const FPointerEvent& Event) const;

	TAttribute<const FSlateBrush*> BrushA;
	TAttribute<const FSlateBrush*> BrushB;
	TAttribute<FText> LabelA;
	TAttribute<FText> LabelB;
	TAttribute<EHyperAIStudioCompareMode> Mode;
	TAttribute<FText> EmptyMessage;

	float Divider = 0.5f;
	float Zoom = 1.0f;
	/** Offset in fractions of the pane, so it survives a resize. */
	FVector2f Pan = FVector2f::ZeroVector;
	bool bDraggingDivider = false;
	bool bPanning = false;
	FVector2f DragStart = FVector2f::ZeroVector;
	FVector2f PanAtDragStart = FVector2f::ZeroVector;
};

/** Two brightness histograms of equal length, drawn over each other. */
class SHyperAIStudioHistogramView final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioHistogramView) {}
		SLATE_ATTRIBUTE(TArray<float>, HistogramA)
		SLATE_ATTRIBUTE(TArray<float>, HistogramB)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override;

private:
	TAttribute<TArray<float>> HistogramA;
	TAttribute<TArray<float>> HistogramB;
};

/**
 * The preview tab: a reference image, what a pack produced from it, and the numbers between them.
 * Packs register themselves through FHyperAIStudioPreviewPanel; this panel knows none of them by name.
 */
class SHyperAIStudioPreviewPanel final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioPreviewPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SHyperAIStudioPreviewPanel() override;

	virtual void OnDragEnter(const FGeometry& Geometry, const FDragDropEvent& Event) override;
	virtual void OnDragLeave(const FDragDropEvent& Event) override;
	virtual FReply OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event) override;
	virtual FReply OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event) override;

private:
	void RebuildSourceOptions();
	void SetReference(const FString& Path, bool bRemember);
	void Refresh();
	void RebuildStats();
	FReply OnBrowseClicked();
	FReply OnRefreshClicked();
	FReply OnWarningActionClicked();
	TSharedRef<SWidget> BuildRecentsMenu();
	TSharedRef<SWidget> BuildSourceMenu();
	const FHyperAIStudioPreviewPanel::FSource* CurrentSource() const;
	/** The one file in a drop this panel can take, or empty. */
	FString DroppedImagePath(const FDragDropEvent& Event) const;
	void HandleOpenRequested(FName SourceId, const FString& ReferenceImagePath);

	FName SourceId;
	FString ReferencePath;
	FString Status;
	bool bStatusIsError = false;
	bool bReferenceExpanded = false;
	bool bDropAccepted = false;
	EHyperAIStudioCompareMode Mode = EHyperAIStudioCompareMode::SideBySide;
	FHyperAIStudioPreviewComparison Comparison;

	FHyperAIStudioPreviewImage ImageA;
	FHyperAIStudioPreviewImage ImageB;
	FHyperAIStudioPreviewImage Thumbnail;

	TSharedPtr<SHyperAIStudioComparisonView> ComparisonView;
	TSharedPtr<class SWrapBox> StatsBox;
	FDelegateHandle SourcesChangedHandle;
	FDelegateHandle OpenRequestedHandle;
};
