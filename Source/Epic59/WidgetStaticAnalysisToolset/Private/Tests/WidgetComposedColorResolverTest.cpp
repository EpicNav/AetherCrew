// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "WidgetComposedColorResolver.h"
#include "WidgetComposedColorResolverTestFixtures.h"
#include "WidgetStaticAnalysisSchemaTestFlags.h"

#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/Image.h"
#include "Components/RichTextBlock.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

BEGIN_DEFINE_SPEC(
	FWidgetComposedColorResolverTest,
	"AIAssistant.Toolsets.WidgetStaticAnalysisToolset.ComposedColorResolver",
	WidgetStaticAnalysisSchemaTest::Flags)

/** The brush FButtonStyle stores for State itself, before SButton's NoDrawType Disabled fallback. */
static const FSlateBrush* StateOwnBrush(const FButtonStyle& Style, EWidgetComposedColorState State);

/** True when Entry has at least one Gap of Kind. */
static bool HasGap(const FWidgetComposedColorEntry& Entry, EColorResolutionGapKind Kind);

END_DEFINE_SPEC(FWidgetComposedColorResolverTest)

const FSlateBrush* FWidgetComposedColorResolverTest::StateOwnBrush(const FButtonStyle& Style, EWidgetComposedColorState State)
{
	switch (State)
	{
	case EWidgetComposedColorState::Hovered:
		return &Style.Hovered;
	case EWidgetComposedColorState::Pressed:
		return &Style.Pressed;
	case EWidgetComposedColorState::Disabled:
		return &Style.Disabled;
	default:
		return &Style.Normal;
	}
}

bool FWidgetComposedColorResolverTest::HasGap(const FWidgetComposedColorEntry& Entry, EColorResolutionGapKind Kind)
{
	return Entry.Gaps.ContainsByPredicate([Kind](const FWidgetColorResolutionGap& Gap) { return Gap.Kind == Kind; });
}

void FWidgetComposedColorResolverTest::Define()
{
	Describe("v1 formula coverage", [this]()
	{
		It("Button: one row per interactable state, fully resolved by default", [this]()
		{
			UButton* Button = NewObject<UButton>(GetTransientPackage());

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(Button, {});

			TestEqual(TEXT("one entry per interactable state"), Entries.Num(), 4);
			if (Entries.Num() != 4) return;

			const EWidgetComposedColorState ExpectedStates[] = {
				EWidgetComposedColorState::Normal,
				EWidgetComposedColorState::Hovered,
				EWidgetComposedColorState::Pressed,
				EWidgetComposedColorState::Disabled,
			};
			const TArray<FString> ExpectedFactors = {TEXT("Brush.TintColor"), TEXT("AncestorColorAndOpacityTint"), TEXT("BackgroundColor")};

			// Real per-state style brushes -- not assumed to be white. The engine/editor's own default
			// FButtonStyle (FDefaultStyleCache::GetEditor().GetButtonStyle(), Button.cpp) legitimately
			// tints its Hovered/Pressed/Disabled brushes differently from Normal (a genuine visual-feedback
			// design choice in the real default style, confirmed the first time this test actually ran --
			// hardcoding "white" for every state was an unverified assumption, not a real contract). Each
			// state's own brush TintColor is read directly here so the expected value tracks whatever the
			// live default style actually specifies, matching ResolveButton's own documented formula
			// (Brush.GetTint(WS) * AncestorColorAndOpacityTint * BackgroundColor) instead of guessing it.
			const FButtonStyle& Style = Button->GetStyle();
			const FSlateBrush* DisabledRowBrush = Style.Disabled.DrawAs == ESlateBrushDrawType::NoDrawType ? &Style.Normal : &Style.Disabled;
			const FSlateBrush* ExpectedStateBrushes[] = {&Style.Normal, &Style.Hovered, &Style.Pressed, DisabledRowBrush};
			const FLinearColor BackgroundColor = Button->GetBackgroundColor();

			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				const FWidgetComposedColorEntry& Entry = Entries[Index];
				TestTrue(TEXT("state order matches Normal/Hovered/Pressed/Disabled"),
					Entry.State == ExpectedStates[Index]);
				if (ExpectedStateBrushes[Index] == StateOwnBrush(Style, Entry.State))
				{
					TestEqual(TEXT("composition factors, in evaluation order"), Entry.CompositionFactors, ExpectedFactors);
				}
				TestEqual(TEXT("default Button style resolves with no Gaps"), Entry.Gaps.Num(), 0);
				TestTrue(TEXT("bResolved"), Entry.bResolved);

				const FLinearColor ExpectedColor = ExpectedStateBrushes[Index]->TintColor.GetSpecifiedColor() * BackgroundColor;
				TestEqual(TEXT("composed colour matches this state's own brush tint x background (no ancestor tint on a root widget)"),
					Entry.ComposedLinearColor, ExpectedColor);
			}
		});

		It("Border: a single Normal row, fully resolved by default", [this]()
		{
			UBorder* Border = NewObject<UBorder>(GetTransientPackage());

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(Border, {});

			TestEqual(TEXT("single Normal row -- SBorder's OnPaint never branches on state"), Entries.Num(), 1);
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("state is Normal"), Entries[0].State == EWidgetComposedColorState::Normal);
			TestEqual(TEXT("composition factors, in evaluation order"), Entries[0].CompositionFactors,
				TArray<FString>{TEXT("Brush.TintColor"), TEXT("AncestorColorAndOpacityTint"), TEXT("BackgroundColor")});
			TestEqual(TEXT("default Border resolves with no Gaps"), Entries[0].Gaps.Num(), 0);
			TestTrue(TEXT("bResolved"), Entries[0].bResolved);
			TestEqual(TEXT("default composed colour is white"), Entries[0].ComposedLinearColor, FLinearColor::White);
		});

		It("Image: a single Normal row, fully resolved by default", [this]()
		{
			UImage* Image = NewObject<UImage>(GetTransientPackage());

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(Image, {});

			TestEqual(TEXT("single Normal row -- SImage's OnPaint never branches on state"), Entries.Num(), 1);
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("state is Normal"), Entries[0].State == EWidgetComposedColorState::Normal);
			TestEqual(TEXT("composition factors, in evaluation order"), Entries[0].CompositionFactors,
				TArray<FString>{TEXT("AncestorColorAndOpacityTint"), TEXT("ColorAndOpacity"), TEXT("Brush.TintColor")});
			TestEqual(TEXT("default Image resolves with no Gaps"), Entries[0].Gaps.Num(), 0);
			TestTrue(TEXT("bResolved"), Entries[0].bResolved);
			TestEqual(TEXT("default composed colour is white"), Entries[0].ComposedLinearColor, FLinearColor::White);
		});

		It("Text: a single Normal row, fully resolved by default", [this]()
		{
			UTextBlock* TextBlock = NewObject<UTextBlock>(GetTransientPackage());

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(TextBlock, {});

			TestEqual(TEXT("single Normal row -- STextBlock's OnPaint never branches on state"), Entries.Num(), 1);
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("state is Normal"), Entries[0].State == EWidgetComposedColorState::Normal);
			TestEqual(TEXT("composition factors, in evaluation order (no brush term)"), Entries[0].CompositionFactors,
				TArray<FString>{TEXT("AncestorColorAndOpacityTint"), TEXT("ColorAndOpacity")});
			TestEqual(TEXT("default Text resolves with no Gaps"), Entries[0].Gaps.Num(), 0);
			TestTrue(TEXT("bResolved"), Entries[0].bResolved);
			TestEqual(TEXT("default composed colour is white"), Entries[0].ComposedLinearColor, FLinearColor::White);
		});
	});

	Describe("unhandled widget kinds", [this]()
	{
		It("SCheckBox (UCheckBox) reports an empty ComposedColors array", [this]()
		{
			UCheckBox* CheckBox = NewObject<UCheckBox>(GetTransientPackage());

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(CheckBox, {});

			TestEqual(TEXT("no known formula for SCheckBox (a pure SCompoundWidget, no OnPaint of its own)"), Entries.Num(), 0);
		});

		It("URichTextBlock reports an empty ComposedColors array", [this]()
		{
			URichTextBlock* RichTextBlock = NewObject<URichTextBlock>(GetTransientPackage());

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(RichTextBlock, {});

			TestEqual(TEXT("no known formula for URichTextBlock (per-inline-tag-run style DataTable model)"), Entries.Num(), 0);
		});
	});

	Describe("Gaps honesty", [this]()
	{
		It("a brush backed by a texture/material resource reports a Gap, never a guessed colour", [this]()
		{
			UImage* Image = NewObject<UImage>(GetTransientPackage());
			// A concrete, non-abstract UObject subclass -- plain UObject itself is UCLASS(Abstract) and
			// NewObject<UObject> trips a "Class which was marked abstract" ensure (an Error-severity log
			// line, which automation treats as a test failure) rather than constructing anything. Only
			// this pointer's non-null-ness matters to TryResolveBrushTintFactor's own check below, so any
			// concrete class would do; UTexture2D is the semantically obvious "brush resource" stand-in.
			UTexture2D* FakeResource = NewObject<UTexture2D>(GetTransientPackage(), FName(TEXT("WidgetComposedColorResolverTest_FakeResource")));
			Image->SetBrushResourceObject(FakeResource);

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(Image, {});

			TestEqual(TEXT("still a single Normal row"), Entries.Num(), 1);
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("brush resource produces a Gap"), Entries[0].Gaps.ContainsByPredicate(
				[](const FWidgetColorResolutionGap& Gap) { return Gap.Kind == EColorResolutionGapKind::MaterialOrTextureResource; }));
			TestEqual(TEXT("ComposedLinearColor stays at its default, never a guessed value"),
				Entries[0].ComposedLinearColor, FLinearColor::Black);
		});

		It("a ColorAndOpacity in ColorTable mode reports an UnserializedColorTable Gap, never a guessed colour", [this]()
		{
			UTextBlock* TextBlock = NewObject<UTextBlock>(GetTransientPackage());
			// ColorTableId is a private, non-UPROPERTY field -- never serialized, so this mode can
			// never be trusted statically regardless of which table entry it names.
			TextBlock->SetColorAndOpacity(FSlateColor(EStyleColor::White));

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(TextBlock, {});

			TestEqual(TEXT("still a single Normal row"), Entries.Num(), 1);
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("ColorTable mode produces an UnserializedColorTable Gap"), Entries[0].Gaps.ContainsByPredicate(
				[](const FWidgetColorResolutionGap& Gap) { return Gap.Kind == EColorResolutionGapKind::UnserializedColorTable; }));
			TestEqual(TEXT("ComposedLinearColor stays at its default, never a guessed value"),
				Entries[0].ComposedLinearColor, FLinearColor::Black);
		});

		It("a bound ancestor Border's ContentColorAndOpacity reports an UnresolvedAncestorChain Gap on a nested widget", [this]()
		{
			UBorder* AncestorBorder = NewObject<UBorder>(GetTransientPackage());
			UTextBlock* ChildText = NewObject<UTextBlock>(GetTransientPackage());
			AncestorBorder->SetContent(ChildText);

			// Design-time widgets never have their bindings applied, so boundness comes from the Blueprint.
			FWidgetComposedColorResolverContext Context;
			Context.BoundProperties.Add(TPair<FName, FName>(AncestorBorder->GetFName(), TEXT("ContentColorAndOpacity")));

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(ChildText, Context);

			TestEqual(TEXT("still a single Normal row"), Entries.Num(), 1);
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("bound ancestor tint produces an UnresolvedAncestorChain Gap"), HasGap(Entries[0], EColorResolutionGapKind::UnresolvedAncestorChain));
			TestFalse(TEXT("not resolved"), Entries[0].bResolved);
			TestEqual(TEXT("ComposedLinearColor stays at its default, never a guessed value"),
				Entries[0].ComposedLinearColor, FLinearColor::Black);
		});

		It("a bound widget's OWN Background reports OwnColorDelegateBound, not UnresolvedAncestorChain", [this]()
		{
			// The widget's own value being dynamic is not an ancestor-chain problem, so it must report the
			// distinct OwnColorDelegateBound kind.
			UBorder* Border = NewObject<UBorder>(GetTransientPackage());

			FWidgetComposedColorResolverContext Context;
			Context.BoundProperties.Add(TPair<FName, FName>(Border->GetFName(), TEXT("Background")));

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(Border, Context);

			TestEqual(TEXT("still a single Normal row"), Entries.Num(), 1);
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("the widget's own bound Brush produces an OwnColorDelegateBound Gap"), HasGap(Entries[0], EColorResolutionGapKind::OwnColorDelegateBound));
			TestFalse(TEXT("never mislabeled as UnresolvedAncestorChain -- this widget has no ancestor at all"),
				HasGap(Entries[0], EColorResolutionGapKind::UnresolvedAncestorChain));
			TestEqual(TEXT("ComposedLinearColor stays at its default, never a guessed value"),
				Entries[0].ComposedLinearColor, FLinearColor::Black);
		});

		It("a bound Button BackgroundColor reports OwnColorDelegateBound on every state row", [this]()
		{
			UButton* Button = NewObject<UButton>(GetTransientPackage());

			FWidgetComposedColorResolverContext Context;
			Context.BoundProperties.Add(TPair<FName, FName>(Button->GetFName(), TEXT("BackgroundColor")));

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(Button, Context);
			TestEqual(TEXT("four rows"), Entries.Num(), 4);
			for (const FWidgetComposedColorEntry& Entry : Entries)
			{
				TestTrue(TEXT("OwnColorDelegateBound"), HasGap(Entry, EColorResolutionGapKind::OwnColorDelegateBound));
				TestFalse(TEXT("not resolved"), Entry.bResolved);
			}
		});

		It("a NoDrawType Border brush reports BrushNotDrawn", [this]()
		{
			UBorder* Border = NewObject<UBorder>(GetTransientPackage());
			FSlateBrush Brush = Border->Background;
			Brush.DrawAs = ESlateBrushDrawType::NoDrawType;
			Border->SetBrush(Brush);

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(Border, {});
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("BrushNotDrawn"), HasGap(Entries[0], EColorResolutionGapKind::BrushNotDrawn));
			TestFalse(TEXT("not resolved"), Entries[0].bResolved);
		});

		It("a NoDrawType Disabled brush makes the Disabled row use the Normal brush, as SButton does", [this]()
		{
			UButton* Button = NewObject<UButton>(GetTransientPackage());
			FButtonStyle Style = Button->GetStyle();
			Style.Normal.DrawAs = ESlateBrushDrawType::Box;
			Style.Normal.TintColor = FSlateColor(FLinearColor(0.2f, 0.4f, 0.6f, 1.0f));
			Style.Normal.SetResourceObject(nullptr);
			Style.Disabled.DrawAs = ESlateBrushDrawType::NoDrawType;
			Button->SetStyle(Style);

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(Button, {});
			if (Entries.Num() != 4) return;

			const FWidgetComposedColorEntry& Disabled = Entries[3];
			TestTrue(TEXT("Disabled row"), Disabled.State == EWidgetComposedColorState::Disabled);
			TestFalse(TEXT("no BrushNotDrawn: the Normal brush is drawn"), HasGap(Disabled, EColorResolutionGapKind::BrushNotDrawn));
			TestTrue(TEXT("resolved"), Disabled.bResolved);
			TestEqual(TEXT("Disabled row uses the Normal brush tint"), Disabled.ComposedLinearColor,
				FLinearColor(0.2f, 0.4f, 0.6f, 1.0f) * Button->GetBackgroundColor());
			TestTrue(TEXT("factor names the substituted brush"), Disabled.CompositionFactors.Num() > 0 && Disabled.CompositionFactors[0].Contains(TEXT("DisabledEffect")));
		});

		It("a non-neutral owning UserWidget tint reports OwnerWidgetTint", [this]()
		{
			UTextBlock* TextBlock = NewObject<UTextBlock>(GetTransientPackage());

			FWidgetComposedColorResolverContext Context;
			Context.OwnerColorAndOpacity = FLinearColor(1.0f, 0.0f, 0.0f, 1.0f);

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(TextBlock, Context);
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("OwnerWidgetTint"), HasGap(Entries[0], EColorResolutionGapKind::OwnerWidgetTint));
			TestFalse(TEXT("not resolved"), Entries[0].bResolved);
		});

		It("a native subclass of a formula widget reports StyleDrivenSubclass", [this]()
		{
			UWidgetComposedColorResolverTestTextBlock* TextBlock = NewObject<UWidgetComposedColorResolverTestTextBlock>(GetTransientPackage());

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(TextBlock, {});
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("StyleDrivenSubclass"), HasGap(Entries[0], EColorResolutionGapKind::StyleDrivenSubclass));
			TestFalse(TEXT("not resolved"), Entries[0].bResolved);
		});
	});

	Describe("ancestor tint compounding", [this]()
	{
		It("an ancestor Button's ColorAndOpacity compounds into a nested widget's composed colour", [this]()
		{
			UButton* AncestorButton = NewObject<UButton>(GetTransientPackage());
			const FLinearColor AncestorTint(0.25f, 0.5f, 0.75f, 1.0f);
			AncestorButton->SetColorAndOpacity(AncestorTint);

			UTextBlock* ChildText = NewObject<UTextBlock>(GetTransientPackage());
			AncestorButton->SetContent(ChildText);

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(ChildText, {});

			TestEqual(TEXT("single Normal row for the nested Text"), Entries.Num(), 1);
			if (Entries.Num() != 1) return;

			TestEqual(TEXT("no Gaps -- UButton::ColorAndOpacity has no bindable delegate, always statically known"),
				Entries[0].Gaps.Num(), 0);
			TestEqual(TEXT("composed colour is the ancestor Button's tint (Text's own ColorAndOpacity defaults to white)"),
				Entries[0].ComposedLinearColor, AncestorTint);
		});

		It("an ancestor Border's ContentColorAndOpacity compounds into a nested widget's composed colour", [this]()
		{
			UBorder* AncestorBorder = NewObject<UBorder>(GetTransientPackage());
			const FLinearColor AncestorTint(0.1f, 0.2f, 0.3f, 1.0f);
			AncestorBorder->SetContentColorAndOpacity(AncestorTint);

			UImage* ChildImage = NewObject<UImage>(GetTransientPackage());
			AncestorBorder->SetContent(ChildImage);

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(ChildImage, {});

			TestEqual(TEXT("single Normal row for the nested Image"), Entries.Num(), 1);
			if (Entries.Num() != 1) return;

			TestEqual(TEXT("no Gaps -- the ancestor Border's ContentColorAndOpacity delegate isn't bound"),
				Entries[0].Gaps.Num(), 0);
			TestEqual(TEXT("composed colour is the ancestor Border's tint (Image's own factors default to white)"),
				Entries[0].ComposedLinearColor, AncestorTint);
		});

		It("the widget's own RenderOpacity and every ancestor's multiply into alpha", [this]()
		{
			UBorder* AncestorBorder = NewObject<UBorder>(GetTransientPackage());
			AncestorBorder->SetRenderOpacity(0.5f);

			UTextBlock* ChildText = NewObject<UTextBlock>(GetTransientPackage());
			ChildText->SetRenderOpacity(0.5f);
			AncestorBorder->SetContent(ChildText);

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(ChildText, {});
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("resolved"), Entries[0].bResolved);
			TestEqual(TEXT("alpha is 0.5 * 0.5"), Entries[0].ComposedLinearColor, FLinearColor(1.0f, 1.0f, 1.0f, 0.25f));
		});

		It("a bound ancestor RenderOpacity reports UnresolvedAncestorChain", [this]()
		{
			UBorder* AncestorBorder = NewObject<UBorder>(GetTransientPackage());
			UTextBlock* ChildText = NewObject<UTextBlock>(GetTransientPackage());
			AncestorBorder->SetContent(ChildText);

			FWidgetComposedColorResolverContext Context;
			Context.BoundProperties.Add(TPair<FName, FName>(AncestorBorder->GetFName(), TEXT("RenderOpacity")));

			const TArray<FWidgetComposedColorEntry> Entries = FWidgetComposedColorResolver::Resolve(ChildText, Context);
			if (Entries.Num() != 1) return;

			TestTrue(TEXT("UnresolvedAncestorChain"), HasGap(Entries[0], EColorResolutionGapKind::UnresolvedAncestorChain));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
