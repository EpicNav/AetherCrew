// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioUIToolset.h"
#include "Math/Box2D.h"

/**
 * hyper_ui_render_review: a Widget Blueprint drawn off screen at a real screen size with FWidgetRenderer (the thumbnail
 * renderer's way: a transient design-time instance in the editor world, or the instance playing in PIE), then measured
 * from the geometry Slate laid it out with. The checks run on plain widget boxes and pixels, so they are tested without
 * rendering anything.
 */
namespace HyperAIStudio::UI::Review
{
	/** One widget as the render laid it out. Rect is in render pixels; the local sizes are in Slate units (pixels / DPI). */
	struct FWidgetBox
	{
		/** Root/Menu/PlayButton. */
		FString Path;
		FString Class;
		FBox2D Rect = FBox2D(ForceInit);
		bool bInteractive = false;
		/** A text block: its desired size is the text's. */
		bool bText = false;
		/** Something the player sees or uses (not a layout panel or a full-screen backdrop). */
		bool bContent = true;
		bool bFocusable = true;
		/** It clips what is drawn outside it (Clipping ClipToBounds). */
		bool bClipsChildren = false;
		bool bWraps = false;
		FVector2D DesiredLocal = FVector2D::ZeroVector;
		FVector2D AllottedLocal = FVector2D::ZeroVector;
		/** Glyph height in render pixels; 0 when it is not plain text. */
		float FontPixels = 0.f;
		/** The text's colour, linear; unset when it inherits a foreground colour. */
		TOptional<FLinearColor> TextColor;
	};

	/** "1920x1080" (or 1920*1080) into a size from 320 to 7680 a side. */
	bool ParseResolution(const FString& Text, FIntPoint& Out);
	/** WCAG relative luminance of a linear colour. */
	float RelativeLuminance(const FLinearColor& Linear);
	/** WCAG contrast ratio, 1 to 21. */
	float ContrastRatio(const FLinearColor& A, const FLinearColor& B);
	/**
	 * The colour most of Rect shows that is not the text's own: what the text is read against. Pixels are sRGB, row by
	 * row, Size wide; the answer is linear. False when Rect holds no such pixel.
	 */
	bool EstimateBackground(const TArray<FColor>& Pixels, FIntPoint Size, const FBox2D& Rect, const FLinearColor& Text, FLinearColor& OutBackground);
	/** "x,y wxh", whole pixels. */
	FString RectText(const FBox2D& Rect);
	/** Glyph height in pixels for a Slate font size (points at 96 DPI) at a UI scale. */
	float FontPixels(float FontSize, float DpiScale);

	/**
	 * Layout checks of one render: clipped text, small text, contrast (when Pixels is given), off screen, outside the
	 * safe zone, cut off by a clipping parent, and interactive widgets overlapping.
	 */
	void CheckLayout(const TArray<FWidgetBox>& Boxes, FIntPoint Screen, const FHyperAIUIRenderReviewRequest& Rules, const FString& Resolution,
		const TArray<FColor>* Pixels, TArray<FHyperAIUIReviewIssue>& Out);

	FHyperAIUIRenderReviewReport Run(const FHyperAIUIRenderReviewRequest& Request);
}
