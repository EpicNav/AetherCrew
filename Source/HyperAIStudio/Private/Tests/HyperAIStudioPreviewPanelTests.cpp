// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioPreviewImage.h"
#include "HyperAIStudioPreviewPanel.h"
#include "HyperAIStudioSettings.h"
#include "SHyperAIStudioPreviewPanel.h"

#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SWindow.h"

namespace HyperAIStudio::Preview::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString Folder()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests"));
	}

	/** A flat PNG inside the project, which is all the picker accepts. */
	bool WriteImage(const FString& Path, const FColor& Color, const int32 Side)
	{
		FImage Image;
		Image.Init(Side, Side, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		for (FColor& Pixel : Image.AsBGRA8())
		{
			Pixel = Color;
		}
		return FImageUtils::SaveImageByExtension(*Path, Image);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPreviewPathTest,
	"HyperAIStudio.Preview.ReferencePathsAreProjectImages",
	HyperAIStudio::Preview::Tests::Flags)

bool FHyperAIStudioPreviewPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Preview::Tests;
	const FString Path = Folder() / TEXT("preview_reference.png");
	if (!TestTrue(TEXT("a test image is written"), WriteImage(Path, FColor(120, 120, 120), 8)))
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); };

	FString Why;
	TestTrue(TEXT("a PNG inside the project is accepted"), FHyperAIStudioPreviewPanel::IsSupportedProjectImage(Path, Why));
	TestTrue(TEXT("nothing selected is refused with a reason"),
		!FHyperAIStudioPreviewPanel::IsSupportedProjectImage(FString(), Why) && !Why.IsEmpty());
	TestTrue(TEXT("a document is refused"),
		!FHyperAIStudioPreviewPanel::IsSupportedProjectImage(Folder() / TEXT("notes.txt"), Why) && Why.Contains(TEXT("PNG")));
	TestTrue(TEXT("an image outside the project is refused, because the tools cannot read it"),
		!FHyperAIStudioPreviewPanel::IsSupportedProjectImage(TEXT("C:/Elsewhere/photo.png"), Why) && Why.Contains(TEXT("project")));
	TestTrue(TEXT("a missing file inside the project is refused"),
		!FHyperAIStudioPreviewPanel::IsSupportedProjectImage(Folder() / TEXT("gone.png"), Why) && Why.Contains(TEXT("no longer")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPreviewImageTest,
	"HyperAIStudio.Preview.ImagesLoadAndReload",
	HyperAIStudio::Preview::Tests::Flags)

bool FHyperAIStudioPreviewImageTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Preview::Tests;
	const FString Path = Folder() / TEXT("preview_reload.png");
	if (!TestTrue(TEXT("a test image is written"), WriteImage(Path, FColor(40, 80, 160), 16)))
	{
		return false;
	}
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*Path); };

	FHyperAIStudioPreviewImage Image;
	TestNull(TEXT("an empty path has no brush"), Image.Get(FString()));
	TestNull(TEXT("a missing file has no brush"), Image.Get(Folder() / TEXT("gone.png")));

	const FSlateBrush* First = Image.Get(Path);
	if (!TestNotNull(*FString::Printf(TEXT("the image loads (%s)"), *Image.GetError()), First))
	{
		return false;
	}
	TestEqual(TEXT("at its own size"), Image.GetSourceSize(), FIntPoint(16, 16));
	TestEqual(TEXT("and the same brush comes back unchanged"), Image.Get(Path), First);

	// The compare tool overwrites its capture in place, so a stale timestamp would show yesterday's frame.
	FPlatformProcess::Sleep(1.1f);
	if (TestTrue(TEXT("the file is written again, larger"), WriteImage(Path, FColor(200, 30, 30), 32)))
	{
		const FSlateBrush* Second = Image.Get(Path);
		TestNotNull(TEXT("the rewritten file loads"), Second);
		TestEqual(TEXT("at its new size"), Image.GetSourceSize(), FIntPoint(32, 32));
		TestNotEqual(TEXT("as a different brush"), Second, First);
	}

	Image.Forget();
	TestEqual(TEXT("forgetting clears the size"), Image.GetSourceSize(), FIntPoint::ZeroValue);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPreviewSourceTest,
	"HyperAIStudio.Preview.PacksRegisterSources",
	HyperAIStudio::Preview::Tests::Flags)

bool FHyperAIStudioPreviewSourceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FName Id(TEXT("preview_test_source"));
	const int32 Before = FHyperAIStudioPreviewPanel::GetSources().Num();
	int32 Changes = 0;
	const FDelegateHandle Handle = FHyperAIStudioPreviewPanel::OnSourcesChanged().AddLambda([&Changes]() { ++Changes; });
	ON_SCOPE_EXIT
	{
		FHyperAIStudioPreviewPanel::OnSourcesChanged().Remove(Handle);
		FHyperAIStudioPreviewPanel::UnregisterSource(Id);
	};

	FHyperAIStudioPreviewPanel::FSource Source;
	Source.Id = Id;
	Source.Label = FText::FromString(TEXT("Zzz test source"));
	Source.bNeedsReference = true;
	Source.Refresh = FHyperAIStudioPreviewRefresh::CreateLambda(
		[](const FString& Reference, FHyperAIStudioPreviewComparison& Out, FString& Error)
		{
			Out.ImagePathA = Reference;
			Out.Stats.Add({FText::FromString(TEXT("Distance")), FText::FromString(TEXT("0.00")), EHyperAIStudioPreviewTone::Good});
			return true;
		});
	FHyperAIStudioPreviewPanel::RegisterSource(Source);
	TestEqual(TEXT("registering adds one source"), FHyperAIStudioPreviewPanel::GetSources().Num(), Before + 1);
	TestEqual(TEXT("and reports the change"), Changes, 1);

	const FHyperAIStudioPreviewPanel::FSource* Found = FHyperAIStudioPreviewPanel::FindSource(Id);
	if (!TestNotNull(TEXT("the source is found by id"), Found))
	{
		return false;
	}
	TestTrue(TEXT("it says it needs a reference"), Found->bNeedsReference);
	FHyperAIStudioPreviewComparison Comparison;
	FString Error;
	TestTrue(TEXT("and fills a comparison"), Found->Refresh.Execute(TEXT("/ref.png"), Comparison, Error));
	TestEqual(TEXT("with its stats"), Comparison.Stats.Num(), 1);

	// A pack that reloads should replace its entry, not add a second one.
	FHyperAIStudioPreviewPanel::RegisterSource(Source);
	TestEqual(TEXT("registering again replaces it"), FHyperAIStudioPreviewPanel::GetSources().Num(), Before + 1);

	FHyperAIStudioPreviewPanel::UnregisterSource(Id);
	TestEqual(TEXT("withdrawing removes it"), FHyperAIStudioPreviewPanel::GetSources().Num(), Before);
	TestNull(TEXT("and it is gone"), FHyperAIStudioPreviewPanel::FindSource(Id));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPreviewRecentsTest,
	"HyperAIStudio.Preview.RecentReferencesStayShortAndOrdered",
	HyperAIStudio::Preview::Tests::Flags)

bool FHyperAIStudioPreviewRecentsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FString> Restore = Settings->RecentPreviewReferences;
	ON_SCOPE_EXIT { Settings->RecentPreviewReferences = Restore; };

	Settings->RecentPreviewReferences.Reset();
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Settings->RememberPreviewReference(FString::Printf(TEXT("/Game/Ref_%d.png"), Index));
	}
	TestEqual(TEXT("the list stays short"), Settings->RecentPreviewReferences.Num(), 8);
	TestEqual(TEXT("newest first"), Settings->RecentPreviewReferences[0], FString(TEXT("/Game/Ref_11.png")));

	Settings->RememberPreviewReference(TEXT("/Game/Ref_5.png"));
	TestEqual(TEXT("picking one again moves it to the front"), Settings->RecentPreviewReferences[0], FString(TEXT("/Game/Ref_5.png")));
	TestEqual(TEXT("without duplicating it"),
		Settings->RecentPreviewReferences.FilterByPredicate([](const FString& Path) { return Path == TEXT("/Game/Ref_5.png"); }).Num(), 1);

	const int32 Count = Settings->RecentPreviewReferences.Num();
	Settings->RememberPreviewReference(FString());
	TestEqual(TEXT("an empty path is ignored"), Settings->RecentPreviewReferences.Num(), Count);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPreviewPanelBuildsTest,
	"HyperAIStudio.Preview.PanelBuilds",
	HyperAIStudio::Preview::Tests::Flags)

bool FHyperAIStudioPreviewPanelBuildsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// The panel reads whatever sources are registered at construction, including none at all.
	const TSharedRef<SHyperAIStudioPreviewPanel> Panel = SNew(SHyperAIStudioPreviewPanel);
	TestTrue(TEXT("the panel builds"), Panel->GetType() == TEXT("SHyperAIStudioPreviewPanel"));
	const TSharedRef<SHyperAIStudioComparisonView> View = SNew(SHyperAIStudioComparisonView);
	View->ResetView();
	// Desired size is only known after a layout pass, which no window has run for these widgets.
	View->SlatePrepass(1.0f);
	TestTrue(TEXT("the comparison view builds with no images"), View->GetDesiredSize().X > 0.0);
	const TSharedRef<SHyperAIStudioHistogramView> Histogram = SNew(SHyperAIStudioHistogramView)
		.HistogramA(TArray<float>({0.1f, 0.4f, 0.5f}));
	TestTrue(TEXT("the histogram builds"), Histogram->GetType() == TEXT("SHyperAIStudioHistogramView"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPreviewPaintTest,
	"HyperAIStudio.Preview.PanelPaintsBothImages",
	HyperAIStudio::Preview::Tests::Flags)

bool FHyperAIStudioPreviewPaintTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Preview::Tests;
	if (!FSlateApplication::IsInitialized() || !FApp::CanEverRender())
	{
		AddInfo(TEXT("Needs a rendering Slate application; skipped."));
		return true;
	}
	const FString PathA = Folder() / TEXT("preview_paint_a.png");
	const FString PathB = Folder() / TEXT("preview_paint_b.png");
	if (!TestTrue(TEXT("two test images are written"),
		WriteImage(PathA, FColor(40, 60, 110), 64) && WriteImage(PathB, FColor(190, 160, 90), 64)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*PathA);
		IFileManager::Get().Delete(*PathB);
	};

	const FName Id(TEXT("preview_paint_source"));
	FHyperAIStudioPreviewPanel::FSource Source;
	Source.Id = Id;
	Source.Label = FText::FromString(TEXT("Paint test"));
	Source.bNeedsReference = true;
	Source.Refresh = FHyperAIStudioPreviewRefresh::CreateLambda(
		[PathA, PathB](const FString& Reference, FHyperAIStudioPreviewComparison& Out, FString& Error)
		{
			Out.ImagePathA = PathA;
			Out.ImagePathB = PathB;
			Out.LabelA = FText::FromString(TEXT("Reference"));
			Out.LabelB = FText::FromString(TEXT("Level viewport"));
			Out.Stats.Add({FText::FromString(TEXT("Distance")), FText::FromString(TEXT("0.42")), EHyperAIStudioPreviewTone::Warning});
			Out.Stats.Add({FText::FromString(TEXT("Exposure")), FText::FromString(TEXT("-1.20 EV")), EHyperAIStudioPreviewTone::Warning});
			Out.HistogramA = {0.05f, 0.30f, 0.45f, 0.15f, 0.05f};
			Out.HistogramB = {0.25f, 0.35f, 0.20f, 0.15f, 0.05f};
			Out.Warning = FText::FromString(TEXT("Auto exposure is on, so this measures the eye, not the lighting."));
			Out.WarningActionLabel = FText::FromString(TEXT("Lock exposure"));
			Out.WarningAction = FSimpleDelegate::CreateLambda([]() {});
			return true;
		});
	FHyperAIStudioPreviewPanel::RegisterSource(Source);
	ON_SCOPE_EXIT { FHyperAIStudioPreviewPanel::UnregisterSource(Id); };

	const TSharedRef<SHyperAIStudioPreviewPanel> Panel = SNew(SHyperAIStudioPreviewPanel);
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.ClientSize(FVector2D(900.0, 620.0))
		.AutoCenter(EAutoCenter::None)
		.ScreenPosition(FVector2D(0.0, 0.0))
		.CreateTitleBar(false)
		.IsTopmostWindow(false);
	// Slate skips drawing a hidden window, and an undrawn widget cannot be captured.
	FSlateApplication::Get().AddWindow(Window, /*bShowImmediately=*/true);
	Window->SetContent(Panel);
	Window->ShowWindow();
	// The panel takes whichever source was registered first, so point it at this one.
	FHyperAIStudioPreviewPanel::OnOpenRequested().Broadcast(Id, PathA);
	for (int32 Frame = 0; Frame < 6; ++Frame)
	{
		FSlateApplication::Get().Tick();
	}

	TArray<FColor> Pixels;
	FIntVector Size = FIntVector::ZeroValue;
	const bool bCaptured = FSlateApplication::Get().TakeScreenshot(Panel, Pixels, Size);
	FSlateApplication::Get().RequestDestroyWindow(Window);
	if (!TestTrue(TEXT("the panel is captured"), bCaptured && Size.X > 0 && Size.Y > 0 && Pixels.Num() > 0))
	{
		return false;
	}

	// A panel that failed to paint is one flat colour; a real one has the two images and the chrome around them.
	TSet<uint32> Distinct;
	for (const FColor& Pixel : Pixels)
	{
		Distinct.Add(Pixel.ToPackedARGB());
		if (Distinct.Num() > 64) break;
	}
	TestTrue(FString::Printf(TEXT("and painted something (%d colours in %dx%d)"), Distinct.Num(), Size.X, Size.Y), Distinct.Num() > 16);

	FImage Shot;
	Shot.Init(Size.X, Size.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	TArrayView64<FColor> Out = Shot.AsBGRA8();
	for (int64 Index = 0; Index < Out.Num() && Index < Pixels.Num(); ++Index)
	{
		Out[Index] = Pixels[Index];
		Out[Index].A = 255;
	}
	const FString ShotPath = Folder() / TEXT("preview_panel.png");
	TestTrue(TEXT("and the capture is written for a look"), FImageUtils::SaveImageByExtension(*ShotPath, Shot));
	AddInfo(FString::Printf(TEXT("Panel capture: %s"), *ShotPath));
	return true;
}

#endif
