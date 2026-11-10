// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "Input/Events.h"
#include "UObject/ObjectMacros.h"

#include "SlateInspectorToolsetTypes.generated.h"

/** Modifier keys held during an input simulation (click, drag, etc.). */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetModifierKeys59
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	bool bShift = false;

	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	bool bCtrl = false;

	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	bool bAlt = false;

	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	bool bCmd = false;

	/** Convert to Slate's FModifierKeysState. */
	FModifierKeysState ToModifierKeysState() const
	{
		return FModifierKeysState(bShift, bShift, bCtrl, bCtrl, bAlt, bAlt, bCmd, bCmd, false);
	}
};

/**
 * A point inside a widget in normalized coordinates, where (0.5, 0.5) is the center.
 * Both coordinates must be in [0, 1]; values outside are refused rather than clamped.
 * 0 and 1 aim half a pixel inside the widget's edges, so they never land on a neighbor.
 */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetNormalizedPoint
{
	GENERATED_BODY()

	/** 0 = left edge, 0.5 = center, 1 = right edge. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	float X = 0.5f;

	/** 0 = top edge, 0.5 = center, 1 = bottom edge. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	float Y = 0.5f;
};

/** An offset in screen pixels from a drag's start point. */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetPoint
{
	GENERATED_BODY()

	/** Horizontal offset in screen pixels; positive is right. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	double X = 0.0;

	/** Vertical offset in screen pixels; positive is down. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	double Y = 0.0;
};

/**
 * An ordered list of drag offsets. Wraps the array because UHT accepts a
 * default value for a struct parameter but not for a bare TArray parameter.
 */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetPointList
{
	GENERATED_BODY()

	/** Offsets in visiting order, e.g. {"Points": [{"X": 60, "Y": 0}]}; empty means none. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	TArray<FSlateInspectorToolsetPoint> Points;
};

/** Options for USlateInspectorToolset59::Click. */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetClickParams
{
	GENERATED_BODY()

	/** "left", "right", or "middle". */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FString Button = TEXT("left");

	/** True for double-click. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	bool bDoubleClick = false;

	/** Modifier keys held during the click. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetModifierKeys59 Modifiers;

	/** Where in the widget to click. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetNormalizedPoint Position;
};

/** Options for USlateInspectorToolset59::Hover. */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetHoverParams
{
	GENERATED_BODY()

	/** Where in the widget to hover. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetNormalizedPoint Position;
};

/** Options for USlateInspectorToolset59::HitTest. */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetHitTestParams
{
	GENERATED_BODY()

	/** Point to test when Grid is 1. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetNormalizedPoint Position;

	/** Sample an N x N lattice inset in the widget instead of the single Position. Clamped to 1..16. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	int32 Grid = 1;
};

/** Options for USlateInspectorToolset59::Scroll. */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetScrollParams
{
	GENERATED_BODY()

	/** Wheel notches. Positive scrolls up / zooms in, negative down / out. The magnitude is clamped to 100.
	 *  0 sends no wheel event, and is refused unless a gesture delta is set. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	int32 Ticks = -3;

	/** Horizontal trackpad gesture delta in screen pixels, delivered as given; only widgets handling
	 *  OnTouchGesture react to it, since Slate's wheel carries no horizontal axis. 0 sends no gesture. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	float GestureX = 0.f;

	/** Vertical trackpad gesture delta in screen pixels, for widgets handling OnTouchGesture; prefer
	 *  Ticks for ordinary vertical scrolling. 0 sends no gesture. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	float GestureY = 0.f;

	/** Modifier keys held during the scroll (Ctrl+wheel is zoom in many panels). */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetModifierKeys59 Modifiers;

	/** Where in the widget to scroll. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetNormalizedPoint Position;
};

/** Options for USlateInspectorToolset59::Drag. */
USTRUCT(BlueprintType)
struct FSlateInspectorToolsetDragParams
{
	GENERATED_BODY()

	/** Modifier keys held during the drag. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetModifierKeys59 Modifiers;

	/** "left", "right", or "middle"; middle pans graphs and right pans scroll boxes. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FString Button = TEXT("left");

	/** Offsets from the press point visited in order before the drop, e.g. {"Points": [{"X": 60, "Y": 0}]}.
	 *  Use them for gestures a straight line would not produce, such as dragging around an obstacle. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetPointList Waypoints;

	/** Where in the source widget to press. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetNormalizedPoint StartPosition;

	/** Where in the target widget to drop, e.g. Y = 0.1 to drop against its top edge. Ignored when EndRef is empty. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	FSlateInspectorToolsetNormalizedPoint EndPosition;

	/** Extra screen pixels added to the drop point, positive = right. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	float MoveByX = 0.f;

	/** Extra screen pixels added to the drop point, positive = down. */
	UPROPERTY(BlueprintReadWrite, Category = "SlateInspectorToolset")
	float MoveByY = 0.f;
};
