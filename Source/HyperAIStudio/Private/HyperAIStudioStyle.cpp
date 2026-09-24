// Games by Hyper 2026.

#include "HyperAIStudioStyle.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleColors.h"

TSharedPtr<FSlateStyleSet> FHyperAIStudioStyle::Instance;

void FHyperAIStudioStyle::Register()
{
	if (Instance.IsValid())
	{
		return;
	}

	TSharedRef<FSlateStyleSet> Style = MakeShared<FSlateStyleSet>(GetStyleSetName());
	Style->SetParentStyleName(FAppStyle::GetAppStyleSetName());
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio")))
	{
		Style->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	}

	const FTextBlockStyle Normal = FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
	auto AddText = [&Style, &Normal](const FName Name, const FSlateFontInfo& Font, const FSlateColor& Color)
	{
		FTextBlockStyle Text = Normal;
		Text.SetFont(Font);
		Text.SetColorAndOpacity(Color);
		Style->Set(Name, Text);
	};
	const FSlateColor Subdued = FSlateColor::UseSubduedForeground();

	// The type ramp: one size per role, so every page reads alike.
	AddText("HyperAIStudio.Text.Title", FCoreStyle::GetDefaultFontStyle("Bold", 12), FStyleColors::Foreground);
	AddText("HyperAIStudio.Text.Display", FCoreStyle::GetDefaultFontStyle("Bold", 18), FStyleColors::ForegroundHover);
	AddText("HyperAIStudio.Text.PageTitle", FCoreStyle::GetDefaultFontStyle("Bold", 15), FStyleColors::ForegroundHover);
	AddText("HyperAIStudio.Text.CardTitle", FCoreStyle::GetDefaultFontStyle("Bold", 11), FStyleColors::ForegroundHover);
	AddText("HyperAIStudio.Text.Body", FCoreStyle::GetDefaultFontStyle("Regular", 10), FStyleColors::Foreground);
	AddText("HyperAIStudio.Text.Subdued", FCoreStyle::GetDefaultFontStyle("Regular", 10), Subdued);
	AddText("HyperAIStudio.Text.Caption", FCoreStyle::GetDefaultFontStyle("Regular", 9), Subdued);
	AddText("HyperAIStudio.Text.Metric", FCoreStyle::GetDefaultFontStyle("Bold", 16), FStyleColors::ForegroundHover);
	FSlateFontInfo SectionFont = FCoreStyle::GetDefaultFontStyle("Bold", 8);
	SectionFont.LetterSpacing = 120;
	AddText("HyperAIStudio.Text.Section", SectionFont, Subdued);
	AddText("HyperAIStudio.Text.Nav", FCoreStyle::GetDefaultFontStyle("Regular", 10), FStyleColors::Foreground);
	AddText("HyperAIStudio.Text.NavSelected", FCoreStyle::GetDefaultFontStyle("Bold", 10), FStyleColors::ForegroundHover);

	// Markdown, for agent replies and reports.
	AddText("Md.Body", FCoreStyle::GetDefaultFontStyle("Regular", 10), FStyleColors::Foreground);
	AddText("Md.Bold", FCoreStyle::GetDefaultFontStyle("Bold", 10), FStyleColors::ForegroundHover);
	AddText("Md.Italic", FCoreStyle::GetDefaultFontStyle("Italic", 10), FStyleColors::Foreground);
	AddText("Md.Code", FCoreStyle::GetDefaultFontStyle("Mono", 9), FLinearColor(0.91f, 0.74f, 0.48f));
	AddText("Md.H1", FCoreStyle::GetDefaultFontStyle("Bold", 15), FStyleColors::ForegroundHover);
	AddText("Md.H2", FCoreStyle::GetDefaultFontStyle("Bold", 12), FStyleColors::ForegroundHover);
	AddText("Md.H3", FCoreStyle::GetDefaultFontStyle("Bold", 10), FStyleColors::ForegroundHover);
	AddText("Md.Quote", FCoreStyle::GetDefaultFontStyle("Italic", 10), Subdued);
	AddText("Md.TableHead", FCoreStyle::GetDefaultFontStyle("Bold", 9), Subdued);
	FHyperlinkStyle Link = FCoreStyle::Get().GetWidgetStyle<FHyperlinkStyle>("Hyperlink");
	FTextBlockStyle LinkText = Normal;
	LinkText.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 10));
	LinkText.SetColorAndOpacity(FStyleColors::AccentBlue);
	Link.SetTextStyle(LinkText);
	Link.SetPadding(FMargin(0.0f));
	Style->Set("Md.Link", Link);

	// A badge is state, not chrome: a rounded outlined pill instead of the flat recessed panel brush.
	Style->Set("HyperAIStudio.Badge", new FSlateRoundedBoxBrush(FStyleColors::Input, 4.0f, FStyleColors::InputOutline, 1.0f));
	Style->Set("HyperAIStudio.Panel", new FSlateRoundedBoxBrush(FStyleColors::Recessed, 4.0f));
	// Tinted at use with the terminal's own background, so padding around the cell grid reads as part of the terminal.
	Style->Set("HyperAIStudio.TerminalFrame", new FSlateRoundedBoxBrush(FLinearColor::White, 6.0f));
	Style->Set("HyperAIStudio.TabIcon", new FSlateVectorImageBrush(Style->RootToContentDir(TEXT("HyperAIStudioTab"), TEXT(".svg")), FVector2f(16.0f, 16.0f)));

	// Surfaces: the page behind, the cards on it, the rail and bars around it.
	Style->Set("HyperAIStudio.Page", new FSlateColorBrush(FStyleColors::Recessed));
	Style->Set("HyperAIStudio.Card", new FSlateRoundedBoxBrush(FStyleColors::Panel, 6.0f, FStyleColors::Header, 1.0f));
	Style->Set("HyperAIStudio.Card.Hovered", new FSlateRoundedBoxBrush(FStyleColors::Header, 6.0f, FStyleColors::Hover2, 1.0f));
	Style->Set("HyperAIStudio.Rail", new FSlateColorBrush(FStyleColors::Panel));
	Style->Set("HyperAIStudio.Bar", new FSlateColorBrush(FStyleColors::Panel));
	Style->Set("HyperAIStudio.Divider", new FSlateColorBrush(FStyleColors::Recessed));
	Style->Set("HyperAIStudio.Chip", new FSlateRoundedBoxBrush(FLinearColor::White, 9.0f));
	Style->Set("HyperAIStudio.Dot", new FSlateRoundedBoxBrush(FLinearColor::White, 4.0f, FVector2f(8.0f, 8.0f)));
	Style->Set("HyperAIStudio.Count", new FSlateRoundedBoxBrush(FStyleColors::AccentBlue, 8.0f));
	Style->Set("HyperAIStudio.CodeBlock", new FSlateRoundedBoxBrush(FStyleColors::Input, 4.0f, FStyleColors::Header, 1.0f));
	Style->Set("HyperAIStudio.TableFrame", new FSlateRoundedBoxBrush(FStyleColors::Header, 4.0f));
	Style->Set("HyperAIStudio.TableHeader", new FSlateColorBrush(FStyleColors::Header));
	Style->Set("HyperAIStudio.TableRow", new FSlateColorBrush(FStyleColors::Panel));
	Style->Set("HyperAIStudio.TableRowAlt", new FSlateColorBrush(FStyleColors::Recessed));

	// Navigation: a quiet row that lights on hover, and a selected row with an accent edge.
	const FButtonStyle Simple = FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton");
	FButtonStyle NavItem = Simple;
	NavItem.SetNormal(FSlateNoResource());
	NavItem.SetHovered(FSlateRoundedBoxBrush(FStyleColors::Hover, 4.0f));
	NavItem.SetPressed(FSlateRoundedBoxBrush(FStyleColors::Hover2, 4.0f));
	NavItem.SetNormalPadding(FMargin(0.0f));
	NavItem.SetPressedPadding(FMargin(0.0f));
	Style->Set("HyperAIStudio.NavItem", NavItem);
	Style->Set("HyperAIStudio.NavItem.Selected", new FSlateRoundedBoxBrush(FStyleColors::Select.GetSpecifiedColor().CopyWithNewOpacity(0.22f), 4.0f));
	Style->Set("HyperAIStudio.NavItem.Accent", new FSlateRoundedBoxBrush(FStyleColors::Primary, 1.5f));
	FButtonStyle Tile = Simple;
	Tile.SetNormal(FSlateRoundedBoxBrush(FStyleColors::Panel, 6.0f, FStyleColors::Header, 1.0f));
	Tile.SetHovered(FSlateRoundedBoxBrush(FStyleColors::Header, 6.0f, FStyleColors::Primary, 1.0f));
	Tile.SetPressed(FSlateRoundedBoxBrush(FStyleColors::Hover2, 6.0f, FStyleColors::Primary, 1.0f));
	Tile.SetNormalPadding(FMargin(0.0f));
	Tile.SetPressedPadding(FMargin(0.0f));
	Style->Set("HyperAIStudio.Tile", Tile);

	// Icons: a set for the navigation, drawn white and tinted by the theme; the logo in its own colours.
	for (const TCHAR* Icon : {TEXT("Home"), TEXT("Chat"), TEXT("Images"), TEXT("Match"), TEXT("Playtest"), TEXT("Memory"), TEXT("Skills"),
		TEXT("Agents"), TEXT("Tools"), TEXT("Activity"), TEXT("Settings"), TEXT("Menu"), TEXT("Report"), TEXT("Terminal"), TEXT("Thinking"),
		TEXT("File")})
	{
		Style->Set(FName(*FString::Printf(TEXT("HyperAIStudio.Icon.%s"), Icon)),
			new FSlateVectorImageBrush(Style->RootToContentDir(FString::Printf(TEXT("Icons/%s"), Icon), TEXT(".svg")), FVector2f(16.0f, 16.0f)));
	}
	Style->Set("HyperAIStudio.Logo", new FSlateVectorImageBrush(Style->RootToContentDir(TEXT("Icons/Logo"), TEXT(".svg")), FVector2f(24.0f, 24.0f)));

	FSlateStyleRegistry::RegisterSlateStyle(*Style);
	Instance = Style;
}

void FHyperAIStudioStyle::Unregister()
{
	if (Instance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*Instance);
		Instance.Reset();
	}
}

const ISlateStyle& FHyperAIStudioStyle::Get()
{
	check(Instance.IsValid());
	return *Instance;
}

FName FHyperAIStudioStyle::GetStyleSetName()
{
	static const FName Name(TEXT("HyperAIStudioStyle"));
	return Name;
}
