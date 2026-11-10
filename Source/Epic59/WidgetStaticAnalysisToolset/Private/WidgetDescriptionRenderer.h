// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "WidgetDescriptionTypes.h"

class UWidget;
class UWidgetBlueprint;

/**
 * Walks a UMG widget tree (via UWidgetBlueprintEditorSubsystem::WalkWidgetTree) and renders one
 * FWidgetDescriptionEntry per widget, plus an optional verbose per-property text
 * description, computing Depth, structured NamedSlotHost, corrected bInherited, and UIComponents
 * fresh for each entry.
 */
class FWidgetDescriptionRenderer
{
public:
	/**
	 * Callers must ensure WidgetBlueprint is non-null, has a widget tree, and that StartWidget
	 * (if given) belongs to that tree -- this does not re-validate (that's the top-level
	 * UFUNCTION's job).
	 *
	 * @param StartWidget              Root of the description; nullptr describes the whole tree.
	 * @param MaxDepth                 Levels below StartWidget to include; -1 for no limit.
	 * @param PropertyNameFilter       Substring match against property name; TEXT("") matches every property.
	 * @param bSkipDefaultValues       When true (default), a property whose value matches the class default is excluded.
	 * @param bIncludeFullDescription  When true, populates Description (verbose per-property text,
	 *                                 one line per entry in Widgets, 1:1 correlated by index); when
	 *                                 false, Description stays empty and Widgets is still fully populated.
	 */
	static FWidgetDescriptionResult RenderDescription(
		const UWidgetBlueprint* WidgetBlueprint,
		const UWidget* StartWidget = nullptr,
		int32 MaxDepth = -1,
		const FString& PropertyNameFilter = TEXT(""),
		bool bSkipDefaultValues = true,
		bool bIncludeFullDescription = false);
};
