// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Extensions/UIComponent.h"
#include "Math/Color.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectPtr.h"
#include "UObject/SoftObjectPath.h"
#include "WidgetDescriptionTypes.generated.h"

/** Info for a single UI component attached to a widget instance. */
USTRUCT(BlueprintType)
struct FWidgetUIComponentInfo
{
	GENERATED_BODY()

	/** The component archetype instance. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TObjectPtr<UUIComponent> Component = nullptr;

	/** Class path for this component. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FSoftClassPath ComponentClassPath;
};

/** One {name, exported value} property pair, shared shape for Properties/SlotProperties. */
USTRUCT(BlueprintType)
struct FWidgetPropertyEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString PropertyName;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString ExportedValue;
};

/**
 * Which interactable state an FWidgetComposedColorEntry describes. Non-interactable widget kinds
 * (Border, Image, Text) always report a single Normal entry -- their own OnPaint never branches on
 * interaction state (only SButton, and SCheckBox's ToggleButton mode, do).
 */
UENUM(BlueprintType)
enum class EWidgetComposedColorState : uint8
{
	Normal,
	Hovered,
	Pressed,
	Disabled,
};

/** Why one composition factor could not be resolved to a concrete linear colour. */
UENUM(BlueprintType)
enum class EColorResolutionGapKind : uint8
{
	/** An FSlateColor factor whose ColorUseRule is Foreground/Foreground_Subdued/UseStyle -- it
	 *  resolves from the runtime widget style, not a stored value. */
	NonSpecifiedColorMode,
	/** An FSlateColor factor whose ColorUseRule is ColorTable -- ColorTableId is a private,
	 *  non-UPROPERTY field, so it is never serialized and cannot be trusted. */
	UnserializedColorTable,
	/** A brush factor carries a texture or material ResourceObject, or a whole FSlateBrush/colour is
	 *  supplied by a bound Blueprint function rather than a static value. */
	MaterialOrTextureResource,
	/** An ancestor's own contribution to the compounded tint could not be resolved: the ancestor's
	 *  ColorAndOpacity/ContentColorAndOpacity/RenderOpacity is bound, or the widget sits in a named slot
	 *  whose host's own tree around the slot is not walked. */
	UnresolvedAncestorChain,
	/** The widget's OWN colour/brush/opacity factor is driven at runtime by a legacy property binding or
	 *  an enabled MVVM binding -- not an ancestor-chain problem, the widget's own value is dynamic. */
	OwnColorDelegateBound,
	/** The Widget Blueprint's own UserWidget ColorAndOpacity or RenderOpacity is not neutral. It tints
	 *  every widget in the tree, and a placed instance can override it, so it is not folded in. */
	OwnerWidgetTint,
	/** The brush's DrawAs is NoDrawType, so Slate draws nothing for this factor. */
	BrushNotDrawn,
	/** The widget is a native subclass of the formula's widget class (for example a CommonUI text or
	 *  button), which may apply colour from its own style at runtime. */
	StyleDrivenSubclass,
};

/** One unresolved factor in an FWidgetComposedColorEntry's composition chain. */
USTRUCT(BlueprintType)
struct FWidgetColorResolutionGap
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	EColorResolutionGapKind Kind = EColorResolutionGapKind::NonSpecifiedColorMode;

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString Detail;
};

/** One interactable-state row of a widget's composed rendered colour (FWidgetComposedColorResolver). */
USTRUCT(BlueprintType)
struct FWidgetComposedColorEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	EWidgetComposedColorState State = EWidgetComposedColorState::Normal;

	/** True only when every factor resolved. When false, ComposedLinearColor is opaque black and means nothing. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	bool bResolved = false;

	/** Product of CompositionFactors, in evaluation order. Only meaningful when bResolved. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FLinearColor ComposedLinearColor = FLinearColor::Black;

	/** Ordered, e.g. ["Brush.TintColor", "AncestorColorAndOpacityTint", "BackgroundColor"]; always populated,
	 *  even when Gaps is not empty. AncestorColorAndOpacityTint includes the RenderOpacity of the widget and its ancestors. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FString> CompositionFactors;

	/** One entry per factor that could not be resolved; empty when bResolved. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetColorResolutionGap> Gaps;
};

/**
 * One widget's structured metadata from a DescribeWidget tree walk.
 *
 * Hierarchy context:
 *   - Parent set        -> widget was added as a child of this panel
 *   - NamedSlotHost set  -> widget is named slot content on this host
 *   - Both null          -> root widget, or content in a slot inherited from a parent class
 *   - bInherited true    -> defined in the C++ (or Blueprint) parent class, not user-created
 */
USTRUCT(BlueprintType)
struct FWidgetDescriptionEntry
{
	GENERATED_BODY()

	/** The widget instance. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TObjectPtr<UWidget> Widget = nullptr;

	/** Parent panel widget. nullptr when this is the root widget or lives in a named slot (see NamedSlotHost). */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TObjectPtr<UPanelWidget> Parent = nullptr;

	/** Class path for this widget. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FSoftClassPath WidgetClassPath;

	/** Widget instance name. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FName WidgetName;

	/** Whether this widget is exposed as a blueprint variable. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	bool bIsVariable = false;

	/** 0 at the walk's StartWidget; increases by one per level of descent (panel child or named slot content alike). */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 Depth = 0;

	/** Set when this widget is the content placed in a named slot on this host; nullptr otherwise.
	 *  Also nullptr for content in a slot inherited from a parent class (the widget tree hosts it); the
	 *  Description line still carries its "# named-slot" tag. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TObjectPtr<UWidget> NamedSlotHost = nullptr;

	/** True when this widget's name was already declared as a widget property on WidgetBlueprint's ParentClass. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	bool bInherited = false;

	/** UI components attached to this widget, in display order. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetUIComponentInfo> UIComponents;

	/** This widget's own reflected properties. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetPropertyEntry> Properties;

	/** The widget's Slot object's reflected properties (padding, alignment, anchors, ...). */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetPropertyEntry> SlotProperties;

	/** Composed rendered colour per interactable state; empty when no known formula for this widget kind,
	 *  or when DescribeWidget's bIncludeComposedColors is false. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetComposedColorEntry> ComposedColors;
};

/** Return value of DescribeWidget. */
USTRUCT(BlueprintType)
struct FWidgetDescriptionResult
{
	GENERATED_BODY()

	/** Widgets found within StartWidget/MaxDepth's bound. Keep alphabetically first: output keys are sorted,
	 *  and this is the canary if Widgets gets cut. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	int32 AvailableWidgetCount = 0;

	/** Verbose per-property text tree; only populated when bIncludeFullDescription=true. Line [N] correlates 1:1 with Widgets[N]
	 *  (line breaks inside values are escaped as \n and \r, backslashes as \\).
	 *  Other value text is not escaped, so a value can imitate the line's own markers; read Widgets for
	 *  anything programmatic. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	FString Description;

	/** Flat walk-order list, regardless of bIncludeFullDescription. */
	UPROPERTY(BlueprintReadOnly, Category = "WidgetStaticAnalysis")
	TArray<FWidgetDescriptionEntry> Widgets;
};
