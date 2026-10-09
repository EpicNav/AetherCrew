// Games by Hyper 2026.

#include "HyperAIStudioShell.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/LayoutService.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioPromptPresets.h"
#include "HyperAIStudioStyle.h"
#include "HyperAIStudioWidgets.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "SHyperAIStudioActivitySidebar.h"
#include "SHyperAIStudioAutoApprovalBanner.h"
#include "SHyperAIStudioQuickActionWindow.h"
#include "SHyperAIStudioShell.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Styling/StyleDefaults.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioShell"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioShell, Log, All);

namespace HyperAIStudio::Shell
{
	/**
	 * HyperAIStudio.Screenshot [Window] [name]: the Studio as the editor drew it, as a PNG under Saved/HyperAIStudio/Screens.
	 * Window takes the whole window the Studio is in: the editor itself when the Studio is docked there.
	 */
	void SaveScreenshot(const TArray<FString>& InArgs)
	{
		TArray<FString> Args = InArgs;
		const bool bWindow = Args.RemoveAll([](const FString& Arg) { return Arg.Equals(TEXT("Window"), ESearchCase::IgnoreCase); }) > 0;
		const FString Name = FPaths::MakeValidFileName(Args.IsEmpty() ? FString(TEXT("studio")) : FString::Join(Args, TEXT("-")), TEXT('_'));
		const TSharedPtr<SDockTab> Studio = FSlateApplication::IsInitialized()
			? FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(FHyperAIStudioShell::GetTabName())) : nullptr;
		const TSharedPtr<SWidget> Widget = !Studio.IsValid() ? TSharedPtr<SWidget>() : bWindow ? StaticCastSharedPtr<SWidget>(Studio->GetParentWindow()) : Studio->GetContent().ToSharedPtr();
		TArray<FColor> Pixels;
		FIntVector Size;
		if (!Widget.IsValid() || !FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Size) || Size.X <= 0 || Size.Y <= 0)
		{
			UE_LOG(LogHyperAIStudioShell, Warning, TEXT("HyperAIStudio.Screenshot: the HyperAI Studio window is not open."));
			return;
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), static_cast<int64>(Size.X) * Size.Y), Png);
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("Screens") / (Name + TEXT(".png")));
		UE_LOG(LogHyperAIStudioShell, Display, TEXT("HyperAIStudio.Screenshot: %s"), FFileHelper::SaveArrayToFile(Png, *Path) ? *Path : TEXT("could not be written"));
	}

	/** HyperAIStudio.OpenPage <page id>: brings a page of the Studio forward, such as Match, Images or Chat2. */
	void OpenPageFromConsole(const TArray<FString>& Args)
	{
		const FName PageId = Args.IsEmpty() ? FName(TEXT("Home")) : FName(*Args[0]);
		UE_LOG(LogHyperAIStudioShell, Display, TEXT("HyperAIStudio.OpenPage %s: %s"), *PageId.ToString(),
			FHyperAIStudioShell::Open(PageId).IsValid() ? TEXT("shown") : TEXT("no such page"));
	}

	FAutoConsoleCommand OpenPageCommand(TEXT("HyperAIStudio.OpenPage"),
		TEXT("Bring a page of HyperAI Studio forward by its id, such as Home, Chat, Chat2, Match, Images or Memory: HyperAIStudio.OpenPage <page>."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&OpenPageFromConsole));

	FAutoConsoleCommand ScreenshotCommand(TEXT("HyperAIStudio.Screenshot"),
		TEXT("Save the HyperAI Studio window, as the editor drew it, to Saved/HyperAIStudio/Screens/<name>.png; Window takes the window it is in: HyperAIStudio.Screenshot [Window] [name]."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&SaveScreenshot));

	constexpr float ExpandedRail = 204.0f;
	constexpr float CompactRail = 48.0f;
	/** Narrower than this, the rail folds to icons by itself. */
	constexpr float CompactBelow = 1000.0f;
	/** Narrower than this, the search box shrinks. */
	constexpr float NarrowBelow = 780.0f;
	constexpr double StatusSeconds = 2.0;
	/** Prompt presets in search: a few that fit, never enough to bury the pages and actions. */
	constexpr int32 MaxPresetResults = 6;
	const TCHAR* ConfigSection = TEXT("HyperAIStudio.Studio");
	const FName HomePage(TEXT("Home"));

	TArray<FHyperAIStudioPage>& Pages()
	{
		static TArray<FHyperAIStudioPage> Registered;
		return Registered;
	}

	TArray<FHyperAIStudioAction>& Actions()
	{
		static TArray<FHyperAIStudioAction> Registered;
		return Registered;
	}

	FSimpleMulticastDelegate& Changed()
	{
		static FSimpleMulticastDelegate Delegate;
		return Delegate;
	}

	TWeakPtr<SHyperAIStudioShell>& Live()
	{
		static TWeakPtr<SHyperAIStudioShell> Shell;
		return Shell;
	}

	int32 GroupRank(const FName Group)
	{
		static const FName Order[] = {TEXT("Start"), TEXT("Create"), TEXT("Test"), TEXT("Project"), TEXT("System")};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Order); ++Index)
		{
			if (Order[Index] == Group)
			{
				return Index;
			}
		}
		return UE_ARRAY_COUNT(Order);
	}

	FText GroupLabel(const FName Group)
	{
		if (Group == TEXT("Create"))
		{
			return LOCTEXT("GroupCreate", "Create");
		}
		if (Group == TEXT("Test"))
		{
			return LOCTEXT("GroupTest", "Test");
		}
		if (Group == TEXT("Project"))
		{
			return LOCTEXT("GroupProject", "Project");
		}
		if (Group == TEXT("System"))
		{
			return LOCTEXT("GroupSystem", "System");
		}
		return FText::FromName(Group);
	}

	TSharedRef<SDockTab> SpawnStudio(const FSpawnTabArgs& Args)
	{
		const TSharedRef<SDockTab> Tab = SNew(SDockTab)
			.TabRole(ETabRole::MajorTab)
			.Label(LOCTEXT("StudioTab", "HyperAI Studio"));
		const TSharedRef<SHyperAIStudioShell> Shell = SNew(SHyperAIStudioShell, Tab, Args.GetOwnerWindow());
		Tab->SetContent(Shell);
		Live() = Shell;
		return Tab;
	}
}

FName FHyperAIStudioShell::GetTabName()
{
	static const FName Name(TEXT("HyperAIStudio"));
	return Name;
}

void FHyperAIStudioShell::RegisterPage(const FHyperAIStudioPage& Page)
{
	using namespace HyperAIStudio::Shell;
	Pages().RemoveAll([&Page](const FHyperAIStudioPage& Known) { return Known.Id == Page.Id; });
	Pages().Add(Page);
	Changed().Broadcast();
}

void FHyperAIStudioShell::UnregisterPage(const FName Id)
{
	using namespace HyperAIStudio::Shell;
	if (Pages().RemoveAll([Id](const FHyperAIStudioPage& Known) { return Known.Id == Id; }) > 0)
	{
		Changed().Broadcast();
	}
}

TArray<FHyperAIStudioPage> FHyperAIStudioShell::GetPages()
{
	using namespace HyperAIStudio::Shell;
	TArray<FHyperAIStudioPage> Sorted = Pages();
	Sorted.StableSort([](const FHyperAIStudioPage& A, const FHyperAIStudioPage& B)
	{
		const int32 RankA = GroupRank(A.Group);
		const int32 RankB = GroupRank(B.Group);
		return RankA != RankB ? RankA < RankB : A.Order < B.Order;
	});
	return Sorted;
}

const FHyperAIStudioPage* FHyperAIStudioShell::FindPage(const FName Id)
{
	return HyperAIStudio::Shell::Pages().FindByPredicate([Id](const FHyperAIStudioPage& Known) { return Known.Id == Id; });
}

void FHyperAIStudioShell::RegisterAction(const FHyperAIStudioAction& Action)
{
	using namespace HyperAIStudio::Shell;
	Actions().RemoveAll([&Action](const FHyperAIStudioAction& Known) { return Known.Id == Action.Id; });
	Actions().Add(Action);
	Changed().Broadcast();
}

void FHyperAIStudioShell::UnregisterAction(const FName Id)
{
	using namespace HyperAIStudio::Shell;
	if (Actions().RemoveAll([Id](const FHyperAIStudioAction& Known) { return Known.Id == Id; }) > 0)
	{
		Changed().Broadcast();
	}
}

TArray<FHyperAIStudioAction> FHyperAIStudioShell::GetActions()
{
	return HyperAIStudio::Shell::Actions();
}

TSharedPtr<SDockTab> FHyperAIStudioShell::Open(const FName PageId)
{
	using namespace HyperAIStudio::Shell;
	if (!FSlateApplication::IsInitialized() || IsRunningCommandlet())
	{
		return nullptr;
	}
	if (const TSharedPtr<SHyperAIStudioShell> Shell = Live().Pin())
	{
		if (const TSharedPtr<SDockTab> Studio = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(GetTabName())))
		{
			Studio->ActivateInParent(ETabActivationCause::SetDirectly);
		}
		return PageId.IsNone() ? nullptr : Shell->OpenPage(PageId);
	}
	// The window spawns as it is invoked and is in place once this returns, so the page opens straight away.
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(GetTabName()));
	const TSharedPtr<SHyperAIStudioShell> Shell = Live().Pin();
	return Shell.IsValid() && !PageId.IsNone() ? Shell->OpenPage(PageId) : nullptr;
}

TSharedPtr<SDockTab> FHyperAIStudioShell::FindPageTab(const FName PageId)
{
	const TSharedPtr<SHyperAIStudioShell> Shell = HyperAIStudio::Shell::Live().Pin();
	return Shell.IsValid() ? Shell->FindPageTab(PageId) : nullptr;
}

bool FHyperAIStudioShell::IsOpen()
{
	return HyperAIStudio::Shell::Live().IsValid();
}

FSimpleMulticastDelegate& FHyperAIStudioShell::OnChanged()
{
	return HyperAIStudio::Shell::Changed();
}

void FHyperAIStudioShell::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GetTabName(), FOnSpawnTab::CreateStatic(&HyperAIStudio::Shell::SpawnStudio))
		.SetDisplayName(LOCTEXT("StudioTab", "HyperAI Studio"))
		.SetTooltipText(LOCTEXT("StudioTabTooltip", "Every HyperAI feature in one window: chat, images, match, playtest, memory, skills, agents, tools, activity and settings."))
		.SetIcon(FSlateIcon(FHyperAIStudioStyle::GetStyleSetName(), "HyperAIStudio.TabIcon"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
	// The first time it opens, as a window big enough for the rail, a page and the tab strip; after that it opens
	// where the user left it.
	FTabManager::RegisterDefaultTabWindowSize(FTabId(GetTabName()), FVector2D(1360.0, 860.0));
}

void FHyperAIStudioShell::UnregisterTabSpawner()
{
	FTabManager::UnregisterDefaultTabWindowSize(FTabId(GetTabName()));
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GetTabName());
	}
}

void SHyperAIStudioShell::Construct(const FArguments& InArgs, const TSharedRef<SDockTab>& InOwnerTab, const TSharedPtr<SWindow>& OwnerWindow)
{
	using namespace HyperAIStudio::Shell;
	using namespace HyperAIStudio::UI;
	OwnerTab = InOwnerTab;
	Service = MakeShared<FHyperAIStudioService>();
	int32 SavedRail = 0;
	if (GConfig && GConfig->GetInt(ConfigSection, TEXT("Rail"), SavedRail, GEditorPerProjectIni))
	{
		Rail = static_cast<ERail>(FMath::Clamp(SavedRail, 0, 2));
	}

	// Every page gets a closed place in the one stack, so it opens there, never in a window of its own.
	TabManager = FGlobalTabmanager::Get()->NewTabManager(InOwnerTab);
	TabManager->SetOnPersistLayout(FTabManager::FOnPersistLayout::CreateLambda([](const TSharedRef<FTabManager::FLayout>& Layout)
	{
		FLayoutSaveRestore::SaveToConfig(GEditorLayoutIni, Layout);
	}));
	const TSharedRef<FTabManager::FStack> Stack = FTabManager::NewStack();
	FString Known;
	for (const FHyperAIStudioPage& Page : FHyperAIStudioShell::GetPages())
	{
		RegisterSpawner(Page);
		Stack->AddTab(Page.Id, Page.Id == HomePage ? ETabState::OpenedTab : ETabState::ClosedTab);
		Known += Page.Id.ToString();
	}
	Stack->SetForegroundTab(FTabId(HomePage));
	// A page added in a later version changes the layout's name, so a saved layout never leaves it homeless.
	const FName LayoutName(*FString::Printf(TEXT("HyperAIStudio_Studio_v1_%08x"), GetTypeHash(Known)));
	TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout(LayoutName)
		->AddArea(FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)->Split(Stack));
	Layout = FLayoutSaveRestore::LoadFromConfig(GEditorLayoutIni, Layout);
	InOwnerTab->SetOnPersistVisualState(SDockTab::FOnPersistVisualState::CreateSP(this, &SHyperAIStudioShell::PersistLayout));
	const TSharedPtr<SWidget> Dock = TabManager->RestoreFrom(Layout, OwnerWindow);
	CurrentPage = HomePage;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(Style().GetBrush("HyperAIStudio.Page"))
		.Padding(0.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildHeader()
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).HeightOverride(1.0f)
				[
					SNew(SImage).Image(Style().GetBrush("HyperAIStudio.Divider"))
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					BuildRail()
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(FMargin(4.0f, 4.0f, 4.0f, 2.0f))
				[
					Dock.IsValid() ? Dock.ToSharedRef() : SNullWidget::NullWidget
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildStatusBar()
			]
		]
	];
	RebuildNav();
	ActiveTabHandle = FGlobalTabmanager::Get()->OnActiveTabChanged_Subscribe(FOnActiveTabChanged::FDelegate::CreateSP(this, &SHyperAIStudioShell::OnActiveTabChanged));
	ForegroundHandle = FGlobalTabmanager::Get()->OnTabForegrounded_Subscribe(FOnActiveTabChanged::FDelegate::CreateSP(this, &SHyperAIStudioShell::OnActiveTabChanged));
	ChangedHandle = FHyperAIStudioShell::OnChanged().AddSP(this, &SHyperAIStudioShell::OnPagesChanged);
	RefreshStatus();
}

SHyperAIStudioShell::~SHyperAIStudioShell()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->OnActiveTabChanged_Unsubscribe(ActiveTabHandle);
		FGlobalTabmanager::Get()->OnTabForegrounded_Unsubscribe(ForegroundHandle);
	}
	FHyperAIStudioShell::OnChanged().Remove(ChangedHandle);
}

void SHyperAIStudioShell::RegisterSpawner(const FHyperAIStudioPage& Page)
{
	if (!TabManager.IsValid() || TabManager->HasTabSpawner(Page.Id))
	{
		return;
	}
	TabManager->RegisterTabSpawner(Page.Id, FOnSpawnTab::CreateSP(this, &SHyperAIStudioShell::SpawnPage, Page.Id))
		.SetDisplayName(Page.Label)
		.SetTooltipText(Page.Tooltip)
		.SetIcon(FSlateIcon(FHyperAIStudioStyle::GetStyleSetName(), Page.Icon));
}

TSharedRef<SDockTab> SHyperAIStudioShell::SpawnPage(const FSpawnTabArgs& Args, const FName PageId)
{
	using namespace HyperAIStudio::UI;
	const FHyperAIStudioPage* Page = FHyperAIStudioShell::FindPage(PageId);
	const TSharedRef<SDockTab> Tab = SNew(SDockTab)
		.TabRole(ETabRole::PanelTab)
		.Label(Page ? Page->Label : FText::FromName(PageId));
	TSharedRef<SWidget> Content = Page && Page->Create
		? Page->Create(Tab)
		: EmptyState(Icon("Icons.Warning"), LOCTEXT("MissingPageTitle", "This page is not available"),
			LOCTEXT("MissingPageBody", "The feature that provides it is not loaded. Check that its plugin module is enabled."));
	Tab->SetContent(
		SNew(SBorder)
		.BorderImage(Style().GetBrush("HyperAIStudio.Page"))
		.Padding(0.0f)
		[
			Content
		]);
	return Tab;
}

TSharedPtr<SDockTab> SHyperAIStudioShell::OpenPage(const FName PageId)
{
	if (!TabManager.IsValid())
	{
		return nullptr;
	}
	if (!TabManager->HasTabSpawner(PageId))
	{
		const FHyperAIStudioPage* Page = FHyperAIStudioShell::FindPage(PageId);
		if (!Page)
		{
			return nullptr;
		}
		RegisterSpawner(*Page);
	}
	const TSharedPtr<SDockTab> Tab = TabManager->TryInvokeTab(FTabId(PageId));
	if (Tab.IsValid())
	{
		CurrentPage = PageId;
		Tab->ActivateInParent(ETabActivationCause::SetDirectly);
	}
	return Tab;
}

TSharedPtr<SDockTab> SHyperAIStudioShell::FindPageTab(const FName PageId) const
{
	return TabManager.IsValid() ? TabManager->FindExistingLiveTab(FTabId(PageId)) : nullptr;
}

bool SHyperAIStudioShell::IsRailCompact() const
{
	using namespace HyperAIStudio::Shell;
	return Rail == ERail::Compact || (Rail == ERail::Auto && Width < CompactBelow);
}

void SHyperAIStudioShell::SetRail(const ERail InRail)
{
	Rail = InRail;
	if (GConfig)
	{
		GConfig->SetInt(HyperAIStudio::Shell::ConfigSection, TEXT("Rail"), static_cast<int32>(Rail), GEditorPerProjectIni);
	}
}

void SHyperAIStudioShell::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	Width = AllottedGeometry.GetLocalSize().X;
	if (InCurrentTime >= NextStatusAt)
	{
		NextStatusAt = InCurrentTime + HyperAIStudio::Shell::StatusSeconds;
		RefreshStatus();
	}
}

FReply SHyperAIStudioShell::OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// Ctrl+K, as in most tools, goes to search from anywhere in the window.
	if (InKeyEvent.GetKey() == EKeys::K && InKeyEvent.IsControlDown() && !InKeyEvent.IsAltDown() && SearchBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(SearchBox, EFocusCause::SetDirectly);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void SHyperAIStudioShell::RefreshStatus()
{
	if (Service.IsValid())
	{
		Mcp = Service->GetMcpRuntimeStatusFast();
		bHasStatus = true;
	}
	Approvals = SHyperAIStudioChatActivitySidebar::GetPendingApprovalCount();
	Running = 0;
	for (const FHyperAIStudioAsyncJobStatus& Job : FHyperAIStudioAsyncJobHost::GetSnapshot())
	{
		Running += Job.State == EHyperAIStudioAsyncJobState::Running;
	}
}

FText SHyperAIStudioShell::GetMcpText() const
{
	if (!bHasStatus)
	{
		return LOCTEXT("McpChecking", "Checking Unreal MCP...");
	}
	if (Mcp.bServerRunning)
	{
		const uint32 Port = Mcp.ActivePort != 0 ? Mcp.ActivePort : Mcp.ConfiguredPort;
		return Port != 0 ? FText::Format(LOCTEXT("McpRunningPort", "Unreal MCP running on port {0}"), FText::AsNumber(Port, &FNumberFormattingOptions::DefaultNoGrouping()))
			: LOCTEXT("McpRunning", "Unreal MCP running");
	}
	return Mcp.bUnrealMCPModuleAvailable ? LOCTEXT("McpStopped", "Unreal MCP is not running") : LOCTEXT("McpMissing", "Unreal MCP is not set up");
}

FSlateColor SHyperAIStudioShell::GetMcpColor() const
{
	using namespace HyperAIStudio::UI;
	return !bHasStatus ? StatusColor(EStatus::Neutral) : Mcp.bServerRunning ? StatusColor(EStatus::Good) : StatusColor(EStatus::Warning);
}

TSharedRef<SWidget> SHyperAIStudioShell::BuildHeader()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::Shell;
	return SNew(SBorder)
		.BorderImage(Style().GetBrush("HyperAIStudio.Bar"))
		.Padding(FMargin(6.0f, 6.0f, 10.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				IconButton(Icon("HyperAIStudio.Icon.Menu"), LOCTEXT("ToggleRail", "Show or hide the page names in the navigation."),
					FOnClicked::CreateLambda([this]()
					{
						SetRail(IsRailCompact() ? ERail::Expanded : ERail::Compact);
						return FReply::Handled();
					}))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.0f, 0.0f, 10.0f, 0.0f))
			[
				SNew(SBox).WidthOverride(24.0f).HeightOverride(24.0f)
				[
					SNew(SImage).Image(Icon("HyperAIStudio.Logo"))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("StudioTitle", "HyperAI Studio"))
					.TextStyle(&Style(), "HyperAIStudio.Text.CardTitle")
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					.Text_Lambda([this]()
					{
						const FHyperAIStudioPage* Page = FHyperAIStudioShell::FindPage(CurrentPage);
						return Page ? Page->Label : FText::GetEmpty();
					})
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(12.0f, 0.0f))
			[
				BuildSearch()
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.0f, 0.0f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("McpTooltip", "The Unreal MCP server agents connect to. Click for setup and status."))
				.OnClicked_Lambda([this]()
				{
					OpenPage(TEXT("Settings"));
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						Dot(TAttribute<FSlateColor>::CreateSP(this, &SHyperAIStudioShell::GetMcpColor))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(STextBlock)
						.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
						.Text_Lambda([this]() { return Width < NarrowBelow ? LOCTEXT("McpShort", "MCP") : GetMcpText(); })
					]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				IconButton(Icon("HyperAIStudio.Icon.Settings"), LOCTEXT("SettingsTooltip", "Settings: setup, agents, MCP servers, image services and preferences."),
					FOnClicked::CreateLambda([this]()
					{
						OpenPage(TEXT("Settings"));
						return FReply::Handled();
					}))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				IconButton(Icon("Icons.Help"), LOCTEXT("HelpTooltip", "HyperAI Studio documentation."),
					FOnClicked::CreateLambda([]()
					{
						FPlatformProcess::LaunchURL(TEXT("https://hyperaistudio.com/docs"), nullptr, nullptr);
						return FReply::Handled();
					}))
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioShell::BuildSearch()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::Shell;
	return SAssignNew(SearchAnchor, SMenuAnchor)
		.Placement(MenuPlacement_BelowAnchor)
		.Method(EPopupMethod::UseCurrentWindow)
		.MenuContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Menu.Background"))
			.Padding(4.0f)
			[
				SNew(SBox)
				.WidthOverride(360.0f)
				.MaxDesiredHeight(380.0f)
				[
					SAssignNew(ResultList, SListView<TSharedPtr<FResult>>)
					.ListItemsSource(&Results)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SHyperAIStudioShell::GenerateResult)
					.OnMouseButtonClick_Lambda([this](TSharedPtr<FResult> Result) { RunResult(Result); })
				]
			])
		[
			SNew(SBox)
			.WidthOverride_Lambda([this]() { return Width < NarrowBelow ? 180.0f : 320.0f; })
			[
				SAssignNew(SearchBox, SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Search pages and actions  (Ctrl+K)"))
				.OnTextChanged_Lambda([this](const FText& Text) { Search(Text.ToString()); })
				.OnTextCommitted_Lambda([this](const FText&, const ETextCommit::Type Commit)
				{
					if (Commit == ETextCommit::OnEnter)
					{
						const TArray<TSharedPtr<FResult>> Selected = ResultList.IsValid() ? ResultList->GetSelectedItems() : TArray<TSharedPtr<FResult>>();
						RunResult(Selected.Num() > 0 ? Selected[0] : Results.Num() > 0 ? Results[0] : nullptr);
					}
				})
				.OnKeyDownHandler(this, &SHyperAIStudioShell::OnSearchKey)
			]
		];
}

void SHyperAIStudioShell::Search(const FString& Query)
{
	Results.Reset();
	const FString Needle = Query.TrimStartAndEnd();
	auto Matches = [&Needle](const FText& Label, const FText& Detail)
	{
		return Label.ToString().Contains(Needle) || Detail.ToString().Contains(Needle);
	};
	if (!Needle.IsEmpty())
	{
		for (const FHyperAIStudioPage& Page : FHyperAIStudioShell::GetPages())
		{
			if (!Page.bHidden && Matches(Page.Label, Page.Tooltip))
			{
				const FName Id = Page.Id;
				Results.Add(MakeShared<FResult>(FResult{Page.Label, Page.Tooltip, Page.Icon, [this, Id]() { OpenPage(Id); }}));
			}
		}
		for (const FHyperAIStudioAction& Action : FHyperAIStudioShell::GetActions())
		{
			if (Matches(Action.Label, Action.Description))
			{
				Results.Add(MakeShared<FResult>(FResult{Action.Label, Action.Description, Action.Icon, Action.Run}));
			}
		}
		// The prompt library, last: running one puts it in the chat page open now, or the first chat.
		const HyperAIStudio::PromptPresets::FLibrary& Library = HyperAIStudio::PromptPresets::Get(FHyperAIStudioService::GetProjectRoot());
		for (const int32 Index : HyperAIStudio::PromptPresets::Search(Library, Needle, FString(), nullptr, HyperAIStudio::Shell::MaxPresetResults))
		{
			const FHyperAIStudioPromptPreset& Preset = Library.Presets[Index];
			const FHyperAIStudioPromptCategory* Category = Library.FindCategory(Preset.Category);
			int32 Tab = 1;
			for (int32 Each = 1; Each <= SHyperAIStudioQuickActionWindow::MaxChatTabs; ++Each)
			{
				Tab = CurrentPage == SHyperAIStudioQuickActionWindow::GetPageId(Each) ? Each : Tab;
			}
			Results.Add(MakeShared<FResult>(FResult{Preset.DisplayTitle(), FText::Format(LOCTEXT("PresetResult", "Prompt: {0}"), Preset.DisplaySummary()),
				Category ? Category->Icon : FName(TEXT("HyperAIStudio.Icon.Chat")), [Tab, Preset]() { SHyperAIStudioQuickActionWindow::InsertPreset(Tab, Preset); }}));
		}
	}
	if (ResultList.IsValid())
	{
		ResultList->RequestListRefresh();
		if (Results.Num() > 0)
		{
			ResultList->SetSelection(Results[0]);
		}
	}
	if (SearchAnchor.IsValid())
	{
		SearchAnchor->SetIsOpen(Results.Num() > 0, /*bFocusMenu=*/false);
	}
}

TSharedRef<ITableRow> SHyperAIStudioShell::GenerateResult(TSharedPtr<FResult> Result, const TSharedRef<STableViewBase>& Owner)
{
	using namespace HyperAIStudio::UI;
	return SNew(STableRow<TSharedPtr<FResult>>, Owner)
		.Padding(FMargin(6.0f, 5.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
			[
				SNew(SImage)
				.Image(Icon(Result->Icon))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(Result->Label).TextStyle(&Style(), "HyperAIStudio.Text.Body")
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(Result->Detail).TextStyle(&Style(), "HyperAIStudio.Text.Caption").AutoWrapText(true)
				]
			]
		];
}

FReply SHyperAIStudioShell::OnSearchKey(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	if (KeyEvent.GetKey() == EKeys::Escape)
	{
		SearchAnchor->SetIsOpen(false);
		SearchBox->SetText(FText::GetEmpty());
		return FReply::Handled();
	}
	if ((KeyEvent.GetKey() == EKeys::Down || KeyEvent.GetKey() == EKeys::Up) && Results.Num() > 0)
	{
		const TArray<TSharedPtr<FResult>> Selected = ResultList->GetSelectedItems();
		int32 Index = Selected.Num() > 0 ? Results.IndexOfByKey(Selected[0]) : -1;
		Index = FMath::Clamp(Index + (KeyEvent.GetKey() == EKeys::Down ? 1 : -1), 0, Results.Num() - 1);
		ResultList->SetSelection(Results[Index]);
		ResultList->RequestScrollIntoView(Results[Index]);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void SHyperAIStudioShell::RunResult(const TSharedPtr<FResult>& Result)
{
	if (!Result.IsValid())
	{
		return;
	}
	SearchAnchor->SetIsOpen(false);
	SearchBox->SetText(FText::GetEmpty());
	if (Result->Run)
	{
		Result->Run();
	}
}

TSharedRef<SWidget> SHyperAIStudioShell::BuildRail()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::Shell;
	return SNew(SBox)
		.WidthOverride_Lambda([this]() { return IsRailCompact() ? CompactRail : ExpandedRail; })
		[
			SNew(SBorder)
			.BorderImage(Style().GetBrush("HyperAIStudio.Rail"))
			.Padding(FMargin(6.0f, 10.0f, 6.0f, 8.0f))
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(NavList, SVerticalBox)
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioShell::BuildNavItem(const FHyperAIStudioPage& Page)
{
	using namespace HyperAIStudio::UI;
	const FName Id = Page.Id;
	const TFunction<int32()> Badge = Page.Badge;
	auto Selected = [this, Id]() { return CurrentPage == Id; };
	auto BadgeCount = [Badge]() { return Badge ? Badge() : 0; };
	const FText Tip = Page.Tooltip.IsEmpty() ? Page.Label : FText::Format(LOCTEXT("NavTip", "{0}\n{1}"), Page.Label, Page.Tooltip);
	return SNew(SButton)
		.ButtonStyle(&Style(), "HyperAIStudio.NavItem")
		.ContentPadding(0.0f)
		.ToolTipText(Tip)
		.OnClicked_Lambda([this, Id]()
		{
			OpenPage(Id);
			return FReply::Handled();
		})
		[
			SNew(SBorder)
			.BorderImage_Lambda([this, Selected]() { return Selected() ? HyperAIStudio::UI::Style().GetBrush("HyperAIStudio.NavItem.Selected") : FStyleDefaults::GetNoBrush(); })
			.Padding(FMargin(0.0f, 6.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SBox)
					.WidthOverride(3.0f)
					[
						SNew(SImage)
						.Image(Style().GetBrush("HyperAIStudio.NavItem.Accent"))
						.Visibility_Lambda([Selected]() { return Selected() ? EVisibility::Visible : EVisibility::Hidden; })
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(9.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage)
						.Image(Icon(Page.Icon))
						.ColorAndOpacity_Lambda([Selected]() { return Selected() ? FStyleColors::ForegroundHover : FStyleColors::Foreground; })
					]
					// Folded, a count becomes a dot on the icon.
					+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(FMargin(0.0f, -3.0f, -4.0f, 0.0f))
					[
						SNew(SBox)
						.WidthOverride(7.0f)
						.HeightOverride(7.0f)
						.Visibility_Lambda([this, BadgeCount]() { return IsRailCompact() && BadgeCount() > 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
						[
							SNew(SImage).Image(Style().GetBrush("HyperAIStudio.Dot")).ColorAndOpacity(FStyleColors::AccentBlue)
						]
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(FMargin(10.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(Page.Label)
					.TextStyle(&Style(), "HyperAIStudio.Text.Nav")
					.Font_Lambda([Selected]() { return FCoreStyle::GetDefaultFontStyle(Selected() ? "Bold" : "Regular", 10); })
					.ColorAndOpacity_Lambda([Selected]() { return Selected() ? FStyleColors::ForegroundHover : FStyleColors::Foreground; })
					.Visibility_Lambda([this]() { return IsRailCompact() ? EVisibility::Collapsed : EVisibility::Visible; })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return IsRailCompact() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						Count(TAttribute<int32>::CreateLambda(BadgeCount))
					]
				]
			]
		];
}

void SHyperAIStudioShell::RebuildNav()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::Shell;
	if (!NavList.IsValid())
	{
		return;
	}
	NavList->ClearChildren();
	FName Group = NAME_None;
	bool bFirst = true;
	for (const FHyperAIStudioPage& Page : FHyperAIStudioShell::GetPages())
	{
		if (Page.bHidden)
		{
			continue;
		}
		if (Page.Group != Group)
		{
			Group = Page.Group;
			if (!bFirst)
			{
				// Expanded, each group has its name; folded, a thin rule between groups.
				NavList->AddSlot().AutoHeight().Padding(FMargin(10.0f, 14.0f, 4.0f, 5.0f))
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return IsRailCompact() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SectionLabel(GroupLabel(Group))
					]
				];
				NavList->AddSlot().AutoHeight().Padding(FMargin(8.0f, 8.0f))
				[
					SNew(SBox)
					.HeightOverride(1.0f)
					.Visibility_Lambda([this]() { return IsRailCompact() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(SImage).Image(Style().GetBrush("HyperAIStudio.Divider"))
					]
				];
			}
			bFirst = false;
		}
		NavList->AddSlot().AutoHeight().Padding(FMargin(0.0f, 1.0f))
		[
			BuildNavItem(Page)
		];
	}
}

void SHyperAIStudioShell::OnPagesChanged()
{
	for (const FHyperAIStudioPage& Page : FHyperAIStudioShell::GetPages())
	{
		RegisterSpawner(Page);
	}
	RebuildNav();
}

void SHyperAIStudioShell::OnActiveTabChanged(TSharedPtr<SDockTab> Previous, TSharedPtr<SDockTab> Active)
{
	if (Active.IsValid() && TabManager.IsValid() && Active->GetTabManagerPtr() == TabManager)
	{
		CurrentPage = Active->GetLayoutIdentifier().TabType;
	}
}

void SHyperAIStudioShell::PersistLayout()
{
	if (TabManager.IsValid())
	{
		TabManager->SavePersistentLayout();
	}
}

TSharedRef<SWidget> SHyperAIStudioShell::BuildStatusBar()
{
	using namespace HyperAIStudio::UI;
	return SNew(SBorder)
		.BorderImage(Style().GetBrush("HyperAIStudio.Bar"))
		.Padding(FMargin(12.0f, 4.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				Dot(TAttribute<FSlateColor>::CreateSP(this, &SHyperAIStudioShell::GetMcpColor))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
				.Text(TAttribute<FText>::CreateSP(this, &SHyperAIStudioShell::GetMcpText))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(16.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(4.0f, 0.0f))
				.Visibility_Lambda([this]() { return Approvals > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked_Lambda([this]()
				{
					OpenPage(TEXT("Activity"));
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					.ColorAndOpacity(FStyleColors::Warning)
					.Text_Lambda([this]() { return FText::Format(LOCTEXT("Approvals", "{0} {0}|plural(one=item,other=items) waiting for you"), Approvals); })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(16.0f, 0.0f, 0.0f, 0.0f))
			[
				// Auto-approval is never on out of sight: every Studio page shows it here, with Stop.
				SNew(SBorder)
				.Visibility_Lambda([]() { return FHyperAIStudioAutoApproval::IsActive() ? EVisibility::Visible : EVisibility::Collapsed; })
				.BorderImage(Style().GetBrush("HyperAIStudio.TerminalFrame"))
				.BorderBackgroundColor_Lambda([]() { return FSlateColor(FStyleColors::AccentOrange.GetSpecifiedColor().CopyWithNewOpacity(0.3f)); })
				.Padding(FMargin(4.0f, 0.0f))
				.ToolTipText_Lambda([]() { return FHyperAIStudioAutoApproval::DescribeBannerTooltip(); })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(FMargin(4.0f, 0.0f))
						.OnClicked_Lambda([this]()
						{
							OpenPage(TEXT("Activity"));
							return FReply::Handled();
						})
						[
							SNew(STextBlock)
							.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
							.Text_Lambda([]() { return FHyperAIStudioAutoApproval::DescribeBanner(); })
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ContentPadding(FMargin(4.0f, 0.0f))
						.ToolTipText(SHyperAIStudioAutoApprovalBanner::GetStopTooltip())
						.OnClicked_Lambda([]()
						{
							FHyperAIStudioAutoApproval::Stop(EHyperAIStudioAutoApprovalEnd::Stopped);
							return FReply::Handled();
						})
						[
							SNew(STextBlock)
							.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
							.ColorAndOpacity(FStyleColors::AccentOrange)
							.Text(LOCTEXT("AutoApprovalStop", "Stop"))
						]
					]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(16.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
				.Visibility_Lambda([this]() { return Running > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text_Lambda([this]() { return FText::Format(LOCTEXT("Running", "{0} {0}|plural(one=job,other=jobs) running"), Running); })
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
				.Text_Lambda([]()
				{
					const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
					return World ? FText::FromString(World->GetMapName()) : FText::GetEmpty();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(16.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
				.Text_Lambda([]()
				{
					const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
					return Plugin.IsValid() ? FText::Format(LOCTEXT("Version", "HyperAI Studio {0}"), FText::FromString(Plugin->GetDescriptor().VersionName))
						: LOCTEXT("VersionUnknown", "HyperAI Studio");
				})
			]
		];
}

#undef LOCTEXT_NAMESPACE
