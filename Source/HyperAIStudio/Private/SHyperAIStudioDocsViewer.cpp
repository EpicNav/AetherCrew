// Games by Hyper 2026.

#include "SHyperAIStudioDocsViewer.h"

#include "Containers/Ticker.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioWidgets.h"
#include "IWebBrowserDialog.h"
#include "IWebBrowserWindow.h"
#include "Rendering/DrawElements.h"
#include "SWebBrowser.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioDocs"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDocs, Log, All);

namespace HyperAIStudio::DocsViewer
{
	/** The docs' own background, so a page never flashes white as it loads. */
	const FColor Background(17, 20, 20);
	constexpr double StatusSeconds = 6.0;
	/** A page opens one website a second at most. */
	constexpr double ExternalGapSeconds = 1.0;
	const TCHAR* Home = TEXT("index.html");

	/**
	 * Draws the browser only in a window on screen. Unreal's browser reads its window's DPI and handle as it ticks,
	 * which Slate does while it paints, and an off-screen render's window has neither: drawn there, it would crash.
	 */
	class SOnScreenOnly : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SOnScreenOnly) {}
			SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			ChildSlot
			[
				InArgs._Content.Widget
			];
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			const SWindow* Window = OutDrawElements.GetPaintWindow();
			if (!Window || !Window->GetNativeWindow().IsValid())
			{
				return LayerId;
			}
			return SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
		}
	};
}

void SHyperAIStudioDocsViewer::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio;
	Root = Docs::GetRoot();
	for (const Docs::FPage& Page : Docs::GetPages(Root))
	{
		Pages.Add(MakeShared<Docs::FPage>(Page));
	}
	CurrentUrl = Docs::MakeUrl(Root, InArgs._InitialPage);
	if (CurrentUrl.IsEmpty())
	{
		CurrentUrl = Docs::MakeUrl(Root, DocsViewer::Home);
	}

	if (InArgs._UseBrowser && !CurrentUrl.IsEmpty() && Docs::IsViewerAvailable())
	{
		// The page loaded first is the one checked above; every later one passes HandleBeforeNavigation. Nothing is bound
		// to JavaScript, and nothing downloads: 5.8's CEF client returns no download handler, and CEF then allows none.
		bConstructing = true;
		SAssignNew(Browser, SWebBrowser)
			.InitialURL(CurrentUrl)
			.ShowControls(false)
			.ShowAddressBar(false)
			.ShowErrorMessage(true)
			.BackgroundColor(DocsViewer::Background)
			.OnBeforeNavigation(this, &SHyperAIStudioDocsViewer::HandleBeforeNavigation)
			.OnBeforePopup(this, &SHyperAIStudioDocsViewer::HandleBeforePopup)
			.OnLoadUrl(this, &SHyperAIStudioDocsViewer::HandleLoadUrl)
			.OnShowDialog(this, &SHyperAIStudioDocsViewer::HandleShowDialog)
			.OnSuppressContextMenu(this, &SHyperAIStudioDocsViewer::HandleSuppressContextMenu)
			.OnUrlChanged(this, &SHyperAIStudioDocsViewer::HandleUrlChanged)
			.OnLoadCompleted(this, &SHyperAIStudioDocsViewer::HandleLoadCompleted)
			.OnLoadError(this, &SHyperAIStudioDocsViewer::HandleLoadError);
		bConstructing = false;
		if (bBrowserFailed)
		{
			UE_LOG(LogHyperAIStudioDocs, Display, TEXT("The Docs page has no web browser: Unreal's browser could not be made in this editor."));
			Browser.Reset();
		}
	}

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			BuildToolbar()
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox).HeightOverride(1.0f)
			[
				SNew(SImage).Image(HyperAIStudio::UI::Style().GetBrush("HyperAIStudio.Divider"))
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			Browser.IsValid() ? StaticCastSharedRef<SWidget>(SNew(DocsViewer::SOnScreenOnly)[Browser.ToSharedRef()]) : BuildFallback()
		]
	];
	SyncPicker();
}

TSharedRef<SWidget> SHyperAIStudioDocsViewer::BuildToolbar()
{
	using namespace HyperAIStudio::UI;
	using FPagePtr = TSharedPtr<HyperAIStudio::Docs::FPage>;
	auto NavButton = [](const FName Brush, const FText& Tip, TFunction<void()> Run, const TAttribute<bool>& Enabled)
	{
		const TSharedRef<SWidget> Widget = IconButton(Icon(Brush), Tip, FOnClicked::CreateLambda([Run]()
		{
			Run();
			return FReply::Handled();
		}));
		Widget->SetEnabled(Enabled);
		return Widget;
	};
	return SNew(SBorder)
		.BorderImage(Style().GetBrush("HyperAIStudio.Bar"))
		.Padding(FMargin(6.0f, 4.0f, 8.0f, 4.0f))
		[
			SNew(SHorizontalBox)
			// Without a browser, there is nothing here to go back or forward in.
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				.Visibility(Browser.IsValid() ? EVisibility::Visible : EVisibility::Collapsed)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					NavButton(TEXT("Icons.ArrowLeft"), LOCTEXT("BackTip", "Back to the page before."),
						[this]() { if (Browser.IsValid()) { Browser->GoBack(); } }, TAttribute<bool>::CreateSP(this, &SHyperAIStudioDocsViewer::CanGoBack))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					NavButton(TEXT("Icons.ArrowRight"), LOCTEXT("ForwardTip", "Forward again."),
						[this]() { if (Browser.IsValid()) { Browser->GoForward(); } }, TAttribute<bool>::CreateSP(this, &SHyperAIStudioDocsViewer::CanGoForward))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					NavButton(TEXT("HyperAIStudio.Icon.Home"), LOCTEXT("HomeTip", "The docs' first page: everything HyperAI adds."),
						[this]() { Navigate(HyperAIStudio::DocsViewer::Home); }, true)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					NavButton(TEXT("Icons.Refresh"), LOCTEXT("ReloadTip", "Load the page again from the plugin's Docs folder."),
						[this]() { if (Browser.IsValid()) { Browser->Reload(); } }, true)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
				[
					SAssignNew(Picker, SComboBox<FPagePtr>)
					.OptionsSource(&Pages)
					.ToolTipText(LOCTEXT("PickerTip", "Go to a page of the docs."))
					.OnGenerateWidget_Lambda([](FPagePtr Page) -> TSharedRef<SWidget>
					{
						return SNew(STextBlock).Text(FText::FromString(Page.IsValid() ? Page->Title : FString()));
					})
					.OnSelectionChanged_Lambda([this](FPagePtr Page, const ESelectInfo::Type Info)
					{
						// Direct is the picker following the page the browser shows.
						if (Page.IsValid() && Info != ESelectInfo::Direct)
						{
							Navigate(Page->File);
						}
					})
					[
						SNew(SBox)
						.MinDesiredWidth(220.0f)
						[
							SNew(STextBlock).Text(this, &SHyperAIStudioDocsViewer::GetPageTitle)
						]
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(12.0f, 0.0f))
			[
				SNew(STextBlock)
				.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
				.Text(this, &SHyperAIStudioDocsViewer::GetStatusText)
				.ToolTipText(LOCTEXT("PolicyTip", "This page shows only HyperAI's own documentation, from the plugin's Docs folder. Links to websites open in your system browser; anything else is not opened."))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				Button(LOCTEXT("OpenInBrowser", "Open in browser"), FOnClicked::CreateSP(this, &SHyperAIStudioDocsViewer::OnOpenInBrowser),
					Icon("Icons.OpenInBrowser"), false,
					LOCTEXT("OpenInBrowserTip", "Open this page in your system browser, to print it or read it beside the editor."))
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioDocsViewer::BuildFallback()
{
	using namespace HyperAIStudio::UI;
	if (CurrentUrl.IsEmpty())
	{
		return EmptyState(Icon("Icons.Warning"), LOCTEXT("MissingTitle", "The docs are not in this copy of HyperAI"),
			LOCTEXT("MissingBody", "The plugin's Docs folder is missing. The same guides are on the website."),
			LOCTEXT("OpenWebsite", "Open the website"), FOnClicked::CreateLambda([]()
			{
				FPlatformProcess::LaunchURL(*FHyperAIStudioService::GetDocumentationUrl(), nullptr, nullptr);
				return FReply::Handled();
			}));
	}
	return EmptyState(Icon("Icons.Info"), LOCTEXT("NoBrowserTitle", "The docs open in your browser"),
		LOCTEXT("NoBrowserBody", "This editor has no web browser to show them here: Unreal's WebBrowser module could not load Chromium (CEF), or the browser is turned off. Open in browser shows the same pages, from the plugin's Docs folder."),
		LOCTEXT("OpenInBrowser", "Open in browser"), FOnClicked::CreateSP(this, &SHyperAIStudioDocsViewer::OnOpenInBrowser));
}

bool SHyperAIStudioDocsViewer::Navigate(const FString& Page)
{
	const FString Url = HyperAIStudio::Docs::MakeUrl(Root, Page.IsEmpty() ? FString(HyperAIStudio::DocsViewer::Home) : Page);
	if (Url.IsEmpty())
	{
		return false;
	}
	CurrentUrl = Url;
	if (Browser.IsValid())
	{
		Browser->LoadURL(Url);
	}
	SyncPicker();
	return true;
}

FString SHyperAIStudioDocsViewer::GetCurrentPage() const
{
	const HyperAIStudio::Docs::FUrlCheck Check = HyperAIStudio::Docs::CheckUrl(CurrentUrl, Root, /*bPage=*/true);
	if (Check.Verdict != HyperAIStudio::Docs::EVerdict::Inside)
	{
		return HyperAIStudio::DocsViewer::Home;
	}
	return Check.Anchor.IsEmpty() ? Check.Relative : Check.Relative + TEXT("#") + Check.Anchor;
}

bool SHyperAIStudioDocsViewer::IsLoaded() const
{
	return Browser.IsValid() && Browser->IsLoaded();
}

bool SHyperAIStudioDocsViewer::HandleBeforeNavigation(const FString& Url, const FWebNavigationRequest& Request)
{
	using namespace HyperAIStudio::Docs;
	const FUrlCheck Check = Evaluate(Url, /*bPage=*/true);
	if (Check.Verdict == EVerdict::Inside)
	{
		return false;
	}
	// Only a link followed in the page itself leaves for the user's browser: never a redirect or a frame's navigation.
	const bool bFollowed = Request.bIsMainFrame && !Request.bIsRedirect && Request.TransitionSource == EWebTransitionSource::Link;
	if (Check.Verdict == EVerdict::External && bFollowed)
	{
		OpenExternal(Url);
		return true;
	}
	UE_LOG(LogHyperAIStudioDocs, Log, TEXT("Docs page: not going to %s (%s)."), *Url.Left(300),
		Check.Verdict == EVerdict::External ? TEXT("a website, not from a link") : *Check.Reason);
	if (bFollowed)
	{
		SetStatus(LOCTEXT("NotOpened", "That link leads outside HyperAI's docs, so it was not opened."));
	}
	return true;
}

bool SHyperAIStudioDocsViewer::HandleBeforePopup(FString Url, FString Target)
{
	using namespace HyperAIStudio::Docs;
	const FUrlCheck Check = Evaluate(Url, /*bPage=*/true);
	if (Check.Verdict == EVerdict::Inside)
	{
		// Here, a tick later: not from inside the browser's own callback.
		const FString Page = Check.Anchor.IsEmpty() ? Check.Relative : Check.Relative + TEXT("#") + Check.Anchor;
		const TWeakPtr<SHyperAIStudioDocsViewer> Weak = SharedThis(this);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak, Page](float)
		{
			if (const TSharedPtr<SHyperAIStudioDocsViewer> Viewer = Weak.Pin())
			{
				Viewer->Navigate(Page);
			}
			return false;
		}));
	}
	else if (Check.Verdict == EVerdict::External)
	{
		OpenExternal(Url);
	}
	else
	{
		UE_LOG(LogHyperAIStudioDocs, Log, TEXT("Docs page: not opening a window on %s (%s)."), *Url.Left(300), *Check.Reason);
	}
	// No page gets a window of its own.
	return true;
}

bool SHyperAIStudioDocsViewer::HandleLoadUrl(const FString& Method, const FString& Url, FString& OutResponse)
{
	// Pages load the Docs folder's files, such as styles/ and assets/; anything else is answered with nothing.
	++LoadsChecked;
	if (HyperAIStudio::Docs::Evaluate(Url, /*bPage=*/false).Verdict == HyperAIStudio::Docs::EVerdict::Inside)
	{
		return false;
	}
	UE_LOG(LogHyperAIStudioDocs, Verbose, TEXT("Docs page: not loading %s."), *Url.Left(300));
	OutResponse.Reset();
	return true;
}

EWebBrowserDialogEventResponse SHyperAIStudioDocsViewer::HandleShowDialog(const TWeakPtr<IWebBrowserDialog>& Dialog)
{
	// The docs ask nothing of the user: alerts, prompts and leave-page questions are dismissed unseen.
	return EWebBrowserDialogEventResponse::Ignore;
}

bool SHyperAIStudioDocsViewer::HandleSuppressContextMenu()
{
	// Its View source, Print and Save as lead out of the docs; Ctrl+C still copies.
	return true;
}

void SHyperAIStudioDocsViewer::HandleUrlChanged(const FText& Url)
{
	CurrentUrl = Url.ToString();
	SyncPicker();
}

void SHyperAIStudioDocsViewer::HandleLoadCompleted()
{
	// Printing is for the user's browser (Open in browser): here the pages' Print guide button is hidden and does nothing.
	if (Browser.IsValid())
	{
		Browser->ExecuteJavascript(TEXT("window.print=function(){};document.querySelectorAll('.print-button').forEach(function(b){b.hidden=true;});"));
	}
	SyncPicker();
}

void SHyperAIStudioDocsViewer::HandleLoadError()
{
	if (bConstructing)
	{
		bBrowserFailed = true;
	}
}

void SHyperAIStudioDocsViewer::OpenExternal(const FString& Url)
{
	const double Now = FPlatformTime::Seconds();
	if (Now - LastExternalAt < HyperAIStudio::DocsViewer::ExternalGapSeconds)
	{
		UE_LOG(LogHyperAIStudioDocs, Log, TEXT("Docs page: not opening %s, a second website in a second."), *Url.Left(300));
		return;
	}
	LastExternalAt = Now;
	FString Error;
	FPlatformProcess::LaunchURL(*Url, nullptr, &Error);
	const FText Site = FText::FromString(FGenericPlatformHttp::GetUrlDomain(Url));
	SetStatus(Error.IsEmpty() ? FText::Format(LOCTEXT("OpenedSite", "Opened {0} in your browser."), Site)
		: FText::Format(LOCTEXT("OpenSiteFailed", "Could not open {0}: {1}"), Site, FText::FromString(Error)));
}

FReply SHyperAIStudioDocsViewer::OnOpenInBrowser()
{
	SetStatus(HyperAIStudio::Docs::OpenInBrowser(GetCurrentPage()) ? LOCTEXT("OpenedPage", "Opened this page in your browser.")
		: LOCTEXT("OpenPageFailed", "Could not open this page in your browser."));
	return FReply::Handled();
}

bool SHyperAIStudioDocsViewer::CanGoBack() const
{
	return Browser.IsValid() && Browser->CanGoBack();
}

bool SHyperAIStudioDocsViewer::CanGoForward() const
{
	return Browser.IsValid() && Browser->CanGoForward();
}

FText SHyperAIStudioDocsViewer::GetPageTitle() const
{
	const FString Current = GetCurrentPage();
	for (const TSharedPtr<HyperAIStudio::Docs::FPage>& Page : Pages)
	{
		if (Current.Equals(Page->File, ESearchCase::IgnoreCase) || Current.StartsWith(Page->File + TEXT("#"), ESearchCase::IgnoreCase))
		{
			return FText::FromString(Page->Title);
		}
	}
	return LOCTEXT("PickPage", "Pages");
}

FText SHyperAIStudioDocsViewer::GetStatusText() const
{
	return FPlatformTime::Seconds() < StatusUntil ? Status : LOCTEXT("WebLinksHint", "Web links open in your browser");
}

void SHyperAIStudioDocsViewer::SetStatus(const FText& Text)
{
	Status = Text;
	StatusUntil = FPlatformTime::Seconds() + HyperAIStudio::DocsViewer::StatusSeconds;
}

void SHyperAIStudioDocsViewer::SyncPicker()
{
	if (!Picker.IsValid())
	{
		return;
	}
	FString Current = GetCurrentPage();
	int32 Hash = INDEX_NONE;
	if (Current.FindChar(TEXT('#'), Hash))
	{
		Current.LeftInline(Hash);
	}
	const TSharedPtr<HyperAIStudio::Docs::FPage>* Found = Pages.FindByPredicate([&Current](const TSharedPtr<HyperAIStudio::Docs::FPage>& Page)
	{
		return Page->File.Equals(Current, ESearchCase::IgnoreCase);
	});
	if (Found && Picker->GetSelectedItem() != *Found)
	{
		Picker->SetSelectedItem(*Found);
	}
}

#undef LOCTEXT_NAMESPACE
