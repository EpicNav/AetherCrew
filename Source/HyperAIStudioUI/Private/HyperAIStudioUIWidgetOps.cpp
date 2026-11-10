// Games by Hyper 2026.

#include "HyperAIStudioUIWidgetOps.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/Widget.h"
#include "Engine/Texture2D.h"
#include "Fonts/SlateFontInfo.h"
#include "HyperAIStudioNames.h"
#include "Internationalization/TextNamespaceUtil.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "Materials/MaterialInterface.h"
#include "Misc/OutputDeviceNull.h"
#include "Styling/SlateColor.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace HyperAIStudio::UI::WidgetOps
{
	namespace
	{
		constexpr int32 MaxIdentifierChars = 64;
		constexpr int32 MaxNames = 32;

		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty() || Value.Len() > MaxIdentifierChars || !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_')))
			{
				return false;
			}
			for (const TCHAR Char : Value)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT('_'))
				{
					return false;
				}
			}
			return true;
		}

		/** The op's value: its typed number, bool or color field when set, or else its string. */
		FString ValueText(const FHyperAIUIPlanOperation& Operation)
		{
			if (Operation.bHasNumberValue) return FString::SanitizeFloat(Operation.NumberValue);
			if (Operation.bHasBoolValue) return Operation.bBoolValue ? TEXT("true") : TEXT("false");
			if (Operation.bHasColorValue)
			{
				const FLinearColor& Color = Operation.ColorValue;
				return FString::Printf(TEXT("%g,%g,%g,%g"), Color.R, Color.G, Color.B, Color.A);
			}
			return Operation.StringValue.TrimStartAndEnd();
		}

		bool ParseNumbers(const FString& Text, TArray<double>& Out)
		{
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT(","), false);
			Out.Reset();
			for (const FString& Part : Parts)
			{
				double Value = 0.0;
				const FString Trimmed = Part.TrimStartAndEnd();
				if (Trimmed.IsEmpty() || !Trimmed.IsNumeric() || !LexTryParseString(Value, *Trimmed) || !FMath::IsFinite(Value))
				{
					return false;
				}
				Out.Add(Value);
			}
			return !Out.IsEmpty();
		}

		bool ParseBool(const FString& Text, bool& Out)
		{
			if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase)) { Out = true; return true; }
			if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase)) { Out = false; return true; }
			return false;
		}

		/** "#RRGGBB" or "#RRGGBBAA" in sRGB, as designers write them, or "r,g,b[,a]" in linear 0-1. */
		bool ParseColor(const FString& Text, FLinearColor& Out)
		{
			if (Text.StartsWith(TEXT("#")))
			{
				const FString Hex = Text.RightChop(1);
				if (Hex.Len() != 6 && Hex.Len() != 8)
				{
					return false;
				}
				for (const TCHAR Char : Hex)
				{
					if (!FChar::IsHexDigit(Char)) return false;
				}
				Out = FLinearColor(FColor::FromHex(Hex));
				return true;
			}
			TArray<double> Parts;
			if (!ParseNumbers(Text, Parts) || (Parts.Num() != 3 && Parts.Num() != 4))
			{
				return false;
			}
			Out = FLinearColor(Parts[0], Parts[1], Parts[2], Parts.Num() == 4 ? Parts[3] : 1.0);
			return true;
		}

		FString ColorText(const FLinearColor& Color)
		{
			return FString::Printf(TEXT("(R=%f,G=%f,B=%f,A=%f)"), Color.R, Color.G, Color.B, Color.A);
		}

		/** "10", "10,20" (horizontal, vertical) or "left,top,right,bottom". */
		bool ParseMargin(const FString& Text, FMargin& Out)
		{
			TArray<double> Parts;
			if (!ParseNumbers(Text, Parts)) return false;
			if (Parts.Num() == 1) { Out = FMargin(Parts[0]); return true; }
			if (Parts.Num() == 2) { Out = FMargin(Parts[0], Parts[1]); return true; }
			if (Parts.Num() == 4) { Out = FMargin(Parts[0], Parts[1], Parts[2], Parts[3]); return true; }
			return false;
		}

		bool ParseVector2(const FString& Text, FVector2D& Out)
		{
			TArray<double> Parts;
			if (!ParseNumbers(Text, Parts) || Parts.Num() != 2) return false;
			Out = FVector2D(Parts[0], Parts[1]);
			return true;
		}

		/** Anchor presets by name, or "minx,miny,maxx,maxy". */
		bool ParseAnchors(const FString& Text, FAnchors& Out)
		{
			static const TMap<FString, FAnchors> Presets = {
				{TEXT("top_left"), FAnchors(0.f, 0.f)}, {TEXT("top"), FAnchors(0.5f, 0.f)}, {TEXT("top_right"), FAnchors(1.f, 0.f)},
				{TEXT("left"), FAnchors(0.f, 0.5f)}, {TEXT("center"), FAnchors(0.5f, 0.5f)}, {TEXT("right"), FAnchors(1.f, 0.5f)},
				{TEXT("bottom_left"), FAnchors(0.f, 1.f)}, {TEXT("bottom"), FAnchors(0.5f, 1.f)}, {TEXT("bottom_right"), FAnchors(1.f, 1.f)},
				{TEXT("top_fill"), FAnchors(0.f, 0.f, 1.f, 0.f)}, {TEXT("center_fill"), FAnchors(0.f, 0.5f, 1.f, 0.5f)},
				{TEXT("bottom_fill"), FAnchors(0.f, 1.f, 1.f, 1.f)}, {TEXT("left_fill"), FAnchors(0.f, 0.f, 0.f, 1.f)},
				{TEXT("middle_fill"), FAnchors(0.5f, 0.f, 0.5f, 1.f)}, {TEXT("right_fill"), FAnchors(1.f, 0.f, 1.f, 1.f)},
				{TEXT("fill"), FAnchors(0.f, 0.f, 1.f, 1.f)}};
			if (const FAnchors* Preset = Presets.Find(Text.ToLower()))
			{
				Out = *Preset;
				return true;
			}
			TArray<double> Parts;
			if (!ParseNumbers(Text, Parts) || Parts.Num() != 4 || Parts[0] > Parts[2] || Parts[1] > Parts[3])
			{
				return false;
			}
			Out = FAnchors(Parts[0], Parts[1], Parts[2], Parts[3]);
			return true;
		}

		/** Agent words for enum values, in Unreal's text for the enum. */
		bool EnumText(const FString& Id, const FString& Value, FString& Out)
		{
			static const TMap<FString, TMap<FString, FString>> Tables = {
				{TEXT("visibility"), {{TEXT("visible"), TEXT("Visible")}, {TEXT("collapsed"), TEXT("Collapsed")}, {TEXT("hidden"), TEXT("Hidden")},
					{TEXT("hit_test_invisible"), TEXT("HitTestInvisible")}, {TEXT("self_hit_test_invisible"), TEXT("SelfHitTestInvisible")}}},
				{TEXT("checked_state"), {{TEXT("checked"), TEXT("Checked")}, {TEXT("unchecked"), TEXT("Unchecked")}, {TEXT("undetermined"), TEXT("Undetermined")}}},
				{TEXT("justification"), {{TEXT("left"), TEXT("Left")}, {TEXT("center"), TEXT("Center")}, {TEXT("right"), TEXT("Right")}}},
				{TEXT("h_align"), {{TEXT("left"), TEXT("HAlign_Left")}, {TEXT("center"), TEXT("HAlign_Center")}, {TEXT("right"), TEXT("HAlign_Right")},
					{TEXT("fill"), TEXT("HAlign_Fill")}}},
				{TEXT("v_align"), {{TEXT("top"), TEXT("VAlign_Top")}, {TEXT("center"), TEXT("VAlign_Center")}, {TEXT("bottom"), TEXT("VAlign_Bottom")},
					{TEXT("fill"), TEXT("VAlign_Fill")}}}};
			const TMap<FString, FString>* Table = Tables.Find(Id);
			const FString* Found = Table ? Table->Find(Value.ToLower()) : nullptr;
			if (Found)
			{
				Out = *Found;
			}
			return Found != nullptr;
		}

		FString EnumWords(const FString& Id)
		{
			if (Id == TEXT("visibility")) return TEXT("visible, collapsed, hidden, hit_test_invisible or self_hit_test_invisible");
			if (Id == TEXT("checked_state")) return TEXT("checked, unchecked or undetermined");
			if (Id == TEXT("justification")) return TEXT("left, center or right");
			if (Id == TEXT("h_align")) return TEXT("left, center, right or fill");
			return TEXT("top, center, bottom or fill");
		}

		enum class EValue : uint8
		{
			Text, Bool, Number, UnitNumber, Int, Enum, Margin, Color, SlotSize,
			FontSize, Image, SizeOverride, RenderAngle, RenderScale, RenderTranslation, RenderPivot, AccessibleText,
			Anchors, Offsets, Position, Size, Alignment, AutoSize, ZOrder
		};

		struct FSpec
		{
			const TCHAR* Id;
			/** The UPROPERTY written through reflection, or empty for a typed setter. */
			const TCHAR* Property;
			EValue Value;
		};

		const FSpec PropertySpecs[] = {
			{TEXT("text"), TEXT("Text"), EValue::Text},
			{TEXT("hint_text"), TEXT("HintText"), EValue::Text},
			{TEXT("tool_tip"), TEXT("ToolTipText"), EValue::Text},
			{TEXT("accessible_text"), TEXT("AccessibleText"), EValue::AccessibleText},
			{TEXT("visibility"), TEXT("Visibility"), EValue::Enum},
			{TEXT("is_enabled"), TEXT("bIsEnabled"), EValue::Bool},
			{TEXT("is_variable"), TEXT("bIsVariable"), EValue::Bool},
			{TEXT("is_focusable"), TEXT("IsFocusable"), EValue::Bool},
			{TEXT("render_opacity"), TEXT("RenderOpacity"), EValue::UnitNumber},
			{TEXT("percent"), TEXT("Percent"), EValue::UnitNumber},
			{TEXT("value"), TEXT("Value"), EValue::Number},
			{TEXT("min_value"), TEXT("MinValue"), EValue::Number},
			{TEXT("max_value"), TEXT("MaxValue"), EValue::Number},
			{TEXT("step_size"), TEXT("StepSize"), EValue::Number},
			{TEXT("checked_state"), TEXT("CheckedState"), EValue::Enum},
			{TEXT("justification"), TEXT("Justification"), EValue::Enum},
			{TEXT("auto_wrap"), TEXT("AutoWrapText"), EValue::Bool},
			{TEXT("min_desired_width"), TEXT("MinDesiredWidth"), EValue::Number},
			{TEXT("padding"), TEXT("Padding"), EValue::Margin},
			{TEXT("font_size"), TEXT("Font"), EValue::FontSize},
			{TEXT("image"), TEXT("Brush"), EValue::Image},
			{TEXT("width_override"), TEXT(""), EValue::SizeOverride},
			{TEXT("height_override"), TEXT(""), EValue::SizeOverride},
			{TEXT("render_angle"), TEXT(""), EValue::RenderAngle},
			{TEXT("render_scale"), TEXT(""), EValue::RenderScale},
			{TEXT("render_translation"), TEXT(""), EValue::RenderTranslation},
			{TEXT("render_pivot"), TEXT(""), EValue::RenderPivot}};

		const FSpec StyleSpecs[] = {
			{TEXT("color_and_opacity"), TEXT("ColorAndOpacity"), EValue::Color},
			{TEXT("brush_color"), TEXT("BrushColor"), EValue::Color},
			{TEXT("fill_color_and_opacity"), TEXT("FillColorAndOpacity"), EValue::Color},
			{TEXT("background_color"), TEXT("BackgroundColor"), EValue::Color},
			{TEXT("content_color"), TEXT("ContentColorAndOpacity"), EValue::Color},
			{TEXT("shadow_color"), TEXT("ShadowColorAndOpacity"), EValue::Color}};

		const FSpec SlotSpecs[] = {
			{TEXT("padding"), TEXT("Padding"), EValue::Margin},
			{TEXT("h_align"), TEXT("HorizontalAlignment"), EValue::Enum},
			{TEXT("v_align"), TEXT("VerticalAlignment"), EValue::Enum},
			{TEXT("fill"), TEXT("Size"), EValue::SlotSize},
			{TEXT("row"), TEXT("Row"), EValue::Int},
			{TEXT("column"), TEXT("Column"), EValue::Int},
			{TEXT("row_span"), TEXT("RowSpan"), EValue::Int},
			{TEXT("column_span"), TEXT("ColumnSpan"), EValue::Int},
			// A canvas slot keeps these inside its LayoutData; they go through UCanvasPanelSlot's setters.
			{TEXT("anchors"), TEXT(""), EValue::Anchors},
			{TEXT("offsets"), TEXT(""), EValue::Offsets},
			{TEXT("position"), TEXT(""), EValue::Position},
			{TEXT("size"), TEXT(""), EValue::Size},
			{TEXT("alignment"), TEXT(""), EValue::Alignment},
			{TEXT("auto_size"), TEXT(""), EValue::AutoSize},
			{TEXT("z_order"), TEXT(""), EValue::ZOrder}};

		template <int32 N>
		const FSpec* FindSpec(const FSpec (&Specs)[N], const FString& Id)
		{
			for (const FSpec& Spec : Specs)
			{
				if (Id == Spec.Id) return &Spec;
			}
			return nullptr;
		}

		template <int32 N>
		FString SpecIds(const FSpec (&Specs)[N])
		{
			TArray<FString> Ids;
			for (const FSpec& Spec : Specs) Ids.Add(Spec.Id);
			return FString::Join(Ids, TEXT(", "));
		}

		bool IsCanvasSpec(const FSpec& Spec)
		{
			return Spec.Value >= EValue::Anchors;
		}

		/**
		 * The Unreal text a reflected property imports for this value, or false with the reason. Text, font size,
		 * image and the typed-setter specs have no text form here.
		 */
		bool ToImportText(const FSpec& Spec, const FProperty& Property, const FString& Value, FString& OutText, FString& OutError)
		{
			bool bFlag = false;
			TArray<double> Numbers;
			FMargin Margin;
			FLinearColor Color;
			switch (Spec.Value)
			{
			case EValue::Bool:
				if (!ParseBool(Value, bFlag)) { OutError = TEXT("the value is true or false."); return false; }
				OutText = bFlag ? TEXT("True") : TEXT("False");
				return true;
			case EValue::Number:
			case EValue::UnitNumber:
			case EValue::Int:
				if (!ParseNumbers(Value, Numbers) || Numbers.Num() != 1) { OutError = TEXT("the value is one number."); return false; }
				if (Spec.Value == EValue::UnitNumber && (Numbers[0] < 0.0 || Numbers[0] > 1.0)) { OutError = TEXT("the value runs from 0 to 1."); return false; }
				if (Spec.Value == EValue::Int && (Numbers[0] != FMath::RoundToDouble(Numbers[0]) || Numbers[0] < 0.0)) { OutError = TEXT("the value is a whole number, 0 or more."); return false; }
				OutText = Spec.Value == EValue::Int ? FString::FromInt(static_cast<int32>(Numbers[0])) : FString::SanitizeFloat(Numbers[0]);
				return true;
			case EValue::Enum:
				if (!EnumText(Spec.Id, Value, OutText)) { OutError = TEXT("the value is ") + EnumWords(Spec.Id) + TEXT("."); return false; }
				return true;
			case EValue::Margin:
				if (!ParseMargin(Value, Margin)) { OutError = TEXT("the value is one number, horizontal,vertical, or left,top,right,bottom."); return false; }
				OutText = FString::Printf(TEXT("(Left=%f,Top=%f,Right=%f,Bottom=%f)"), Margin.Left, Margin.Top, Margin.Right, Margin.Bottom);
				return true;
			case EValue::Color:
			{
				if (!ParseColor(Value, Color)) { OutError = TEXT("the color is #RRGGBB[AA] or r,g,b[,a] from 0 to 1."); return false; }
				const FStructProperty* Struct = CastField<FStructProperty>(&Property);
				OutText = Struct && Struct->Struct == FSlateColor::StaticStruct()
					? FString::Printf(TEXT("(SpecifiedColor=%s,ColorUseRule=UseColor_Specified)"), *ColorText(Color)) : ColorText(Color);
				return true;
			}
			case EValue::SlotSize:
				if (Value.Equals(TEXT("auto"), ESearchCase::IgnoreCase)) { OutText = TEXT("(SizeRule=Automatic,Value=1.000000)"); return true; }
				if (!ParseNumbers(Value, Numbers) || Numbers.Num() != 1 || Numbers[0] <= 0.0) { OutError = TEXT("fill is auto, or the share of the free space to fill (a number above 0)."); return false; }
				OutText = FString::Printf(TEXT("(SizeRule=Fill,Value=%f)"), Numbers[0]);
				return true;
			default:
				OutError = TEXT("no text form");
				return false;
			}
		}

		/** A scratch value of a property's type, initialized and destroyed with it. */
		struct FScratch
		{
			const FProperty& Property;
			void* Data;

			explicit FScratch(const FProperty& InProperty)
				: Property(InProperty), Data(FMemory::Malloc(InProperty.GetSize(), InProperty.GetMinAlignment()))
			{
				Property.InitializeValue(Data);
			}
			~FScratch()
			{
				Property.DestroyValue(Data);
				FMemory::Free(Data);
			}
			FScratch(const FScratch&) = delete;
			FScratch& operator=(const FScratch&) = delete;
		};

		bool Import(const FProperty& Property, const FString& Text, UObject* Owner, void* Destination)
		{
			// The import makes names of the text, and loads an asset it names (making the package when nothing is there).
			if (!FHyperAIStudioNames::CanImportText(Property, Text, TEXT("value"))) return false;
			FOutputDeviceNull Quiet;
			const TCHAR* End = Property.ImportText_Direct(*Text, Destination, Owner, PPF_None, &Quiet);
			while (End && FChar::IsWhitespace(*End))
			{
				++End;
			}
			return End && *End == TEXT('\0');
		}

		/** The Details panel's order: PreEditChange (which records the object for undo), write through the setter, PostEditChangeProperty. */
		void Write(UObject& Owner, FProperty& Property, const void* Value)
		{
			Owner.PreEditChange(&Property);
			Property.SetValue_InContainer(&Owner, Value);
			FPropertyChangedEvent Event(&Property, EPropertyChangeType::ValueSet);
			Owner.PostEditChangeProperty(Event);
		}

		/** Keyed in the package's localization namespace with a new key, as the Details panel keys text it commits. */
		FText LocalizedText(UObject& Owner, const FString& Value)
		{
			const FString Namespace = TextNamespaceUtil::BuildFullNamespace(FString(), TextNamespaceUtil::EnsurePackageNamespace(&Owner),
				/*bAlwaysApplyPackageNamespace=*/true);
			return FText::ChangeKey(FTextKey(Namespace), FTextKey(TextNamespaceUtil::GenerateRandomTextKey()), FText::FromString(Value));
		}

		UWidget* FindWidget(const UWidgetBlueprint& Blueprint, const FString& Subject)
		{
			return Blueprint.WidgetTree ? Blueprint.WidgetTree->FindWidget(FName(*WidgetNameOf(Subject))) : nullptr;
		}

		/** A widget class by short UMG name (Button, TextBlock), class path, or Widget Blueprint path. */
		UClass* ResolveWidgetClass(const FString& Text)
		{
			if (Text.IsEmpty())
			{
				return nullptr;
			}
			if (!Text.Contains(TEXT("/")))
			{
				return FHyperAIStudioNames::IsName(Text, nullptr) ? FSoftClassPath(TEXT("/Script/UMG.") + Text).TryLoadClass<UWidget>() : nullptr;
			}
			// Loaded when nothing is in memory: an FName past its length, or a package with "//", stops the editor.
			if (!FHyperAIStudioNames::IsObjectPath(Text, nullptr))
			{
				return nullptr;
			}
			if (UClass* Class = FSoftClassPath(Text).TryLoadClass<UWidget>())
			{
				return Class;
			}
			const UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FSoftObjectPath(Text).TryLoad());
			return Blueprint && Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(UWidget::StaticClass())
				? Blueprint->GeneratedClass.Get() : nullptr;
		}

		/** What an existing widget's slot is, or what a planned widget's slot will be: its parent panel's slot class. */
		const UClass* SlotClassOf(const UWidgetBlueprint& Blueprint, const FPlannedTree& Tree, const FName Name)
		{
			if (const UWidget* Widget = Blueprint.WidgetTree ? Blueprint.WidgetTree->FindWidget(Name) : nullptr)
			{
				if (Widget->Slot)
				{
					return Widget->Slot->GetClass();
				}
			}
			const FPlannedWidget* Planned = Tree.Widgets.Find(Name);
			const FPlannedWidget* Parent = Planned && !Planned->Parent.IsNone() ? Tree.Widgets.Find(Planned->Parent) : nullptr;
			const UPanelWidget* Panel = Parent && Parent->Class ? Cast<UPanelWidget>(Parent->Class->GetDefaultObject()) : nullptr;
			return Panel ? Panel->GetSlotClass() : nullptr;
		}

		bool ParseNavigationRule(const FString& Text, EUINavigationRule& Out)
		{
			static const TMap<FString, EUINavigationRule> Rules = {
				{TEXT("escape"), EUINavigationRule::Escape}, {TEXT("stop"), EUINavigationRule::Stop},
				{TEXT("wrap"), EUINavigationRule::Wrap}, {TEXT("explicit"), EUINavigationRule::Explicit}};
			const EUINavigationRule* Found = Rules.Find(Text.ToLower());
			if (Found) Out = *Found;
			return Found != nullptr;
		}

		/** The directions an op names: up, down, left, right, next, previous, or all six. */
		TArray<FWidgetNavigationData*> NavigationSlots(UWidgetNavigation& Navigation, const FString& Direction)
		{
			const FString Lower = Direction.ToLower();
			if (Lower == TEXT("all")) return {&Navigation.Up, &Navigation.Down, &Navigation.Left, &Navigation.Right, &Navigation.Next, &Navigation.Previous};
			if (Lower == TEXT("up")) return {&Navigation.Up};
			if (Lower == TEXT("down")) return {&Navigation.Down};
			if (Lower == TEXT("left")) return {&Navigation.Left};
			if (Lower == TEXT("right")) return {&Navigation.Right};
			if (Lower == TEXT("next")) return {&Navigation.Next};
			if (Lower == TEXT("previous")) return {&Navigation.Previous};
			return {};
		}

		bool IsDirection(const FString& Direction)
		{
			static const TSet<FString> Directions = {TEXT("up"), TEXT("down"), TEXT("left"), TEXT("right"), TEXT("next"), TEXT("previous"), TEXT("all")};
			return Directions.Contains(Direction.ToLower());
		}

		/** A texture or material an Image can draw. */
		UObject* LoadBrushResource(const FString& Path)
		{
			UObject* Asset = FHyperAIStudioNames::IsObjectPath(Path, nullptr) ? FSoftObjectPath(Path).TryLoad() : nullptr;
			return Asset && (Asset->IsA<UTexture2D>() || Asset->IsA<UMaterialInterface>()) ? Asset : nullptr;
		}

		/** Validates a property or style spec's value against a widget class. OutProperty is set for reflected specs. */
		bool CheckValue(const FSpec& Spec, const UClass& Class, const FString& Value, FProperty*& OutProperty, FString& OutError)
		{
			OutProperty = nullptr;
			const FString ClassName = Class.GetName();
			switch (Spec.Value)
			{
			case EValue::FontSize:
			{
				const FStructProperty* Font = CastField<FStructProperty>(FindFProperty<FProperty>(&Class, TEXT("Font")));
				TArray<double> Numbers;
				if (!Font || Font->Struct != FSlateFontInfo::StaticStruct())
				{
					OutError = FString::Printf(TEXT("%s has no font; font_size is for text widgets."), *ClassName);
					return false;
				}
				if (!ParseNumbers(Value, Numbers) || Numbers.Num() != 1 || Numbers[0] < 1.0 || Numbers[0] > 1000.0)
				{
					OutError = TEXT("font_size is a point size from 1 to 1000.");
					return false;
				}
				OutProperty = const_cast<FStructProperty*>(Font);
				return true;
			}
			case EValue::Image:
				if (!Class.IsChildOf(UImage::StaticClass()))
				{
					OutError = FString::Printf(TEXT("%s is not an Image; image is for Image widgets."), *ClassName);
					return false;
				}
				if (!LoadBrushResource(Value))
				{
					OutError = FString::Printf(TEXT("No texture or material at %s."), *Value);
					return false;
				}
				return true;
			case EValue::SizeOverride:
			{
				TArray<double> Numbers;
				if (!Class.IsChildOf(USizeBox::StaticClass()))
				{
					OutError = FString::Printf(TEXT("%s is not a SizeBox; %s is for SizeBox widgets."), *ClassName, Spec.Id);
					return false;
				}
				if (!ParseNumbers(Value, Numbers) || Numbers.Num() != 1 || Numbers[0] < 0.0)
				{
					OutError = TEXT("the override is a size in slate units, 0 or more.");
					return false;
				}
				return true;
			}
			case EValue::AccessibleText:
				if (Value.IsEmpty())
				{
					OutError = TEXT("accessible_text is what a screen reader says for the widget, e.g. Start a new game.");
					return false;
				}
				return true;
			case EValue::RenderAngle:
			{
				TArray<double> Numbers;
				if (!ParseNumbers(Value, Numbers) || Numbers.Num() != 1)
				{
					OutError = TEXT("render_angle is degrees.");
					return false;
				}
				return true;
			}
			case EValue::RenderScale:
			case EValue::RenderTranslation:
			case EValue::RenderPivot:
			{
				FVector2D Vector;
				if (!ParseVector2(Value, Vector))
				{
					OutError = FString::Printf(TEXT("%s is x,y."), Spec.Id);
					return false;
				}
				return true;
			}
			default:
				break;
			}
			FProperty* Property = FindFProperty<FProperty>(&Class, Spec.Property);
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_EditConst))
			{
				OutError = FString::Printf(TEXT("%s has no %s. %s"), *ClassName, Spec.Id,
					Spec.Value == EValue::Text ? TEXT("A Button's label is a TextBlock inside it: add one and set its text.") : TEXT(""));
				return false;
			}
			if (Spec.Value == EValue::Text)
			{
				if (!CastField<FTextProperty>(Property))
				{
					OutError = FString::Printf(TEXT("%s.%s is not text."), *ClassName, Spec.Property);
					return false;
				}
				OutProperty = Property;
				return true;
			}
			FString ImportText;
			if (!ToImportText(Spec, *Property, Value, ImportText, OutError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), Spec.Id, *OutError);
				return false;
			}
			const FScratch Scratch(*Property);
			if (!Import(*Property, ImportText, Class.GetDefaultObject(), Scratch.Data))
			{
				OutError = FString::Printf(TEXT("%s: '%s' does not fit %s.%s."), Spec.Id, *Value.Left(96), *ClassName, Spec.Property);
				return false;
			}
			OutProperty = Property;
			return true;
		}

		bool CheckSlotValue(const FSpec& Spec, const UClass& SlotClass, const FString& Value, FString& OutError)
		{
			const FString SlotName = SlotClass.GetName();
			if (IsCanvasSpec(Spec))
			{
				FAnchors Anchors;
				FMargin Margin;
				FVector2D Vector;
				bool bFlag = false;
				TArray<double> Numbers;
				if (!SlotClass.IsChildOf(UCanvasPanelSlot::StaticClass()))
				{
					OutError = FString::Printf(TEXT("%s is for a widget in a CanvasPanel; this one's slot is a %s."), Spec.Id, *SlotName);
					return false;
				}
				const bool bValid = Spec.Value == EValue::Anchors ? ParseAnchors(Value, Anchors)
					: Spec.Value == EValue::Offsets ? ParseMargin(Value, Margin)
					: Spec.Value == EValue::AutoSize ? ParseBool(Value, bFlag)
					: Spec.Value == EValue::ZOrder ? ParseNumbers(Value, Numbers) && Numbers.Num() == 1 && Numbers[0] == FMath::RoundToDouble(Numbers[0])
					: ParseVector2(Value, Vector);
				if (!bValid)
				{
					OutError = Spec.Value == EValue::Anchors
						? TEXT("anchors is a preset (top_left, top, top_right, left, center, right, bottom_left, bottom, bottom_right, top_fill, center_fill, bottom_fill, left_fill, middle_fill, right_fill, fill) or minx,miny,maxx,maxy.")
						: Spec.Value == EValue::Offsets ? TEXT("offsets is left,top,right,bottom (with point anchors: x,y,width,height).")
						: Spec.Value == EValue::AutoSize ? TEXT("auto_size is true or false.")
						: Spec.Value == EValue::ZOrder ? TEXT("z_order is a whole number.")
						: FString::Printf(TEXT("%s is x,y."), Spec.Id);
					return false;
				}
				return true;
			}
			FProperty* Property = FindFProperty<FProperty>(&SlotClass, Spec.Property);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("A %s has no %s."), *SlotName, Spec.Id);
				return false;
			}
			FString ImportText;
			if (!ToImportText(Spec, *Property, Value, ImportText, OutError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), Spec.Id, *OutError);
				return false;
			}
			const FScratch Scratch(*Property);
			if (!Import(*Property, ImportText, SlotClass.GetDefaultObject(), Scratch.Data))
			{
				OutError = FString::Printf(TEXT("%s: '%s' does not fit %s.%s."), Spec.Id, *Value.Left(96), *SlotName, Spec.Property);
				return false;
			}
			return true;
		}

		bool WriteReflected(UObject& Owner, FProperty& Property, const FSpec& Spec, const FString& Value, FString& OutError)
		{
			if (Spec.Value == EValue::Text)
			{
				const FText Text = LocalizedText(Owner, Value);
				Write(Owner, Property, &Text);
				return true;
			}
			FString ImportText;
			if (!ToImportText(Spec, Property, Value, ImportText, OutError))
			{
				return false;
			}
			const FScratch Scratch(Property);
			if (!Import(Property, ImportText, &Owner, Scratch.Data))
			{
				OutError = FString::Printf(TEXT("'%s' no longer fits %s."), *Value.Left(96), Spec.Property);
				return false;
			}
			Write(Owner, Property, Scratch.Data);
			return true;
		}

		/** True when Owner's Property already holds what Value imports to. */
		bool Holds(UObject& Owner, const FProperty& Property, const FSpec& Spec, const FString& Value)
		{
			if (Spec.Value == EValue::Text)
			{
				FText Current;
				Property.GetValue_InContainer(&Owner, &Current);
				return Current.ToString() == Value;
			}
			FString ImportText;
			FString Error;
			const FScratch Expected(Property);
			const FScratch Current(Property);
			if (!ToImportText(Spec, Property, Value, ImportText, Error) || !Import(Property, ImportText, &Owner, Expected.Data))
			{
				return false;
			}
			Property.GetValue_InContainer(&Owner, Current.Data);
			return Property.Identical(Expected.Data, Current.Data);
		}
	}

	FPlannedTree::FPlannedTree(const UWidgetBlueprint& Blueprint)
	{
		if (!Blueprint.WidgetTree)
		{
			return;
		}
		Root = Blueprint.WidgetTree->RootWidget ? Blueprint.WidgetTree->RootWidget->GetFName() : NAME_None;
		Blueprint.WidgetTree->ForEachWidget([this](UWidget* Widget)
		{
			if (Widget)
			{
				const UPanelWidget* Parent = Widget->GetParent();
				Widgets.Add(Widget->GetFName(), {Widget->GetClass(), Parent ? Parent->GetFName() : NAME_None});
			}
		});
	}

	const FPlannedWidget* FPlannedTree::Find(const FString& Subject) const
	{
		return Widgets.Find(FName(*WidgetNameOf(Subject)));
	}

	int32 FPlannedTree::ChildCount(const FName Parent) const
	{
		int32 Count = 0;
		for (const TPair<FName, FPlannedWidget>& Each : Widgets)
		{
			Count += Each.Value.Parent == Parent ? 1 : 0;
		}
		return Count;
	}

	FString FPlannedTree::Names() const
	{
		TArray<FString> Names;
		for (const TPair<FName, FPlannedWidget>& Each : Widgets)
		{
			if (Names.Num() < MaxNames) Names.Add(Each.Key.ToString());
		}
		Names.Sort();
		return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
	}

	FString WidgetNameOf(const FString& Subject)
	{
		return Subject.StartsWith(TEXT("name:"), ESearchCase::CaseSensitive) ? Subject.RightChop(5) : Subject;
	}

	bool IsWidgetOp(const FString& Kind)
	{
		return Kind == TEXT("widget.add") || Kind == TEXT("widget.set_property") || Kind == TEXT("widget.set_style")
			|| Kind == TEXT("widget.set_slot") || Kind == TEXT("widget.set_navigation");
	}

	bool Validate(const FHyperAIUIPlanOperation& Operation, const UWidgetBlueprint& Blueprint, FPlannedTree& Tree,
		FString& OutNote, FString& OutError)
	{
		OutNote.Reset();
		OutError.Reset();
		const FString Name = WidgetNameOf(Operation.SubjectId);
		const FString Value = ValueText(Operation);
		if (!Operation.ExpectedElementFingerprint.IsEmpty())
		{
			OutError = TEXT("leave expected_element_fingerprint empty: the plan checks the whole Widget Blueprint's revision.");
			return false;
		}
		if (Operation.Kind == TEXT("widget.add"))
		{
			if (!IsIdentifier(Name))
			{
				OutError = TEXT("subject_id is the new widget's name: letters, digits and _, up to 64.");
				return false;
			}
			if (Tree.Widgets.Contains(FName(*Name)) || Blueprint.Animations.ContainsByPredicate([&Name](const UWidgetAnimation* Each) { return Each && Each->GetName() == Name; }))
			{
				OutError = FString::Printf(TEXT("the Widget Blueprint already has a widget or animation named %s."), *Name);
				return false;
			}
			const UClass* Class = ResolveWidgetClass(Operation.StringValue.TrimStartAndEnd());
			if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				OutError = FString::Printf(TEXT("string_value '%s' is not a widget class: give a UMG name (CanvasPanel, VerticalBox, HorizontalBox, Overlay, SizeBox, Border, Button, TextBlock, Image, ProgressBar, Slider, CheckBox, Spacer, ScrollBox, UniformGridPanel) or a Widget Blueprint path."),
					*Operation.StringValue.Left(96));
				return false;
			}
			if (Class == Blueprint.GeneratedClass.Get() || Class == Blueprint.SkeletonGeneratedClass.Get())
			{
				OutError = TEXT("a Widget Blueprint cannot contain itself.");
				return false;
			}
			const FString ParentName = WidgetNameOf(Operation.SecondaryName);
			if (ParentName.IsEmpty())
			{
				if (!Tree.Root.IsNone())
				{
					OutError = FString::Printf(TEXT("the root is %s already: name a panel as secondary_name to add under it."), *Tree.Root.ToString());
					return false;
				}
				Tree.Root = FName(*Name);
			}
			else
			{
				const FPlannedWidget* Parent = Tree.Find(ParentName);
				const UPanelWidget* Panel = Parent && Parent->Class ? Cast<UPanelWidget>(Parent->Class->GetDefaultObject()) : nullptr;
				if (!Parent)
				{
					OutError = FString::Printf(TEXT("no widget named '%s' to add under; it has %s."), *ParentName, *Tree.Names());
					return false;
				}
				if (!Panel)
				{
					OutError = FString::Printf(TEXT("%s is a %s, which holds no children; add under a panel (CanvasPanel, VerticalBox, Overlay, Border, Button...)."),
						*ParentName, *Parent->Class->GetName());
					return false;
				}
				if (!Panel->CanHaveMultipleChildren() && Tree.ChildCount(FName(*ParentName)) > 0)
				{
					OutError = FString::Printf(TEXT("%s is a %s, which holds one child and has it already."), *ParentName, *Parent->Class->GetName());
					return false;
				}
			}
			Tree.Widgets.Add(FName(*Name), {Class, ParentName.IsEmpty() ? NAME_None : FName(*ParentName)});
			OutNote = FString::Printf(TEXT("add %s %s%s"), *Class->GetName(), *Name, ParentName.IsEmpty() ? TEXT(" as the root") : *(TEXT(" in ") + ParentName));
			return true;
		}

		const FPlannedWidget* Widget = Tree.Find(Name);
		if (!Widget || !Widget->Class)
		{
			OutError = FString::Printf(TEXT("no widget named '%s'; it has %s."), *Name, *Tree.Names());
			return false;
		}
		if (Operation.Kind == TEXT("widget.set_navigation"))
		{
			EUINavigationRule Rule = EUINavigationRule::Escape;
			if (!IsDirection(Operation.Name))
			{
				OutError = TEXT("name is the direction: up, down, left, right, next, previous or all.");
				return false;
			}
			if (!ParseNavigationRule(Operation.StringValue.TrimStartAndEnd(), Rule))
			{
				OutError = TEXT("string_value is the rule: escape, stop, wrap or explicit (with secondary_name, the widget to focus).");
				return false;
			}
			const FString Target = WidgetNameOf(Operation.SecondaryName);
			if (Rule == EUINavigationRule::Explicit ? (!Tree.Find(Target) || Target == Name) : !Target.IsEmpty())
			{
				OutError = Rule == EUINavigationRule::Explicit
					? FString::Printf(TEXT("explicit navigation needs secondary_name, another widget to focus; it has %s."), *Tree.Names())
					: TEXT("only explicit navigation takes secondary_name.");
				return false;
			}
			OutNote = FString::Printf(TEXT("%s %s: %s%s"), *Name, *Operation.Name.ToLower(), *Operation.StringValue.ToLower(),
				Target.IsEmpty() ? TEXT("") : *(TEXT(" to ") + Target));
			return true;
		}
		if (Operation.Kind == TEXT("widget.set_slot"))
		{
			const FSpec* Spec = FindSpec(SlotSpecs, Operation.Name);
			const UClass* SlotClass = SlotClassOf(Blueprint, Tree, FName(*Name));
			if (!Spec)
			{
				OutError = TEXT("name is one of ") + SpecIds(SlotSpecs) + TEXT(".");
				return false;
			}
			if (!SlotClass)
			{
				OutError = FString::Printf(TEXT("%s is the root, which has no slot; lay out its children instead."), *Name);
				return false;
			}
			if (!CheckSlotValue(*Spec, *SlotClass, Value, OutError))
			{
				return false;
			}
			OutNote = FString::Printf(TEXT("%s slot %s = %s"), *Name, Spec->Id, *Value.Left(64));
			return true;
		}
		const bool bStyle = Operation.Kind == TEXT("widget.set_style");
		const FSpec* Spec = bStyle ? FindSpec(StyleSpecs, Operation.Name) : FindSpec(PropertySpecs, Operation.Name);
		if (!Spec)
		{
			OutError = TEXT("name is one of ") + (bStyle ? SpecIds(StyleSpecs) : SpecIds(PropertySpecs)) + TEXT(".");
			return false;
		}
		FProperty* Property = nullptr;
		if (!CheckValue(*Spec, *Widget->Class, Value, Property, OutError))
		{
			return false;
		}
		OutNote = FString::Printf(TEXT("%s %s = %s"), *Name, Spec->Id, *Value.Left(64));
		return true;
	}

	bool Apply(UWidgetBlueprint& Blueprint, const FHyperAIUIPlanOperation& Operation, FString& OutError)
	{
		UWidgetTree* WidgetTree = Blueprint.WidgetTree;
		const FString Name = WidgetNameOf(Operation.SubjectId);
		const FString Value = ValueText(Operation);
		if (!WidgetTree)
		{
			OutError = TEXT("The Widget Blueprint has no widget tree.");
			return false;
		}
		if (Operation.Kind == TEXT("widget.add"))
		{
			UClass* Class = ResolveWidgetClass(Operation.StringValue.TrimStartAndEnd());
			const FString ParentName = WidgetNameOf(Operation.SecondaryName);
			UPanelWidget* Parent = ParentName.IsEmpty() ? nullptr : Cast<UPanelWidget>(WidgetTree->FindWidget(FName(*ParentName)));
			if (!Class || (!ParentName.IsEmpty() && (!Parent || !Parent->CanAddMoreChildren())) || (ParentName.IsEmpty() && WidgetTree->RootWidget))
			{
				OutError = FString::Printf(TEXT("%s could not be added where the plan put it."), *Name);
				return false;
			}
			WidgetTree->Modify();
			UWidget* Widget = WidgetTree->ConstructWidget<UWidget>(Class, FName(*Name));
			if (!Widget)
			{
				OutError = FString::Printf(TEXT("%s could not be constructed."), *Name);
				return false;
			}
			if (Parent)
			{
				Parent->Modify();
				if (!Parent->AddChild(Widget))
				{
					OutError = FString::Printf(TEXT("%s refused %s as a child."), *ParentName, *Name);
					return false;
				}
			}
			else
			{
				WidgetTree->RootWidget = Widget;
			}
			// Every widget is a variable with a stable guid, registered the way the designer registers a widget it adds;
			// the compiler ensures on one without. A stale entry from a widget removed outside the designer is reused.
			if (!Blueprint.WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
			{
				Blueprint.OnVariableAdded(Widget->GetFName());
			}
			return true;
		}
		UWidget* Widget = WidgetTree->FindWidget(FName(*Name));
		if (!Widget)
		{
			OutError = FString::Printf(TEXT("No widget named %s."), *Name);
			return false;
		}
		if (Operation.Kind == TEXT("widget.set_navigation"))
		{
			EUINavigationRule Rule = EUINavigationRule::Escape;
			ParseNavigationRule(Operation.StringValue.TrimStartAndEnd(), Rule);
			Widget->Modify();
			if (!Widget->Navigation)
			{
				// Built before it is transactional, so undo drops the widget's reference instead of replaying its construction.
				UWidgetNavigation* Navigation = NewObject<UWidgetNavigation>(Widget);
				Navigation->SetFlags(RF_Transactional);
				Widget->Navigation = Navigation;
			}
			Widget->Navigation->Modify();
			const FName Target = Rule == EUINavigationRule::Explicit ? FName(*WidgetNameOf(Operation.SecondaryName)) : NAME_None;
			for (FWidgetNavigationData* Data : NavigationSlots(*Widget->Navigation, Operation.Name))
			{
				Data->Rule = Rule;
				Data->WidgetToFocus = Target;
			}
			return true;
		}
		if (Operation.Kind == TEXT("widget.set_slot"))
		{
			const FSpec* Spec = FindSpec(SlotSpecs, Operation.Name);
			UPanelSlot* Slot = Widget->Slot;
			if (!Spec || !Slot)
			{
				OutError = FString::Printf(TEXT("%s has no slot to set %s on."), *Name, *Operation.Name);
				return false;
			}
			if (!IsCanvasSpec(*Spec))
			{
				FProperty* Property = FindFProperty<FProperty>(Slot->GetClass(), Spec->Property);
				return Property && WriteReflected(*Slot, *Property, *Spec, Value, OutError);
			}
			UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(Slot);
			FProperty* LayoutProperty = FindFProperty<FProperty>(UCanvasPanelSlot::StaticClass(), TEXT("LayoutData"));
			if (!Canvas)
			{
				OutError = FString::Printf(TEXT("%s is not in a CanvasPanel."), *Name);
				return false;
			}
			FAnchors Anchors;
			FMargin Margin;
			FVector2D Vector;
			bool bFlag = false;
			TArray<double> Numbers;
			Canvas->PreEditChange(LayoutProperty);
			switch (Spec->Value)
			{
			case EValue::Anchors: ParseAnchors(Value, Anchors); Canvas->SetAnchors(Anchors); break;
			case EValue::Offsets: ParseMargin(Value, Margin); Canvas->SetOffsets(Margin); break;
			case EValue::Position: ParseVector2(Value, Vector); Canvas->SetPosition(Vector); break;
			case EValue::Size: ParseVector2(Value, Vector); Canvas->SetSize(Vector); break;
			case EValue::Alignment: ParseVector2(Value, Vector); Canvas->SetAlignment(Vector); break;
			case EValue::AutoSize: ParseBool(Value, bFlag); Canvas->SetAutoSize(bFlag); break;
			default: ParseNumbers(Value, Numbers); Canvas->SetZOrder(Numbers.IsEmpty() ? 0 : static_cast<int32>(Numbers[0])); break;
			}
			FPropertyChangedEvent Event(LayoutProperty, EPropertyChangeType::ValueSet);
			Canvas->PostEditChangeProperty(Event);
			return true;
		}
		const bool bStyle = Operation.Kind == TEXT("widget.set_style");
		const FSpec* Spec = bStyle ? FindSpec(StyleSpecs, Operation.Name) : FindSpec(PropertySpecs, Operation.Name);
		if (!Spec)
		{
			OutError = FString::Printf(TEXT("Unknown %s %s."), bStyle ? TEXT("style") : TEXT("property"), *Operation.Name);
			return false;
		}
		TArray<double> Numbers;
		FVector2D Vector;
		switch (Spec->Value)
		{
		case EValue::FontSize:
		{
			FStructProperty* Font = CastField<FStructProperty>(FindFProperty<FProperty>(Widget->GetClass(), TEXT("Font")));
			if (!Font || Font->Struct != FSlateFontInfo::StaticStruct() || !ParseNumbers(Value, Numbers))
			{
				OutError = TEXT("The font size no longer applies.");
				return false;
			}
			FSlateFontInfo Info;
			Font->GetValue_InContainer(Widget, &Info);
			Info.Size = static_cast<float>(Numbers[0]);
			Write(*Widget, *Font, &Info);
			return true;
		}
		case EValue::Image:
		{
			UImage* Image = Cast<UImage>(Widget);
			UObject* Resource = LoadBrushResource(Value);
			FProperty* Brush = FindFProperty<FProperty>(UImage::StaticClass(), TEXT("Brush"));
			if (!Image || !Resource)
			{
				OutError = FString::Printf(TEXT("%s cannot show %s."), *Name, *Value);
				return false;
			}
			Image->PreEditChange(Brush);
			if (UTexture2D* Texture = Cast<UTexture2D>(Resource))
			{
				Image->SetBrushFromTexture(Texture, /*bMatchSize=*/true);
			}
			else
			{
				Image->SetBrushFromMaterial(Cast<UMaterialInterface>(Resource));
			}
			FPropertyChangedEvent Event(Brush, EPropertyChangeType::ValueSet);
			Image->PostEditChangeProperty(Event);
			return true;
		}
		case EValue::SizeOverride:
		{
			USizeBox* SizeBox = Cast<USizeBox>(Widget);
			const bool bWidth = Operation.Name == TEXT("width_override");
			FProperty* Property = FindFProperty<FProperty>(USizeBox::StaticClass(), bWidth ? TEXT("WidthOverride") : TEXT("HeightOverride"));
			if (!SizeBox || !ParseNumbers(Value, Numbers))
			{
				OutError = TEXT("The size override no longer applies.");
				return false;
			}
			SizeBox->PreEditChange(Property);
			if (bWidth) SizeBox->SetWidthOverride(Numbers[0]);
			else SizeBox->SetHeightOverride(Numbers[0]);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			SizeBox->PostEditChangeProperty(Event);
			return true;
		}
		case EValue::AccessibleText:
		{
			// The Accessibility section's three fields: override the defaults, describe with custom text, and the text.
			FProperty* Property = FindFProperty<FProperty>(UWidget::StaticClass(), TEXT("AccessibleText"));
			Widget->PreEditChange(Property);
			Widget->bOverrideAccessibleDefaults = true;
			Widget->AccessibleBehavior = ESlateAccessibleBehavior::Custom;
			Widget->AccessibleText = LocalizedText(*Widget, Value);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Widget->PostEditChangeProperty(Event);
			return true;
		}
		case EValue::RenderAngle:
		case EValue::RenderScale:
		case EValue::RenderTranslation:
		case EValue::RenderPivot:
		{
			const bool bPivot = Spec->Value == EValue::RenderPivot;
			FProperty* Property = FindFProperty<FProperty>(UWidget::StaticClass(), bPivot ? TEXT("RenderTransformPivot") : TEXT("RenderTransform"));
			FWidgetTransform Transform = Widget->GetRenderTransform();
			Widget->PreEditChange(Property);
			if (Spec->Value == EValue::RenderAngle && ParseNumbers(Value, Numbers)) Transform.Angle = Numbers[0];
			else if (Spec->Value == EValue::RenderScale && ParseVector2(Value, Vector)) Transform.Scale = Vector;
			else if (Spec->Value == EValue::RenderTranslation && ParseVector2(Value, Vector)) Transform.Translation = Vector;
			if (bPivot && ParseVector2(Value, Vector)) Widget->SetRenderTransformPivot(Vector);
			else Widget->SetRenderTransform(Transform);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Widget->PostEditChangeProperty(Event);
			return true;
		}
		default:
		{
			FProperty* Property = FindFProperty<FProperty>(Widget->GetClass(), Spec->Property);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no %s."), *Name, Spec->Id);
				return false;
			}
			return WriteReflected(*Widget, *Property, *Spec, Value, OutError);
		}
		}
	}

	bool Landed(const UWidgetBlueprint& Blueprint, const FHyperAIUIPlanOperation& Operation, FString& OutWhy)
	{
		const FString Name = WidgetNameOf(Operation.SubjectId);
		const FString Value = ValueText(Operation);
		UWidget* Widget = FindWidget(Blueprint, Operation.SubjectId);
		OutWhy = FString::Printf(TEXT("%s %s"), *Operation.Kind, *Name);
		if (!Widget)
		{
			return false;
		}
		if (Operation.Kind == TEXT("widget.add"))
		{
			const UClass* Class = ResolveWidgetClass(Operation.StringValue.TrimStartAndEnd());
			const FString ParentName = WidgetNameOf(Operation.SecondaryName);
			const UPanelWidget* Parent = Widget->GetParent();
			return Widget->GetClass() == Class && (ParentName.IsEmpty() ? Blueprint.WidgetTree->RootWidget == Widget
				: Parent && Parent->GetName() == ParentName);
		}
		if (Operation.Kind == TEXT("widget.set_navigation"))
		{
			EUINavigationRule Rule = EUINavigationRule::Escape;
			ParseNavigationRule(Operation.StringValue.TrimStartAndEnd(), Rule);
			const FName Target = Rule == EUINavigationRule::Explicit ? FName(*WidgetNameOf(Operation.SecondaryName)) : NAME_None;
			if (!Widget->Navigation)
			{
				return false;
			}
			for (const FWidgetNavigationData* Data : NavigationSlots(*Widget->Navigation, Operation.Name))
			{
				if (Data->Rule != Rule || Data->WidgetToFocus != Target) return false;
			}
			return true;
		}
		if (Operation.Kind == TEXT("widget.set_slot"))
		{
			const FSpec* Spec = FindSpec(SlotSpecs, Operation.Name);
			UPanelSlot* Slot = Widget->Slot;
			if (!Spec || !Slot)
			{
				return false;
			}
			if (!IsCanvasSpec(*Spec))
			{
				const FProperty* Property = FindFProperty<FProperty>(Slot->GetClass(), Spec->Property);
				return Property && Holds(*Slot, *Property, *Spec, Value);
			}
			const UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(Slot);
			FAnchors Anchors;
			FMargin Margin;
			FVector2D Vector;
			bool bFlag = false;
			TArray<double> Numbers;
			if (!Canvas) return false;
			switch (Spec->Value)
			{
			case EValue::Anchors: return ParseAnchors(Value, Anchors) && Canvas->GetAnchors().Minimum.Equals(Anchors.Minimum) && Canvas->GetAnchors().Maximum.Equals(Anchors.Maximum);
			case EValue::Offsets: return ParseMargin(Value, Margin) && Canvas->GetOffsets() == Margin;
			case EValue::Position: return ParseVector2(Value, Vector) && Canvas->GetPosition().Equals(Vector);
			case EValue::Size: return ParseVector2(Value, Vector) && Canvas->GetSize().Equals(Vector);
			case EValue::Alignment: return ParseVector2(Value, Vector) && Canvas->GetAlignment().Equals(Vector);
			case EValue::AutoSize: return ParseBool(Value, bFlag) && Canvas->GetAutoSize() == bFlag;
			default: return ParseNumbers(Value, Numbers) && Canvas->GetZOrder() == static_cast<int32>(Numbers[0]);
			}
		}
		const bool bStyle = Operation.Kind == TEXT("widget.set_style");
		const FSpec* Spec = bStyle ? FindSpec(StyleSpecs, Operation.Name) : FindSpec(PropertySpecs, Operation.Name);
		TArray<double> Numbers;
		FVector2D Vector;
		if (!Spec)
		{
			return false;
		}
		switch (Spec->Value)
		{
		case EValue::FontSize:
		{
			const FStructProperty* Font = CastField<FStructProperty>(FindFProperty<FProperty>(Widget->GetClass(), TEXT("Font")));
			FSlateFontInfo Info;
			if (!Font || !ParseNumbers(Value, Numbers)) return false;
			Font->GetValue_InContainer(Widget, &Info);
			return FMath::IsNearlyEqual(Info.Size, static_cast<float>(Numbers[0]));
		}
		case EValue::Image:
		{
			const UImage* Image = Cast<UImage>(Widget);
			const UObject* Resource = LoadBrushResource(Value);
			return Image && Resource && Image->GetBrush().GetResourceObject() == Resource;
		}
		case EValue::SizeOverride:
		{
			const USizeBox* SizeBox = Cast<USizeBox>(Widget);
			const bool bWidth = Operation.Name == TEXT("width_override");
			return SizeBox && ParseNumbers(Value, Numbers) && FMath::IsNearlyEqual(bWidth ? SizeBox->GetWidthOverride() : SizeBox->GetHeightOverride(), static_cast<float>(Numbers[0]))
				&& (bWidth ? SizeBox->IsWidthOverride() : SizeBox->IsHeightOverride());
		}
		case EValue::AccessibleText: return Widget->bOverrideAccessibleDefaults && Widget->AccessibleBehavior == ESlateAccessibleBehavior::Custom
			&& Widget->AccessibleText.ToString() == Value;
		case EValue::RenderAngle: return ParseNumbers(Value, Numbers) && FMath::IsNearlyEqual(Widget->GetRenderTransform().Angle, static_cast<float>(Numbers[0]));
		case EValue::RenderScale: return ParseVector2(Value, Vector) && Widget->GetRenderTransform().Scale.Equals(Vector);
		case EValue::RenderTranslation: return ParseVector2(Value, Vector) && Widget->GetRenderTransform().Translation.Equals(Vector);
		case EValue::RenderPivot: return ParseVector2(Value, Vector) && Widget->GetRenderTransformPivot().Equals(Vector);
		default:
		{
			const FProperty* Property = FindFProperty<FProperty>(Widget->GetClass(), Spec->Property);
			return Property && Holds(*Widget, *Property, *Spec, Value);
		}
		}
	}

	FProperty* FindAnimatableFloat(const UClass& Class, const FString& Name)
	{
		FProperty* Property = FindFProperty<FProperty>(&Class, FName(*Name));
		return Property && CastField<FFloatProperty>(Property) && Property->HasAnyPropertyFlags(CPF_Edit)
			&& Property->HasAnyPropertyFlags(CPF_BlueprintVisible) && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated)
			? Property : nullptr;
	}

	FString AnimatableFloatNames(const UClass& Class)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(&Class); It; ++It)
		{
			if (Names.Num() < MaxNames && FindAnimatableFloat(Class, It->GetName()))
			{
				Names.Add(It->GetName());
			}
		}
		Names.Sort();
		return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
	}

	void AppendTreeRevision(const UWidgetBlueprint& Blueprint, TArray<FString>& OutEntries)
	{
		if (!Blueprint.WidgetTree)
		{
			return;
		}
		Blueprint.WidgetTree->ForEachWidget([&OutEntries](UWidget* Widget)
		{
			if (!Widget)
			{
				return;
			}
			const UPanelWidget* Parent = Widget->GetParent();
			FString Entry = FString::Printf(TEXT("%s|%s|%s|%s"), *Widget->GetName(), *Widget->GetClass()->GetPathName(),
				Parent ? *Parent->GetName() : TEXT("-"), Widget->Slot ? *Widget->Slot->GetClass()->GetName() : TEXT("-"));
			if (const UWidgetNavigation* Navigation = Widget->Navigation)
			{
				for (const FWidgetNavigationData* Data : {&Navigation->Up, &Navigation->Down, &Navigation->Left, &Navigation->Right, &Navigation->Next, &Navigation->Previous})
				{
					Entry += FString::Printf(TEXT("|%d:%s"), static_cast<int32>(Data->Rule), *Data->WidgetToFocus.ToString());
				}
			}
			OutEntries.Add(MoveTemp(Entry));
		});
		OutEntries.Sort();
	}
}
