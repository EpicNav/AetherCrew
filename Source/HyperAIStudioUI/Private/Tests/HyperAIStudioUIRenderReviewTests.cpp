// Games by Hyper 2026.

#include "HyperAIStudioUIRenderReview.h"
#include "HyperAIStudioUIToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetNavigation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "WidgetBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HyperAIStudio::UI::ReviewTests
{
	using namespace HyperAIStudio::UI::Review;

	FWidgetBox Box(const TCHAR* Path, const FVector2D Min, const FVector2D Size)
	{
		FWidgetBox Out;
		Out.Path = Path;
		Out.Rect = FBox2D(Min, Min + Size);
		Out.DesiredLocal = Size;
		Out.AllottedLocal = Size;
		return Out;
	}

	bool HasIssue(const TArray<FHyperAIUIReviewIssue>& Issues, const TCHAR* Code, const TCHAR* WidgetSuffix = nullptr)
	{
		return Issues.ContainsByPredicate([Code, WidgetSuffix](const FHyperAIUIReviewIssue& Issue)
		{
			return Issue.Code == Code && (!WidgetSuffix || Issue.Widget.EndsWith(WidgetSuffix));
		});
	}

	FString Codes(const TArray<FHyperAIUIReviewIssue>& Issues)
	{
		TArray<FString> Out;
		for (const FHyperAIUIReviewIssue& Issue : Issues) Out.Add(Issue.Code + TEXT("@") + Issue.Widget);
		return FString::Join(Out, TEXT(", "));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIRenderReviewChecksTest,
	"HyperAIStudio.NativeTools.UI.RenderReview.Checks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIRenderReviewChecksTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::UI::ReviewTests;
	FIntPoint Size;
	TestTrue(TEXT("1920x1080 parses"), ParseResolution(TEXT("1920x1080"), Size) && Size == FIntPoint(1920, 1080));
	TestTrue(TEXT("2560*1440 parses"), ParseResolution(TEXT(" 2560*1440 "), Size) && Size == FIntPoint(2560, 1440));
	TestFalse(TEXT("a word does not"), ParseResolution(TEXT("full hd"), Size));
	TestFalse(TEXT("nor a size too small"), ParseResolution(TEXT("100x100"), Size));
	TestTrue(TEXT("black on white is 21:1"), FMath::IsNearlyEqual(ContrastRatio(FLinearColor::Black, FLinearColor::White), 21.f, 0.01f));
	TestTrue(TEXT("a colour on itself is 1:1"), FMath::IsNearlyEqual(ContrastRatio(FLinearColor(0.3f, 0.2f, 0.1f), FLinearColor(0.3f, 0.2f, 0.1f)), 1.f, 0.001f));
	TestTrue(TEXT("a 12 pt font at UI scale 1 is 16 px"), FMath::IsNearlyEqual(FontPixels(12.f, 1.f), 16.f));
	TestEqual(TEXT("rects print as x,y wxh"), RectText(FBox2D(FVector2D(10.2, 20.6), FVector2D(110.2, 70.6))), FString(TEXT("10,21 100x50")));

	// White text on a red plate, glyphs covering a quarter of the box: the plate is what it is read against.
	TArray<FColor> Pixels;
	const FIntPoint Screen(64, 32);
	Pixels.Init(FColor(200, 30, 30), Screen.X * Screen.Y);
	for (int32 Y = 8; Y < 16; ++Y)
	{
		for (int32 X = 8; X < 24; ++X) Pixels[Y * Screen.X + X] = FColor::White;
	}
	FLinearColor Background;
	TestTrue(TEXT("a background is found"), EstimateBackground(Pixels, Screen, FBox2D(FVector2D(0, 0), FVector2D(32, 32)), FLinearColor::White, Background));
	TestTrue(TEXT("and it is the red plate"), FMath::Abs(Background.ToFColor(true).R - 200) <= 8);
	TestFalse(TEXT("a box off the image has none"), EstimateBackground(Pixels, Screen, FBox2D(FVector2D(100, 100), FVector2D(120, 120)), FLinearColor::White, Background));

	FHyperAIUIRenderReviewRequest Rules;
	const FIntPoint Full(1920, 1080);
	TArray<FHyperAIUIReviewIssue> Issues;
	{
		// A clean layout: a backdrop, a title and two buttons well inside the safe zone.
		TArray<FWidgetBox> Boxes = {Box(TEXT("Root"), FVector2D(0, 0), FVector2D(1920, 1080)), Box(TEXT("Root/Title"), FVector2D(700, 200), FVector2D(520, 60)),
			Box(TEXT("Root/Play"), FVector2D(800, 500), FVector2D(320, 80)), Box(TEXT("Root/Quit"), FVector2D(800, 620), FVector2D(320, 80))};
		Boxes[0].bContent = false;
		Boxes[1].bText = true;
		Boxes[1].FontPixels = 40.f;
		Boxes[2].bInteractive = Boxes[3].bInteractive = true;
		CheckLayout(Boxes, Full, Rules, TEXT("1920x1080"), nullptr, Issues);
		TestEqual(TEXT("a clean layout raises nothing: ") + Codes(Issues), Issues.Num(), 0);
	}
	{
		Issues.Reset();
		TArray<FWidgetBox> Boxes = {Box(TEXT("Root/Title"), FVector2D(700, 200), FVector2D(100, 40)), Box(TEXT("Root/Small"), FVector2D(700, 300), FVector2D(100, 20)),
			Box(TEXT("Root/Off"), FVector2D(1850, 400), FVector2D(200, 60)), Box(TEXT("Root/Edge"), FVector2D(20, 20), FVector2D(100, 30)),
			Box(TEXT("Root/A"), FVector2D(900, 600), FVector2D(200, 60)), Box(TEXT("Root/B"), FVector2D(1000, 620), FVector2D(200, 60)),
			Box(TEXT("Root/Clip"), FVector2D(400, 700), FVector2D(100, 100)), Box(TEXT("Root/Clip/Picture"), FVector2D(460, 760), FVector2D(80, 80))};
		Boxes[0].bText = true;
		Boxes[0].DesiredLocal = FVector2D(260, 40);
		Boxes[1].bText = true;
		Boxes[1].FontPixels = 9.f;
		Boxes[4].bInteractive = Boxes[5].bInteractive = true;
		Boxes[6].bContent = false;
		Boxes[6].bClipsChildren = true;
		CheckLayout(Boxes, Full, Rules, TEXT("1920x1080"), nullptr, Issues);
		TestTrue(TEXT("text wider than its slot is clipped: ") + Codes(Issues), HasIssue(Issues, TEXT("text_clipped"), TEXT("Title")));
		TestTrue(TEXT("9 px text is too small"), HasIssue(Issues, TEXT("text_too_small"), TEXT("Small")));
		TestTrue(TEXT("a box past the right edge is off screen"), HasIssue(Issues, TEXT("offscreen"), TEXT("Off")));
		TestTrue(TEXT("a box in the outer 5% is outside the safe zone"), HasIssue(Issues, TEXT("outside_safe_zone"), TEXT("Edge")));
		TestTrue(TEXT("two buttons on top of each other overlap"), HasIssue(Issues, TEXT("overlapping_interactive"), TEXT("A")));
		TestTrue(TEXT("a picture past its clipping panel is cut off"), HasIssue(Issues, TEXT("clipped_by_parent"), TEXT("Picture")));
		Rules.SafeZone = 1.f;
		Issues.Reset();
		CheckLayout(Boxes, Full, Rules, TEXT("1920x1080"), nullptr, Issues);
		TestFalse(TEXT("safe zone 1 checks only the screen edge"), HasIssue(Issues, TEXT("outside_safe_zone")));
	}
	{
		// Dark grey text on the dark backdrop reads poorly; white on it reads fine.
		Issues.Reset();
		Rules = FHyperAIUIRenderReviewRequest();
		TArray<FColor> Dark;
		Dark.Init(FColor(31, 31, 31), Full.X * Full.Y);
		TArray<FWidgetBox> Boxes = {Box(TEXT("Root/Dim"), FVector2D(600, 400), FVector2D(300, 40)), Box(TEXT("Root/Bright"), FVector2D(600, 500), FVector2D(300, 40))};
		Boxes[0].bText = Boxes[1].bText = true;
		Boxes[0].FontPixels = Boxes[1].FontPixels = 20.f;
		Boxes[0].TextColor = FLinearColor(0.02f, 0.02f, 0.02f);
		Boxes[1].TextColor = FLinearColor::White;
		CheckLayout(Boxes, Full, Rules, TEXT("1920x1080"), &Dark, Issues);
		TestTrue(TEXT("dim text is low contrast: ") + Codes(Issues), HasIssue(Issues, TEXT("low_contrast"), TEXT("Dim")));
		TestFalse(TEXT("white text is not"), HasIssue(Issues, TEXT("low_contrast"), TEXT("Bright")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioUIRenderReviewEndToEndTest,
	"HyperAIStudio.NativeTools.UI.RenderReview.EndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioUIRenderReviewEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::UI::ReviewTests;
	// A menu with a problem of every kind, built the way the widget blueprint factory builds one.
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Name = TEXT("WBP_HyperAIReview_") + Suffix;
	const FString Target = FString::Printf(TEXT("/Game/__HyperAIStudioTests/%s.%s"), *Name, *Name);
	UPackage* Package = CreatePackage(*(TEXT("/Game/__HyperAIStudioTests/") + Name));
	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UUserWidget::StaticClass(), Package,
		FName(*Name), BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("the fixture Widget Blueprint exists"), Blueprint)) return false;
	ON_SCOPE_EXIT { ObjectTools::ForceDeleteObjects({Blueprint}, /*ShowConfirmation=*/false); };
	UWidgetTree& Tree = *Blueprint->WidgetTree;
	UCanvasPanel* Root = Tree.ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Tree.RootWidget = Root;
	auto Place = [Root](UWidget* Widget, const FVector2D Position, const FVector2D Size)
	{
		UCanvasPanelSlot* Slot = Root->AddChildToCanvas(Widget);
		Slot->SetAutoSize(false);
		Slot->SetPosition(Position);
		Slot->SetSize(Size);
		return Slot;
	};
	UTextBlock* Title = Tree.ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Title"));
	Title->SetText(FText::FromString(TEXT("A title far too long for the little box it was given")));
	Title->SetColorAndOpacity(FSlateColor(FLinearColor(0.01f, 0.01f, 0.01f)));
	Place(Title, FVector2D(700, 200), FVector2D(120, 40));
	UTextBlock* Small = Tree.ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Small"));
	Small->SetText(FText::FromString(TEXT("fine print")));
	FSlateFontInfo SmallFont = Small->GetFont();
	SmallFont.Size = 7;
	Small->SetFont(SmallFont);
	Small->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	Place(Small, FVector2D(700, 320), FVector2D(300, 30));
	UTextBlock* Hud = Tree.ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Hud"));
	Hud->SetText(FText::FromString(TEXT("HP")));
	Hud->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	Place(Hud, FVector2D(10, 10), FVector2D(80, 40));
	UButton* Play = Tree.ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Play"));
	// Is Focusable off and an explicit Down rule to a widget that does not exist, as the Details panel would set them.
	if (FBoolProperty* Focusable = FindFProperty<FBoolProperty>(UButton::StaticClass(), TEXT("IsFocusable")))
	{
		Focusable->SetPropertyValue_InContainer(Play, false);
	}
	Play->Navigation = NewObject<UWidgetNavigation>(Play, NAME_None, RF_Transactional);
	Play->Navigation->Down.Rule = EUINavigationRule::Explicit;
	Play->Navigation->Down.WidgetToFocus = TEXT("Missing");
	Place(Play, FVector2D(1820, 500), FVector2D(200, 60));
	UButton* Quit = Tree.ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Quit"));
	Place(Quit, FVector2D(1760, 520), FVector2D(120, 60));
	UCanvasPanel* Clip = Tree.ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Clip"));
	Clip->SetClipping(EWidgetClipping::ClipToBounds);
	Place(Clip, FVector2D(600, 600), FVector2D(100, 100));
	UImage* Picture = Tree.ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Picture"));
	UCanvasPanelSlot* PictureSlot = Clip->AddChildToCanvas(Picture);
	PictureSlot->SetAutoSize(false);
	PictureSlot->SetPosition(FVector2D(70, 70));
	PictureSlot->SetSize(FVector2D(80, 80));
	FAssetRegistryModule::AssetCreated(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// Refusals first: nothing is drawn for a bad request.
	FHyperAIUIRenderReviewRequest Request;
	Request.TargetPath = Target;
	Request.Resolutions = {TEXT("full hd")};
	TestEqual(TEXT("a resolution that is not WxH is refused"), UHyperAIStudioUIToolset::hyper_ui_render_review(Request).Status, FString(TEXT("invalid_request")));
	Request.Resolutions = {TEXT("1920x1080"), TEXT("1280x720"), TEXT("2560x1440"), TEXT("3840x2160"), TEXT("800x600")};
	TestEqual(TEXT("so are more than four sizes"), UHyperAIStudioUIToolset::hyper_ui_render_review(Request).Status, FString(TEXT("invalid_request")));
	FHyperAIUIRenderReviewRequest Missing;
	Missing.TargetPath = TEXT("/Game/__HyperAIStudioTests/WBP_DoesNotExist.WBP_DoesNotExist");
	TestEqual(TEXT("a missing asset is not_found"), UHyperAIStudioUIToolset::hyper_ui_render_review(Missing).Status, FString(TEXT("not_found")));
	FHyperAIUIRenderReviewRequest FromPlay;
	FromPlay.TargetPath = Target;
	FromPlay.bFromPlay = true;
	if (!GEditor || !GEditor->PlayWorld)
	{
		TestEqual(TEXT("from_play without Play In Editor is no_pie_world"), UHyperAIStudioUIToolset::hyper_ui_render_review(FromPlay).Status, FString(TEXT("no_pie_world")));
	}

	Request.Resolutions = {TEXT("1920x1080"), TEXT("1280x720")};
	const FHyperAIUIRenderReviewReport Report = UHyperAIStudioUIToolset::hyper_ui_render_review(Request);
	if (Report.Status == TEXT("cannot_render"))
	{
		AddInfo(TEXT("Skipped the render checks: ") + Report.Diagnostic);
		return true;
	}
	if (!TestTrue(TEXT("the menu is reviewed: ") + Report.Diagnostic, Report.bOk && Report.Status == TEXT("reviewed"))) return false;
	TestEqual(TEXT("from a design-time instance"), Report.Source, FString(TEXT("design_time")));
	if (!TestEqual(TEXT("two renders"), Report.Renders.Num(), 2)) return false;
	const TPair<int32, FIntPoint> Expected[] = {{0, FIntPoint(1920, 1080)}, {1, FIntPoint(1280, 720)}};
	for (const TPair<int32, FIntPoint>& Each : Expected)
	{
		const FHyperAIUIRender& Render = Report.Renders[Each.Key];
		FImage Image;
		TestTrue(FString::Printf(TEXT("the %s PNG is written and reads back"), *Render.Resolution), FImageUtils::LoadImage(*Render.ImagePath, Image));
		TestTrue(FString::Printf(TEXT("at full size (%dx%d)"), Image.SizeX, Image.SizeY), Image.SizeX == Each.Value.X && Image.SizeY == Each.Value.Y);
		TestTrue(TEXT("with the widgets measured"), Render.Widgets >= 8);
	}
	TestTrue(TEXT("720p draws at the project's smaller UI scale"), Report.Renders[1].DpiScale < Report.Renders[0].DpiScale);
	const FString Seen = Codes(Report.Issues);
	TestTrue(TEXT("the long title is clipped: ") + Seen, HasIssue(Report.Issues, TEXT("text_clipped"), TEXT("Title")));
	TestTrue(TEXT("and too dark on the backdrop"), HasIssue(Report.Issues, TEXT("low_contrast"), TEXT("Title")));
	TestTrue(TEXT("the fine print is too small"), HasIssue(Report.Issues, TEXT("text_too_small"), TEXT("Small")));
	TestTrue(TEXT("Play hangs off the right edge"), HasIssue(Report.Issues, TEXT("offscreen"), TEXT("Play")));
	TestTrue(TEXT("Play and Quit overlap"), HasIssue(Report.Issues, TEXT("overlapping_interactive")));
	TestTrue(TEXT("the HUD text sits outside the safe zone"), HasIssue(Report.Issues, TEXT("outside_safe_zone"), TEXT("Hud")));
	TestTrue(TEXT("the picture is cut off by its clipping panel"), HasIssue(Report.Issues, TEXT("clipped_by_parent"), TEXT("Picture")));
	TestTrue(TEXT("Play cannot take focus"), HasIssue(Report.Issues, TEXT("not_focusable"), TEXT("Play")));
	TestTrue(TEXT("its explicit navigation points nowhere"), HasIssue(Report.Issues, TEXT("broken_navigation"), TEXT("Play")));
	TestTrue(TEXT("and nothing has initial focus"), HasIssue(Report.Issues, TEXT("no_initial_focus")));
	TestTrue(TEXT("errors are listed first"), Report.Issues.Num() > 0 && Report.Issues[0].Severity == TEXT("error"));
	TestEqual(TEXT("the counts add up"), Report.ErrorCount + Report.WarningCount, Report.Issues.Num());

	// The polish loop's compare: the render against itself scores near the top and is matched.
	const FString Reference = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()) / TEXT("HyperAIStudio") / TEXT("UIReviewTests") / (Name + TEXT("_reference.png"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Reference), /*Tree=*/true);
	TestTrue(TEXT("the reference is copied in"), IFileManager::Get().Copy(*Reference, *Report.Renders[0].ImagePath) == COPY_OK);
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Reference); };
	Request.Resolutions = {TEXT("1920x1080")};
	Request.ReferenceImagePath = Reference;
	Request.SessionId = TEXT("uireview-") + Suffix;
	const FHyperAIUIRenderReviewReport Compared = UHyperAIStudioUIToolset::hyper_ui_render_review(Request);
	TestTrue(TEXT("the second review runs: ") + Compared.Diagnostic, Compared.bOk);
	if (!Compared.Reference.bCompared)
	{
		AddInfo(TEXT("Compare not checked: ") + Compared.Reference.Diagnostic);
		return true;
	}
	TestTrue(FString::Printf(TEXT("the render matches itself (%.1f)"), Compared.Reference.Score), Compared.Reference.Score >= 95.f && Compared.Reference.bMatched);
	TestTrue(TEXT("with a heat map written"), !Compared.Reference.HeatMapPath.IsEmpty()
		&& IFileManager::Get().FileExists(*FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Compared.Reference.HeatMapPath)
			? FPaths::ProjectDir() / Compared.Reference.HeatMapPath : Compared.Reference.HeatMapPath)));
	TestEqual(TEXT("in the polish loop's session"), Compared.Reference.SessionId, Request.SessionId);
	return true;
}

#endif
