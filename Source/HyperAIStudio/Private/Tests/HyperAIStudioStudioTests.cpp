// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioMarkdown.h"
#include "HyperAIStudioPreviewPanel.h"
#include "HyperAIStudioShell.h"
#include "HyperAIStudioWidgetShot.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioSettingsPage.h"
#include "SHyperAIStudioShell.h"
#include "SHyperAIStudioToolsPage.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"

namespace HyperAIStudio::StudioTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** Lets the editor run: tickers, HTTP and game-thread tasks, until Done or Seconds pass. */
	bool Pump(const double Seconds, TFunctionRef<bool()> Done)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		while (!Done() && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.01f);
		}
		return Done();
	}

	const FToolMenuEntry* FindMenuEntry(const FName Menu, const FName Entry)
	{
		const UToolMenu* Found = UToolMenus::Get() ? UToolMenus::Get()->FindMenu(Menu) : nullptr;
		if (!Found)
		{
			return nullptr;
		}
		for (const FToolMenuSection& Section : Found->Sections)
		{
			for (const FToolMenuEntry& Block : Section.Blocks)
			{
				if (Block.Name == Entry)
				{
					return &Block;
				}
			}
		}
		return nullptr;
	}

	/** A report the way agents are asked to write one: a line on what happened, then the results. */
	const TCHAR* Report = TEXT(
		"# Playtest: arena\n"
		"The bot played **4 cases** in their own copies of the level; one needs a look. Details are in `hyper_verify_run`'s report.\n"
		"\n"
		"## Results\n"
		"| Case | Result | Time |\n"
		"|---|---|---|\n"
		"| reach-around-wall | PASS reached the goal round a 7 m wall | 4.2 s |\n"
		"| defeat-targets | PASS both targets down, 11 shots | 9.8 s |\n"
		"| duel | FAIL lost 2-3 to a skill 0.9 opponent | 30.0 s |\n"
		"| explore | WARN 71% of the floor seen | 20.0 s |\n"
		"\n"
		"## Next\n"
		"1. Widen the duel's cover so the weaker player can break line of sight.\n"
		"2. Run `duel` again with *skill 0.6*.\n"
		"- The map is [Saved/HyperAIStudio/Screens](Saved/HyperAIStudio/Screens).\n"
		"  - Blue is Player0's path.\n"
		"> Nothing in the level was changed; the cases ran in copies.\n"
		"\n"
		"```json\n"
		"{ \"case\": \"duel\", \"skill\": 0.6, \"goal\": \"defeat\" }\n"
		"```\n"
		"---\n");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioStudioPagesTest,
	"HyperAIStudio.Studio.Pages",
	HyperAIStudio::StudioTests::Flags)

bool FHyperAIStudioStudioPagesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::StudioTests;
	// Every feature is a page of the one window.
	for (const TCHAR* Id : {TEXT("Home"), TEXT("Chat"), TEXT("Images"), TEXT("Match"), TEXT("Playtest"), TEXT("Memory"), TEXT("Skills"),
		TEXT("Agents"), TEXT("Tools"), TEXT("Activity"), TEXT("Settings")})
	{
		const FHyperAIStudioPage* Page = FHyperAIStudioShell::FindPage(Id);
		TestTrue(FString::Printf(TEXT("The %s page is registered, with a label, an icon and a tooltip"), Id),
			Page && !Page->Label.IsEmpty() && !Page->Icon.IsNone() && !Page->Tooltip.IsEmpty() && Page->Create && !Page->bHidden);
	}
	for (const TCHAR* Id : {TEXT("Chat2"), TEXT("Chat3"), TEXT("Chat4")})
	{
		const FHyperAIStudioPage* Page = FHyperAIStudioShell::FindPage(Id);
		TestTrue(FString::Printf(TEXT("%s opens from a chat's new-chat button, not the navigation"), Id), Page && Page->bHidden);
	}
	const FHyperAIStudioPage* Activity = FHyperAIStudioShell::FindPage(TEXT("Activity"));
	TestTrue(TEXT("Activity counts the approvals waiting"), Activity && Activity->Badge);

	// The navigation's order: Start, Create, Test, Project, System.
	const TArray<FHyperAIStudioPage> Pages = FHyperAIStudioShell::GetPages();
	TArray<FString> Order;
	for (const FHyperAIStudioPage& Page : Pages)
	{
		if (!Page.bHidden)
		{
			Order.Add(Page.Id.ToString());
		}
	}
	const FString Joined = FString::Join(Order, TEXT(","));
	AddInfo(TEXT("Navigation: ") + Joined);
	TestEqual(TEXT("The navigation lists the pages by group and order"), Joined,
		FString(TEXT("Home,Chat,Images,Match,Playtest,Memory,Skills,Agents,Tools,Activity,Settings")));

	TArray<FString> Actions;
	for (const FHyperAIStudioAction& Action : FHyperAIStudioShell::GetActions())
	{
		Actions.Add(Action.Id.ToString());
	}
	TestTrue(TEXT("Search can start a chat, open setup and the docs"),
		Actions.Contains(TEXT("NewChat")) && Actions.Contains(TEXT("StartSetup")) && Actions.Contains(TEXT("OpenDocs")));

	// One way in from the Tools menu, with its shortcut, where there were six entries.
	const FToolMenuEntry* Entry = FindMenuEntry(TEXT("LevelEditor.MainMenu.Tools"), TEXT("OpenHyperAIStudio"));
	TestTrue(TEXT("Tools > HyperAI Studio is there"), Entry && Entry->Label.Get(FText::GetEmpty()).ToString() == TEXT("HyperAI Studio"));
	for (const TCHAR* Old : {TEXT("OpenHyperAIStudioChat"), TEXT("OpenHyperAIStudioPreview"), TEXT("OpenHyperAIStudioPlaytest"),
		TEXT("OpenHyperAIStudioQuickAction"), TEXT("OpenHyperAIStudioImages")})
	{
		TestNull(FString::Printf(TEXT("The old %s entry is gone"), Old), FindMenuEntry(TEXT("LevelEditor.MainMenu.Tools"), Old));
	}
	const TSharedPtr<FUICommandInfo> Command = FInputBindingManager::Get().FindCommandInContext(TEXT("HyperAIStudio"), TEXT("OpenStudio"));
	TestTrue(TEXT("Open HyperAI Studio is a command, so its shortcut can be changed"), Command.IsValid());
	if (Command.IsValid())
	{
		const FInputChord Chord = Command->GetDefaultChord(EMultipleKeyBindingIndex::Primary);
		TestTrue(TEXT("Its shortcut is Ctrl+Alt+H"), Chord.Key == EKeys::H && Chord.bCtrl && Chord.bAlt && !Chord.bShift);
	}
	TestTrue(TEXT("The window has one tab spawner"), FGlobalTabmanager::Get()->HasTabSpawner(FHyperAIStudioShell::GetTabName()));
	for (const TCHAR* Old : {TEXT("HyperAIStudioChat"), TEXT("HyperAIStudioPreview"), TEXT("HyperAIStudioPlaytest"), TEXT("HyperAIStudioImages")})
	{
		TestFalse(FString::Printf(TEXT("The old %s tab is gone"), Old), FGlobalTabmanager::Get()->HasTabSpawner(Old));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioStudioWindowTest,
	"HyperAIStudio.Studio.Window",
	HyperAIStudio::StudioTests::Flags)

bool FHyperAIStudioStudioWindowTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::StudioTests;
	// The shell as the window holds it, with its own tab manager and dock area.
	const TSharedRef<SDockTab> Owner = SNew(SDockTab).TabRole(ETabRole::MajorTab);
	FString SavedRail;
	const bool bHadRail = GConfig->GetString(TEXT("HyperAIStudio.Studio"), TEXT("Rail"), SavedRail, GEditorPerProjectIni);
	ON_SCOPE_EXIT
	{
		if (bHadRail)
		{
			GConfig->SetString(TEXT("HyperAIStudio.Studio"), TEXT("Rail"), *SavedRail, GEditorPerProjectIni);
		}
		else
		{
			GConfig->RemoveKey(TEXT("HyperAIStudio.Studio"), TEXT("Rail"), GEditorPerProjectIni);
		}
	};
	const TSharedRef<SHyperAIStudioShell> Shell = SNew(SHyperAIStudioShell, Owner, nullptr);
	Owner->SetContent(Shell);

	TestTrue(TEXT("Home opens"), Shell->OpenPage(TEXT("Home")).IsValid() && Shell->GetCurrentPage() == TEXT("Home"));
	TestFalse(TEXT("An unknown page does not open"), Shell->OpenPage(TEXT("NoSuchPage")).IsValid());
	for (const TCHAR* Id : {TEXT("Settings"), TEXT("Playtest"), TEXT("Memory"), TEXT("Skills"), TEXT("Agents"), TEXT("Activity"), TEXT("Match"), TEXT("Images")})
	{
		const TSharedPtr<SDockTab> Tab = Shell->OpenPage(Id);
		TestTrue(FString::Printf(TEXT("%s opens as a tab in the window"), Id), Tab.IsValid() && Shell->GetCurrentPage() == Id);
		TestTrue(FString::Printf(TEXT("%s is found again rather than opened twice"), Id), Shell->FindPageTab(Id) == Tab);
	}

	// Search finds pages and actions by name and by what they are for.
	Shell->Search(TEXT("play"));
	TestTrue(TEXT("\"play\" finds Playtest"), Shell->GetSearchResultCount() >= 1);
	Shell->Search(TEXT("new chat"));
	TestTrue(TEXT("\"new chat\" finds the action"), Shell->GetSearchResultCount() >= 1);
	Shell->Search(TEXT("reference image"));
	TestTrue(TEXT("\"reference image\" finds a page by its description"), Shell->GetSearchResultCount() >= 1);
	Shell->Search(TEXT("qqzzxxvv"));
	TestEqual(TEXT("Nonsense finds nothing"), Shell->GetSearchResultCount(), 0);
	Shell->Search(FString());

	// The rail: full at a desktop width, icons only when narrow or when asked.
	Shell->SetRail(SHyperAIStudioShell::ERail::Auto);
	Shell->OpenPage(TEXT("Home"));
	// A tab opened in the background flashes for a second, as every editor tab does; let that pass before the pictures.
	Pump(1.2, []() { return false; });
	const FString Wide = HyperAIStudio::Tests::SaveWidgetShot(Shell, FIntPoint(1440, 900), TEXT("step14-studio-home"), 12);
	TestFalse(TEXT("At 1440 px the rail shows labels"), Shell->IsRailCompact());
	HyperAIStudio::Tests::SaveWidgetShot(Shell, FIntPoint(900, 760), TEXT("step14-studio-narrow"), 6);
	TestTrue(TEXT("At 900 px the rail folds to icons"), Shell->IsRailCompact());
	Shell->SetRail(SHyperAIStudioShell::ERail::Expanded);
	HyperAIStudio::Tests::SaveWidgetShot(Shell, FIntPoint(900, 760), TEXT("step14-studio-narrow-expanded"), 4);
	TestFalse(TEXT("Expanded keeps labels even when narrow"), Shell->IsRailCompact());
	Shell->SetRail(SHyperAIStudioShell::ERail::Compact);
	Shell->OpenPage(TEXT("Playtest"));
	Pump(1.2, []() { return false; });
	HyperAIStudio::Tests::SaveWidgetShot(Shell, FIntPoint(1440, 900), TEXT("step14-studio-compact-playtest"), 12);
	TestTrue(TEXT("Compact keeps icons even when wide"), Shell->IsRailCompact());
	Shell->SetRail(SHyperAIStudioShell::ERail::Auto);
	if (!Wide.IsEmpty())
	{
		AddInfo(TEXT("Picture studio: ") + Wide);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioStudioOpenTest,
	"HyperAIStudio.Studio.Open",
	HyperAIStudio::StudioTests::Flags)

bool FHyperAIStudioStudioOpenTest::RunTest(const FString& Parameters)
{
	// The real way in: the window spawns and the page is there as Open returns, so a caller can talk to it.
	const TSharedPtr<SDockTab> Settings = FHyperAIStudioShell::Open(TEXT("Settings"));
	ON_SCOPE_EXIT
	{
		if (const TSharedPtr<SDockTab> Studio = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(FHyperAIStudioShell::GetTabName())))
		{
			Studio->RequestCloseTab();
		}
	};
	TestTrue(TEXT("Open(Settings) opens the window on Settings"), Settings.IsValid() && FHyperAIStudioShell::IsOpen());
	TestTrue(TEXT("The page is found by id"), FHyperAIStudioShell::FindPageTab(TEXT("Settings")) == Settings);
	FHyperAIStudioPreviewPanel::Open();
	TestTrue(TEXT("The preview panel's Open goes to the Match page"), FHyperAIStudioShell::FindPageTab(TEXT("Match")).IsValid());
	TestTrue(TEXT("Opening again brings the page forward rather than a second copy"),
		FHyperAIStudioShell::Open(TEXT("Settings")) == FHyperAIStudioShell::FindPageTab(TEXT("Settings")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensStudioPagesTest,
	"HyperAIStudio.Screens.StudioPages",
	HyperAIStudio::StudioTests::Flags)

bool FHyperAIStudioScreensStudioPagesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::StudioTests;
	// Settings: sections on the left, like Project Settings; image services in a details panel.
	const TSharedRef<SHyperAIStudioSettingsPage> Settings = SNew(SHyperAIStudioSettingsPage);
	TestEqual(TEXT("Settings opens on Setup"), Settings->GetSection(), FName(TEXT("Setup")));
	HyperAIStudio::Tests::SaveWidgetShot(Settings, FIntPoint(1200, 820), TEXT("step14-settings-setup"), 8);
	Settings->ShowSection(TEXT("Images"));
	TestEqual(TEXT("Image services opens"), Settings->GetSection(), FName(TEXT("Images")));
	HyperAIStudio::Tests::SaveWidgetShot(Settings, FIntPoint(1200, 820), TEXT("step14-settings-images"), 8);
	Settings->ShowSection(TEXT("Preferences"));
	HyperAIStudio::Tests::SaveWidgetShot(Settings, FIntPoint(1200, 820), TEXT("step14-settings-preferences"), 8);
	Settings->ShowSection(TEXT("Nonsense"));
	TestEqual(TEXT("An unknown section leaves the page where it was"), Settings->GetSection(), FName(TEXT("Preferences")));

	// Tools: every tool, searchable, each opening as a form beside the list.
	const TSharedRef<SHyperAIStudioToolsPage> Tools = SNew(SHyperAIStudioToolsPage);
	const bool bLoaded = Pump(150.0, [&Tools]() { return !Tools->IsRefreshing() && Tools->GetToolCount() > 0; });
	if (!bLoaded)
	{
		AddWarning(TEXT("The Tools page did not get the tool list from Unreal MCP in time; its picture shows it loading."));
	}
	else
	{
		AddInfo(FString::Printf(TEXT("Tools listed: %d"), Tools->GetToolCount()));
		const int32 Everything = Tools->GetShownCount();
		Tools->SetSource(EHyperAIStudioToolInventorySource::HyperAI);
		TestTrue(TEXT("HyperAI's own tools filter"), Tools->GetShownCount() > 0 && Tools->GetShownCount() < Everything);
		Tools->SetSource({});
		Tools->SetQuery(TEXT("image"));
		TestTrue(TEXT("Search narrows the list"), Tools->GetShownCount() > 0 && Tools->GetShownCount() < Everything);
		TestTrue(TEXT("Picking a tool opens its form"), Tools->SelectTool(TEXT("hyper_image_compare")));
		TestFalse(TEXT("An unknown tool is not picked"), Tools->SelectTool(TEXT("no_such_tool")));
	}
	HyperAIStudio::Tests::SaveWidgetShot(Tools, FIntPoint(1400, 860), TEXT("step14-tools"), 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMarkdownTest,
	"HyperAIStudio.Studio.Markdown",
	HyperAIStudio::StudioTests::Flags)

bool FHyperAIStudioMarkdownTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Markdown;
	const TArray<FBlock> Blocks = Parse(HyperAIStudio::StudioTests::Report);
	TArray<FString> Kinds;
	for (const FBlock& Block : Blocks)
	{
		Kinds.Add(FString::FromInt(static_cast<int32>(Block.Kind)));
	}
	AddInfo(TEXT("Blocks: ") + FString::Join(Kinds, TEXT(",")));
	// Heading, paragraph, heading, table, heading, 2 numbered, 2 bullets, quote, code, rule.
	TestEqual(TEXT("The report parses into its blocks"), Blocks.Num(), 12);
	if (Blocks.Num() == 12)
	{
		TestTrue(TEXT("# is a level 1 heading"), Blocks[0].Kind == EBlock::Heading && Blocks[0].Level == 1 && Blocks[0].Text == TEXT("Playtest: arena"));
		TestTrue(TEXT("A paragraph keeps its inline markdown"), Blocks[1].Kind == EBlock::Paragraph && Blocks[1].Text.Contains(TEXT("**4 cases**")));
		TestTrue(TEXT("## is a level 2 heading"), Blocks[2].Kind == EBlock::Heading && Blocks[2].Level == 2);
		TestTrue(TEXT("The table has a header and 4 rows of 3 cells"), Blocks[3].Kind == EBlock::Table && Blocks[3].Rows.Num() == 5
			&& Blocks[3].Rows[0].Num() == 3 && Blocks[3].Rows[4][0] == TEXT("explore"));
		TestTrue(TEXT("Numbered items keep their numbers"), Blocks[5].Kind == EBlock::Numbered && Blocks[5].Number == 1 && Blocks[6].Number == 2);
		TestTrue(TEXT("An indented bullet is one level deeper"), Blocks[7].Kind == EBlock::Bullet && Blocks[7].Level == 0
			&& Blocks[8].Kind == EBlock::Bullet && Blocks[8].Level == 1);
		TestTrue(TEXT("> is a quote"), Blocks[9].Kind == EBlock::Quote);
		TestTrue(TEXT("A fence keeps its language and code"), Blocks[10].Kind == EBlock::Code && Blocks[10].Label == TEXT("json")
			&& Blocks[10].Text.Contains(TEXT("\"skill\": 0.6")));
		TestTrue(TEXT("--- is a rule"), Blocks[11].Kind == EBlock::Rule);
	}
	const TArray<FBlock> Picture = Parse(TEXT("![The duel from above](C:/Maps/duel.png)"));
	TestTrue(TEXT("A picture on its own line is an image block with its caption"), Picture.Num() == 1 && Picture[0].Kind == EBlock::Image
		&& Picture[0].Text == TEXT("C:/Maps/duel.png") && Picture[0].Label == TEXT("The duel from above"));

	FString Rest;
	TestEqual(TEXT("PASS is a pass"), CellStatus(TEXT("PASS reached the goal"), Rest), FString(TEXT("pass")));
	TestEqual(TEXT("The rest follows the status"), Rest, FString(TEXT("reached the goal")));
	TestEqual(TEXT("FAIL: is a fail"), CellStatus(TEXT("FAIL: lost"), Rest), FString(TEXT("fail")));
	TestEqual(TEXT("timed_out is a warning"), CellStatus(TEXT("timed_out"), Rest), FString(TEXT("warn")));
	TestEqual(TEXT("PASSAGE is not a status"), CellStatus(TEXT("PASSAGE blocked"), Rest), FString());

	const FString Rich = ToRichText(TEXT("**Done** in `a<b` with _care_, see [docs](https://hyperaistudio.com/docs) and snake_case_name"));
	AddInfo(TEXT("Rich: ") + Rich);
	TestTrue(TEXT("Bold, code, italic and links become Md styles"), Rich.Contains(TEXT("<Md.Bold>Done</>")) && Rich.Contains(TEXT("<Md.Code>a&lt;b</>"))
		&& Rich.Contains(TEXT("<Md.Italic>care</>")) && Rich.Contains(TEXT("href=\"https://hyperaistudio.com/docs\"")));
	TestTrue(TEXT("Underscores inside a word are not italic"), Rich.Contains(TEXT("snake_case_name")));

	const FString Html = ToHtml(TEXT("A <script>alert(1)</script> in text\n\n| a | b |\n|---|---|\n| PASS ok | <b> |\n"));
	TestFalse(TEXT("HTML in the markdown is escaped"), Html.Contains(TEXT("<script>")));
	TestTrue(TEXT("Tables become HTML tables with status chips"), Html.Contains(TEXT("<table")) && Html.Contains(TEXT("PASS")));

	const FString File = SaveReport(TEXT("Playtest: arena"), HyperAIStudio::StudioTests::Report);
	FString Saved;
	TestTrue(TEXT("The report is saved as a page"), !File.IsEmpty() && FFileHelper::LoadFileToString(Saved, *File)
		&& Saved.StartsWith(TEXT("<!DOCTYPE html>")) && Saved.Contains(TEXT("reach-around-wall")));
	if (!File.IsEmpty())
	{
		AddInfo(TEXT("Picture html-report: ") + File);
	}

	// The same report as Slate shows it in a chat or a results panel.
	const TSharedRef<SHyperAIStudioMarkdown> View = SNew(SHyperAIStudioMarkdown).Markdown(HyperAIStudio::StudioTests::Report);
	const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(
		SNew(SBox).WidthOverride(860.0f)[View], FIntPoint(900, 900), TEXT("step14-markdown"), 8);
	if (!Shot.IsEmpty())
	{
		AddInfo(TEXT("Picture markdown: ") + Shot);
	}
	View->SetMarkdown(TEXT("Just a line."));
	TestEqual(TEXT("The view shows new markdown"), View->GetMarkdown(), FString(TEXT("Just a line.")));
	return true;
}

#endif
