// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "CoreTypes.h"
#include "Math/Color.h"
#include "Templates/Tuple.h"
#include "UObject/NameTypes.h"
#include "WidgetDescriptionTypes.h"

class UWidget;
class UWidgetBlueprint;
struct FSlateBrush;
struct FSlateColor;

/** Per-Widget-Blueprint inputs the resolver needs beyond the widget itself; build once per tree with MakeContext. */
struct FWidgetComposedColorResolverContext
{
	/** (widget name, property name) pairs driven at runtime by a legacy property binding or an enabled
	 *  MVVM binding. Template widgets never have their bindings applied, so their delegates can't say. */
	TSet<TPair<FName, FName>> BoundProperties;

	/** The Widget Blueprint's own UserWidget class defaults, which every widget in its tree is drawn under. */
	FLinearColor OwnerColorAndOpacity = FLinearColor::White;
	float OwnerRenderOpacity = 1.0f;
};

/**
 * Resolves the composed linear tint that a widget's own Slate OnPaint would draw, wherever a formula
 * is actually known -- a fixed if-chain over exactly Button/Border/Image/Text, never an extensible
 * registry: the v1 formula coverage set is closed, and a future formula (e.g. URichTextBlock's
 * per-inline-tag-run DataTable model) needs a different shape a registry wouldn't fit anyway.
 *
 * Never guesses: a widget whose formula can't fully resolve (a colour factor not in Specified mode,
 * an unserialized colour table, a material/texture-backed or undrawn brush, a bound colour, an
 * unresolved ancestor tint, or a style-driven subclass) reports Gaps naming what failed, leaves
 * bResolved false and ComposedLinearColor at its default.
 *
 * Reports the composed pre-display **linear** tint only -- never a display-space value. It does not
 * model the per-player GSlateContrast setting, DisplayGamma, retainer-box re-compositing, or Global
 * Invalidation's cached-style fast path, any of which can further change the actually-displayed pixel
 * beyond what this resolver reports (structural, always-true limitation; disclosed once via the
 * AICallable tool's own doc comment, not repeated here per entry). Button's DisabledEffect is a draw
 * effect applied on top of the reported tint and is not modelled either.
 */
class FWidgetComposedColorResolver
{
public:
	/**
	 * Button gets one entry per interactable state (Normal/Hovered/Pressed/Disabled) since SButton's
	 * OnPaint is the only one of the four that actually branches on state (state only selects which
	 * brush feeds the brush-tint factor -- the other factors are shared across states). Border and
	 * Image each get a single Normal entry -- their own OnPaint never branches on state. Text gets a
	 * single Normal entry (no brush term at all). Any other widget kind -- including SCheckBox (a
	 * pure SCompoundWidget with no OnPaint of its own) and URichTextBlock/CommonRichTextBlock
	 * (resolves colour per inline-tag run from a style DataTable, a fundamentally different model) --
	 * returns an empty array; a compound widget's own child widgets still get their own entries from
	 * their own calls to this function elsewhere in DescribeWidget's flat walk.
	 */
	static TArray<FWidgetComposedColorEntry> Resolve(const UWidget* Widget, const FWidgetComposedColorResolverContext& Context);

	/** Collects WidgetBlueprint's bound widget properties and its own UserWidget tint for Resolve. */
	static FWidgetComposedColorResolverContext MakeContext(const UWidgetBlueprint* WidgetBlueprint);

private:
	static TArray<FWidgetComposedColorEntry> ResolveButton(const class UButton* Button, const FWidgetComposedColorResolverContext& Context);
	static FWidgetComposedColorEntry ResolveBorder(const class UBorder* Border, const FWidgetComposedColorResolverContext& Context);
	static FWidgetComposedColorEntry ResolveImage(const class UImage* Image, const FWidgetComposedColorResolverContext& Context);
	static FWidgetComposedColorEntry ResolveText(const class UTextBlock* TextBlock, const FWidgetComposedColorResolverContext& Context);

	/** Resolves an FSlateColor factor: Specified -> a concrete value; ColorTable/Foreground/
	 *  Foreground_Subdued/UseStyle -> a Gap naming why. */
	static bool TryResolveSlateColorFactor(const FSlateColor& Color, const FString& FactorLabel,
		FLinearColor& OutColor, TArray<FWidgetColorResolutionGap>& OutGaps);

	/** Resolves a brush's tint factor (TintColor, via TryResolveSlateColorFactor) plus a Gap if the
	 *  brush carries a texture/material ResourceObject -- its tint alone no longer tells the whole
	 *  story once actual image/material content is involved -- or is NoDrawType and draws nothing. */
	static bool TryResolveBrushTintFactor(const FSlateBrush& Brush, const FString& FactorLabel,
		FLinearColor& OutColor, TArray<FWidgetColorResolutionGap>& OutGaps);

	/** Walks the ancestor chain (UWidget::GetParent()) accumulating the multiplicative tint every
	 *  ancestor Button/Border compounds into its children's FWidgetStyle (only SCompoundWidget-based
	 *  widgets that expose a ColorAndOpacity-like property do this -- confirmed for Button/Border;
	 *  every other UPanelWidget kind is a confirmed pass-through per SPanel's own OnArrangeChildren,
	 *  which forwards the incoming style unchanged), and the alpha every widget's RenderOpacity blends
	 *  in (SWidget::Paint), the widget's own included. A bound ancestor factor, a named-slot boundary,
	 *  or a non-neutral owning UserWidget is reported as a Gap instead. */
	static bool TryResolveAncestorTint(const UWidget* Widget, const FWidgetComposedColorResolverContext& Context,
		FLinearColor& OutTint, TArray<FWidgetColorResolutionGap>& OutGaps);

	/** True (no gap added) when Widget's PropertyName is not bound; otherwise records a Gap of Kind --
	 *  a bound property's runtime value can't be read without executing the graph or the ViewModel, the
	 *  same "can't statically confirm" honesty rule as every other Gap kind here. Kind is caller-supplied
	 *  so a widget's OWN bound property (OwnColorDelegateBound) and an ancestor's
	 *  (UnresolvedAncestorChain) are told apart. */
	static bool RequireNotBound(const FWidgetComposedColorResolverContext& Context, const UWidget* Widget, FName PropertyName,
		EColorResolutionGapKind Kind, TArray<FWidgetColorResolutionGap>& OutGaps);
};
