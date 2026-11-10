// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetComposedColorResolver.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/Button.h"
#include "Components/Border.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetMVVMBindingUtils.h"

namespace
{
	/** Blueprint subclasses inherit their native parent's OnPaint unchanged; a native subclass may not. */
	const UClass* FindNativeClass(const UWidget* Widget)
	{
		const UClass* Class = Widget->GetClass();
		while (Class && !Class->HasAnyClassFlags(CLASS_Native))
		{
			Class = Class->GetSuperClass();
		}
		return Class;
	}

	/** Gaps every entry and drops its colour when Widget is a native subclass of FormulaClass. */
	void ApplyStyleDrivenSubclassGap(const UWidget* Widget, const UClass* FormulaClass, TArray<FWidgetComposedColorEntry>& InOutEntries)
	{
		const UClass* NativeClass = FindNativeClass(Widget);
		if (NativeClass == FormulaClass)
		{
			return;
		}

		for (FWidgetComposedColorEntry& Entry : InOutEntries)
		{
			Entry.Gaps.Add(FWidgetColorResolutionGap{EColorResolutionGapKind::StyleDrivenSubclass,
				FString::Printf(TEXT("%s is a %s, a native subclass of %s that may apply colour from its own style at runtime."),
					*Widget->GetName(), *GetNameSafe(NativeClass), *FormulaClass->GetName())});
			Entry.bResolved = false;
			Entry.ComposedLinearColor = FLinearColor::Black;
		}
	}

	/** True when this widget is placed under a named slot (its own or a parent class's), whose host's tree is not walked. */
	bool IsNamedSlotContent(const UWidget* Widget)
	{
		const UWidgetTree* WidgetTree = Widget->GetTypedOuter<UWidgetTree>();
		return WidgetTree && WidgetTree->RootWidget != Widget;
	}
}

TArray<FWidgetComposedColorEntry> FWidgetComposedColorResolver::Resolve(const UWidget* Widget, const FWidgetComposedColorResolverContext& Context)
{
	TArray<FWidgetComposedColorEntry> Entries;

	if (const UButton* Button = Cast<UButton>(Widget))
	{
		Entries = ResolveButton(Button, Context);
		ApplyStyleDrivenSubclassGap(Widget, UButton::StaticClass(), Entries);
	}
	else if (const UBorder* Border = Cast<UBorder>(Widget))
	{
		Entries = {ResolveBorder(Border, Context)};
		ApplyStyleDrivenSubclassGap(Widget, UBorder::StaticClass(), Entries);
	}
	else if (const UImage* Image = Cast<UImage>(Widget))
	{
		Entries = {ResolveImage(Image, Context)};
		ApplyStyleDrivenSubclassGap(Widget, UImage::StaticClass(), Entries);
	}
	else if (const UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
	{
		Entries = {ResolveText(TextBlock, Context)};
		ApplyStyleDrivenSubclassGap(Widget, UTextBlock::StaticClass(), Entries);
	}

	// SCheckBox, URichTextBlock/CommonRichTextBlock, and every other widget kind: no known formula.
	return Entries;
}

FWidgetComposedColorResolverContext FWidgetComposedColorResolver::MakeContext(const UWidgetBlueprint* WidgetBlueprint)
{
	FWidgetComposedColorResolverContext Context;
	if (!WidgetBlueprint)
	{
		return Context;
	}

	for (const FDelegateEditorBinding& Binding : WidgetBlueprint->Bindings)
	{
		Context.BoundProperties.Add(TPair<FName, FName>(FName(*Binding.ObjectName), Binding.PropertyName));
	}

	if (const UMVVMBlueprintView* View = FWidgetMVVMBindingUtils::FindView(WidgetBlueprint))
	{
		for (const FMVVMBlueprintViewBinding& Binding : View->GetBindings())
		{
			// A disabled binding never pushes a value.
			if (!Binding.bEnabled)
			{
				continue;
			}

			const FResolvedMVVMDestination Resolved = FWidgetMVVMBindingUtils::ResolveDestination(Binding.DestinationPath, WidgetBlueprint);
			if (!Resolved.bIsWidget)
			{
				continue;
			}

			// A setter destination (SetColorAndOpacity) drives the property it sets.
			FString PropertyName = Resolved.FieldName.ToString();
			if (!Resolved.bIsProperty)
			{
				PropertyName.RemoveFromStart(TEXT("Set"), ESearchCase::CaseSensitive);
			}
			Context.BoundProperties.Add(TPair<FName, FName>(Resolved.WidgetName, FName(*PropertyName)));
		}
	}

	if (const UUserWidget* OwnerDefaults = WidgetBlueprint->GeneratedClass ? Cast<UUserWidget>(WidgetBlueprint->GeneratedClass->GetDefaultObject()) : nullptr)
	{
		Context.OwnerColorAndOpacity = OwnerDefaults->GetColorAndOpacity();
		Context.OwnerRenderOpacity = OwnerDefaults->GetRenderOpacity();
	}

	return Context;
}

TArray<FWidgetComposedColorEntry> FWidgetComposedColorResolver::ResolveButton(const UButton* Button, const FWidgetComposedColorResolverContext& Context)
{
	TArray<FWidgetComposedColorEntry> Entries;
	if (!Button)
	{
		return Entries;
	}

	// SButton::OnPaint (SButton.cpp:170-189):
	//   Brush->GetTint(WS) * WS.GetColorAndOpacityTint() * GetBorderBackgroundColor().GetColor(WS)
	// State only selects which of the four style brushes supplies the brush-tint factor; the other
	// two factors are shared across every state.
	const FButtonStyle& Style = Button->GetStyle();
	const FLinearColor BackgroundColor = Button->GetBackgroundColor();

	FLinearColor AncestorTint = FLinearColor::White;
	TArray<FWidgetColorResolutionGap> SharedGaps;
	const bool bAncestorResolved = TryResolveAncestorTint(Button, Context, AncestorTint, SharedGaps);
	const bool bStyleNotBound = RequireNotBound(Context, Button, TEXT("WidgetStyle"), EColorResolutionGapKind::OwnColorDelegateBound, SharedGaps);
	const bool bBackgroundNotBound = RequireNotBound(Context, Button, TEXT("BackgroundColor"), EColorResolutionGapKind::OwnColorDelegateBound, SharedGaps);

	// SButton::GetShowDisabledEffect: a NoDrawType Disabled brush means a disabled button keeps drawing
	// its current (Normal, since it can't be hovered or pressed) brush with DisabledEffect instead.
	const bool bDisabledDrawsNormal = Style.Disabled.DrawAs == ESlateBrushDrawType::NoDrawType;

	struct FStateBrush
	{
		EWidgetComposedColorState State;
		const FSlateBrush* Brush;
		const TCHAR* BrushFactorLabel;
	};
	const FStateBrush StateBrushes[] = {
		{EWidgetComposedColorState::Normal, &Style.Normal, TEXT("Brush.TintColor")},
		{EWidgetComposedColorState::Hovered, &Style.Hovered, TEXT("Brush.TintColor")},
		{EWidgetComposedColorState::Pressed, &Style.Pressed, TEXT("Brush.TintColor")},
		bDisabledDrawsNormal
			? FStateBrush{EWidgetComposedColorState::Disabled, &Style.Normal, TEXT("Brush.TintColor (Normal brush, drawn with DisabledEffect)")}
			: FStateBrush{EWidgetComposedColorState::Disabled, &Style.Disabled, TEXT("Brush.TintColor")},
	};

	for (const FStateBrush& StateBrush : StateBrushes)
	{
		FWidgetComposedColorEntry Entry;
		Entry.State = StateBrush.State;
		Entry.CompositionFactors = {StateBrush.BrushFactorLabel, TEXT("AncestorColorAndOpacityTint"), TEXT("BackgroundColor")};

		FLinearColor BrushTint = FLinearColor::White;
		const bool bBrushResolved = TryResolveBrushTintFactor(*StateBrush.Brush, StateBrush.BrushFactorLabel, BrushTint, Entry.Gaps);

		Entry.Gaps.Append(SharedGaps);

		if (bBrushResolved && bAncestorResolved && bStyleNotBound && bBackgroundNotBound)
		{
			Entry.ComposedLinearColor = BrushTint * AncestorTint * BackgroundColor;
			Entry.bResolved = true;
		}

		Entries.Add(MoveTemp(Entry));
	}

	return Entries;
}

FWidgetComposedColorEntry FWidgetComposedColorResolver::ResolveBorder(const UBorder* Border, const FWidgetComposedColorResolverContext& Context)
{
	FWidgetComposedColorEntry Entry;
	if (!Border)
	{
		return Entry;
	}

	// SBorder::OnPaint (SBorder.cpp:114-121) -- identical 3-factor chain to SButton (SButton is built
	// on SBorder), and never branches on interaction state:
	//   BrushResource->GetTint(WS) * WS.GetColorAndOpacityTint() * BorderBackgroundColorAttribute.Get().GetColor(WS)
	Entry.State = EWidgetComposedColorState::Normal;
	Entry.CompositionFactors = {TEXT("Brush.TintColor"), TEXT("AncestorColorAndOpacityTint"), TEXT("BackgroundColor")};

	FLinearColor BrushTint = FLinearColor::White;
	const bool bBrushTintResolved = TryResolveBrushTintFactor(Border->Background, TEXT("Brush.TintColor"), BrushTint, Entry.Gaps);
	const bool bBrushNotBound = RequireNotBound(Context, Border, TEXT("Background"), EColorResolutionGapKind::OwnColorDelegateBound, Entry.Gaps);

	FLinearColor AncestorTint = FLinearColor::White;
	const bool bAncestorResolved = TryResolveAncestorTint(Border, Context, AncestorTint, Entry.Gaps);

	// UBorder::BrushColor feeds SBorder's BorderBackgroundColor attribute (Border.cpp:73,77); a plain
	// FLinearColor, always Specified once it isn't bound.
	const FLinearColor BackgroundColor = Border->GetBrushColor();
	const bool bBackgroundNotBound = RequireNotBound(Context, Border, TEXT("BrushColor"), EColorResolutionGapKind::OwnColorDelegateBound, Entry.Gaps);

	if (bBrushTintResolved && bBrushNotBound && bAncestorResolved && bBackgroundNotBound)
	{
		Entry.ComposedLinearColor = BrushTint * AncestorTint * BackgroundColor;
		Entry.bResolved = true;
	}

	return Entry;
}

FWidgetComposedColorEntry FWidgetComposedColorResolver::ResolveImage(const UImage* Image, const FWidgetComposedColorResolverContext& Context)
{
	FWidgetComposedColorEntry Entry;
	if (!Image)
	{
		return Entry;
	}

	// SImage::OnPaint (SImage.cpp:59) -- the widget's own ColorAndOpacity takes BackgroundColor's place,
	// and it never branches on interaction state:
	//   WS.GetColorAndOpacityTint() * ColorAndOpacityAttribute.Get().GetColor(WS) * ImageBrush->GetTint(WS)
	Entry.State = EWidgetComposedColorState::Normal;
	Entry.CompositionFactors = {TEXT("AncestorColorAndOpacityTint"), TEXT("ColorAndOpacity"), TEXT("Brush.TintColor")};

	FLinearColor AncestorTint = FLinearColor::White;
	const bool bAncestorResolved = TryResolveAncestorTint(Image, Context, AncestorTint, Entry.Gaps);

	// UImage::ColorAndOpacity is a plain FLinearColor (converted to an always-Specified FSlateColor at
	// the SImage binding boundary), so only a binding can make it unresolvable.
	const FLinearColor OwnColor = Image->GetColorAndOpacity();
	const bool bOwnColorNotBound = RequireNotBound(Context, Image, TEXT("ColorAndOpacity"), EColorResolutionGapKind::OwnColorDelegateBound, Entry.Gaps);

	FLinearColor BrushTint = FLinearColor::White;
	const bool bBrushTintResolved = TryResolveBrushTintFactor(Image->GetBrush(), TEXT("Brush.TintColor"), BrushTint, Entry.Gaps);
	const bool bBrushNotBound = RequireNotBound(Context, Image, TEXT("Brush"), EColorResolutionGapKind::OwnColorDelegateBound, Entry.Gaps);

	if (bAncestorResolved && bOwnColorNotBound && bBrushTintResolved && bBrushNotBound)
	{
		Entry.ComposedLinearColor = AncestorTint * OwnColor * BrushTint;
		Entry.bResolved = true;
	}

	return Entry;
}

FWidgetComposedColorEntry FWidgetComposedColorResolver::ResolveText(const UTextBlock* TextBlock, const FWidgetComposedColorResolverContext& Context)
{
	FWidgetComposedColorEntry Entry;
	if (!TextBlock)
	{
		return Entry;
	}

	// STextBlock::OnPaint (STextBlock.cpp:314) -- two factors, no brush term at all (the drop-shadow
	// colour is a separate, parallel chain at STextBlock.cpp:296, not fused into this one), and never
	// branches on interaction state:
	//   WS.GetColorAndOpacityTint() * GetColorAndOpacity().GetColor(WS)
	Entry.State = EWidgetComposedColorState::Normal;
	Entry.CompositionFactors = {TEXT("AncestorColorAndOpacityTint"), TEXT("ColorAndOpacity")};

	FLinearColor AncestorTint = FLinearColor::White;
	const bool bAncestorResolved = TryResolveAncestorTint(TextBlock, Context, AncestorTint, Entry.Gaps);

	// UTextBlock::ColorAndOpacity is a genuine FSlateColor -- it can be non-Specified or a colour-table
	// reference, unlike Button/Border/Image's own plain-FLinearColor factors.
	FLinearColor OwnColor = FLinearColor::White;
	const bool bOwnColorResolved = TryResolveSlateColorFactor(TextBlock->GetColorAndOpacity(), TEXT("ColorAndOpacity"), OwnColor, Entry.Gaps);
	const bool bOwnColorNotBound = RequireNotBound(Context, TextBlock, TEXT("ColorAndOpacity"), EColorResolutionGapKind::OwnColorDelegateBound, Entry.Gaps);

	if (bAncestorResolved && bOwnColorResolved && bOwnColorNotBound)
	{
		Entry.ComposedLinearColor = AncestorTint * OwnColor;
		Entry.bResolved = true;
	}

	return Entry;
}

bool FWidgetComposedColorResolver::TryResolveSlateColorFactor(const FSlateColor& Color, const FString& FactorLabel,
	FLinearColor& OutColor, TArray<FWidgetColorResolutionGap>& OutGaps)
{
	// FSlateColor's ColorUseRule has no public getter (only IsColorSpecified(), which conflates
	// Specified and ColorTable together) because the engine never needs to tell them apart from the
	// outside -- GetColor(WS) already does the right thing at paint time. Reading the reflected
	// UPROPERTY directly (the same technique FWidgetPropertyReflectionUtils uses generically) is the only
	// way to distinguish "Specified" from "ColorTable" from "Foreground/Subdued/UseStyle" statically.
	static const FProperty* ColorUseRuleProperty = FSlateColor::StaticStruct()->FindPropertyByName(TEXT("ColorUseRule"));

	ESlateColorStylingMode Mode = ESlateColorStylingMode::UseColor_Specified;
	if (ColorUseRuleProperty)
	{
		if (const uint8* ValuePtr = ColorUseRuleProperty->ContainerPtrToValuePtr<uint8>(&Color))
		{
			Mode = static_cast<ESlateColorStylingMode>(*ValuePtr);
		}
	}

	switch (Mode)
	{
	case ESlateColorStylingMode::UseColor_Specified:
		OutColor = Color.GetSpecifiedColor();
		return true;

	case ESlateColorStylingMode::UseColor_ColorTable:
		OutGaps.Add(FWidgetColorResolutionGap{EColorResolutionGapKind::UnserializedColorTable,
			FString::Printf(TEXT("%s references a colour table entry (ColorTableId), which is not serialized and cannot be resolved statically."), *FactorLabel)});
		return false;

	default: // UseColor_Foreground, UseColor_Foreground_Subdued, UseColor_UseStyle
		OutGaps.Add(FWidgetColorResolutionGap{EColorResolutionGapKind::NonSpecifiedColorMode,
			FString::Printf(TEXT("%s is not in Specified colour mode -- it resolves from the runtime widget style, not a stored value."), *FactorLabel)});
		return false;
	}
}

bool FWidgetComposedColorResolver::TryResolveBrushTintFactor(const FSlateBrush& Brush, const FString& FactorLabel,
	FLinearColor& OutColor, TArray<FWidgetColorResolutionGap>& OutGaps)
{
	// SButton/SBorder/SImage all skip drawing a NoDrawType brush entirely.
	if (Brush.DrawAs == ESlateBrushDrawType::NoDrawType)
	{
		OutGaps.Add(FWidgetColorResolutionGap{EColorResolutionGapKind::BrushNotDrawn,
			FString::Printf(TEXT("%s's brush is NoDrawType, so nothing is drawn for it."), *FactorLabel)});
		return false;
	}

	bool bResolved = TryResolveSlateColorFactor(Brush.TintColor, FactorLabel, OutColor, OutGaps);

	if (Brush.GetResourceObject() != nullptr)
	{
		OutGaps.Add(FWidgetColorResolutionGap{EColorResolutionGapKind::MaterialOrTextureResource,
			FString::Printf(TEXT("%s's brush has a texture or material resource; its tint alone does not determine the rendered pixel."), *FactorLabel)});
		bResolved = false;
	}

	return bResolved;
}

bool FWidgetComposedColorResolver::TryResolveAncestorTint(const UWidget* Widget, const FWidgetComposedColorResolverContext& Context,
	FLinearColor& OutTint, TArray<FWidgetColorResolutionGap>& OutGaps)
{
	OutTint = FLinearColor::White;

	if (!Widget)
	{
		return true;
	}

	// SWidget::Paint blends a widget's own RenderOpacity into the style before its OnPaint runs.
	if (!RequireNotBound(Context, Widget, TEXT("RenderOpacity"), EColorResolutionGapKind::OwnColorDelegateBound, OutGaps))
	{
		return false;
	}
	OutTint.A *= Widget->GetRenderOpacity();

	const UWidget* Current = Widget;
	while (Current)
	{
		UPanelWidget* ParentPanel = Current->GetParent();
		if (!ParentPanel)
		{
			break;
		}

		if (!RequireNotBound(Context, ParentPanel, TEXT("RenderOpacity"), EColorResolutionGapKind::UnresolvedAncestorChain, OutGaps))
		{
			return false;
		}
		OutTint.A *= ParentPanel->GetRenderOpacity();

		if (const UButton* AncestorButton = Cast<UButton>(ParentPanel))
		{
			if (!RequireNotBound(Context, AncestorButton, TEXT("ColorAndOpacity"), EColorResolutionGapKind::UnresolvedAncestorChain, OutGaps))
			{
				return false;
			}
			OutTint *= AncestorButton->GetColorAndOpacity();
		}
		else if (const UBorder* AncestorBorder = Cast<UBorder>(ParentPanel))
		{
			if (!RequireNotBound(Context, AncestorBorder, TEXT("ContentColorAndOpacity"), EColorResolutionGapKind::UnresolvedAncestorChain, OutGaps))
			{
				return false;
			}
			OutTint *= AncestorBorder->GetContentColorAndOpacity();
		}
		// Every other UPanelWidget kind (Overlay, VerticalBox, CanvasPanel, ScrollBox, etc.) is a
		// confirmed pass-through: SPanel-derived Slate widgets forward the incoming FWidgetStyle to
		// their children unchanged -- only SCompoundWidget-based widgets compound ColorAndOpacity, and
		// Button/Border are the only two in the v1 formula set that both do so and expose it to UMG.

		Current = ParentPanel;
	}

	if (IsNamedSlotContent(Current))
	{
		OutGaps.Add(FWidgetColorResolutionGap{EColorResolutionGapKind::UnresolvedAncestorChain,
			FString::Printf(TEXT("%s sits in a named slot; the slot host's own widget tree around it can tint it and is not resolved."), *Current->GetName())});
		return false;
	}

	if (!Context.OwnerColorAndOpacity.Equals(FLinearColor::White) || !FMath::IsNearlyEqual(Context.OwnerRenderOpacity, 1.0f))
	{
		OutGaps.Add(FWidgetColorResolutionGap{EColorResolutionGapKind::OwnerWidgetTint,
			FString::Printf(TEXT("The Widget Blueprint's own ColorAndOpacity (%s) and RenderOpacity (%g) tint every widget it draws, and a placed instance can override them."),
				*Context.OwnerColorAndOpacity.ToString(), Context.OwnerRenderOpacity)});
		return false;
	}

	return true;
}

bool FWidgetComposedColorResolver::RequireNotBound(const FWidgetComposedColorResolverContext& Context, const UWidget* Widget, FName PropertyName,
	EColorResolutionGapKind Kind, TArray<FWidgetColorResolutionGap>& OutGaps)
{
	if (!Context.BoundProperties.Contains(TPair<FName, FName>(Widget->GetFName(), PropertyName)))
	{
		return true;
	}

	OutGaps.Add(FWidgetColorResolutionGap{Kind,
		FString::Printf(TEXT("%s's %s is bound (legacy property binding or MVVM), not a static value; resolving it would require running the Blueprint or ViewModel."),
			*Widget->GetName(), *PropertyName.ToString())});
	return false;
}
