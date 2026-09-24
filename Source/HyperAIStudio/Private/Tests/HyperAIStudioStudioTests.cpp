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
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "HyperAIStudioPreviewPanel.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioShell.h"
#include "HyperAIStudioVerify.h"
#include "HyperAIStudioWidgetShot.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "JsonObjectConverter.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioPlaytestPanel.h"
#include "SHyperAIStudioSettingsPage.h"
#include "SHyperAIStudioShell.h"
#include "SHyperAIStudioToolForm.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioJsonReportTest,
	"HyperAIStudio.Studio.JsonReport",
	HyperAIStudio::StudioTests::Flags)

bool FHyperAIStudioJsonReportTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Markdown;
	const FString Picture = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("JsonReport") / TEXT("side-by-side.png"));
	HyperAIStudio::Tests::WriteScenePng(Picture, FIntPoint(64, 40));
	const FString Json = FString::Printf(TEXT(R"({"returnValue": {"status": "completed", "summary": "Scored 72 of 100: close; the sky is too blue.",
		"bestScore": 72, "MCPPort": 8020, "sessionId": "abc", "passed": false, "tags": ["sky", "tone"], "sideBySidePath": "%s",
		"cells": [{"row": 0, "col": 1, "delta": 0.21}, {"row": 2, "col": 3, "delta": 0.125}], "detail": {"palette": 80, "tone": 64}}})"),
		*Picture.Replace(TEXT("\\"), TEXT("/")));
	const FString Report = FromJson(Json);
	AddInfo(TEXT("Report: ") + Report.Replace(TEXT("\n"), TEXT(" / ")));
	TestTrue(TEXT("It leads with the summary"), Report.StartsWith(TEXT("Scored 72 of 100: close; the sky is too blue.")));
	TestTrue(TEXT("Fields read as words"), Report.Contains(TEXT("| Best score | 72 |")) && Report.Contains(TEXT("| MCP port | 8020 |"))
		&& Report.Contains(TEXT("| Session ID | abc |")) && Report.Contains(TEXT("| Passed | no |")));
	TestTrue(TEXT("A status field keeps its word, which a table shows as a chip"), Report.Contains(TEXT("| Status | completed |")));
	TestTrue(TEXT("A list of words is one cell"), Report.Contains(TEXT("| Tags | sky, tone |")));
	TestTrue(TEXT("Records become a table of their own"), Report.Contains(TEXT("## Cells (2)")) && Report.Contains(TEXT("| Row | Col | Delta |"))
		&& Report.Contains(TEXT("| 0 | 1 | 0.21 |")) && Report.Contains(TEXT("| 2 | 3 | 0.125 |")));
	TestTrue(TEXT("A nested object gets a heading"), Report.Contains(TEXT("## Detail")) && Report.Contains(TEXT("| Palette | 80 |")));
	TestTrue(TEXT("A picture on disk is linked and shown"), Report.Contains(TEXT("[side-by-side.png](")) && Report.Contains(TEXT("![side-by-side.png](")));
	TestTrue(TEXT("Not JSON gives nothing, so callers show the text"), FromJson(TEXT("plain words")).IsEmpty() && FromJson(TEXT("{broken")).IsEmpty());
	TestTrue(TEXT("A list of values is a one-column table"), FromJson(TEXT("[1, 2]")).Contains(TEXT("| 1 |")));
	TestTrue(TEXT("Marks in values are escaped"), FromJson(TEXT(R"({"name": "IA_Move|*x*"})")).Contains(TEXT("IA\\_Move\\|\\*x\\*")));

	FString Rest;
	TestEqual(TEXT("\"completed\" alone is a pass"), CellStatus(TEXT("completed"), Rest), FString(TEXT("pass")));
	TestEqual(TEXT("\"running\" alone is info"), CellStatus(TEXT("running"), Rest), FString(TEXT("info")));
	TestEqual(TEXT("but a sentence starting with Success is not a status"), CellStatus(TEXT("Success rate is 50%"), Rest), FString());

	// Agents are asked for replies that read this way.
	const FString Agents = FHyperAIStudioService::BuildAgentsInstructionText();
	TestTrue(TEXT("AGENTS.md asks for a brief explanation, then results"), Agents.Contains(TEXT("## Replies"))
		&& Agents.Contains(TEXT("Start with what happened")) && Agents.Contains(TEXT("`PASS`")) && Agents.Contains(TEXT("![caption](absolute/path.png)")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensResultReportsTest,
	"HyperAIStudio.Screens.ResultReports",
	HyperAIStudio::StudioTests::Flags)

bool FHyperAIStudioScreensResultReportsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::StudioTests;
	namespace Verify = HyperAIStudio::Verify;
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("ResultReports"));
	const FString Reference = Folder / TEXT("reference.png");
	const FString Attempt = Folder / TEXT("attempt.png");
	HyperAIStudio::Tests::WriteScenePng(Reference, FIntPoint(480, 300));
	HyperAIStudio::Tests::WriteScenePng(Attempt, FIntPoint(480, 300), FLinearColor(0.8f, 0.55f, 0.35f), FLinearColor(0.3f, 0.2f, 0.4f));

	// A tool's answer as a report, beside its JSON.
	const TSharedRef<SHyperAIStudioToolForm> Form = SNew(SHyperAIStudioToolForm)
		.Toolset(TEXT("HyperAIStudioLighting.HyperAIStudioLightingToolset"))
		.Tool(TEXT("hyper_image_compare"))
		.Description(TEXT("How close an attempt is to a reference image, for scenes, materials, effects and UI."));
	FString Error;
	Form->SetArguments(FString::Printf(TEXT("{\"request\": {\"referenceImagePath\": \"%s\", \"candidateImagePath\": \"%s\", \"mode\": \"look\", \"targetScore\": 80, \"grid\": 4}}"),
		*Reference.Replace(TEXT("\\"), TEXT("/")), *Attempt.Replace(TEXT("\\"), TEXT("/"))), Error);
	Form->Run();
	const double End = FPlatformTime::Seconds() + 30.0;
	while (Form->IsRunning() && FPlatformTime::Seconds() < End)
	{
		Form->Tick(FGeometry(), 0.0, 0.05f);
		FTSTicker::GetCoreTicker().Tick(0.02f);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
	Form->Tick(FGeometry(), 0.0, 0.05f);
	AddInfo(TEXT("Tool report: ") + Form->GetReport().Left(500).Replace(TEXT("\n"), TEXT(" / ")));
	TestTrue(TEXT("The answer reads as a report with its score and pictures"), Form->GetReport().Contains(TEXT("| Score |")) && Form->GetReport().Contains(TEXT("![")));
	HyperAIStudio::Tests::SaveWidgetShot(Form, FIntPoint(900, 1180), TEXT("step15-tool-report"), 10);
	Form->ShowJson(true);
	HyperAIStudio::Tests::SaveWidgetShot(Form, FIntPoint(900, 700), TEXT("step15-tool-json"), 4);

	// A playtest batch as a report: one sentence, the cases, then each case's steps and pictures.
	Verify::FBatchOutcome Batch;
	Batch.Status = TEXT("failed");
	Batch.Map = TEXT("/Game/Maps/Lvl_Arena");
	Batch.WallSeconds = 21.4;
	Batch.Limit = 4;
	auto Step = [](const TCHAR* Action, const TCHAR* Status, const TCHAR* Code, const TCHAR* Detail, const FString& File = FString())
	{
		Verify::FStepOutcome Out;
		Out.Action = Action;
		Out.Status = Status;
		Out.Code = Code;
		Out.Detail = Detail;
		Out.File = File;
		return Out;
	};
	Verify::FCaseOutcome& Reach = Batch.Cases.AddDefaulted_GetRef();
	Reach.Name = TEXT("reach-around-wall");
	Reach.Status = TEXT("passed");
	Reach.Diagnostic = TEXT("reached the goal round a 7 m wall");
	Reach.SimulatedSeconds = 4.2f;
	Reach.Steps = {Step(TEXT("Play"), TEXT("passed"), TEXT(""), TEXT("Reach: within 150 cm of the goal after 4.2 s, 11.8 m walked, 1 jump."), Reference),
		Step(TEXT("Capture"), TEXT("passed"), TEXT(""), TEXT("reached"), Attempt)};
	Verify::FCaseOutcome& Duel = Batch.Cases.AddDefaulted_GetRef();
	Duel.Name = TEXT("duel");
	Duel.Status = TEXT("failed");
	Duel.Diagnostic = TEXT("Player0 lost 2-3 to a skill 0.9 opponent");
	Duel.SimulatedSeconds = 30.0f;
	Duel.FailedStep = 2;
	Duel.Steps = {Step(TEXT("AddPlayer"), TEXT("passed"), TEXT(""), TEXT("Player1 joined at the far end.")),
		Step(TEXT("Play"), TEXT("failed"), TEXT("goal_not_met"), TEXT("Player1 won with 40 health left; Player0 fired 23 shots, 9 hits."))};
	Verify::FCaseOutcome& Explore = Batch.Cases.AddDefaulted_GetRef();
	Explore.Name = TEXT("explore");
	Explore.Status = TEXT("timed_out");
	Explore.Diagnostic = TEXT("71% of the floor seen before the case's 20 s ran out");
	Explore.SimulatedSeconds = 20.0f;
	Explore.Steps = {Step(TEXT("Play"), TEXT("timed_out"), TEXT("timeout"), TEXT("Explore: 35 squares of 4 m visited."))};
	const FString BatchReport = SHyperAIStudioPlaytestPanel::BuildReport(Batch);
	AddInfo(TEXT("Playtest report: ") + BatchReport.Left(600).Replace(TEXT("\n"), TEXT(" / ")));
	TestTrue(TEXT("It says what happened first"), BatchReport.Contains(TEXT("## Playtest: Lvl_Arena")) && BatchReport.Contains(TEXT("1 of 3 cases passed")));
	TestTrue(TEXT("then the cases, with their results"), BatchReport.Contains(TEXT("| reach-around-wall | PASSED reached the goal round a 7 m wall | 4.2 s |"))
		&& BatchReport.Contains(TEXT("| duel | FAILED")) && BatchReport.Contains(TEXT("| explore | TIMED_OUT")));
	TestTrue(TEXT("then each case's steps, with why one failed"), BatchReport.Contains(TEXT("### duel")) && BatchReport.Contains(TEXT("FAILED goal_not_met: Player1 won")));
	TestTrue(TEXT("and the pictures its steps made"), BatchReport.Contains(TEXT("![reference](")) && BatchReport.Contains(TEXT("![attempt](")));
	const TSharedRef<SHyperAIStudioPlaytestPanel> Panel = SNew(SHyperAIStudioPlaytestPanel);
	Panel->ShowOutcome(Batch);
	HyperAIStudio::Tests::SaveWidgetShot(Panel, FIntPoint(1400, 1000), TEXT("step15-playtest-report"), 10);
	TestTrue(TEXT("The report saves as an HTML page"), !HyperAIStudio::Markdown::SaveReport(TEXT("Playtest Lvl_Arena"), BatchReport).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensStudioPolishTest,
	"HyperAIStudio.Screens.StudioPolish",
	HyperAIStudio::StudioTests::Flags)

bool FHyperAIStudioScreensStudioPolishTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::StudioTests;
	// Playtest cases are a form, over the same request the JSON holds.
	const TSharedRef<SHyperAIStudioPlaytestPanel> Playtest = SNew(SHyperAIStudioPlaytestPanel);
	TestFalse(TEXT("Playtest opens on the form"), Playtest->IsJsonShown());
	Playtest->AddTemplate(TEXT("Reach"));
	TestTrue(TEXT("A template still adds its case"), Playtest->GetCases().Contains(TEXT("\"reach\"")));
	TestFalse(TEXT("and the form shows it"), Playtest->IsJsonShown());
	HyperAIStudio::Tests::SaveWidgetShot(Playtest, FIntPoint(1400, 900), TEXT("step17-playtest-form"), 8);
	TestTrue(TEXT("The JSON is one click away"), Playtest->ShowJson(true) && Playtest->IsJsonShown());
	HyperAIStudio::Tests::SaveWidgetShot(Playtest, FIntPoint(1400, 900), TEXT("step17-playtest-json"), 4);
	TestTrue(TEXT("and so is the form again"), Playtest->ShowJson(false) && !Playtest->IsJsonShown());
	Playtest->SetCases(TEXT("{\"cases\": [ {broken"));
	TestTrue(TEXT("JSON the form cannot hold is shown as JSON to fix"), Playtest->IsJsonShown());
	TestFalse(TEXT("and the form waits until it can"), Playtest->ShowJson(false));

	// Written back without the fields left at their defaults.
	FHyperAIVerifyRequest Request;
	FHyperAIVerifyCase& Case = Request.Cases.AddDefaulted_GetRef();
	Case.Name = TEXT("explore");
	FHyperAIBotStep& Step = Case.Steps.AddDefaulted_GetRef();
	Step.Action = EHyperAIBotAction::Play;
	Step.Goal = EHyperAIBotGoal::Explore;
	Step.Seconds = 20.0f;
	const FString Json = SHyperAIStudioPlaytestPanel::RequestToJson(Request);
	AddInfo(TEXT("Form as JSON: ") + Json.Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\t"), TEXT("")));
	TestTrue(TEXT("The JSON keeps what was set"), Json.Contains(TEXT("\"action\": \"Play\"")) && Json.Contains(TEXT("\"goal\": \"Explore\""))
		&& Json.Contains(TEXT("\"seconds\": 20")) && Json.Contains(TEXT("\"name\": \"explore\"")));
	TestFalse(TEXT("and leaves out what was not"), Json.Contains(TEXT("\"skill\"")) || Json.Contains(TEXT("\"playerIndex\"")));
	FHyperAIVerifyRequest Back;
	TestTrue(TEXT("and reads back as the same request"), FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Back, 0, 0)
		&& Back.Cases.Num() == 1 && Back.Cases[0].Steps.Num() == 1 && Back.Cases[0].Steps[0].Goal == EHyperAIBotGoal::Explore
		&& FMath::IsNearlyEqual(Back.Cases[0].Steps[0].Seconds, 20.0f));

	// Pages that were panels of the old window, in the new one.
	const TSharedRef<SDockTab> Owner = SNew(SDockTab).TabRole(ETabRole::MajorTab);
	const TSharedRef<SHyperAIStudioShell> Shell = SNew(SHyperAIStudioShell, Owner, nullptr);
	Owner->SetContent(Shell);
	Shell->SetRail(SHyperAIStudioShell::ERail::Expanded);
	ON_SCOPE_EXIT { Shell->SetRail(SHyperAIStudioShell::ERail::Auto); };
	for (const TCHAR* Page : {TEXT("Memory"), TEXT("Settings")})
	{
		Shell->OpenPage(Page);
		Pump(1.2, []() { return false; });
		HyperAIStudio::Tests::SaveWidgetShot(Shell, FIntPoint(1440, 900), FString::Printf(TEXT("step17-page-%s"), Page).ToLower(), 10);
	}
	return true;
}

#endif
