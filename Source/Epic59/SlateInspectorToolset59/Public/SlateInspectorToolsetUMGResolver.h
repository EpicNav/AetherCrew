// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/SWidget.h"

/**
 * Result of resolving a live SWidget back to the UMG UWidget that created it.
 *
 * UMG attaches an FReflectionMetaData to the top SWidget produced by each
 * UWidget (see UWidget::TakeWidget_Private), which is how the Widget Reflector
 * recovers the authoring UObject.  This mirror uses the same metadata.
 */
struct FSlateInspectorToolsetUMGInfo
{
	/** UWidget name in its hierarchy (FReflectionMetaData::Name), e.g. "SaveButton". */
	FString WidgetName;

	/** Name of the UClass that constructed the Slate widget, e.g. "Button". */
	FString WidgetClassName;

	/** Path of the owning asset (usually a WidgetBlueprint); empty when unknown. */
	FString AssetPath;

	/** True when the metadata came from a parent widget rather than the widget itself. */
	bool bResolvedOnParent = false;

	/** The live UWidget that owns the Slate widget. */
	TWeakObjectPtr<UObject> SourceObject;

	/** Owning UUserWidget chain, innermost first, as {ObjectName, ClassName} pairs. */
	TArray<TPair<FString, FString>> OuterUserWidgetChain;
};

/** Resolves live Slate widgets back to their UMG UWidget wrappers for the toolset. */
class FSlateInspectorToolsetUMGResolver
{
public:

	/**
	 * Resolve the UMG UWidget wrapping a Slate widget, if any.
	 *
	 * @param Widget               The live Slate widget to resolve.
	 * @param bAllowParentFallback When true, walks up to the nearest ancestor
	 *                             carrying UMG metadata if the widget itself has none.
	 * @return The resolved info, or unset when the widget is pure Slate (no UMG owner).
	 */
	SLATEINSPECTORTOOLSET59_API static TOptional<FSlateInspectorToolsetUMGInfo> Resolve(TSharedRef<SWidget> Widget, bool bAllowParentFallback = true);

	/**
	 * Build a compact single-token annotation for snapshots, e.g. "WBP_HUD.SaveButton(Button)".
	 * Uses the asset base filename rather than its full path to stay terse.
	 */
	SLATEINSPECTORTOOLSET59_API static FString MakeCompactAnnotation(const FSlateInspectorToolsetUMGInfo& Info);
};
