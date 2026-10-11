// Games by Hyper 2026.

#include "SHyperAIStudioLoreSetupPanel.h"

#include "Async/Async.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioWidgets.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioLoreSetupPanel"

namespace HyperAIStudio::LoreSetupPanel
{
	using namespace HyperAIStudio::UI;

	TSharedRef<SWidget> Body(const TAttribute<FText>& Text, const FName TextStyle = TEXT("HyperAIStudio.Text.Body"))
	{
		return SNew(STextBlock).AutoWrapText(true).TextStyle(&Style(), TextStyle).Text(Text);
	}

	/** A command or path the user may want to copy: selectable, in the editor's monospace font. */
	TSharedRef<SWidget> Code(const TAttribute<FText>& Text)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(8.0f, 5.0f))
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("MonoFont"))
				.Text(Text)
			];
	}

	/** A labelled text box whose text lives in Value. */
	TSharedRef<SWidget> Field(const FText& Label, FString& Value, const FText& Hint, TFunction<void()> OnChanged, TAttribute<bool> Enabled)
	{
		FString* Target = &Value;
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
			[
				SNew(SBox).WidthOverride(150.0f)
				[
					SNew(STextBlock).TextStyle(&Style(), "HyperAIStudio.Text.Subdued").Text(Label)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SEditableTextBox)
				.Text_Lambda([Target]() { return FText::FromString(*Target); })
				.HintText(Hint)
				.IsEnabled(Enabled)
				.OnTextChanged_Lambda([Target, OnChanged](const FText& Text)
				{
					*Target = Text.ToString();
					if (OnChanged)
					{
						OnChanged();
					}
				})
			];
	}

	TSharedRef<SWidget> Check(const FText& Label, bool& Value, const FText& ToolTip, TFunction<void()> OnChanged, TAttribute<bool> Enabled)
	{
		bool* Target = &Value;
		return SNew(SCheckBox)
			.IsChecked_Lambda([Target]() { return *Target ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.IsEnabled(Enabled)
			.ToolTipText(ToolTip)
			.OnCheckStateChanged_Lambda([Target, OnChanged](const ECheckBoxState State)
			{
				*Target = State == ECheckBoxState::Checked;
				if (OnChanged)
				{
					OnChanged();
				}
			})
			[
				SNew(STextBlock).TextStyle(&Style(), "HyperAIStudio.Text.Body").Text(Label)
			];
	}

	FText ModeText(const FHyperAIStudioLoreWorkspace& Workspace)
	{
		switch (Workspace.Mode)
		{
		case EHyperAIStudioLoreMode::Local: return LOCTEXT("ModeLocal", "Local: the history stays in this computer's .lore folder; no server, no locks.");
		case EHyperAIStudioLoreMode::Remote: return FText::Format(LOCTEXT("ModeRemote", "Server: pushes to {0}, and agents lock files there before editing them."), FText::FromString(Workspace.RemoteUrl));
		case EHyperAIStudioLoreMode::Unknown: return LOCTEXT("ModeUnknown", "Unknown: .lore/config.toml could not be read, so HyperAI treats it as a server repository: agents lock files before editing them, and a submit pushes.");
		default: return LOCTEXT("ModeNone", "Not set up: this project is in no Lore workspace.");
		}
	}

	/** Why the lore command, a setup or a server address cannot be used, in the user's language; English when unknown. */
	FText ProblemText(const EHyperAIStudioLoreProblem Problem, const FHyperAIStudioLoreSetupOptions& Options, const FString& English)
	{
		const FText Folder = FText::FromString(FPaths::ConvertRelativePathToFull(Options.Folder));
		switch (Problem)
		{
		case EHyperAIStudioLoreProblem::CliNotFound: return LOCTEXT("ProblemCliNotFound", "The lore command is not on PATH, nor in %USERPROFILE%\\bin where Lore's installer puts it.");
		case EHyperAIStudioLoreProblem::CliCommandMissing: return LOCTEXT("ProblemCliCommandMissing", "The lore command set below names a file that does not exist.");
		case EHyperAIStudioLoreProblem::CliCommandNotLore: return LOCTEXT("ProblemCliCommandNotLore", "The lore command set below is not a file named lore.exe, so HyperAI does not run it.");
		case EHyperAIStudioLoreProblem::NotAFolder: return FText::Format(LOCTEXT("ProblemNotAFolder", "{0} is not a folder."), Folder);
		case EHyperAIStudioLoreProblem::AlreadyWorkspace: return FText::Format(LOCTEXT("ProblemAlreadyWorkspace", "{0} is a Lore workspace already."), Folder);
		case EHyperAIStudioLoreProblem::InsideWorkspace: return FText::Format(LOCTEXT("ProblemInsideWorkspace", "{0} is inside the Lore workspace {1} already; set up the workspace's own folder, or none."),
			Folder, FText::FromString(FHyperAIStudioLore::FindWorkspace(Options.Folder).Root));
		case EHyperAIStudioLoreProblem::BadName: return FText::Format(LOCTEXT("ProblemBadName", "\"{0}\" is not a repository name Lore accepts: letters, digits, '/', '-', '_' and '.', up to 100."),
			FText::FromString(Options.Name));
		case EHyperAIStudioLoreProblem::BadIdentity: return LOCTEXT("ProblemBadIdentity", "The identity is a name or email: no quotes or control characters, and not starting with '-'.");
		case EHyperAIStudioLoreProblem::ServerNoScheme: return LOCTEXT("ProblemServerNoScheme", "A Lore server is an address such as lore://127.0.0.1:41337 (scheme, host, and port when it is not the default).");
		case EHyperAIStudioLoreProblem::ServerBadScheme: return LOCTEXT("ProblemServerBadScheme", "The server address does not start with a URL scheme, such as lore://.");
		case EHyperAIStudioLoreProblem::ServerSpacesOrQuotes: return LOCTEXT("ProblemServerSpaces", "A server address has no spaces or quotes.");
		case EHyperAIStudioLoreProblem::ServerBadHost: return LOCTEXT("ProblemServerBadHost", "A server address names its host, and carries no user name or password.");
		default: return FText::FromString(English);
		}
	}

	/** The project's .gitignore, when there is one and the setup copies it. */
	FString GitIgnoreFor(const FHyperAIStudioLoreSetupOptions& Options)
	{
		FString Text;
		if (Options.bCopyGitIgnore)
		{
			FFileHelper::LoadFileToString(Text, *(FPaths::ConvertRelativePathToFull(Options.Folder) / TEXT(".gitignore")));
		}
		return Text;
	}

	FText StepTitle(const FHyperAIStudioLoreSetupStep& Step, const FHyperAIStudioLoreSetupOptions& Options, const bool bIgnoreExists)
	{
		const FText Server = FText::FromString(Options.ServerUrl.TrimStartAndEnd());
		switch (Step.Kind)
		{
		case EHyperAIStudioLoreStepKind::CheckServer: return FText::Format(LOCTEXT("StepCheckServer", "Check that the Lore server {0} answers"), Server);
		case EHyperAIStudioLoreStepKind::WriteIgnore: return bIgnoreExists
			? LOCTEXT("StepIgnoreAppend", "Add Unreal's build output and caches to .loreignore")
			: LOCTEXT("StepIgnoreWrite", "Write .loreignore for Unreal's build output and caches");
		case EHyperAIStudioLoreStepKind::Create: return Options.ServerUrl.TrimStartAndEnd().IsEmpty()
			? FText::Format(LOCTEXT("StepCreateLocal", "Create the local repository {0} in this folder"), FText::FromString(Options.Name))
			: FText::Format(LOCTEXT("StepCreateRemote", "Create the repository {0} on {1}, with this folder as its workspace"), FText::FromString(Options.Name), Server);
		case EHyperAIStudioLoreStepKind::Stage: return LOCTEXT("StepStage", "Stage every project file that is not ignored");
		case EHyperAIStudioLoreStepKind::Commit: return FText::Format(LOCTEXT("StepCommit", "Commit them as \"{0}\""), FText::FromString(Step.Arguments.Last()));
		case EHyperAIStudioLoreStepKind::Push: return FText::Format(LOCTEXT("StepPush", "Push the first revision to {0}"), Server);
		}
		return FText::FromString(Step.Title);
	}

	FText StepDetail(const FHyperAIStudioLoreSetupStep& Step, const FHyperAIStudioLoreSetupOptions& Options, const bool bIgnoreExists)
	{
		const FText Folder = FText::FromString(FPaths::ConvertRelativePathToFull(Options.Folder));
		const FText Remote = FText::FromString(FHyperAIStudioLore::RepositoryUrl(Options.ServerUrl, Options.Name));
		switch (Step.Kind)
		{
		case EHyperAIStudioLoreStepKind::CheckServer:
			return LOCTEXT("DetailCheckServer", "Reads the server's list of repositories. Changes nothing. Undo: nothing to undo.");
		case EHyperAIStudioLoreStepKind::WriteIgnore:
		{
			const FText Base = bIgnoreExists
				? LOCTEXT("DetailIgnoreAppend", "Appends lines, so Binaries, Intermediate, Saved, DerivedDataCache, plugin builds, IDE files, *.sln and HyperAI's per-machine .hyperai/runtime folder stay out of the repository. Undo: remove the lines under the HyperAI Studio headings.")
				: LOCTEXT("DetailIgnoreWrite", "Writes the file, so Binaries, Intermediate, Saved, DerivedDataCache, plugin builds, IDE files, *.sln and HyperAI's per-machine .hyperai/runtime folder stay out of the repository. Undo: delete .loreignore.");
			const FString Root = FPaths::ConvertRelativePathToFull(Options.Folder);
			FString Current;
			FFileHelper::LoadFileToString(Current, *(Root / TEXT(".loreignore")));
			const int32 GitRules = FHyperAIStudioLore::CountMissingGitRules(Current, GitIgnoreFor(Options));
			if (GitRules > 0)
			{
				return FText::Format(LOCTEXT("DetailIgnoreGitCopied", "{0} Lore does not read .gitignore, so the project's .gitignore rules that .loreignore lacks ({1}) are copied in too."),
					Base, FText::AsNumber(GitRules));
			}
			return !Options.bCopyGitIgnore && FPaths::FileExists(Root / TEXT(".gitignore"))
				? FText::Format(LOCTEXT("DetailIgnoreGitSkipped", "{0} Lore does not read .gitignore: whatever only the project's .gitignore keeps out will be committed."), Base)
				: Base;
		}
		case EHyperAIStudioLoreStepKind::Create:
			return Options.ServerUrl.TrimStartAndEnd().IsEmpty()
				? FText::Format(LOCTEXT("DetailCreateLocal", "Adds a .lore folder to {0}, where Lore keeps the history on this computer. No server is used; your files are not changed. Undo: delete the .lore folder; the repository and its history go, your files stay."), Folder)
				: FText::Format(LOCTEXT("DetailCreateRemote", "Registers the repository on the server and adds a .lore folder to {0}. Your files are not changed. Undo: delete the .lore folder, and run lore repository delete {1} to remove it from the server."), Folder, Remote);
		case EHyperAIStudioLoreStepKind::Stage:
			return LOCTEXT("DetailStage", "Walks the folder and stages what .loreignore leaves in; a large project takes a while. Undo: lore unstage .");
		case EHyperAIStudioLoreStepKind::Commit:
			return LOCTEXT("DetailCommit", "Records the first revision. Works without a network. Undo: the revision stays in the history; deleting .lore starts over.");
		case EHyperAIStudioLoreStepKind::Push:
			return FText::Format(LOCTEXT("DetailPush", "Uploads the project's files to the server, where the team can clone them. Undo: lore repository delete {0} removes it from the server."), Remote);
		}
		return FText::FromString(Step.Detail);
	}
}

void SHyperAIStudioLoreSetupPanel::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::LoreSetupPanel;
	Folder = FPaths::ConvertRelativePathToFull(InArgs._Folder.IsEmpty() ? FPaths::ProjectDir() : InArgs._Folder);
	Folder.RemoveFromEnd(TEXT("/"));
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	Name = FHyperAIStudioLore::RepositoryName(FApp::HasProjectName() ? FString(FApp::GetProjectName()) : FPaths::GetCleanFilename(Folder));
	ServerUrl = Settings->LoreServerUrl.TrimStartAndEnd();
	bUseServer = !ServerUrl.IsEmpty();
	Identity = Settings->LoreIdentity.TrimStartAndEnd();
	CommitMessage = TEXT("Add the Unreal project");
	CommandPath = FHyperAIStudioLore::GetCommandOverride();
	Refresh();

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(FMargin(0.0f, 0.0f, 12.0f, 14.0f))
		[
			PageHeader(LOCTEXT("Title", "Version control"),
				LOCTEXT("Subtitle", "Epic's Lore (lore.org) for this project: a local repository now, a server when you have one. Nothing here runs until you confirm it."),
				Button(LOCTEXT("Refresh", "Refresh"), FOnClicked::CreateLambda([this]() { Refresh(); return FReply::Handled(); }), nullptr, false,
					LOCTEXT("RefreshTip", "Look for the lore command, the workspace and sign-ins again.")))
		]
		+ SScrollBox::Slot().Padding(FMargin(0.0f, 0.0f, 12.0f, 12.0f)) [ BuildCliCard() ]
		+ SScrollBox::Slot().Padding(FMargin(0.0f, 0.0f, 12.0f, 12.0f)) [ BuildProjectCard() ]
		+ SScrollBox::Slot().Padding(FMargin(0.0f, 0.0f, 12.0f, 12.0f)) [ BuildSetupCard() ]
		+ SScrollBox::Slot().Padding(FMargin(0.0f, 0.0f, 12.0f, 12.0f)) [ BuildOnlineCard() ]
	];
	RebuildSteps();
}

void SHyperAIStudioLoreSetupPanel::Refresh()
{
	if (bRunning)
	{
		return;
	}
	Cli = FHyperAIStudioLore::DetectCli();
	Workspace = FHyperAIStudioLore::FindWorkspace(Folder);
	if (!Workspace.Root.IsEmpty())
	{
		// A finished setup's report gives way to the workspace it made.
		Result = FText::GetEmpty();
	}
	SignIns.Reset();
	if (Cli.bFound && (Workspace.Mode == EHyperAIStudioLoreMode::Remote || Workspace.Mode == EHyperAIStudioLoreMode::Unknown))
	{
		FString Text;
		FHyperAIStudioLore::ListSignIns(Cli.Path, SignIns, Text);
	}
	Replan();
}

FHyperAIStudioLoreSetupOptions SHyperAIStudioLoreSetupPanel::GetOptions() const
{
	FHyperAIStudioLoreSetupOptions Options;
	Options.Folder = Folder;
	Options.Name = Name.TrimStartAndEnd();
	Options.ServerUrl = bUseServer ? ServerUrl.TrimStartAndEnd() : FString();
	Options.Identity = Identity.TrimStartAndEnd();
	Options.bWriteIgnore = bWriteIgnore;
	Options.bCopyGitIgnore = bCopyGitIgnore;
	Options.bFirstCommit = bFirstCommit;
	Options.CommitMessage = CommitMessage;
	return Options;
}

void SHyperAIStudioLoreSetupPanel::Replan()
{
	if (bRunning)
	{
		return;
	}
	PlanError.Reset();
	PlanErrorText = FText::GetEmpty();
	const FHyperAIStudioLoreSetupOptions Options = GetOptions();
	EHyperAIStudioLoreProblem Problem = EHyperAIStudioLoreProblem::None;
	if (!FHyperAIStudioLore::PlanSetup(Options, Plan, PlanError, &Problem))
	{
		Plan.Reset();
		PlanErrorText = HyperAIStudio::LoreSetupPanel::ProblemText(Problem, Options, PlanError);
	}
	if (bUseServer && ServerUrl.TrimStartAndEnd().IsEmpty())
	{
		Plan.Reset();
		PlanErrorText = LOCTEXT("NoServer", "Enter the server's address, such as lore://127.0.0.1:41337, or keep the repository on this computer.");
		PlanError = PlanErrorText.ToString();
	}
	StepStates.Init(EStepState::Waiting, Plan.Num());
	StepOutputs.Init(FString(), Plan.Num());
	RebuildSteps();
}

TSharedRef<SWidget> SHyperAIStudioLoreSetupPanel::BuildCliCard()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::LoreSetupPanel;
	return Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				Dot(TAttribute<FSlateColor>::CreateLambda([this]() { return StatusColor(!Cli.bFound ? EStatus::Error : Cli.Version.IsEmpty() ? EStatus::Warning : EStatus::Good); }))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				Body(TAttribute<FText>::CreateLambda([this]()
				{
					if (!Cli.bFound)
					{
						return FText::Format(LOCTEXT("CliMissing", "The lore command was not found. {0} Install it with Epic's installer (PowerShell: irm https://raw.githubusercontent.com/EpicGames/lore/main/scripts/install.ps1 | iex), then press Refresh."),
							ProblemText(Cli.Problem, GetOptions(), Cli.Error));
					}
					return Cli.Version.IsEmpty()
						? FText::Format(LOCTEXT("CliOdd", "Found {0}, but lore --version did not report a version (exit code {1}): {2}"), FText::FromString(Cli.Path),
							FText::AsNumber(Cli.ExitCode), FText::FromString(Cli.Output))
						: FText::Format(LOCTEXT("CliFound", "lore {0} at {1}."), FText::FromString(Cli.Version), FText::FromString(Cli.Path));
				}))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
			[
				SNew(SBox).WidthOverride(150.0f)[ SNew(STextBlock).TextStyle(&Style(), "HyperAIStudio.Text.Subdued").Text(LOCTEXT("CommandLabel", "Lore command")) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SEditableTextBox)
				.Text_Lambda([this]() { return FText::FromString(CommandPath); })
				.HintText(LOCTEXT("CommandHint", "Empty: found on PATH or in %USERPROFILE%\\bin. Otherwise the full path of lore.exe."))
				.OnTextChanged_Lambda([this](const FText& Text) { CommandPath = Text.ToString(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				Button(LOCTEXT("UseCommand", "Use"), FOnClicked::CreateSP(this, &SHyperAIStudioLoreSetupPanel::OnUseCommand), nullptr, false,
					LOCTEXT("UseCommandTip", "Keep this path for this project. Only a file named lore.exe is accepted, and only here: agents' settings tools cannot choose which program HyperAI runs."))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			SNew(SBox).Visibility_Lambda([this]() { return CommandText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				Body(TAttribute<FText>::CreateLambda([this]() { return CommandText; }), TEXT("HyperAIStudio.Text.Caption"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
		[
			Body(LOCTEXT("CliNote", "Lore is pre-1.0 and changes quickly. HyperAI uses only its command line, and never signs in or reads its tokens."), TEXT("HyperAIStudio.Text.Caption"))
		],
		LOCTEXT("CliTitle", "The lore command"),
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
		[
			Button(LOCTEXT("InstallGuide", "Install guide"), FOnClicked::CreateLambda([]()
			{
				FPlatformProcess::LaunchURL(FHyperAIStudioLore::InstallGuideUrl, nullptr, nullptr);
				return FReply::Handled();
			}), nullptr, false, LOCTEXT("InstallGuideTip", "Open Epic's guide to installing the lore command."))
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			Button(LOCTEXT("LoreOrg", "lore.org"), FOnClicked::CreateLambda([]()
			{
				FPlatformProcess::LaunchURL(FHyperAIStudioLore::HomeUrl, nullptr, nullptr);
				return FReply::Handled();
			}), nullptr, false, LOCTEXT("LoreOrgTip", "Open lore.org, Epic's page for Lore."))
		]);
}

TSharedRef<SWidget> SHyperAIStudioLoreSetupPanel::BuildProjectCard()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::LoreSetupPanel;
	return Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			Fact(LOCTEXT("FactFolder", "Project folder"), FText::FromString(Folder))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			Fact(LOCTEXT("FactWorkspace", "Lore workspace"), TAttribute<FText>::CreateLambda([this]()
			{
				return Workspace.Root.IsEmpty() ? LOCTEXT("NoWorkspace", "None") : FText::FromString(Workspace.Root);
			}))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			Fact(LOCTEXT("FactMode", "Repository"), TAttribute<FText>::CreateLambda([this]() { return ModeText(Workspace); }))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
		[
			Fact(LOCTEXT("FactIdentity", "Commits recorded as"), TAttribute<FText>::CreateLambda([this]()
			{
				return Workspace.Identity.IsEmpty() ? LOCTEXT("NoIdentity", "Not set") : FText::FromString(Workspace.Identity);
			}))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
		[
			Body(TAttribute<FText>::CreateLambda([this]()
			{
				ISourceControlModule& Module = ISourceControlModule::Get();
				if (Module.IsEnabled() && Module.GetProvider().GetName() != FName(TEXT("None")))
				{
					return FText::Format(LOCTEXT("ProviderFirst", "Unreal's {0} provider is set up, so agent edits check out through it rather than through Lore."), FText::FromName(Module.GetProvider().GetName()));
				}
				switch (Workspace.Mode)
				{
				case EHyperAIStudioLoreMode::Local: return LOCTEXT("AgentsLocal", "Agent edits are marked dirty in Lore once saved; hyper_source_control_submit commits them here after you approve.");
				case EHyperAIStudioLoreMode::Remote:
				case EHyperAIStudioLoreMode::Unknown: return LOCTEXT("AgentsRemote", "Agent edits lock each file on the server first (a teammate's lock turns the edit away); hyper_source_control_submit commits and pushes after you approve.");
				default: return LOCTEXT("AgentsNone", "Until it is set up, agent edits are saved as they are, with no version control.");
				}
			}), TEXT("HyperAIStudio.Text.Caption"))
		],
		LOCTEXT("ProjectTitle", "This project"));
}

TSharedRef<SWidget> SHyperAIStudioLoreSetupPanel::BuildSetupCard()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::LoreSetupPanel;
	// Once the folder is a workspace (a setup just finished), the form stays to show what ran, but cannot run again.
	const TAttribute<bool> Editable = TAttribute<bool>::CreateLambda([this]() { return !bRunning && Workspace.Root.IsEmpty(); });
	auto Changed = [this]() { Replan(); };
	return SNew(SBox)
		.Visibility_Lambda([this]() { return Workspace.Root.IsEmpty() || bRunning || !Result.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
		[
			Card(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					Body(LOCTEXT("SetupIntro", "Makes this folder a Lore workspace. A local repository needs no server and suits working alone; a server repository can be shared, pushed and locked. Lore sets the server when the repository is made."))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
				[
					Field(LOCTEXT("NameLabel", "Repository name"), Name, LOCTEXT("NameHint", "Letters, digits, '-', '_', '.' and '/'"), Changed, Editable)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					Check(LOCTEXT("UseServer", "Make it on a Lore server instead of this computer only"), bUseServer,
						LOCTEXT("UseServerTip", "On: lore repository create <server>/<name>. Off: lore repository create --offline <name>, kept in this folder's .lore."), Changed, Editable)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					SNew(SBox).Visibility_Lambda([this]() { return bUseServer ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						Field(LOCTEXT("ServerLabel", "Server"), ServerUrl, LOCTEXT("ServerHint", "lore://127.0.0.1:41337"), Changed, Editable)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					Field(LOCTEXT("IdentityLabel", "Commits recorded as"), Identity, LOCTEXT("IdentityHint", "Optional: your name or email"), Changed, Editable)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					Check(LOCTEXT("WriteIgnore", "Keep Unreal's build output and caches out (.loreignore)"), bWriteIgnore,
						LOCTEXT("WriteIgnoreTip", "Binaries, Intermediate, Saved, DerivedDataCache, plugin builds, .vs, *.sln and other IDE files, and HyperAI's per-machine .hyperai/runtime folder. An existing .loreignore keeps its lines; missing ones are added."), Changed, Editable)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(20.0f, 6.0f, 0.0f, 0.0f))
				[
					SNew(SBox).Visibility_Lambda([this]() { return FPaths::FileExists(Folder / TEXT(".gitignore")) ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						Check(LOCTEXT("CopyGitIgnore", "Also keep out what the project's .gitignore ignores (Lore does not read .gitignore)"), bCopyGitIgnore,
							LOCTEXT("CopyGitIgnoreTip", "Copies the rules of .gitignore that .loreignore lacks, under a heading of their own after the Unreal ones. Off: files only .gitignore keeps out, such as per-user settings, are committed."),
							Changed, TAttribute<bool>::CreateLambda([this]() { return !bRunning && Workspace.Root.IsEmpty() && bWriteIgnore; }))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					Check(LOCTEXT("FirstCommit", "Commit the project as the first revision"), bFirstCommit,
						LOCTEXT("FirstCommitTip", "Stages every file the ignore rules leave in and commits it (and pushes it, on a server)."), Changed, Editable)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					SNew(SBox).Visibility_Lambda([this]() { return bFirstCommit ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						Field(LOCTEXT("MessageLabel", "Commit message"), CommitMessage, LOCTEXT("MessageHint", "Add the Unreal project"), Changed, Editable)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 12.0f, 0.0f, 4.0f))
				[
					SectionLabel(LOCTEXT("StepsLabel", "What setup will do"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(StepList, SVerticalBox)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
				[
					Body(TAttribute<FText>::CreateLambda([this]() { return Result; }))
				],
				LOCTEXT("SetupTitle", "Set up Lore for this project"),
				SNew(SBox)
				.IsEnabled(Editable)
				[
					Button(LOCTEXT("SetUp", "Set up..."), FOnClicked::CreateSP(this, &SHyperAIStudioLoreSetupPanel::OnSetUp), nullptr, true,
						LOCTEXT("SetUpTip", "Shows the steps once more and runs them after you confirm."))
				])
		];
}

TSharedRef<SWidget> SHyperAIStudioLoreSetupPanel::BuildOnlineCard()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::LoreSetupPanel;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TSharedRef<FString> Server = MakeShared<FString>(Settings->LoreServerUrl);
	const TSharedRef<FString> Who = MakeShared<FString>(Settings->LoreIdentity);
	// The workspace's server, else the Lore Server setting, else a placeholder for the user to fill.
	auto ServerForLogin = [this]()
	{
		const FString Server = Workspace.Mode == EHyperAIStudioLoreMode::Remote ? Workspace.RemoteUrl : GetDefault<UHyperAIStudioSettings>()->LoreServerUrl.TrimStartAndEnd();
		return Server.IsEmpty() ? FString(TEXT("<server>")) : Server;
	};
	return Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			Body(LOCTEXT("OnlineIntro", "For when a server is available: a loreserver you host (lore://host:41337), or Epic's hosted Lore once it opens. These settings are kept per user and project, never in the project's files."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
			[
				SNew(SBox).WidthOverride(150.0f)[ SNew(STextBlock).TextStyle(&Style(), "HyperAIStudio.Text.Subdued").Text(LOCTEXT("LoreServerLabel", "Lore Server")) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SEditableTextBox)
				.Text(FText::FromString(*Server))
				.HintText(LOCTEXT("LoreServerHint", "Empty: local only. lore://127.0.0.1:41337 for a loreserver on this computer."))
				.OnTextChanged_Lambda([Server](const FText& Text) { *Server = Text.ToString(); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
			[
				SNew(SBox).WidthOverride(150.0f)[ SNew(STextBlock).TextStyle(&Style(), "HyperAIStudio.Text.Subdued").Text(LOCTEXT("LoreIdentityLabel", "Lore Identity")) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SEditableTextBox)
				.Text(FText::FromString(*Who))
				.HintText(LOCTEXT("LoreIdentityHint", "Optional: recorded on commits in a repository set up here"))
				.OnTextChanged_Lambda([Who](const FText& Text) { *Who = Text.ToString(); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 8.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				Button(LOCTEXT("SaveServer", "Save"), FOnClicked::CreateLambda([this, Server, Who]()
				{
					FString Error;
					EHyperAIStudioLoreProblem Problem = EHyperAIStudioLoreProblem::None;
					const FString Url = Server->TrimStartAndEnd();
					if (!Url.IsEmpty() && !FHyperAIStudioLore::IsValidServerUrl(Url, Error, &Problem))
					{
						ServerCheck = ECheck::Invalid;
						ServerCheckText = ProblemText(Problem, GetOptions(), Error).ToString();
						return FReply::Handled();
					}
					UHyperAIStudioSettings* Mutable = GetMutableDefault<UHyperAIStudioSettings>();
					Mutable->LoreServerUrl = Url;
					Mutable->LoreIdentity = Who->TrimStartAndEnd();
					Mutable->SaveConfig();
					ServerUrl = Url;
					Identity = Mutable->LoreIdentity;
					bUseServer = !Url.IsEmpty();
					ServerCheck = ECheck::Unknown;
					ServerCheckText = LOCTEXT("Saved", "Saved.").ToString();
					Replan();
					return FReply::Handled();
				}), nullptr, false, LOCTEXT("SaveServerTip", "Keep the server and identity for this project (Preferences has them too)."))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				Button(LOCTEXT("CheckServer", "Check server"), FOnClicked::CreateSP(this, &SHyperAIStudioLoreSetupPanel::OnCheckServer), nullptr, false,
					LOCTEXT("CheckServerTip", "Asks the server for its repositories (lore repository list). Changes nothing."))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				Dot(TAttribute<FSlateColor>::CreateLambda([this]()
				{
					return StatusColor(ServerCheck == ECheck::Answered ? EStatus::Good : ServerCheck == ECheck::Failed || ServerCheck == ECheck::Invalid ? EStatus::Error
						: ServerCheck == ECheck::Checking ? EStatus::Busy : EStatus::Neutral);
				}))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				Body(TAttribute<FText>::CreateLambda([this]()
				{
					switch (ServerCheck)
					{
					case ECheck::Checking: return LOCTEXT("Checking", "Asking the server...");
					case ECheck::Answered: return FText::Format(LOCTEXT("ServerAnswered", "The server answered. {0}"), FText::FromString(ServerCheckText.Left(400)));
					case ECheck::Failed: return FText::Format(LOCTEXT("ServerFailed", "No answer: {0}"), FText::FromString(ServerCheckText.Left(600)));
					default: return ServerCheckText.IsEmpty() ? LOCTEXT("NotChecked", "Not checked yet.") : FText::FromString(ServerCheckText);
					}
				}), TEXT("HyperAIStudio.Text.Caption"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 12.0f, 0.0f, 4.0f))
		[
			SectionLabel(LOCTEXT("SignInLabel", "Sign-in"))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			Body(TAttribute<FText>::CreateLambda([this]()
			{
				if (Workspace.Mode == EHyperAIStudioLoreMode::Local)
				{
					return LOCTEXT("SignInLocal", "A local repository needs no sign-in.");
				}
				if (!SignIns.IsEmpty())
				{
					return FText::Format(LOCTEXT("SignedIn", "The lore command is signed in as: {0}"), FText::FromString(FString::Join(SignIns, TEXT("; "))));
				}
				return LOCTEXT("SignInHow", "A server with sign-in turned on (Epic's hosted Lore signs in with an Epic account) needs you to sign in once, yourself, in a terminal: run the command below. It opens the sign-in page in your browser. HyperAI only shows the command; it never signs in, and never reads or keeps tokens.");
			}))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([this]() { return Workspace.Mode == EHyperAIStudioLoreMode::Local ? EVisibility::Collapsed : EVisibility::Visible; })
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				Code(TAttribute<FText>::CreateLambda([ServerForLogin]() { return FText::FromString(FHyperAIStudioLore::LoginCommand(ServerForLogin())); }))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				Button(LOCTEXT("CopyLogin", "Copy"), FOnClicked::CreateLambda([ServerForLogin]()
				{
					FPlatformApplicationMisc::ClipboardCopy(*FHyperAIStudioLore::LoginCommand(ServerForLogin()));
					return FReply::Handled();
				}), nullptr, false, LOCTEXT("CopyLoginTip", "Copy the command, to paste in a terminal."))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
		[
			Body(TAttribute<FText>::CreateLambda([this]()
			{
				return Workspace.Mode == EHyperAIStudioLoreMode::Local
					? LOCTEXT("MoveLater", "This repository is local. Lore 0.10 sets the server when a repository is made, so moving to a server later means a new repository there (set it up from a copy of the project, or ask the server's owner to import it); this local history stays here.")
					: LOCTEXT("JoinLater", "To work on a project a teammate already put on a server: lore clone <server>/<name> <folder>, then open its .uproject.");
			}), TEXT("HyperAIStudio.Text.Caption"))
		],
		LOCTEXT("OnlineTitle", "Server and sign-in"));
}

void SHyperAIStudioLoreSetupPanel::RebuildSteps()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::LoreSetupPanel;
	if (!StepList.IsValid())
	{
		return;
	}
	StepList->ClearChildren();
	if (!Workspace.Root.IsEmpty() && !bRunning && Result.IsEmpty())
	{
		return;
	}
	if (Plan.IsEmpty())
	{
		StepList->AddSlot().AutoHeight()
		[
			Body(PlanErrorText.IsEmpty() ? LOCTEXT("NothingPlanned", "Nothing to do.") : PlanErrorText)
		];
		return;
	}
	const FHyperAIStudioLoreSetupOptions Options = GetOptions();
	const bool bIgnoreExists = FPaths::FileExists(Folder / TEXT(".loreignore"));
	for (int32 Index = 0; Index < Plan.Num(); ++Index)
	{
		const FHyperAIStudioLoreSetupStep& Step = Plan[Index];
		StepList->AddSlot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(FMargin(0.0f, 4.0f, 8.0f, 0.0f))
			[
				Dot(TAttribute<FSlateColor>::CreateLambda([this, Index]()
				{
					const EStepState State = StepStates.IsValidIndex(Index) ? StepStates[Index] : EStepState::Waiting;
					return StatusColor(State == EStepState::Done ? EStatus::Good : State == EStepState::Failed ? EStatus::Error : State == EStepState::Running ? EStatus::Busy : EStatus::Neutral);
				}))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					Body(FText::Format(LOCTEXT("StepLine", "{0}. {1}"), FText::AsNumber(Index + 1), StepTitle(Step, Options, bIgnoreExists)))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
				[
					Body(StepDetail(Step, Options, bIgnoreExists), TEXT("HyperAIStudio.Text.Caption"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
				[
					Code(FText::FromString(Step.Command))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
				[
					SNew(SBox).Visibility_Lambda([this, Index]() { return StepOutputs.IsValidIndex(Index) && !StepOutputs[Index].IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						Body(TAttribute<FText>::CreateLambda([this, Index]() { return StepOutputs.IsValidIndex(Index) ? FText::FromString(StepOutputs[Index].Right(1200)) : FText::GetEmpty(); }), TEXT("HyperAIStudio.Text.Caption"))
					]
				]
			]
		];
	}
}

FReply SHyperAIStudioLoreSetupPanel::OnSetUp()
{
	if (bRunning)
	{
		return FReply::Handled();
	}
	Result = FText::GetEmpty();
	Refresh();
	if (!Cli.bFound)
	{
		Result = LOCTEXT("NeedCli", "Install the lore command first (Install guide, above), then press Refresh.");
		return FReply::Handled();
	}
	if (Plan.IsEmpty())
	{
		Result = PlanErrorText;
		return FReply::Handled();
	}
	if (FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, GetConfirmQuestion(), LOCTEXT("ConfirmTitle", "Set up Lore")) != EAppReturnType::Yes)
	{
		Result = LOCTEXT("Cancelled", "Nothing was changed.");
		return FReply::Handled();
	}
	RunPlan();
	return FReply::Handled();
}

FText SHyperAIStudioLoreSetupPanel::GetConfirmQuestion() const
{
	const FHyperAIStudioLoreSetupOptions Options = GetOptions();
	const bool bIgnoreExists = FPaths::FileExists(Folder / TEXT(".loreignore"));
	FString List;
	for (int32 Index = 0; Index < Plan.Num(); ++Index)
	{
		List += FString::Printf(TEXT("%d. %s\n    %s\n"), Index + 1, *HyperAIStudio::LoreSetupPanel::StepTitle(Plan[Index], Options, bIgnoreExists).ToString(), *Plan[Index].Command);
	}
	const FString Server = Options.ServerUrl.TrimStartAndEnd();
	if (Server.IsEmpty())
	{
		return FText::Format(LOCTEXT("Confirm", "Set up Lore in {0}?\n\n{1}\nThis adds a .lore folder (and .loreignore) to the project folder; your project files are not changed. To undo it, delete the .lore folder.\n\nRun these steps now?"),
			FText::FromString(Folder), FText::FromString(List));
	}
	// On a server, deleting .lore undoes neither the repository there nor the files a first commit uploads to it.
	const FText Repository = FText::FromString(FHyperAIStudioLore::RepositoryUrl(Server, Options.Name));
	return Options.bFirstCommit
		? FText::Format(LOCTEXT("ConfirmServer", "Set up Lore in {0} on the server {2}?\n\n{1}\nThis creates the repository {3} on the server, uploads to it every project file the ignore rules leave in, and adds a .lore folder (and .loreignore) to the project folder. To undo it, run lore repository delete {3}, which removes the repository and its files from the server, and delete the .lore folder.\n\nRun these steps now?"),
			FText::FromString(Folder), FText::FromString(List), FText::FromString(Server), Repository)
		: FText::Format(LOCTEXT("ConfirmServerNoCommit", "Set up Lore in {0} on the server {2}?\n\n{1}\nThis creates the repository {3} on the server (no files are uploaded yet) and adds a .lore folder (and .loreignore) to the project folder. To undo it, run lore repository delete {3} and delete the .lore folder.\n\nRun these steps now?"),
			FText::FromString(Folder), FText::FromString(List), FText::FromString(Server), Repository);
}

FReply SHyperAIStudioLoreSetupPanel::OnUseCommand()
{
	EHyperAIStudioLoreProblem Problem = EHyperAIStudioLoreProblem::None;
	if (!FHyperAIStudioLore::SetCommandOverride(CommandPath, Problem))
	{
		CommandText = HyperAIStudio::LoreSetupPanel::ProblemText(Problem, GetOptions(), FString());
		return FReply::Handled();
	}
	CommandPath = FHyperAIStudioLore::GetCommandOverride();
	CommandText = CommandPath.IsEmpty()
		? LOCTEXT("CommandCleared", "HyperAI looks for lore on PATH and in %USERPROFILE%\\bin.")
		: FText::Format(LOCTEXT("CommandUsed", "HyperAI runs {0} for Lore."), FText::FromString(CommandPath));
	Refresh();
	return FReply::Handled();
}

void SHyperAIStudioLoreSetupPanel::CountSteps(int32& OutDone, int32& OutFailed) const
{
	OutDone = 0;
	OutFailed = 0;
	for (const EStepState State : StepStates)
	{
		OutDone += State == EStepState::Done ? 1 : 0;
		OutFailed += State == EStepState::Failed ? 1 : 0;
	}
}

void SHyperAIStudioLoreSetupPanel::RunPlan()
{
	if (bRunning || Plan.IsEmpty() || !Cli.bFound)
	{
		return;
	}
	const FHyperAIStudioLoreSetupOptions Options = GetOptions();
	bRunning = true;
	Result = FText::GetEmpty();
	StepStates.Init(EStepState::Waiting, Plan.Num());
	StepOutputs.Init(FString(), Plan.Num());
	RebuildSteps();
	const TWeakPtr<SHyperAIStudioLoreSetupPanel> Weak = StaticCastSharedRef<SHyperAIStudioLoreSetupPanel>(AsShared());
	const FString Executable = Cli.Path;
	const TArray<FHyperAIStudioLoreSetupStep> Steps = Plan;
	Async(EAsyncExecution::Thread, [Weak, Executable, Options, Steps]()
	{
		bool bOk = true;
		for (int32 Index = 0; Index < Steps.Num() && bOk; ++Index)
		{
			AsyncTask(ENamedThreads::GameThread, [Weak, Index]()
			{
				if (const TSharedPtr<SHyperAIStudioLoreSetupPanel> Panel = Weak.Pin())
				{
					Panel->StepStates[Index] = EStepState::Running;
				}
			});
			FString Text;
			bOk = FHyperAIStudioLore::RunStep(Executable, Options, Steps[Index], Text);
			AsyncTask(ENamedThreads::GameThread, [Weak, Index, bOk, Text]()
			{
				if (const TSharedPtr<SHyperAIStudioLoreSetupPanel> Panel = Weak.Pin())
				{
					Panel->OnStepFinished(Index, bOk, Text);
				}
			});
		}
		AsyncTask(ENamedThreads::GameThread, [Weak, bOk]()
		{
			if (const TSharedPtr<SHyperAIStudioLoreSetupPanel> Panel = Weak.Pin())
			{
				Panel->OnSetupFinished(bOk);
			}
		});
	});
}

void SHyperAIStudioLoreSetupPanel::OnStepFinished(const int32 Index, const bool bOk, const FString& Text)
{
	if (StepStates.IsValidIndex(Index))
	{
		StepStates[Index] = bOk ? EStepState::Done : EStepState::Failed;
		StepOutputs[Index] = Text;
		// The ignore step is HyperAI's own, so its report is in the user's language; the lore command's own output is shown as it is.
		if (Plan.IsValidIndex(Index) && Plan[Index].Kind == EHyperAIStudioLoreStepKind::WriteIgnore)
		{
			const FText File = FText::FromString(Folder / TEXT(".loreignore"));
			StepOutputs[Index] = (bOk ? FText::Format(LOCTEXT("IgnoreWritten", "{0} has the ignore rules."), File)
				: FText::Format(LOCTEXT("IgnoreNotWritten", "Could not write {0}."), File)).ToString();
		}
	}
}

void SHyperAIStudioLoreSetupPanel::OnSetupFinished(const bool bOk)
{
	bRunning = false;
	for (EStepState& State : StepStates)
	{
		State = State == EStepState::Waiting ? EStepState::Skipped : State;
	}
	Workspace = FHyperAIStudioLore::FindWorkspace(Folder);
	Result = bOk
		? FText::Format(LOCTEXT("SetupDone", "Done: {0} is now a Lore workspace. {1} Agent edits go through Lore from now on; hyper_source_control_status shows it."), FText::FromString(Folder),
			HyperAIStudio::LoreSetupPanel::ModeText(Workspace))
		: Workspace.Root.IsEmpty()
			? LOCTEXT("SetupFailedNoRepo", "A step failed (its output is above) and no repository was made. Fix what it says and set up again; a .loreignore written so far can stay.")
			: LOCTEXT("SetupFailedRepo", "A step failed (its output is above) after the repository was made. Finish by hand with the commands shown, or delete the .lore folder to start over.");
}

FReply SHyperAIStudioLoreSetupPanel::OnCheckServer()
{
	const FString Url = Workspace.Mode == EHyperAIStudioLoreMode::Remote ? Workspace.RemoteUrl
		: bUseServer && !ServerUrl.TrimStartAndEnd().IsEmpty() ? ServerUrl.TrimStartAndEnd() : GetDefault<UHyperAIStudioSettings>()->LoreServerUrl.TrimStartAndEnd();
	if (Url.IsEmpty())
	{
		ServerCheck = ECheck::Failed;
		ServerCheckText = LOCTEXT("NoServerToCheck", "No server is set: enter one in Lore Server and press Save.").ToString();
		return FReply::Handled();
	}
	FString Error;
	EHyperAIStudioLoreProblem Problem = EHyperAIStudioLoreProblem::None;
	if (!FHyperAIStudioLore::IsValidServerUrl(Url, Error, &Problem))
	{
		ServerCheck = ECheck::Invalid;
		ServerCheckText = HyperAIStudio::LoreSetupPanel::ProblemText(Problem, GetOptions(), Error).ToString();
		return FReply::Handled();
	}
	if (!Cli.bFound || ServerCheck == ECheck::Checking)
	{
		ServerCheck = Cli.bFound ? ServerCheck : ECheck::Failed;
		ServerCheckText = Cli.bFound ? ServerCheckText : LOCTEXT("CheckNeedsCli", "The lore command is needed to check a server.").ToString();
		return FReply::Handled();
	}
	ServerCheck = ECheck::Checking;
	const TWeakPtr<SHyperAIStudioLoreSetupPanel> Weak = StaticCastSharedRef<SHyperAIStudioLoreSetupPanel>(AsShared());
	const FString Executable = Cli.Path;
	Async(EAsyncExecution::Thread, [Weak, Executable, Url]()
	{
		FString Text;
		const bool bOk = FHyperAIStudioLore::CheckServer(Executable, Url, Text);
		AsyncTask(ENamedThreads::GameThread, [Weak, bOk, Text, Url]()
		{
			if (const TSharedPtr<SHyperAIStudioLoreSetupPanel> Panel = Weak.Pin())
			{
				Panel->ServerCheck = bOk ? ECheck::Answered : ECheck::Failed;
				Panel->ServerCheckText = !bOk ? Text : Text.IsEmpty()
					? FText::Format(LOCTEXT("ServerEmpty", "{0}: no repositories yet."), FText::FromString(Url)).ToString()
					: FString::Printf(TEXT("%s: %s"), *Url, *Text);
			}
		});
	});
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
