// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class SDockTab;
class SWidget;

/**
 * HyperAI Studio's own documentation: the HTML pages in the plugin's Docs folder, read on the Studio's Docs page.
 * - The policy: what the viewer may show, what opens in the user's browser, and what it refuses. CheckUrl is a pure
 *   function of the URL and the folder, so tests check it against a table; Evaluate adds the file system's answer.
 * - The pages: Docs/*.html by their titles.
 * - Open: the Docs page at a page and anchor, or the user's browser when this editor has no web browser.
 */
namespace HyperAIStudio::Docs
{
	/** What the viewer does with a URL. */
	enum class EVerdict : uint8
	{
		/** A file in the Docs folder: the viewer shows it, or loads it for a page. */
		Inside,
		/** An http or https address: never shown in the viewer; a link the user follows opens in their browser. */
		External,
		/** Anything else: another file, another drive, a share, ../, or a scheme such as javascript:, data: or blob:. */
		Blocked
	};

	struct FUrlCheck
	{
		EVerdict Verdict = EVerdict::Blocked;
		/** Inside: the file, absolute with forward slashes. */
		FString File;
		/** Inside: where in the Docs folder, such as setup.html or assets/chat.png. */
		FString Relative;
		/** Inside: the anchor after #, without the #. */
		FString Anchor;
		/** Blocked: why, for the log. */
		FString Reason;
	};

	/** The HyperAIStudio plugin's Docs folder, absolute with forward slashes and no trailing slash; empty without the plugin. */
	FString GetRoot();

	/**
	 * Checks Url against Root (as GetRoot gives it) by its text alone. A file: URL is decoded and must be a path on a
	 * drive under Root: no host but localhost, no empty, . or .. segments, no escaped /, \, ., : or control characters,
	 * no streams or device names. Its query is ignored and its anchor kept. bPage asks whether the viewer may go there:
	 * only .html and .htm files are pages; the folder's other files only load into one.
	 */
	FUrlCheck CheckUrl(const FString& Url, const FString& Root, bool bPage);

	/** Whether File is still under Root once every link and junction on the way to each is followed. */
	bool IsRealPathInside(const FString& File, const FString& Root);

	/** What the viewer checks: CheckUrl against GetRoot, then, for a file inside, where it really is. */
	FUrlCheck Evaluate(const FString& Url, bool bPage);

	/** The file: URL of Page under Root, such as setup.html#trouble; empty when that is not a page there. */
	FString MakeUrl(const FString& Root, const FString& Page);

	struct FPage
	{
		/** The file, such as setup.html. */
		FString File;
		/** Its <title>, without the " — HyperAI Studio" the pages end theirs with. */
		FString Title;
	};

	/** Root/*.html: index.html first, then in the order index.html links them, then by name. */
	TArray<FPage> GetPages(const FString& Root);

	/** Whether this editor can show web pages: Unreal's WebBrowser module loaded Chromium (CEF) and nothing turned it off. */
	bool IsViewerAvailable();

	/** The Studio page the viewer lives on. */
	FName GetPageId();

	/** The Docs page's content, for the Studio's page registration. */
	TSharedRef<SWidget> CreatePage(const TSharedRef<SDockTab>& Tab);

	/**
	 * Shows Page, such as setup.html#trouble, on the Studio's Docs page; empty keeps the page it was on. Without a web
	 * browser the Docs page says why and the user's browser opens the page instead. Returns whether the viewer shows it.
	 */
	bool Open(const FString& Page = FString());

	/** Opens Page of the Docs folder in the user's browser, for printing or when the editor has no browser. */
	bool OpenInBrowser(const FString& Page);
}
