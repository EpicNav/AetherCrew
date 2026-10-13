// Games by Hyper 2026.

#include "HyperAIStudioUIRenderReview.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxKey.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/PanelWidget.h"
#include "Components/RichTextBlock.h"
#include "Components/Slider.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/UserInterfaceSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioNames.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"

namespace HyperAIStudio::UI::Review
{
	namespace
	{
		constexpr int32 MaxResolutions = 4;
		/** A text box this much bigger than its slot is clipped, not rounding. */
		constexpr double SlackUnits = 1.0;
		constexpr double EdgeSlackPixels = 1.0;

		void Add(TArray<FHyperAIUIReviewIssue>& Out, const TCHAR* Code, const TCHAR* Severity, const FWidgetBox* Box, const FString& Resolution,
			const FString& Message)
		{
			FHyperAIUIReviewIssue& Issue = Out.AddDefaulted_GetRef();
			Issue.Code = Code;
			Issue.Severity = Severity;
			Issue.Widget = Box ? Box->Path : FString();
			Issue.Resolution = Resolution;
			Issue.Rect = Box ? RectText(Box->Rect) : FString();
			Issue.Message = Message;
		}

		bool IsAncestor(const FWidgetBox& Ancestor, const FWidgetBox& Box)
		{
			return Box.Path.StartsWith(Ancestor.Path + TEXT("/"));
		}

		bool Inside(const FBox2D& Inner, const FBox2D& Outer, const double Slack)
		{
			return Inner.Min.X >= Outer.Min.X - Slack && Inner.Min.Y >= Outer.Min.Y - Slack
				&& Inner.Max.X <= Outer.Max.X + Slack && Inner.Max.Y <= Outer.Max.Y + Slack;
		}

		double Area(const FBox2D& Box)
		{
			return FMath::Max(0.0, Box.Max.X - Box.Min.X) * FMath::Max(0.0, Box.Max.Y - Box.Min.Y);
		}

		FBox2D Overlap(const FBox2D& A, const FBox2D& B)
		{
			return FBox2D(FVector2D(FMath::Max(A.Min.X, B.Min.X), FMath::Max(A.Min.Y, B.Min.Y)),
				FVector2D(FMath::Min(A.Max.X, B.Max.X), FMath::Min(A.Max.Y, B.Max.Y)));
		}

		bool IsInteractive(const UWidget& Widget)
		{
			return Widget.IsA<UButton>() || Widget.IsA<UCheckBox>() || Widget.IsA<USlider>() || Widget.IsA<USpinBox>()
				|| Widget.IsA<UComboBoxString>() || Widget.IsA<UComboBoxKey>() || Widget.IsA<UEditableText>() || Widget.IsA<UEditableTextBox>()
				|| Widget.IsA<UMultiLineEditableText>() || Widget.IsA<UMultiLineEditableTextBox>();
		}

		FString PathOf(const UWidget& Widget)
		{
			TArray<FString> Names;
			for (const UWidget* Current = &Widget; Current && Names.Num() < 64; Current = Current->GetParent())
			{
				Names.Insert(Current->GetName(), 0);
			}
			return FString::Join(Names, TEXT("/"));
		}
	}

	bool ParseResolution(const FString& Text, FIntPoint& Out)
	{
		FString Left;
		FString Right;
		const FString Trimmed = Text.TrimStartAndEnd().ToLower();
		if (!Trimmed.Split(TEXT("x"), &Left, &Right) && !Trimmed.Split(TEXT("*"), &Left, &Right))
		{
			return false;
		}
		int32 Width = 0;
		int32 Height = 0;
		if (!Left.TrimStartAndEnd().IsNumeric() || !Right.TrimStartAndEnd().IsNumeric()
			|| !LexTryParseString(Width, *Left.TrimStartAndEnd()) || !LexTryParseString(Height, *Right.TrimStartAndEnd()))
		{
			return false;
		}
		if (Width < 320 || Height < 240 || Width > 7680 || Height > 4320)
		{
			return false;
		}
		Out = FIntPoint(Width, Height);
		return true;
	}

	float RelativeLuminance(const FLinearColor& Linear)
	{
		return 0.2126f * FMath::Clamp(Linear.R, 0.f, 1.f) + 0.7152f * FMath::Clamp(Linear.G, 0.f, 1.f) + 0.0722f * FMath::Clamp(Linear.B, 0.f, 1.f);
	}

	float ContrastRatio(const FLinearColor& A, const FLinearColor& B)
	{
		const float La = RelativeLuminance(A);
		const float Lb = RelativeLuminance(B);
		return (FMath::Max(La, Lb) + 0.05f) / (FMath::Min(La, Lb) + 0.05f);
	}

	bool EstimateBackground(const TArray<FColor>& Pixels, const FIntPoint Size, const FBox2D& Rect, const FLinearColor& Text, FLinearColor& OutBackground)
	{
		const int32 X0 = FMath::Clamp(FMath::FloorToInt32(Rect.Min.X), 0, Size.X);
		const int32 Y0 = FMath::Clamp(FMath::FloorToInt32(Rect.Min.Y), 0, Size.Y);
		const int32 X1 = FMath::Clamp(FMath::CeilToInt32(Rect.Max.X), 0, Size.X);
		const int32 Y1 = FMath::Clamp(FMath::CeilToInt32(Rect.Max.Y), 0, Size.Y);
		if (X1 <= X0 || Y1 <= Y0 || Pixels.Num() != Size.X * Size.Y)
		{
			return false;
		}
		// Sample at most about 20k pixels; anti-aliased glyph edges land between the two, so the most common colour that
		// is clearly not the text's is the background.
		const int32 Step = FMath::Max(1, FMath::CeilToInt32(FMath::Sqrt(static_cast<double>((X1 - X0)) * (Y1 - Y0) / 20000.0)));
		// Compared in sRGB bytes, as the eye sees them: the text's own colour, give or take anti-aliasing, is not background.
		const FColor TextBytes = Text.ToFColor(true);
		TMap<uint32, TPair<int32, FLinearColor>> Bins;
		for (int32 Pass = 0; Pass < 2 && Bins.IsEmpty(); ++Pass)
		{
			for (int32 Y = Y0; Y < Y1; Y += Step)
			{
				for (int32 X = X0; X < X1; X += Step)
				{
					const FColor& Pixel = Pixels[Y * Size.X + X];
					// Every pixel looks like the text: text and background are the same colour, which the second pass measures.
					if (Pass == 0 && FMath::Abs(Pixel.R - TextBytes.R) <= 12 && FMath::Abs(Pixel.G - TextBytes.G) <= 12 && FMath::Abs(Pixel.B - TextBytes.B) <= 12)
					{
						continue;
					}
					const uint32 Key = (Pixel.R >> 3) | ((Pixel.G >> 3) << 5) | ((Pixel.B >> 3) << 10);
					TPair<int32, FLinearColor>& Bin = Bins.FindOrAdd(Key, TPair<int32, FLinearColor>(0, FLinearColor(0, 0, 0, 0)));
					++Bin.Key;
					Bin.Value += FLinearColor(Pixel);
				}
			}
		}
		int32 Best = 0;
		for (const TPair<uint32, TPair<int32, FLinearColor>>& Bin : Bins)
		{
			if (Bin.Value.Key > Best)
			{
				Best = Bin.Value.Key;
				OutBackground = Bin.Value.Value / static_cast<float>(Bin.Value.Key);
				OutBackground.A = 1.f;
			}
		}
		return Best > 0;
	}

	FString RectText(const FBox2D& Rect)
	{
		return FString::Printf(TEXT("%d,%d %dx%d"), FMath::RoundToInt32(Rect.Min.X), FMath::RoundToInt32(Rect.Min.Y),
			FMath::RoundToInt32(Rect.Max.X - Rect.Min.X), FMath::RoundToInt32(Rect.Max.Y - Rect.Min.Y));
	}

	float FontPixels(const float FontSize, const float DpiScale)
	{
		return FontSize * DpiScale * 96.f / 72.f;
	}

	void CheckLayout(const TArray<FWidgetBox>& Boxes, const FIntPoint Screen, const FHyperAIUIRenderReviewRequest& Rules, const FString& Resolution,
		const TArray<FColor>* Pixels, TArray<FHyperAIUIReviewIssue>& Out)
	{
		const FBox2D ScreenRect(FVector2D::ZeroVector, FVector2D(Screen));
		const double Margin = (1.0 - FMath::Clamp(static_cast<double>(Rules.SafeZone), 0.5, 1.0)) * 0.5;
		const FBox2D SafeRect(FVector2D(Screen.X * Margin, Screen.Y * Margin), FVector2D(Screen.X * (1.0 - Margin), Screen.Y * (1.0 - Margin)));
		for (const FWidgetBox& Box : Boxes)
		{
			const FVector2D Size = Box.Rect.GetSize();
			// A backdrop filling the screen is meant to reach its edges.
			const bool bBackdrop = !Box.bInteractive && !Box.bText && Size.X >= Screen.X * 0.9 && Size.Y >= Screen.Y * 0.9;

			if (Box.bText)
			{
				const bool bTooWide = !Box.bWraps && Box.DesiredLocal.X > Box.AllottedLocal.X + SlackUnits;
				const bool bTooTall = Box.DesiredLocal.Y > Box.AllottedLocal.Y + SlackUnits;
				if (bTooWide || bTooTall)
				{
					Add(Out, TEXT("text_clipped"), TEXT("error"), &Box, Resolution, FString::Printf(
						TEXT("The text needs %.0f x %.0f but its slot gives it %.0f x %.0f (Slate units): it spills out%s, and is cut off wherever a parent clips. Let the slot size to content, wrap it, or shorten it."),
						Box.DesiredLocal.X, Box.DesiredLocal.Y, Box.AllottedLocal.X, Box.AllottedLocal.Y, bTooWide ? TEXT(" at the side") : TEXT(" at the bottom")));
				}
				if (Box.FontPixels > 0.f && Box.FontPixels < Rules.MinTextPixels)
				{
					Add(Out, TEXT("text_too_small"), Box.FontPixels < Rules.MinTextPixels * 0.75f ? TEXT("error") : TEXT("warning"), &Box, Resolution,
						FString::Printf(TEXT("Its glyphs are %.1f px tall here, under %.0f px. Raise the font size, or the DPI curve for this resolution."),
							Box.FontPixels, Rules.MinTextPixels));
				}
				FLinearColor Background;
				if (Pixels && Box.TextColor.IsSet() && EstimateBackground(*Pixels, Screen, Box.Rect, Box.TextColor.GetValue(), Background))
				{
					const float Ratio = ContrastRatio(Box.TextColor.GetValue(), Background);
					// WCAG: large text (18 pt, 24 px) needs 3:1.
					const float Needed = Box.FontPixels >= 24.f ? FMath::Min(3.f, Rules.MinContrast) : Rules.MinContrast;
					if (Ratio < Needed)
					{
						const FColor Text = Box.TextColor.GetValue().ToFColor(true);
						const FColor Back = Background.ToFColor(true);
						Add(Out, TEXT("low_contrast"), Ratio < 2.f ? TEXT("error") : TEXT("warning"), &Box, Resolution, FString::Printf(
							TEXT("Contrast %.1f:1 against what is behind it (text #%02X%02X%02X on #%02X%02X%02X); it needs %.1f:1. Darken or lighten one, or add a shadow or plate."),
							Ratio, Text.R, Text.G, Text.B, Back.R, Back.G, Back.B, Needed));
					}
				}
			}

			if (Box.bContent && !bBackdrop)
			{
				if (!Inside(Box.Rect, ScreenRect, EdgeSlackPixels))
				{
					const bool bGone = Area(Overlap(Box.Rect, ScreenRect)) <= 0.0;
					Add(Out, TEXT("offscreen"), bGone || Box.bInteractive || Box.bText ? TEXT("error") : TEXT("warning"), &Box, Resolution,
						bGone ? FString(TEXT("It is entirely off the screen.")) : FString(TEXT("Part of it is off the screen. Anchor it to the edge it sits by, with an offset inside.")));
				}
				else if (Margin > 0.0 && !Inside(Box.Rect, SafeRect, EdgeSlackPixels))
				{
					Add(Out, TEXT("outside_safe_zone"), TEXT("warning"), &Box, Resolution, FString::Printf(
						TEXT("It reaches outside the title-safe area (the middle %.0f%% of each axis), which TVs may crop. Move it in, or wrap the HUD in a Safe Zone."),
						Rules.SafeZone * 100.f));
				}
			}

			// The nearest ancestor that clips: anything of this widget outside it is not drawn.
			const FWidgetBox* Clipper = nullptr;
			for (const FWidgetBox& Other : Boxes)
			{
				if (Other.bClipsChildren && IsAncestor(Other, Box) && (!Clipper || Other.Path.Len() > Clipper->Path.Len()))
				{
					Clipper = &Other;
				}
			}
			if (Clipper && Box.bContent && !Inside(Box.Rect, Clipper->Rect, EdgeSlackPixels))
			{
				Add(Out, TEXT("clipped_by_parent"), TEXT("warning"), &Box, Resolution, FString::Printf(
					TEXT("It reaches outside %s, which clips its children, so part of it is not drawn."), *FPaths::GetCleanFilename(Clipper->Path)));
			}
		}

		for (int32 A = 0; A < Boxes.Num(); ++A)
		{
			if (!Boxes[A].bInteractive) continue;
			for (int32 B = A + 1; B < Boxes.Num(); ++B)
			{
				if (!Boxes[B].bInteractive || IsAncestor(Boxes[A], Boxes[B]) || IsAncestor(Boxes[B], Boxes[A])) continue;
				const double Shared = Area(Overlap(Boxes[A].Rect, Boxes[B].Rect));
				if (Shared > 4.0)
				{
					Add(Out, TEXT("overlapping_interactive"), TEXT("warning"), &Boxes[A], Resolution, FString::Printf(
						TEXT("It overlaps %s by %.0f px; a click or tap there hits whichever is on top. Space them apart."), *Boxes[B].Path, Shared));
				}
			}
		}
	}

	namespace
	{
		/** The widget as this render laid it out, from the geometry Slate painted it with. */
		void Collect(UUserWidget& Root, const float DpiScale, TArray<FWidgetBox>& Out)
		{
			TArray<UWidget*> Widgets;
			Root.WidgetTree->GetAllWidgets(Widgets);
			for (UWidget* Widget : Widgets)
			{
				const TSharedPtr<SWidget> Slate = Widget ? Widget->GetCachedWidget() : nullptr;
				if (!Slate || !Widget->IsVisible())
				{
					continue;
				}
				const FGeometry& Geometry = Slate->GetPaintSpaceGeometry();
				const FVector2D Position(Geometry.GetAbsolutePosition());
				const FVector2D Size(Geometry.GetAbsoluteSize());
				if (Size.X < 0.5 || Size.Y < 0.5)
				{
					continue;
				}
				FWidgetBox& Box = Out.AddDefaulted_GetRef();
				Box.Path = PathOf(*Widget);
				Box.Class = Widget->GetClass()->GetName();
				Box.Rect = FBox2D(Position, Position + Size);
				Box.bInteractive = IsInteractive(*Widget);
				Box.bContent = Box.bInteractive || !Widget->IsA<UPanelWidget>();
				Box.bFocusable = Slate->SupportsKeyboardFocus();
				Box.bClipsChildren = Widget->GetClipping() == EWidgetClipping::ClipToBounds || Widget->GetClipping() == EWidgetClipping::ClipToBoundsAlways;
				Box.DesiredLocal = FVector2D(Slate->GetDesiredSize());
				Box.AllottedLocal = FVector2D(Geometry.GetLocalSize());
				if (const UTextBlock* Text = Cast<UTextBlock>(Widget))
				{
					Box.bText = !Text->GetText().IsEmpty();
					Box.bWraps = Text->GetAutoWrapText();
					Box.FontPixels = FontPixels(Text->GetFont().Size, DpiScale);
					const FSlateColor Color = Text->GetColorAndOpacity();
					if (Color.IsColorSpecified())
					{
						Box.TextColor = Color.GetSpecifiedColor();
					}
				}
				else if (const URichTextBlock* Rich = Cast<URichTextBlock>(Widget))
				{
					Box.bText = !Rich->GetText().IsEmpty();
					Box.bWraps = Rich->GetAutoWrapText();
				}
			}
		}

		/** Draws Widget at Size and UI scale Dpi over Backdrop; the pixels are sRGB, row by row. */
		bool Draw(const TSharedRef<SWidget>& Widget, const FIntPoint Size, const float Dpi, const FLinearColor& Backdrop, TArray<FColor>& OutPixels)
		{
			FWidgetRenderer Renderer(/*bUseGammaCorrection=*/false, /*bInClearTarget=*/true);
			UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>();
			Target->ClearColor = FLinearColor::Transparent;
			Target->InitCustomFormat(Size.X, Size.Y, PF_FloatRGBA, /*bInForceLinearGamma=*/true);
			Target->UpdateResourceImmediate(true);
			const TStrongObjectPtr<UTextureRenderTarget2D> Keep(Target);
			// Twice: text and images that arrive on first paint (font atlas glyphs, async brushes) are there the second time.
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				Renderer.DrawWidget(Target, Widget, Dpi, FVector2D(Size), 1.f / 30.f);
				FlushRenderingCommands();
			}
			TArray<FLinearColor> Linear;
			FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
			if (!Resource || !Resource->ReadLinearColorPixels(Linear, FReadSurfaceDataFlags(RCM_MinMax)) || Linear.Num() != Size.X * Size.Y)
			{
				return false;
			}
			// Slate leaves colour premultiplied by its coverage over the transparent clear: lay it over the backdrop.
			const FLinearColor Under = FLinearColor(Backdrop.ToFColor(false));
			OutPixels.SetNumUninitialized(Linear.Num());
			for (int32 Index = 0; Index < Linear.Num(); ++Index)
			{
				const FLinearColor& Over = Linear[Index];
				const float Alpha = FMath::Clamp(Over.A, 0.f, 1.f);
				FLinearColor Composite(Over.R + Under.R * (1.f - Alpha), Over.G + Under.G * (1.f - Alpha), Over.B + Under.B * (1.f - Alpha), 1.f);
				OutPixels[Index] = Composite.ToFColor(/*bSRGB=*/true);
			}
			return true;
		}

		bool SavePng(const TArray<FColor>& Pixels, const FIntPoint Size, const FString& File)
		{
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), /*Tree=*/true);
			return !Png.IsEmpty() && FFileHelper::SaveArrayToFile(Png, *File);
		}

		/** Focus and navigation are properties of the tree, the same at every size. */
		void CheckTree(UUserWidget& Root, TArray<FHyperAIUIReviewIssue>& Out)
		{
			TArray<UWidget*> Widgets;
			Root.WidgetTree->GetAllWidgets(Widgets);
			TSet<FName> Names;
			for (const UWidget* Widget : Widgets)
			{
				if (Widget) Names.Add(Widget->GetFName());
			}
			int32 Interactive = 0;
			for (UWidget* Widget : Widgets)
			{
				if (!Widget || !IsInteractive(*Widget) || !Widget->IsVisible())
				{
					continue;
				}
				++Interactive;
				const TSharedPtr<SWidget> Slate = Widget->GetCachedWidget();
				FWidgetBox Box;
				Box.Path = PathOf(*Widget);
				if (Slate && !Slate->SupportsKeyboardFocus())
				{
					Add(Out, TEXT("not_focusable"), TEXT("warning"), &Box, FString(), TEXT("It cannot take keyboard or gamepad focus (Is Focusable is off), so only a mouse can use it."));
				}
				if (const UWidgetNavigation* Navigation = Widget->Navigation)
				{
					const TPair<const TCHAR*, const FWidgetNavigationData*> Directions[] = {
						{TEXT("up"), &Navigation->Up}, {TEXT("down"), &Navigation->Down}, {TEXT("left"), &Navigation->Left},
						{TEXT("right"), &Navigation->Right}, {TEXT("next"), &Navigation->Next}, {TEXT("previous"), &Navigation->Previous}};
					for (const TPair<const TCHAR*, const FWidgetNavigationData*>& Direction : Directions)
					{
						const FWidgetNavigationData& Data = *Direction.Value;
						if (Data.Rule == EUINavigationRule::Explicit && (Data.WidgetToFocus.IsNone() || !Names.Contains(Data.WidgetToFocus)))
						{
							Add(Out, TEXT("broken_navigation"), TEXT("error"), &Box, FString(), FString::Printf(
								TEXT("Its %s navigation is Explicit to '%s', which is not a widget in this tree, so the gamepad stops here."),
								Direction.Key, *Data.WidgetToFocus.ToString()));
						}
					}
				}
			}
			if (Interactive > 0 && Root.GetDesiredFocusWidgetName().IsNone())
			{
				Add(Out, TEXT("no_initial_focus"), TEXT("warning"), nullptr, FString(), FString::Printf(
					TEXT("%d interactive widget(s) but no Desired Focus Widget: a gamepad player starts with nothing focused. Set it in the class defaults."), Interactive));
			}
		}

		FString GetStringField(const UScriptStruct& Struct, const void* Data, const TCHAR* Name)
		{
			const FStrProperty* Property = CastField<FStrProperty>(Struct.FindPropertyByName(Name));
			return Property ? Property->GetPropertyValue_InContainer(Data) : FString();
		}

		void SetStringField(const UScriptStruct& Struct, void* Data, const TCHAR* Name, const FString& Value)
		{
			if (const FStrProperty* Property = CastField<FStrProperty>(Struct.FindPropertyByName(Name)))
			{
				Property->SetPropertyValue_InContainer(Data, Value);
			}
		}

		TArray<FString> GetStringArray(const UScriptStruct& Struct, const void* Data, const TCHAR* Name)
		{
			TArray<FString> Out;
			const FArrayProperty* Array = CastField<FArrayProperty>(Struct.FindPropertyByName(Name));
			const FStrProperty* Inner = Array ? CastField<FStrProperty>(Array->Inner) : nullptr;
			if (Inner)
			{
				FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Data));
				for (int32 Index = 0; Index < Helper.Num(); ++Index) Out.Add(Inner->GetPropertyValue(Helper.GetRawPtr(Index)));
			}
			return Out;
		}

		/**
		 * hyper_image_compare (the lighting pack's match scoring), called the way an agent calls it: that pack is optional,
		 * so it is reached through reflection, and its sessions keep the polish loop's history.
		 */
		void CompareWithReference(const FHyperAIUIRenderReviewRequest& Request, const FString& Candidate, FHyperAIUIReferenceMatch& Out)
		{
			UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/HyperAIStudioLighting.HyperAIStudioLightingToolset"));
			UFunction* Function = Class ? Class->FindFunctionByName(TEXT("hyper_image_compare")) : nullptr;
			const FStructProperty* RequestProperty = Function ? CastField<FStructProperty>(Function->FindPropertyByName(TEXT("Request"))) : nullptr;
			const FStructProperty* ReturnProperty = Function ? CastField<FStructProperty>(Function->GetReturnProperty()) : nullptr;
			if (!RequestProperty || !ReturnProperty
				|| !FHyperAIStudioCapabilityRuntimeIndex::IsOwned(Class, TEXT("HyperAIStudioLighting.HyperAIStudioLightingToolset")))
			{
				Out.Diagnostic = TEXT("The lighting_lookdev pack (hyper_image_compare) is not loaded, so the render was not scored; the PNGs are written.");
				return;
			}
			TArray<uint8> Parameters;
			Parameters.SetNumZeroed(Function->ParmsSize);
			Function->InitializeStruct(Parameters.GetData());
			ON_SCOPE_EXIT { Function->DestroyStruct(Parameters.GetData()); };
			void* Input = RequestProperty->ContainerPtrToValuePtr<void>(Parameters.GetData());
			const UScriptStruct& InputStruct = *RequestProperty->Struct;
			SetStringField(InputStruct, Input, TEXT("ReferenceImagePath"), Request.ReferenceImagePath);
			SetStringField(InputStruct, Input, TEXT("CandidateImagePath"), Candidate);
			SetStringField(InputStruct, Input, TEXT("Mode"), TEXT("aligned"));
			SetStringField(InputStruct, Input, TEXT("SessionId"), Request.SessionId);
			SetStringField(InputStruct, Input, TEXT("Note"), Request.Note);
			if (const FFloatProperty* Target = CastField<FFloatProperty>(InputStruct.FindPropertyByName(TEXT("TargetScore"))))
			{
				Target->SetPropertyValue_InContainer(Input, Request.TargetScore);
			}
			Class->GetDefaultObject()->ProcessEvent(Function, Parameters.GetData());
			const void* Result = ReturnProperty->ContainerPtrToValuePtr<void>(Parameters.GetData());
			const UScriptStruct& ResultStruct = *ReturnProperty->Struct;
			const FBoolProperty* OkProperty = CastField<FBoolProperty>(ResultStruct.FindPropertyByName(TEXT("bOk")));
			if (!OkProperty || !OkProperty->GetPropertyValue_InContainer(Result))
			{
				Out.Diagnostic = FString::Printf(TEXT("hyper_image_compare did not score it (%s): %s"), *GetStringField(ResultStruct, Result, TEXT("Status")),
					*GetStringField(ResultStruct, Result, TEXT("Diagnostic")));
				return;
			}
			Out.bCompared = true;
			Out.TargetScore = Request.TargetScore;
			if (const FFloatProperty* Score = CastField<FFloatProperty>(ResultStruct.FindPropertyByName(TEXT("Score")))) Out.Score = Score->GetPropertyValue_InContainer(Result);
			if (const FBoolProperty* Matched = CastField<FBoolProperty>(ResultStruct.FindPropertyByName(TEXT("bMatched")))) Out.bMatched = Matched->GetPropertyValue_InContainer(Result);
			Out.HeatMapPath = GetStringField(ResultStruct, Result, TEXT("DiffPath"));
			Out.SideBySidePath = GetStringField(ResultStruct, Result, TEXT("SideBySidePath"));
			Out.SessionId = GetStringField(ResultStruct, Result, TEXT("SessionId"));
			Out.Suggestions = GetStringArray(ResultStruct, Result, TEXT("Suggestions"));
			const FArrayProperty* Regions = CastField<FArrayProperty>(ResultStruct.FindPropertyByName(TEXT("Regions")));
			const FStructProperty* Region = Regions ? CastField<FStructProperty>(Regions->Inner) : nullptr;
			if (Region)
			{
				FScriptArrayHelper Helper(Regions, Regions->ContainerPtrToValuePtr<void>(Result));
				for (int32 Index = 0; Index < Helper.Num() && Index < 3; ++Index)
				{
					const void* Cell = Helper.GetRawPtr(Index);
					Out.WorstRegions.Add(FString::Printf(TEXT("%s: %s"), *GetStringField(*Region->Struct, Cell, TEXT("Name")),
						*GetStringField(*Region->Struct, Cell, TEXT("Difference"))));
				}
			}
		}

		FString Refuse(FHyperAIUIRenderReviewReport& Report, const TCHAR* Status, const FString& Why)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Why;
			return Why;
		}
	}

	FHyperAIUIRenderReviewReport Run(const FHyperAIUIRenderReviewRequest& In)
	{
		FHyperAIUIRenderReviewReport Report;
		if (FHyperAIStudioNames::RefuseRequest(In, Report))
		{
			return Report;
		}
		FHyperAIUIRenderReviewRequest Request = In;
		if (!IsInGameThread())
		{
			Refuse(Report, TEXT("invalid_request"), TEXT("hyper_ui_render_review runs on the game thread."));
			return Report;
		}
		TArray<FIntPoint> Sizes;
		if (Request.Resolutions.IsEmpty())
		{
			Request.Resolutions.Add(TEXT("1920x1080"));
		}
		for (const FString& Text : Request.Resolutions)
		{
			FIntPoint Size;
			if (!ParseResolution(Text, Size))
			{
				Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("'%s' is not a resolution; write it as 1920x1080 (320x240 to 7680x4320)."), *Text.Left(32)));
				return Report;
			}
			Sizes.AddUnique(Size);
		}
		if (Sizes.Num() > MaxResolutions)
		{
			Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("At most %d resolutions per review."), MaxResolutions));
			return Report;
		}
		if (Request.DpiScale != 0.f && (Request.DpiScale < 0.25f || Request.DpiScale > 4.f))
		{
			Refuse(Report, TEXT("invalid_request"), TEXT("dpi_scale is 0 (the project's DPI curve) or 0.25 to 4."));
			return Report;
		}
		Request.SafeZone = FMath::Clamp(Request.SafeZone <= 0.f ? 0.9f : Request.SafeZone, 0.5f, 1.f);
		Request.MinTextPixels = FMath::Clamp(Request.MinTextPixels <= 0.f ? 14.f : Request.MinTextPixels, 4.f, 64.f);
		Request.MinContrast = FMath::Clamp(Request.MinContrast <= 0.f ? 4.5f : Request.MinContrast, 1.f, 21.f);
		Request.MaxIssues = FMath::Clamp(Request.MaxIssues <= 0 ? 64 : Request.MaxIssues, 1, 256);

		const FString Path = Request.TargetPath.TrimStartAndEnd();
		FString Reason;
		if (!FHyperAIStudioNames::IsObjectPath(Path, TEXT("TargetPath"), &Reason) || !FPackageName::IsValidObjectPath(Path))
		{
			Refuse(Report, TEXT("invalid_request"), Reason.IsEmpty() ? FString(TEXT("target_path is a Widget Blueprint's object path, such as /Game/UI/WBP_Menu.WBP_Menu.")) : Reason);
			return Report;
		}
		if (!FindObject<UObject>(nullptr, *Path) && !FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)))
		{
			Refuse(Report, TEXT("not_found"), FString::Printf(TEXT("No asset at %s."), *Path));
			return Report;
		}
		UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FSoftObjectPath(Path).TryLoad());
		if (!Blueprint)
		{
			Refuse(Report, TEXT("not_found"), FString::Printf(TEXT("%s is not a Widget Blueprint."), *Path));
			return Report;
		}
		UClass* Class = Blueprint->GeneratedClass;
		if (!Class || !Class->IsChildOf(UUserWidget::StaticClass()) || Blueprint->Status == BS_Error)
		{
			Refuse(Report, TEXT("not_compiled"), FString::Printf(TEXT("%s does not compile; fix it (hyper_ui_validate, or the editor's Compiler Results) and render again."), *Path));
			return Report;
		}
		if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
		{
			Refuse(Report, TEXT("cannot_render"), TEXT("This editor runs without rendering (-nullrhi or a commandlet), so nothing can be drawn."));
			return Report;
		}

		// The widget to draw: the one playing, or a fresh design-time instance as the thumbnail renderer makes one.
		TStrongObjectPtr<UUserWidget> Widget;
		if (Request.bFromPlay)
		{
			UWorld* PlayWorld = GEditor ? GEditor->PlayWorld.Get() : nullptr;
			if (!PlayWorld)
			{
				Refuse(Report, TEXT("no_pie_world"), TEXT("Play In Editor is not running; start it, or leave bFromPlay off to render a fresh instance."));
				return Report;
			}
			UUserWidget* Found = nullptr;
			for (TObjectIterator<UUserWidget> It; It; ++It)
			{
				if (It->GetClass() == Class && It->GetWorld() == PlayWorld && IsValid(*It) && (!Found || (It->IsInViewport() && !Found->IsInViewport())))
				{
					Found = *It;
				}
			}
			if (!Found || !Found->GetCachedWidget())
			{
				Refuse(Report, TEXT("not_found"), FString::Printf(TEXT("No %s is on screen in Play In Editor. Show it in the game first, or leave bFromPlay off."), *Class->GetName()));
				return Report;
			}
			Widget.Reset(Found);
			Report.Source = TEXT("play");
			Report.Notes.Add(TEXT("The playing instance was drawn off screen at each size and handed back; the game view redraws it at its own size next frame."));
		}
		else
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			UUserWidget* Fresh = World ? NewObject<UUserWidget>(World, Class, NAME_None, RF_Transient) : nullptr;
			if (!Fresh)
			{
				Refuse(Report, TEXT("cannot_render"), TEXT("No editor world to make the widget in."));
				return Report;
			}
			Fresh->SetDesignerFlags(EWidgetDesignFlags::Designing | EWidgetDesignFlags::ExecutePreConstruct);
			Fresh->Initialize();
			Widget.Reset(Fresh);
			Report.Source = TEXT("design_time");
		}
		if (!Widget->WidgetTree || !Widget->WidgetTree->RootWidget)
		{
			Refuse(Report, TEXT("not_compiled"), FString::Printf(TEXT("%s has no widgets to draw."), *Path));
			return Report;
		}
		const TSharedRef<SWidget> Slate = Widget->TakeWidget();
		const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("UIReview"));
		TArray<FHyperAIUIReviewIssue> Issues;
		for (const FIntPoint& Size : Sizes)
		{
			const FString Resolution = FString::Printf(TEXT("%dx%d"), Size.X, Size.Y);
			const float Dpi = Request.DpiScale > 0.f ? Request.DpiScale : GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(Size);
			TArray<FColor> Pixels;
			if (!Draw(Slate, Size, Dpi, Request.Backdrop, Pixels))
			{
				Refuse(Report, TEXT("cannot_render"), FString::Printf(TEXT("The %s render could not be read back."), *Resolution));
				return Report;
			}
			FHyperAIUIRender& Render = Report.Renders.AddDefaulted_GetRef();
			Render.Resolution = Resolution;
			Render.DpiScale = Dpi;
			Render.ImagePath = Folder / FString::Printf(TEXT("%s_%s_%s_%d.png"), *Blueprint->GetName(), *Report.Source, *Resolution, FMath::RoundToInt32(Dpi * 100.f));
			if (!SavePng(Pixels, Size, Render.ImagePath))
			{
				Render.ImagePath.Reset();
				Report.Notes.Add(FString::Printf(TEXT("The %s PNG could not be written."), *Resolution));
			}
			TArray<FWidgetBox> Boxes;
			Collect(*Widget, Dpi, Boxes);
			Render.Widgets = Boxes.Num();
			const int32 Before = Issues.Num();
			CheckLayout(Boxes, Size, Request, Resolution, &Pixels, Issues);
			Render.Issues = Issues.Num() - Before;
		}
		// Restore the playing widget's own layout before the game view paints it again.
		if (Request.bFromPlay)
		{
			Slate->Invalidate(EInvalidateWidgetReason::Layout);
		}
		CheckTree(*Widget, Issues);

		Issues.StableSort([](const FHyperAIUIReviewIssue& A, const FHyperAIUIReviewIssue& B)
		{
			return (A.Severity == TEXT("error")) > (B.Severity == TEXT("error"));
		});
		for (const FHyperAIUIReviewIssue& Issue : Issues)
		{
			(Issue.Severity == TEXT("error") ? Report.ErrorCount : Report.WarningCount)++;
		}
		if (Issues.Num() > Request.MaxIssues)
		{
			Report.bTruncated = true;
			Issues.SetNum(Request.MaxIssues);
		}
		Report.Issues = MoveTemp(Issues);

		if (!Request.ReferenceImagePath.TrimStartAndEnd().IsEmpty() && !Report.Renders[0].ImagePath.IsEmpty())
		{
			CompareWithReference(Request, Report.Renders[0].ImagePath, Report.Reference);
		}
		Report.bOk = true;
		Report.Status = TEXT("reviewed");
		Report.Diagnostic = FString::Printf(TEXT("Rendered %s at %d size(s): %d error(s), %d warning(s)%s. Open the PNGs to look at them."),
			*Blueprint->GetName(), Report.Renders.Num(), Report.ErrorCount, Report.WarningCount,
			Report.Reference.bCompared ? *FString::Printf(TEXT("; %.0f of %.0f against the reference%s"), Report.Reference.Score, Report.Reference.TargetScore,
				Report.Reference.bMatched ? TEXT(", matched") : TEXT("")) : TEXT(""));
		if (!Report.Reference.Diagnostic.IsEmpty())
		{
			Report.Notes.Add(Report.Reference.Diagnostic);
		}
		Report.Notes.Add(TEXT("Contrast is read against the render, drawn over the backdrop colour; set backdrop near the game's view behind this UI."));
		return Report;
	}
}

FHyperAIUIRenderReviewReport UHyperAIStudioUIToolset::hyper_ui_render_review(const FHyperAIUIRenderReviewRequest& Request)
{
	return HyperAIStudio::UI::Review::Run(Request);
}
