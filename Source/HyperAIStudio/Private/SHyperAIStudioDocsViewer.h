// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioDocs.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

class IWebBrowserDialog;
class SBox;
class SWebBrowser;
enum class EWebBrowserDialogEventResponse;
struct FWebNavigationRequest;

/**
 * The Studio's Docs page: HyperAI's own documentation, from the plugin's Docs folder, in the editor's web browser.
 * - A toolbar: Back, Forward, Home, Reload, the pages by title, and Open in browser for the page shown.
 * - It shows the Docs folder and nothing else: every navigation, window and file a page asks for passes
 *   HyperAIStudio::Docs's policy. A link to a website opens in the user's browser; anything else is refused. No
 *   JavaScript reaches the editor, the context menu is off, and nothing downloads.
 * - Without a web browser (an editor without CEF) it says so, and Open in browser shows the same pages.
 */
class SHyperAIStudioDocsViewer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioDocsViewer)
		: _UseBrowser(true)
	{}
		/** The page to start on, such as setup.html#trouble; index.html when empty. */
		SLATE_ARGUMENT(FString, InitialPage)
		/** False shows what an editor without a web browser shows. */
		SLATE_ARGUMENT(bool, UseBrowser)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Goes to Page of the Docs folder, such as chat.html#approvals; empty is index.html. False when it is not a page there. */
	bool Navigate(const FString& Page);
	/** The page shown, with its anchor, such as setup.html#trouble. */
	FString GetCurrentPage() const;
	/** Whether the pages show here; false when the editor has no web browser. */
	bool IsBrowserShown() const { return Browser.IsValid(); }
	/** Whether the page shown has finished loading. */
	bool IsLoaded() const;
	/** The pages the picker lists. */
	const TArray<TSharedPtr<HyperAIStudio::Docs::FPage>>& GetPages() const { return Pages; }
	/** How many files the browser asked HandleLoadUrl about, so tests see it asks for each file a page loads. */
	int32 GetLoadsChecked() const { return LoadsChecked; }

	/**
	 * The browser's questions, public so tests can ask them as the browser does. Each returns true to refuse: the
	 * viewer goes nowhere, and a website a link leads to opens in the user's browser instead.
	 */
	bool HandleBeforeNavigation(const FString& Url, const FWebNavigationRequest& Request);
	/** A window a page wants: a page of the docs opens here instead, a website in the user's browser. */
	bool HandleBeforePopup(FString Url, FString Target);
	/** True serves OutResponse, empty, in place of a file from outside the Docs folder. */
	bool HandleLoadUrl(const FString& Method, const FString& Url, FString& OutResponse);

private:
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildFallback();
	EWebBrowserDialogEventResponse HandleShowDialog(const TWeakPtr<IWebBrowserDialog>& Dialog);
	bool HandleSuppressContextMenu();
	void HandleUrlChanged(const FText& Url);
	void HandleLoadCompleted();
	void HandleLoadError();
	void OpenExternal(const FString& Url);
	FReply OnOpenInBrowser();
	bool CanGoBack() const;
	bool CanGoForward() const;
	FText GetPageTitle() const;
	FText GetStatusText() const;
	void SetStatus(const FText& Text);
	void SyncPicker();

	/** The Docs folder, as HyperAIStudio::Docs::GetRoot gives it. */
	FString Root;
	TArray<TSharedPtr<HyperAIStudio::Docs::FPage>> Pages;
	TSharedPtr<SComboBox<TSharedPtr<HyperAIStudio::Docs::FPage>>> Picker;
	TSharedPtr<SWebBrowser> Browser;
	/** The page shown, or asked for while it loads; without a browser, the page Open in browser opens. */
	FString CurrentUrl;
	/** A line in the toolbar on what just happened, such as a website opening, shown for a few seconds. */
	FText Status;
	double StatusUntil = 0.0;
	/** When a website last opened, so a page cannot open them one after another. */
	double LastExternalAt = -1000.0;
	int32 LoadsChecked = 0;
	/** The browser reports it could not be made by failing its first load while it is constructed. */
	bool bConstructing = false;
	bool bBrowserFailed = false;
};
