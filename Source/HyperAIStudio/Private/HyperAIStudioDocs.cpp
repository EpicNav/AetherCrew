// Games by Hyper 2026.

#include "HyperAIStudioDocs.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioShell.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SHyperAIStudioDocsViewer.h"
#include "WebBrowserModule.h"
#include "Widgets/Docking/SDockTab.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDocs, Log, All);

namespace HyperAIStudio::Docs
{
	namespace
	{
		/** Longer than any URL of the docs; a longer one is refused unread. */
		constexpr int32 MaxUrl = 4096;

		/** The Docs page the Studio holds, so Open can take it to a page. */
		TWeakPtr<SHyperAIStudioDocsViewer>& PageViewer()
		{
			static TWeakPtr<SHyperAIStudioDocsViewer> Viewer;
			return Viewer;
		}

		/** The page Open asked for while the Docs page was being made. */
		FString& PendingPage()
		{
			static FString Page;
			return Page;
		}

		FString NormalizeRoot(const FString& Root)
		{
			FString Normalized = Root.Replace(TEXT("\\"), TEXT("/"));
			while (Normalized.EndsWith(TEXT("/")))
			{
				Normalized.LeftChopInline(1);
			}
			return Normalized;
		}

		int32 HexValue(const ANSICHAR Char)
		{
			return Char >= '0' && Char <= '9' ? Char - '0'
				: Char >= 'a' && Char <= 'f' ? Char - 'a' + 10
				: Char >= 'A' && Char <= 'F' ? Char - 'A' + 10
				: -1;
		}

		/**
		 * Decodes a URL path's %XX escapes. False when one is malformed or hides what would change where the path goes,
		 * or how Windows reads it: a separator, a dot, a colon or a control character.
		 */
		bool DecodePath(const FString& Encoded, FString& OutDecoded)
		{
			const FTCHARToUTF8 Utf8(*Encoded);
			const ANSICHAR* Data = Utf8.Get();
			const int32 Length = Utf8.Length();
			TArray<ANSICHAR> Bytes;
			Bytes.Reserve(Length + 1);
			for (int32 Index = 0; Index < Length; ++Index)
			{
				if (Data[Index] != '%')
				{
					Bytes.Add(Data[Index]);
					continue;
				}
				const int32 High = Index + 2 < Length ? HexValue(Data[Index + 1]) : -1;
				const int32 Low = Index + 2 < Length ? HexValue(Data[Index + 2]) : -1;
				if (High < 0 || Low < 0)
				{
					return false;
				}
				const int32 Value = High * 16 + Low;
				if (Value < 0x20 || Value == 0x7F || Value == '/' || Value == '\\' || Value == '.' || Value == ':')
				{
					return false;
				}
				Bytes.Add(static_cast<ANSICHAR>(Value));
				Index += 2;
			}
			Bytes.Add('\0');
			OutDecoded = UTF8_TO_TCHAR(Bytes.GetData());
			return true;
		}

		/** Names Windows gives to devices in every folder, with or without an extension. */
		bool IsDeviceName(const FString& Segment)
		{
			FString Base = Segment;
			int32 Dot = INDEX_NONE;
			if (Base.FindChar(TEXT('.'), Dot))
			{
				Base.LeftInline(Dot);
			}
			Base.TrimEndInline();
			for (const TCHAR* Device : {TEXT("CON"), TEXT("PRN"), TEXT("AUX"), TEXT("NUL"), TEXT("CONIN$"), TEXT("CONOUT$")})
			{
				if (Base.Equals(Device, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return Base.Len() == 4 && (Base.StartsWith(TEXT("COM"), ESearchCase::IgnoreCase) || Base.StartsWith(TEXT("LPT"), ESearchCase::IgnoreCase))
				&& FChar::IsDigit(Base[3]);
		}

		/** Percent-encodes Text, keeping the characters a URL can hold as they are, and / and : when bPath. */
		FString Encode(const FString& Text, const bool bPath)
		{
			const FTCHARToUTF8 Utf8(*Text);
			FString Encoded;
			for (int32 Index = 0; Index < Utf8.Length(); ++Index)
			{
				const ANSICHAR Char = Utf8.Get()[Index];
				const bool bKeep = (Char >= 'a' && Char <= 'z') || (Char >= 'A' && Char <= 'Z') || (Char >= '0' && Char <= '9')
					|| Char == '-' || Char == '_' || Char == '.' || Char == '~' || (bPath && (Char == '/' || Char == ':'));
				if (bKeep)
				{
					Encoded.AppendChar(static_cast<TCHAR>(Char));
				}
				else
				{
					Encoded.Appendf(TEXT("%%%02X"), static_cast<uint8>(Char));
				}
			}
			return Encoded;
		}

		/** Where Path really is, every link and junction followed, with forward slashes; empty when it cannot be opened. */
		FString RealPath(const FString& Path)
		{
#if PLATFORM_WINDOWS
			// No access asked for: enough to ask where a file or folder is.
			const HANDLE Handle = ::CreateFileW(*FPaths::ConvertRelativePathToFull(Path).Replace(TEXT("/"), TEXT("\\")), 0,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if (Handle == INVALID_HANDLE_VALUE)
			{
				return FString();
			}
			FString Real;
			const DWORD Needed = ::GetFinalPathNameByHandleW(Handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (Needed > 0)
			{
				TArray<WCHAR> Buffer;
				Buffer.SetNumZeroed(Needed + 1);
				const DWORD Written = ::GetFinalPathNameByHandleW(Handle, Buffer.GetData(), Needed + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
				if (Written > 0 && Written <= Needed)
				{
					Real = FString(static_cast<int32>(Written), Buffer.GetData());
				}
			}
			::CloseHandle(Handle);
			// \\?\E:\... on a drive, \\?\UNC\server\share\... on a share.
			if (Real.StartsWith(TEXT("\\\\?\\UNC\\")))
			{
				Real = TEXT("\\\\") + Real.Mid(8);
			}
			else if (Real.StartsWith(TEXT("\\\\?\\")))
			{
				Real.RightChopInline(4);
			}
			return NormalizeRoot(Real);
#else
			return FPaths::FileExists(Path) || FPaths::DirectoryExists(Path) ? NormalizeRoot(FPaths::ConvertRelativePathToFull(Path)) : FString();
#endif
		}

		/** The text of the page's <title>, with the entities the docs use read as characters. */
		FString ReadTitle(const FString& Html)
		{
			const int32 Tag = Html.Find(TEXT("<title"), ESearchCase::IgnoreCase);
			const int32 Start = Tag == INDEX_NONE ? INDEX_NONE : Html.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Tag);
			const int32 End = Start == INDEX_NONE ? INDEX_NONE : Html.Find(TEXT("</title>"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
			if (End == INDEX_NONE)
			{
				return FString();
			}
			FString Title = Html.Mid(Start + 1, End - Start - 1);
			const TPair<const TCHAR*, const TCHAR*> Entities[] = {{TEXT("&mdash;"), TEXT("\u2014")}, {TEXT("&ndash;"), TEXT("\u2013")},
				{TEXT("&rsquo;"), TEXT("\u2019")}, {TEXT("&lsquo;"), TEXT("\u2018")}, {TEXT("&nbsp;"), TEXT(" ")}, {TEXT("&quot;"), TEXT("\"")},
				{TEXT("&#39;"), TEXT("'")}, {TEXT("&lt;"), TEXT("<")}, {TEXT("&gt;"), TEXT(">")}, {TEXT("&amp;"), TEXT("&")}};
			for (const TPair<const TCHAR*, const TCHAR*>& Entity : Entities)
			{
				Title.ReplaceInline(Entity.Key, Entity.Value);
			}
			TArray<FString> Words;
			Title.ParseIntoArrayWS(Words);
			Title = FString::Join(Words, TEXT(" "));
			const FString Suffix(TEXT(" \u2014 HyperAI Studio"));
			if (Title.EndsWith(Suffix) && Title.Len() > Suffix.Len())
			{
				Title.LeftChopInline(Suffix.Len());
			}
			return Title;
		}
	}

	FString GetRoot()
	{
		static FString Root;
		if (Root.IsEmpty())
		{
			if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio")))
			{
				Root = NormalizeRoot(FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Docs")));
			}
		}
		return Root;
	}

	FUrlCheck CheckUrl(const FString& Url, const FString& InRoot, const bool bPage)
	{
		FUrlCheck Check;
		auto Block = [&Check](const FString& Reason)
		{
			Check.Verdict = EVerdict::Blocked;
			Check.Reason = Reason;
			Check.File.Reset();
			Check.Relative.Reset();
			Check.Anchor.Reset();
			return Check;
		};
		if (Url.IsEmpty() || Url.Len() > MaxUrl)
		{
			return Block(TEXT("empty or too long"));
		}
		for (const TCHAR Char : Url)
		{
			if (Char < 0x20 || Char == 0x7F)
			{
				return Block(TEXT("a control character"));
			}
		}
		if (FChar::IsWhitespace(Url[0]) || FChar::IsWhitespace(Url[Url.Len() - 1]))
		{
			return Block(TEXT("spaces around it"));
		}

		// The scheme: a letter, then letters, digits, +, - or ., up to the first colon.
		int32 Colon = INDEX_NONE;
		Url.FindChar(TEXT(':'), Colon);
		bool bScheme = Colon > 0 && FChar::IsAlpha(Url[0]);
		for (int32 Index = 1; bScheme && Index < Colon; ++Index)
		{
			bScheme = FChar::IsAlnum(Url[Index]) || Url[Index] == TEXT('+') || Url[Index] == TEXT('-') || Url[Index] == TEXT('.');
		}
		if (!bScheme)
		{
			return Block(TEXT("not a URL"));
		}
		const FString Scheme = Url.Left(Colon).ToLower();
		FString Rest = Url.Mid(Colon + 1);
		int32 Hash = INDEX_NONE;
		if (Rest.FindChar(TEXT('#'), Hash))
		{
			Check.Anchor = Rest.Mid(Hash + 1);
			Rest.LeftInline(Hash);
		}
		int32 Query = INDEX_NONE;
		if (Rest.FindChar(TEXT('?'), Query))
		{
			Rest.LeftInline(Query);
		}

		if (Scheme == TEXT("http") || Scheme == TEXT("https"))
		{
			FString Host = Rest.StartsWith(TEXT("//")) ? Rest.Mid(2) : FString();
			int32 Slash = INDEX_NONE;
			if (Host.FindChar(TEXT('/'), Slash))
			{
				Host.LeftInline(Slash);
			}
			if (Host.IsEmpty() || Host.Contains(TEXT("\\")) || Host.Contains(TEXT(" ")))
			{
				return Block(TEXT("a web address without a host"));
			}
			Check.Verdict = EVerdict::External;
			Check.Anchor.Reset();
			return Check;
		}
		if (Scheme != TEXT("file"))
		{
			return Block(FString::Printf(TEXT("the %s: scheme"), *Scheme));
		}

		const FString Root = NormalizeRoot(InRoot);
		if (Root.IsEmpty())
		{
			return Block(TEXT("no Docs folder"));
		}
		if (!Rest.StartsWith(TEXT("//")))
		{
			return Block(TEXT("a file: URL without //"));
		}
		Rest.RightChopInline(2);
		int32 PathStart = INDEX_NONE;
		if (!Rest.FindChar(TEXT('/'), PathStart))
		{
			return Block(TEXT("a file: URL without a path"));
		}
		const FString Host = Rest.Left(PathStart);
		if (!Host.IsEmpty() && !Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
		{
			return Block(TEXT("a file on another computer"));
		}
		FString Path;
		if (!DecodePath(Rest.Mid(PathStart), Path))
		{
			return Block(TEXT("an escaped separator, dot, colon or control character"));
		}
		// Chromium reads \ as / in a file: URL; so does this.
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Path.StartsWith(TEXT("//")))
		{
			return Block(TEXT("a network share"));
		}
		// file:///E:/Docs/index.html has the path /E:/Docs/index.html.
		if (Path.Len() < 4 || Path[0] != TEXT('/') || !FChar::IsAlpha(Path[1]) || Path[2] != TEXT(':') || Path[3] != TEXT('/'))
		{
			return Block(TEXT("not a path on a drive"));
		}
		Path.RightChopInline(1);
		TArray<FString> Segments;
		Path.Mid(3).ParseIntoArray(Segments, TEXT("/"), /*InCullEmpty=*/false);
		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT(".."))
			{
				return Block(TEXT("an empty, . or .. part"));
			}
			// Windows drops a name's trailing dots and spaces, and reads a colon as a stream.
			if (Segment.EndsWith(TEXT(".")) || Segment.EndsWith(TEXT(" ")) || IsDeviceName(Segment))
			{
				return Block(TEXT("a name Windows reads as another"));
			}
			for (const TCHAR Char : Segment)
			{
				if (FCString::Strchr(TEXT("<>:\"|?*"), Char))
				{
					return Block(TEXT("a character no file name has"));
				}
			}
		}
		const FString Prefix = Root + TEXT("/");
		if (!Path.StartsWith(Prefix, ESearchCase::IgnoreCase) || Path.Len() == Prefix.Len())
		{
			return Block(TEXT("outside the Docs folder"));
		}
		const FString Relative = Path.Mid(Prefix.Len());
		if (bPage && !Relative.EndsWith(TEXT(".html"), ESearchCase::IgnoreCase) && !Relative.EndsWith(TEXT(".htm"), ESearchCase::IgnoreCase))
		{
			return Block(TEXT("not a page"));
		}
		Check.Verdict = EVerdict::Inside;
		Check.File = Prefix + Relative;
		Check.Relative = Relative;
		return Check;
	}

	bool IsRealPathInside(const FString& File, const FString& Root)
	{
		const FString RealRoot = RealPath(Root);
		const FString RealFile = RealPath(File);
		return !RealRoot.IsEmpty() && !RealFile.IsEmpty() && RealFile.StartsWith(RealRoot + TEXT("/"), ESearchCase::IgnoreCase);
	}

	FUrlCheck Evaluate(const FString& Url, const bool bPage)
	{
		const FString Root = GetRoot();
		FUrlCheck Check = CheckUrl(Url, Root, bPage);
		if (Check.Verdict == EVerdict::Inside && !IsRealPathInside(Check.File, Root))
		{
			Check.Verdict = EVerdict::Blocked;
			Check.Reason = TEXT("missing, or a link out of the Docs folder");
		}
		return Check;
	}

	FString MakeUrl(const FString& InRoot, const FString& Page)
	{
		const FString Root = NormalizeRoot(InRoot);
		FString Path = Page;
		FString Anchor;
		int32 Hash = INDEX_NONE;
		if (Path.FindChar(TEXT('#'), Hash))
		{
			Anchor = Path.Mid(Hash + 1);
			Path.LeftInline(Hash);
		}
		if (Root.IsEmpty() || Path.IsEmpty())
		{
			return FString();
		}
		const FString Url = TEXT("file:///") + Encode(Root / Path, /*bPath=*/true) + (Anchor.IsEmpty() ? FString() : TEXT("#") + Encode(Anchor, /*bPath=*/false));
		return CheckUrl(Url, Root, /*bPage=*/true).Verdict == EVerdict::Inside ? Url : FString();
	}

	TArray<FPage> GetPages(const FString& InRoot)
	{
		const FString Root = NormalizeRoot(InRoot);
		TArray<FString> Files;
		if (!Root.IsEmpty())
		{
			IFileManager::Get().FindFiles(Files, *(Root / TEXT("*.html")), /*Files=*/true, /*Directories=*/false);
		}
		// The docs' own order: the overview, then its links.
		TArray<FString> Order = {TEXT("index.html")};
		FString Index;
		if (FFileHelper::LoadFileToString(Index, *(Root / TEXT("index.html"))))
		{
			for (int32 At = Index.Find(TEXT("href=\""), ESearchCase::IgnoreCase); At != INDEX_NONE;
				At = Index.Find(TEXT("href=\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, At + 6))
			{
				const int32 End = Index.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 6);
				FString Link = End == INDEX_NONE ? FString() : Index.Mid(At + 6, End - At - 6);
				int32 Cut = INDEX_NONE;
				if (Link.FindChar(TEXT('#'), Cut) || Link.FindChar(TEXT('?'), Cut))
				{
					Link.LeftInline(Cut);
				}
				if (!Link.IsEmpty() && !Link.Contains(TEXT("/")) && !Link.Contains(TEXT(":")))
				{
					Order.AddUnique(Link.ToLower());
				}
			}
		}
		auto Rank = [&Order](const FString& File)
		{
			const int32 Found = Order.IndexOfByKey(File.ToLower());
			return Found == INDEX_NONE ? Order.Num() : Found;
		};
		Files.Sort([&Rank](const FString& A, const FString& B)
		{
			const int32 RankA = Rank(A);
			const int32 RankB = Rank(B);
			return RankA != RankB ? RankA < RankB : A.Compare(B, ESearchCase::IgnoreCase) < 0;
		});
		TArray<FPage> Pages;
		for (const FString& File : Files)
		{
			FString Html;
			FFileHelper::LoadFileToString(Html, *(Root / File));
			FPage& Page = Pages.AddDefaulted_GetRef();
			Page.File = File;
			Page.Title = ReadTitle(Html);
			if (Page.Title.IsEmpty())
			{
				Page.Title = FPaths::GetBaseFilename(File);
			}
		}
		return Pages;
	}

	bool IsViewerAvailable()
	{
		// What SWebBrowserView and the browser singleton check before they make a browser, without making one.
		if (IsRunningCommandlet() || !FApp::CanEverRender() || FParse::Param(FCommandLine::Get(), TEXT("nocef")))
		{
			return false;
		}
		bool bEnabled = true;
		if (GConfig)
		{
			GConfig->GetBool(TEXT("Browser"), TEXT("bEnabled"), bEnabled, GEngineIni);
		}
		const IWebBrowserModule* Module = bEnabled ? FModuleManager::LoadModulePtr<IWebBrowserModule>(TEXT("WebBrowser")) : nullptr;
		return Module && Module->IsWebModuleAvailable();
	}

	FName GetPageId()
	{
		static const FName Id(TEXT("Docs"));
		return Id;
	}

	TSharedRef<SWidget> CreatePage(const TSharedRef<SDockTab>& /*Tab*/)
	{
		const TSharedRef<SHyperAIStudioDocsViewer> Viewer = SNew(SHyperAIStudioDocsViewer).InitialPage(PendingPage());
		PageViewer() = Viewer;
		return Viewer;
	}

	bool Open(const FString& Page)
	{
		// A page made now, as the Studio opens it, starts where it was asked to; one already open goes there.
		const bool bWasOpen = PageViewer().IsValid();
		PendingPage() = Page;
		const bool bShown = FHyperAIStudioShell::Open(GetPageId()).IsValid();
		PendingPage().Reset();
		const TSharedPtr<SHyperAIStudioDocsViewer> Viewer = bShown ? PageViewer().Pin() : nullptr;
		if (Viewer.IsValid() && bWasOpen && !Page.IsEmpty())
		{
			Viewer->Navigate(Page);
		}
		if (Viewer.IsValid() && Viewer->IsBrowserShown())
		{
			return true;
		}
		// No web browser in this editor, or no Studio window: the user's browser shows the page, and the Docs page says why.
		const FString Shown = Viewer.IsValid() ? Viewer->GetCurrentPage() : Page;
		UE_LOG(LogHyperAIStudioDocs, Display, TEXT("HyperAI's docs open in the system browser: this editor has no web browser for the Docs page (%s)."),
			Shown.IsEmpty() ? TEXT("index.html") : *Shown);
		OpenInBrowser(Shown);
		return false;
	}

	bool OpenInBrowser(const FString& Page)
	{
		const FString Url = MakeUrl(GetRoot(), Page.IsEmpty() ? FString(TEXT("index.html")) : Page);
		if (Url.IsEmpty() || Evaluate(Url, /*bPage=*/true).Verdict != EVerdict::Inside)
		{
			UE_LOG(LogHyperAIStudioDocs, Log, TEXT("HyperAI's docs have no page %s."), *Page);
			return false;
		}
		FString Error;
		FPlatformProcess::LaunchURL(*Url, nullptr, &Error);
		if (!Error.IsEmpty())
		{
			UE_LOG(LogHyperAIStudioDocs, Warning, TEXT("Could not open %s in the system browser: %s"), *Url, *Error);
		}
		return Error.IsEmpty();
	}
}
