// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioDocs.h"
#include "HyperAIStudioShell.h"
#include "HyperAIStudioWidgetShot.h"
#include "ImageUtils.h"
#include "IWebBrowserWindow.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioDocsViewer.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"

namespace HyperAIStudio::DocsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	using HyperAIStudio::Docs::EVerdict;

	const TCHAR* VerdictName(const EVerdict Verdict)
	{
		return Verdict == EVerdict::Inside ? TEXT("Inside") : Verdict == EVerdict::External ? TEXT("External") : TEXT("Blocked");
	}

	/** What the user's browser would have been asked to open, kept from it while a test runs. */
	struct FLaunches
	{
		TArray<FString> Urls;
#if PLATFORM_WINDOWS
		FLaunchURLOverride Previous;
		FLaunches()
		{
			Previous = FPlatformProcess::LaunchURLOverrideDelegate;
			FPlatformProcess::LaunchURLOverrideDelegate.BindLambda([this](const TCHAR* Url) { Urls.Add(Url); });
		}
		~FLaunches()
		{
			FPlatformProcess::LaunchURLOverrideDelegate = Previous;
		}
#endif
	};

	/** Lets the editor run: tickers, the browser's message loop, game-thread tasks and, when asked, Slate, until Done or Seconds pass. */
	bool Pump(const double Seconds, TFunctionRef<bool()> Done, const bool bSlate = false)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (!Done() && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			if (bSlate && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().Tick();
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return Done();
	}

	FWebNavigationRequest Request(const bool bMainFrame, const bool bRedirect, const EWebTransitionSource Source)
	{
		FWebNavigationRequest Out = {};
		Out.bIsMainFrame = bMainFrame;
		Out.bIsRedirect = bRedirect;
		Out.TransitionSource = Source;
		Out.TransitionSourceQualifier = EWebTransitionSourceQualifier::Unknown;
		Out.bIsExplicitTransition = Source == EWebTransitionSource::Explicit;
		return Out;
	}

	TSharedPtr<SHyperAIStudioDocsViewer> FindViewer(const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == TEXT("SHyperAIStudioDocsViewer"))
		{
			return StaticCastSharedRef<SHyperAIStudioDocsViewer>(Widget);
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			if (const TSharedPtr<SHyperAIStudioDocsViewer> Found = FindViewer(Children->GetChildAt(Index)))
			{
				return Found;
			}
		}
		return nullptr;
	}

	/** The widget as the editor drew it in its window, which a live browser needs; empty when it could not be taken. */
	FString SaveWindowShot(const TSharedRef<SWidget>& Widget, const FString& Name)
	{
		TArray<FColor> Pixels;
		FIntVector Size;
		if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().TakeScreenshot(Widget, Pixels, Size) || Size.X <= 0 || Size.Y <= 0)
		{
			return FString();
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), static_cast<int64>(Size.X) * Size.Y), Png);
		const FString File = HyperAIStudio::Tests::ScreensFolder() / (Name + TEXT(".png"));
		IFileManager::Get().MakeDirectory(*HyperAIStudio::Tests::ScreensFolder(), /*Tree=*/true);
		return FFileHelper::SaveArrayToFile(Png, *File) ? File : FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDocsUrlPolicyTest,
	"HyperAIStudio.Docs.UrlPolicy",
	HyperAIStudio::DocsTests::Flags)

bool FHyperAIStudioDocsUrlPolicyTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Docs;
	using namespace HyperAIStudio::DocsTests;
	// The URL's text alone, against a Docs folder that need not exist.
	const FString Root = TEXT("E:/Plugins/HyperAI Studio/Docs");
	const FString Docs = TEXT("file:///E:/Plugins/HyperAI%20Studio/Docs/");
	struct FCase
	{
		FString Url;
		bool bPage;
		EVerdict Expected;
		FString Relative;
		FString Anchor;
	};
	const FCase Cases[] = {
		// Inside: pages, with their anchors; the query is ignored.
		{Docs + TEXT("index.html"), true, EVerdict::Inside, TEXT("index.html"), TEXT("")},
		{Docs + TEXT("setup.html#trouble"), true, EVerdict::Inside, TEXT("setup.html"), TEXT("trouble")},
		{Docs + TEXT("chat.html?lang=zh#approvals"), true, EVerdict::Inside, TEXT("chat.html"), TEXT("approvals")},
		{Docs + TEXT("reference.html?x=1"), true, EVerdict::Inside, TEXT("reference.html"), TEXT("")},
		{TEXT("file:///e:/plugins/hyperai%20studio/docs/memory.html"), true, EVerdict::Inside, TEXT("memory.html"), TEXT("")},
		{TEXT("FILE://localhost/E:/Plugins/HyperAI%20Studio/Docs/tools.html"), true, EVerdict::Inside, TEXT("tools.html"), TEXT("")},
		{TEXT("file:///E:/Plugins/HyperAI Studio/Docs/match.html"), true, EVerdict::Inside, TEXT("match.html"), TEXT("")},
		{TEXT("file:///E:\\Plugins\\HyperAI%20Studio\\Docs\\setup.html"), true, EVerdict::Inside, TEXT("setup.html"), TEXT("")},
		{Docs + TEXT("%E6%96%87%E6%A1%A3.html"), true, EVerdict::Inside, TEXT("\u6587\u6863.html"), TEXT("")},
		// The files a page loads are inside, but not pages to go to.
		{Docs + TEXT("styles/site.css"), false, EVerdict::Inside, TEXT("styles/site.css"), TEXT("")},
		{Docs + TEXT("assets/chat.png"), false, EVerdict::Inside, TEXT("assets/chat.png"), TEXT("")},
		{Docs + TEXT("styles/site.css"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("assets/chat.png"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		// Traversal, plain and escaped.
		{Docs + TEXT("../Source/HyperAIStudio/HyperAIStudio.Build.cs"), false, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("../HyperAIStudio.uplugin"), false, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("%2e%2e/HyperAIStudio.uplugin"), false, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("assets/..%2f..%2fsecret.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("assets/..\\..\\secret.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:///E:/Plugins/HyperAI%20Studio/Docs%5c..%5csecret.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("./index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("assets//chat.png"), false, EVerdict::Blocked, TEXT(""), TEXT("")},
		// Other files: beside the folder, the folder itself, elsewhere, another drive.
		{TEXT("file:///E:/Plugins/HyperAI%20Studio/HyperAIStudio.uplugin"), false, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:///E:/Plugins/HyperAI%20Studio/DocsCopy/index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:///E:/Plugins/HyperAI%20Studio/Docs"), false, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs, false, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:///C:/Windows/win.ini"), false, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:///D:/Plugins/HyperAI%20Studio/Docs/index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:///E:Plugins/HyperAI%20Studio/Docs/index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:///etc/passwd"), false, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:/E:/Plugins/HyperAI%20Studio/Docs/index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		// Names Windows reads as something else.
		{Docs + TEXT("index.html::$DATA"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("index.html%3A%3A$DATA"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("index.html."), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("CON.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("index%00.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("index%zz.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		// Shares.
		{TEXT("file://server/share/Docs/index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:////server/share/Docs/index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:///\\\\server\\share\\Docs\\index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("file:///%5C%5Cserver/share/Docs/index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		// Websites: never shown, opened in the user's browser when a link leads there.
		{TEXT("https://lore.org"), true, EVerdict::External, TEXT(""), TEXT("")},
		{TEXT("http://example.com/page?q=1#part"), true, EVerdict::External, TEXT(""), TEXT("")},
		{TEXT("HTTPS://Example.com/"), true, EVerdict::External, TEXT(""), TEXT("")},
		{TEXT("https://fonts.example.com/font.woff2"), false, EVerdict::External, TEXT(""), TEXT("")},
		{TEXT("https://"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("https:example.com"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		// Every other scheme.
		{TEXT("javascript:alert(1)"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("JavaScript:alert(document.cookie)"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("data:text/html,<script>alert(1)</script>"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("data:image/png;base64,iVBORw0KGgo="), false, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("blob:https://example.com/0f6a1c2e"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("about:blank"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("view-source:") + Docs + TEXT("index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("chrome-devtools://devtools/devtools.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("chrome://settings"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("mailto:someone@example.com"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("ftp://example.com/index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		// Not URLs at all.
		{TEXT("E:/Plugins/HyperAI Studio/Docs/index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT("index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT(""), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{TEXT(" ") + Docs + TEXT("index.html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + TEXT("index.html\n"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
		{Docs + FString::ChrN(5000, TEXT('a')) + TEXT(".html"), true, EVerdict::Blocked, TEXT(""), TEXT("")},
	};
	int32 Passed = 0;
	for (const FCase& Case : Cases)
	{
		const FUrlCheck Check = CheckUrl(Case.Url, Root, Case.bPage);
		const bool bOk = Check.Verdict == Case.Expected && Check.Relative == Case.Relative && Check.Anchor == Case.Anchor;
		Passed += bOk;
		TestTrue(FString::Printf(TEXT("%s (%s) is %s, got %s %s%s"), *Case.Url.Left(120), Case.bPage ? TEXT("page") : TEXT("file"),
			VerdictName(Case.Expected), VerdictName(Check.Verdict), *Check.Relative, *Check.Reason), bOk);
		if (Check.Verdict == EVerdict::Inside)
		{
			TestTrue(TEXT("An inside file is under the folder"), Check.File.StartsWith(Root + TEXT("/")) && Check.File.EndsWith(Check.Relative));
		}
		else
		{
			TestTrue(TEXT("Nothing else names a file"), Check.File.IsEmpty() && Check.Relative.IsEmpty());
		}
	}
	AddInfo(FString::Printf(TEXT("URL policy: %d of %d cases as expected."), Passed, static_cast<int32>(UE_ARRAY_COUNT(Cases))));

	// The viewer's own URLs: anchors kept, spaces and other letters encoded, nothing outside.
	TestEqual(TEXT("A page's URL"), MakeUrl(Root, TEXT("setup.html#trouble")), Docs + TEXT("setup.html#trouble"));
	TestEqual(TEXT("A root with a backslash and a trailing slash"), MakeUrl(TEXT("E:\\Plugins\\HyperAI Studio\\Docs\\"), TEXT("index.html")), Docs + TEXT("index.html"));
	TestEqual(TEXT("Its own URL reads back as the page and anchor"), CheckUrl(MakeUrl(Root, TEXT("chat.html#team")), Root, true).Anchor, FString(TEXT("team")));
	TestEqual(TEXT("Another language's file name round-trips"), CheckUrl(MakeUrl(Root, TEXT("\u6587\u6863.html")), Root, true).Relative, FString(TEXT("\u6587\u6863.html")));
	for (const TCHAR* Bad : {TEXT("../HyperAIStudio.uplugin"), TEXT("styles/site.css"), TEXT("C:/Windows/win.ini"), TEXT(""), TEXT("#top"), TEXT("a/../../x.html")})
	{
		TestTrue(FString::Printf(TEXT("No URL for %s"), Bad), MakeUrl(Root, Bad).IsEmpty());
	}
	TestTrue(TEXT("No URL without a Docs folder"), MakeUrl(FString(), TEXT("index.html")).IsEmpty());
	TestTrue(TEXT("Without a Docs folder nothing is inside"), CheckUrl(Docs + TEXT("index.html"), FString(), true).Verdict == EVerdict::Blocked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDocsRealPathsTest,
	"HyperAIStudio.Docs.RealPaths",
	HyperAIStudio::DocsTests::Flags)

bool FHyperAIStudioDocsRealPathsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Docs;
	// The plugin's own pages pass the full check; a page that is not there does not.
	const FString Root = GetRoot();
	TestTrue(TEXT("The plugin's Docs folder is found"), !Root.IsEmpty() && IFileManager::Get().DirectoryExists(*Root));
	TestTrue(TEXT("index.html is shown"), Evaluate(MakeUrl(Root, TEXT("index.html")), true).Verdict == EVerdict::Inside);
	TestTrue(TEXT("A stylesheet loads"), Evaluate(TEXT("file:///") + Root + TEXT("/styles/site.css"), false).Verdict == EVerdict::Inside);
	TestTrue(TEXT("A missing page is refused"), Evaluate(MakeUrl(Root, TEXT("no-such-page.html")), true).Verdict == EVerdict::Blocked);
	TestTrue(TEXT("The plugin's descriptor is refused"), Evaluate(TEXT("file:///") + FPaths::GetPath(Root) + TEXT("/HyperAIStudio.uplugin"), false).Verdict == EVerdict::Blocked);

	// A junction inside a Docs folder that leads out of it: inside by its text, outside where it really goes.
	const FString Base = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("DocsLinks"));
	const FString TempRoot = Base / TEXT("Docs");
	const FString Outside = Base / TEXT("Outside");
	const FString Link = TempRoot / TEXT("linked");
	IFileManager::Get().DeleteDirectory(*Link, /*RequireExists=*/false, /*Tree=*/false);
	IFileManager::Get().DeleteDirectory(*Base, /*RequireExists=*/false, /*Tree=*/true);
	FFileHelper::SaveStringToFile(TEXT("<title>Page</title>"), *(TempRoot / TEXT("page.html")));
	FFileHelper::SaveStringToFile(TEXT("<title>Secret</title>"), *(Outside / TEXT("secret.html")));
	ON_SCOPE_EXIT
	{
		// The link first, alone, so nothing is deleted through it.
		IFileManager::Get().DeleteDirectory(*Link, /*RequireExists=*/false, /*Tree=*/false);
		IFileManager::Get().DeleteDirectory(*Base, /*RequireExists=*/false, /*Tree=*/true);
	};
	TestTrue(TEXT("A file in the folder is in it"), IsRealPathInside(TempRoot / TEXT("page.html"), TempRoot));
	TestFalse(TEXT("A file beside it is not"), IsRealPathInside(Outside / TEXT("secret.html"), TempRoot));
	TestFalse(TEXT("A file that is not there is not"), IsRealPathInside(TempRoot / TEXT("missing.html"), TempRoot));
	TestFalse(TEXT("The folder is not inside itself"), IsRealPathInside(TempRoot, TempRoot));

	int32 Code = -1;
	FString Out;
	FString Error;
	FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *FString::Printf(TEXT("/c mklink /J \"%s\" \"%s\""),
		*FPaths::ConvertRelativePathToFull(Link).Replace(TEXT("/"), TEXT("\\")), *Outside.Replace(TEXT("/"), TEXT("\\"))), &Code, &Out, &Error);
	if (Code != 0 || !IFileManager::Get().FileExists(*(Link / TEXT("secret.html"))))
	{
		AddWarning(FString::Printf(TEXT("Could not make a junction to test with (%d: %s %s); the link case was not checked."), Code, *Out.TrimEnd(), *Error.TrimEnd()));
		return true;
	}
	const FUrlCheck ByText = CheckUrl(MakeUrl(TempRoot, TEXT("linked/secret.html")), TempRoot, true);
	TestTrue(TEXT("By its text the linked page is inside"), ByText.Verdict == EVerdict::Inside);
	TestFalse(TEXT("but it really is outside, so the viewer refuses it"), IsRealPathInside(ByText.File, TempRoot));
	TestTrue(TEXT("and the folder's own page is still inside"), IsRealPathInside(TempRoot / TEXT("page.html"), TempRoot));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDocsPagesTest,
	"HyperAIStudio.Docs.Pages",
	HyperAIStudio::DocsTests::Flags)

bool FHyperAIStudioDocsPagesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Docs;
	// The plugin's docs, in the order their overview links them.
	const FString Root = GetRoot();
	const TArray<FPage> Pages = GetPages(Root);
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Root / TEXT("*.html")), true, false);
	TArray<FString> Order;
	for (const FPage& Page : Pages)
	{
		Order.Add(Page.File + TEXT("=") + Page.Title);
		TestFalse(FString::Printf(TEXT("%s has a title without the site's name"), *Page.File), Page.Title.IsEmpty() || Page.Title.EndsWith(TEXT("\u2014 HyperAI Studio")));
		TestTrue(FString::Printf(TEXT("%s is a page the viewer shows"), *Page.File), Evaluate(MakeUrl(Root, Page.File), true).Verdict == EVerdict::Inside);
	}
	AddInfo(TEXT("Pages: ") + FString::Join(Order, TEXT(" | ")));
	TestEqual(TEXT("Every Docs/*.html is listed"), Pages.Num(), Files.Num());
	TArray<FString> Names;
	for (const FPage& Page : Pages)
	{
		Names.Add(Page.File);
	}
	TestEqual(TEXT("In the docs' own order"), FString::Join(Names, TEXT(",")), FString(TEXT("index.html,setup.html,chat.html,match.html,memory.html,tools.html,reference.html")));
	if (Pages.Num() >= 2)
	{
		TestEqual(TEXT("The overview keeps its whole title"), Pages[0].Title, FString(TEXT("HyperAI Studio \u2014 Documentation")));
		TestEqual(TEXT("Set up loses the site's name"), Pages[1].Title, FString(TEXT("Set up")));
	}

	// A folder of its own: entities, a page with no title, one the overview does not link, and a page in a subfolder.
	const FString Temp = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("DocsPages"));
	IFileManager::Get().DeleteDirectory(*Temp, false, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Temp, false, true); };
	FFileHelper::SaveStringToFile(TEXT("<title>Start &amp; Overview &mdash; HyperAI Studio</title><a href=\"#main\">skip</a>")
		TEXT("<a href=\"b.html\">B</a><a href=\"a.html#x\">A</a><a href=\"https://example.com/c.html\">C</a>"), *(Temp / TEXT("index.html")));
	FFileHelper::SaveStringToFile(TEXT("<title>Alpha &amp; Beta \u2014 HyperAI Studio</title>"), *(Temp / TEXT("a.html")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(TEXT("<p>No title.</p>"), *(Temp / TEXT("b.html")));
	FFileHelper::SaveStringToFile(TEXT("<TITLE>\r\n  Gamma\r\n  page </TITLE>"), *(Temp / TEXT("c.html")));
	FFileHelper::SaveStringToFile(TEXT("<title>Nested</title>"), *(Temp / TEXT("more") / TEXT("d.html")));
	TArray<FString> Got;
	for (const FPage& Page : GetPages(Temp))
	{
		Got.Add(Page.File + TEXT("=") + Page.Title);
	}
	TestEqual(TEXT("Overview first, its links in order, then the rest; titles read, or the file's name"), FString::Join(Got, TEXT("|")),
		FString(TEXT("index.html=Start & Overview|b.html=b|a.html=Alpha & Beta|c.html=Gamma page")));
	TestEqual(TEXT("No folder, no pages"), GetPages(FString()).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDocsViewerTest,
	"HyperAIStudio.Docs.Viewer",
	HyperAIStudio::DocsTests::Flags)

bool FHyperAIStudioDocsViewerTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Docs;
	using namespace HyperAIStudio::DocsTests;
	FLaunches Launches;
	const FString Root = GetRoot();

	// What an editor without a web browser shows: a message, Open in browser, and nothing to navigate.
	const TSharedRef<SHyperAIStudioDocsViewer> Fallback = SNew(SHyperAIStudioDocsViewer).UseBrowser(false).InitialPage(TEXT("setup.html#trouble"));
	TestFalse(TEXT("Without a browser no page shows here"), Fallback->IsBrowserShown());
	TestEqual(TEXT("It starts on the page asked for"), Fallback->GetCurrentPage(), FString(TEXT("setup.html#trouble")));
	TestEqual(TEXT("The picker lists the docs' pages"), Fallback->GetPages().Num(), GetPages(Root).Num());
	TestTrue(TEXT("A page of the docs is gone to"), Fallback->Navigate(TEXT("chat.html#approvals")) && Fallback->GetCurrentPage() == TEXT("chat.html#approvals"));
	for (const TCHAR* Bad : {TEXT("../HyperAIStudio.uplugin"), TEXT("styles/site.css"), TEXT("C:/Windows/win.ini"), TEXT("https://example.com")})
	{
		TestFalse(FString::Printf(TEXT("%s is not gone to"), Bad), Fallback->Navigate(Bad));
	}
	TestEqual(TEXT("and the page stays"), Fallback->GetCurrentPage(), FString(TEXT("chat.html#approvals")));
	const TSharedRef<SHyperAIStudioDocsViewer> BadStart = SNew(SHyperAIStudioDocsViewer).UseBrowser(false).InitialPage(TEXT("../x.html"));
	TestEqual(TEXT("A bad start page starts on the overview"), BadStart->GetCurrentPage(), FString(TEXT("index.html")));
	const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(Fallback, FIntPoint(1100, 560), TEXT("docs-viewer-no-browser"), 6);
	if (!Shot.IsEmpty())
	{
		AddInfo(TEXT("Picture docs-viewer-no-browser: ") + Shot);
	}

	// The browser's questions, asked as it asks them.
	const FString Index = MakeUrl(Root, TEXT("index.html"));
	const FWebNavigationRequest Link = Request(true, false, EWebTransitionSource::Link);
	TestFalse(TEXT("A page of the docs is shown"), Fallback->HandleBeforeNavigation(Index, Link));
	TestFalse(TEXT("so is one with an anchor, from the toolbar"), Fallback->HandleBeforeNavigation(MakeUrl(Root, TEXT("setup.html#trouble")), Request(true, false, EWebTransitionSource::Explicit)));
	TestEqual(TEXT("and nothing opens outside"), Launches.Urls.Num(), 0);
	TestTrue(TEXT("A link to a website is not shown here"), Fallback->HandleBeforeNavigation(TEXT("https://lore.org"), Link));
	TestTrue(TEXT("but opens in the user's browser"), Launches.Urls.Num() == 1 && Launches.Urls[0] == TEXT("https://lore.org"));
	TestTrue(TEXT("A second website straight after is refused"), Fallback->HandleBeforeNavigation(TEXT("https://example.com"), Link));
	TestEqual(TEXT("and not opened"), Launches.Urls.Num(), 1);
	const TSharedRef<SHyperAIStudioDocsViewer> Fresh = SNew(SHyperAIStudioDocsViewer).UseBrowser(false);
	TestTrue(TEXT("A redirect to a website is refused"), Fresh->HandleBeforeNavigation(TEXT("https://example.com"), Request(true, true, EWebTransitionSource::Link)));
	TestTrue(TEXT("so is a frame going to one"), Fresh->HandleBeforeNavigation(TEXT("https://example.com"), Request(false, false, EWebTransitionSource::AutoSubframe)));
	TestTrue(TEXT("so is a page going to one by itself"), Fresh->HandleBeforeNavigation(TEXT("https://example.com"), Request(true, false, EWebTransitionSource::Explicit)));
	TestEqual(TEXT("none of which opens anything"), Launches.Urls.Num(), 1);
	for (const FString& Bad : {FString(TEXT("javascript:alert(1)")), FString(TEXT("data:text/html,hi")), FString(TEXT("blob:https://example.com/1")),
		FString(TEXT("file:///C:/Windows/win.ini")), FString(TEXT("file://server/share/index.html")), TEXT("file:///") + Root + TEXT("/../HyperAIStudio.uplugin"),
		TEXT("file:///") + Root + TEXT("/assets/chat.png"), MakeUrl(Root, TEXT("index.html")).Replace(TEXT("index.html"), TEXT("missing.html"))})
	{
		TestTrue(FString::Printf(TEXT("%s is refused"), *Bad), Fresh->HandleBeforeNavigation(Bad, Link));
	}
	TestEqual(TEXT("and opens nothing"), Launches.Urls.Num(), 1);

	// Windows: a website in the user's browser, a page of the docs here, anything else nowhere.
	TestTrue(TEXT("A page asking for a window on a website gets none"), Fresh->HandleBeforePopup(TEXT("https://example.com/new"), TEXT("_blank")));
	TestTrue(TEXT("the website opens in the user's browser"), Launches.Urls.Num() == 2 && Launches.Urls[1] == TEXT("https://example.com/new"));
	TestTrue(TEXT("A window on a page of the docs gets none either"), Fresh->HandleBeforePopup(MakeUrl(Root, TEXT("memory.html#held")), TEXT("_blank")));
	Pump(1.0, [&Fresh]() { return Fresh->GetCurrentPage() == TEXT("memory.html#held"); });
	TestEqual(TEXT("the page opens here instead"), Fresh->GetCurrentPage(), FString(TEXT("memory.html#held")));
	TestTrue(TEXT("A window on anything else is refused"), Fresh->HandleBeforePopup(TEXT("javascript:alert(1)"), TEXT("_blank")));
	TestTrue(TEXT("and so is the browser's developer tools"), Fresh->HandleBeforePopup(TEXT("chrome-devtools://devtools/devtools.html"), TEXT("devtools")));
	TestEqual(TEXT("opening nothing"), Launches.Urls.Num(), 2);

	// What a page loads: the Docs folder's files, nothing else.
	FString Response = TEXT("unchanged");
	TestFalse(TEXT("A stylesheet from Docs/styles loads"), Fresh->HandleLoadUrl(TEXT("GET"), TEXT("file:///") + Root + TEXT("/styles/site.css"), Response));
	TestFalse(TEXT("A picture from Docs/assets loads"), Fresh->HandleLoadUrl(TEXT("GET"), TEXT("file:///") + Root + TEXT("/assets/chat.png"), Response));
	TestFalse(TEXT("The page itself loads"), Fresh->HandleLoadUrl(TEXT("GET"), Index, Response));
	TestEqual(TEXT("untouched"), Response, FString(TEXT("unchanged")));
	for (const FString& Bad : {FString(TEXT("https://fonts.example.com/font.woff2")), FString(TEXT("https://cdn.example.com/lib.js")),
		FString(TEXT("file:///C:/Windows/win.ini")), TEXT("file:///") + FPaths::GetPath(Root) + TEXT("/HyperAIStudio.uplugin")})
	{
		Response = TEXT("unchanged");
		TestTrue(FString::Printf(TEXT("%s is answered with nothing"), *Bad), Fresh->HandleLoadUrl(TEXT("GET"), Bad, Response) && Response.IsEmpty());
	}

	// Open in browser: the docs' own file, with its anchor; nothing outside the folder.
	TestTrue(TEXT("A page opens in the user's browser"), OpenInBrowser(TEXT("setup.html#trouble")));
	TestTrue(TEXT("as its file in the Docs folder"), Launches.Urls.Num() == 3 && Launches.Urls[2] == MakeUrl(Root, TEXT("setup.html#trouble")));
	TestFalse(TEXT("Nothing outside does"), OpenInBrowser(TEXT("../HyperAIStudio.uplugin")));
	TestEqual(TEXT("not even asked"), Launches.Urls.Num(), 3);

	// The live browser, where this editor has one.
	if (!IsViewerAvailable())
	{
		AddWarning(TEXT("This editor has no web browser (CEF); the live page was not checked. The fallback above is what it shows."));
		return true;
	}
	const TSharedRef<SHyperAIStudioDocsViewer> Live = SNew(SHyperAIStudioDocsViewer).InitialPage(TEXT("chat.html"));
	if (!Live->IsBrowserShown())
	{
		AddWarning(TEXT("The web browser module is there but no browser could be made; the live page was not checked."));
		return true;
	}
	TestEqual(TEXT("The live viewer starts on its page"), Live->GetCurrentPage(), FString(TEXT("chat.html")));
	if (!Pump(30.0, [&Live]() { return Live->IsLoaded(); }))
	{
		AddWarning(TEXT("The live page did not finish loading off screen in 30 s; the Docs.Open test checks it in a window."));
	}
	TestEqual(TEXT("and stays there"), Live->GetCurrentPage(), FString(TEXT("chat.html")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioDocsOpenTest,
	"HyperAIStudio.Docs.Open",
	HyperAIStudio::DocsTests::Flags)

bool FHyperAIStudioDocsOpenTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Docs;
	using namespace HyperAIStudio::DocsTests;
	FLaunches Launches;
	ON_SCOPE_EXIT
	{
		if (const TSharedPtr<SDockTab> Studio = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(FHyperAIStudioShell::GetTabName())))
		{
			Studio->RequestCloseTab();
		}
	};
	const FHyperAIStudioPage* Page = FHyperAIStudioShell::FindPage(GetPageId());
	TestTrue(TEXT("Docs is a page of the Studio, under System"), Page && Page->Create && !Page->bHidden && Page->Group == TEXT("System"));

	// The Documentation action, as search runs it.
	TFunction<void()> Run;
	for (const FHyperAIStudioAction& Action : FHyperAIStudioShell::GetActions())
	{
		Run = Action.Id == TEXT("OpenDocs") ? Action.Run : Run;
	}
	if (!TestTrue(TEXT("Search has the Documentation action"), static_cast<bool>(Run)))
	{
		return false;
	}
	Run();
	const TSharedPtr<SDockTab> Tab = FHyperAIStudioShell::FindPageTab(GetPageId());
	TestTrue(TEXT("Documentation opens the Studio on its Docs page"), Tab.IsValid() && FHyperAIStudioShell::IsOpen());
	const TSharedPtr<SHyperAIStudioDocsViewer> Viewer = Tab.IsValid() ? FindViewer(Tab->GetContent()) : nullptr;
	if (!TestTrue(TEXT("The page is the viewer"), Viewer.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("on the overview"), Viewer->GetCurrentPage(), FString(TEXT("index.html")));
	if (Viewer->IsBrowserShown())
	{
		TestEqual(TEXT("With a browser, nothing opens outside the editor"), Launches.Urls.Num(), 0);
	}
	else
	{
		TestTrue(TEXT("Without one, the user's browser opens the overview"), Launches.Urls.Num() == 1 && Launches.Urls[0].EndsWith(TEXT("/Docs/index.html")));
	}

	// A page and anchor, such as setup's troubleshooting, in the same page.
	Open(TEXT("setup.html#trouble"));
	TestTrue(TEXT("Opening again uses the same page"), FHyperAIStudioShell::FindPageTab(GetPageId()) == Tab);
	TestEqual(TEXT("which goes to the page and anchor"), Viewer->GetCurrentPage(), FString(TEXT("setup.html#trouble")));
	if (!Viewer->IsBrowserShown())
	{
		AddWarning(TEXT("This editor has no web browser (CEF); the live page was not checked."));
		return true;
	}
	TestTrue(TEXT("The page loads in the editor's browser"), Pump(30.0, [&Viewer]() { return Viewer->IsLoaded() && Viewer->GetCurrentPage() == TEXT("setup.html#trouble"); }, /*bSlate=*/true));
	TestEqual(TEXT("The browser shows the page and anchor asked for"), Viewer->GetCurrentPage(), FString(TEXT("setup.html#trouble")));
	Open(TEXT("index.html"));
	TestTrue(TEXT("and goes back to the overview"), Pump(30.0, [&Viewer]() { return Viewer->IsLoaded() && Viewer->GetCurrentPage() == TEXT("index.html"); }, /*bSlate=*/true));
	// A moment for the page's pictures to arrive and be drawn.
	Pump(2.0, []() { return false; }, /*bSlate=*/true);
	// The overview loads two stylesheets and a picture from the Docs folder; the browser asks the viewer about each.
	AddInfo(FString::Printf(TEXT("Files the browser asked about: %d"), Viewer->GetLoadsChecked()));
	TestTrue(TEXT("The browser asks the viewer about the files pages load"), Viewer->GetLoadsChecked() >= 3);
	const FString Shot = SaveWindowShot(Tab->GetContent(), TEXT("docs-viewer"));
	if (!Shot.IsEmpty())
	{
		AddInfo(TEXT("Picture docs-viewer: ") + Shot);
	}
	TestEqual(TEXT("Nothing opened outside the editor"), Launches.Urls.Num(), 0);
	return true;
}

#endif
