// Games by Hyper 2026.

#include "SHyperAIStudioSkillsPanel.h"

#include "HyperAIStudioStyle.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioService.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Dialog/SCustomDialog.h"
#include "SHyperAIStudioModesPanel.h"
#include "Widgets/Input/SEditableTextBox.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioSkillsPanel"

namespace HyperAIStudio::SkillsPanel
{
	TSharedRef<SWidget> Badge(const FText& Label, const FSlateColor& Color, const FText& ToolTip = FText::GetEmpty())
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(5.0f, 1.0f))
			.ToolTipText(ToolTip)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(Color)
			];
	}

	FText ScopeLabel(const FString& Scope)
	{
		if (Scope == TEXT("project")) return LOCTEXT("ScopeProject", "project");
		if (Scope == TEXT("claude")) return LOCTEXT("ScopeClaude", "Claude Code only");
		if (Scope == TEXT("personal-claude")) return LOCTEXT("ScopePersonalClaude", "personal, Claude Code");
		return LOCTEXT("ScopePersonalAgents", "personal, Codex");
	}

	const void* ParentWindow()
	{
		return FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	}
}

void SHyperAIStudioSkillsPanel::Construct(const FArguments& InArgs)
{
	auto Button = [](const FText& Label, const FText& ToolTip, TFunction<void()> OnClick)
	{
		return SNew(SButton)
			.Text(Label)
			.ToolTipText(ToolTip)
			.OnClicked_Lambda([OnClick = MoveTemp(OnClick)]()
			{
				OnClick();
				return FReply::Handled();
			});
	};

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Title", "Skills"))
			.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.PageTitle")
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("Subtitle", "Project skills live once in .agents/skills, where Codex reads them, and are copied to .claude/skills for Claude Code. Agents pick them by their description, or run them from the chat's Skills menu."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				Button(LOCTEXT("Sync", "Sync"), LOCTEXT("SyncTooltip", "Install or update the built-in skills and refresh Claude Code's copies. Nothing you edited is overwritten."), [this]()
				{
					const FString Root = FHyperAIStudioService::GetProjectRoot();
					HyperAIStudio::Skills::FSyncReport Report = HyperAIStudio::Skills::InstallBuiltIns(Root, HyperAIStudio::Skills::BuiltInDirectory());
					const HyperAIStudio::Skills::FSyncReport Mirror = HyperAIStudio::Skills::SyncClaudeMirror(Root);
					Report.Notes.Append(Mirror.Notes);
					ShowNotes(LOCTEXT("SyncTitle", "Skills synced"), Report.Notes, Report.Changed + Mirror.Changed);
					Reload();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				Button(LOCTEXT("NewSkill", "New Skill..."), LOCTEXT("NewSkillTooltip", "Make a project skill in .agents/skills, for Claude Code and Codex, and open its SKILL.md to write."), [this]()
				{
					const TSharedRef<SEditableTextBox> NameBox = SNew(SEditableTextBox).HintText(LOCTEXT("NewNameHint", "lowercase-with-hyphens, such as door-setup"));
					const TSharedRef<SEditableTextBox> DescriptionBox = SNew(SEditableTextBox).HintText(LOCTEXT("NewDescriptionHint", "What it does and when to use it; agents pick it by this"));
					const TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
						.Title(LOCTEXT("NewSkillTitle", "New Skill"))
						.Content()
						[
							SNew(SBox).WidthOverride(520.0f)
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(LOCTEXT("NewNameLabel", "Name")) ]
								+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)[ NameBox ]
								+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(LOCTEXT("NewDescriptionLabel", "Description")) ]
								+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)[ DescriptionBox ]
							]
						]
						.Buttons({SCustomDialog::FButton(LOCTEXT("Create", "Create")), SCustomDialog::FButton(LOCTEXT("Cancel", "Cancel"))});
					if (Dialog->ShowModal() != 0)
					{
						return;
					}
					const FString Name = NameBox->GetText().ToString().TrimStartAndEnd();
					FString Error;
					if (!HyperAIStudio::Skills::Create(FHyperAIStudioService::GetProjectRoot(), Name, DescriptionBox->GetText().ToString(), Error))
					{
						FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
						return;
					}
					Reload();
					FPlatformProcess::LaunchFileInDefaultExternalApplication(*FPaths::Combine(HyperAIStudio::Skills::CanonicalDirectory(FHyperAIStudioService::GetProjectRoot()), Name, TEXT("SKILL.md")));
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				Button(LOCTEXT("ImportFolder", "Import Folder..."), LOCTEXT("ImportFolderTooltip", "Copy a skill folder (with a SKILL.md) into this project."), [this]()
				{
					FString Folder;
					if (IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
						Desktop && Desktop->OpenDirectoryDialog(HyperAIStudio::SkillsPanel::ParentWindow(), LOCTEXT("PickFolder", "Skill folder").ToString(), FString(), Folder))
					{
						ImportFrom(Folder);
					}
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				Button(LOCTEXT("ImportZip", "Import Zip..."), LOCTEXT("ImportZipTooltip", "Copy a zipped skill into this project."), [this]()
				{
					TArray<FString> Files;
					if (IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
						Desktop && Desktop->OpenFileDialog(HyperAIStudio::SkillsPanel::ParentWindow(), LOCTEXT("PickZip", "Zipped skill").ToString(),
							FString(), FString(), TEXT("Zip archives (*.zip)|*.zip"), EFileDialogFlags::None, Files) && Files.Num() == 1)
					{
						ImportFrom(Files[0]);
					}
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				Button(LOCTEXT("ChatModes", "Chat Modes..."), LOCTEXT("ChatModesTooltip", "The modes a chat can run in: Plan, Build, Review, Art pass, QA and the project's own."), []()
				{
					SHyperAIStudioModesPanel::OpenWindow();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				Button(LOCTEXT("ImportEpic", "Import Epic Skills"), LOCTEXT("ImportEpicTooltip", "Copy Epic's editor AgentSkills (their description and instructions) into this project as SKILL.md skills."), [this]()
				{
					TArray<FString> Notes;
					const int32 Written = HyperAIStudio::Skills::ImportEpicSkills(FHyperAIStudioService::GetProjectRoot(), Notes);
					HyperAIStudio::Skills::SyncClaudeMirror(FHyperAIStudioService::GetProjectRoot());
					ShowNotes(LOCTEXT("EpicTitle", "Epic skills imported"), Notes, Written);
					Reload();
				})
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			[
				SAssignNew(List, SListView<FSkillPtr>)
				.ListItemsSource(&Skills)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SHyperAIStudioSkillsPanel::GenerateRow)
			]
		]
	];
	Reload();
}

void SHyperAIStudioSkillsPanel::Reload()
{
	Skills.Reset();
	for (FHyperAIStudioSkill& Skill : HyperAIStudio::Skills::ListProjectSkills(FHyperAIStudioService::GetProjectRoot(), /*bIncludeInvalid=*/true))
	{
		Skills.Add(MakeShared<FHyperAIStudioSkill>(MoveTemp(Skill)));
	}
	for (FHyperAIStudioSkill& Skill : HyperAIStudio::Skills::ListPersonalSkills())
	{
		Skills.Add(MakeShared<FHyperAIStudioSkill>(MoveTemp(Skill)));
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

void SHyperAIStudioSkillsPanel::ShowNotes(const FText& Title, const TArray<FString>& Notes, const int32 Changed)
{
	FString Text = FText::Format(LOCTEXT("Changed", "{0} {0}|plural(one=skill,other=skills) changed."), Changed).ToString();
	if (!Notes.IsEmpty())
	{
		Text += TEXT("\n\n") + FString::Join(Notes, TEXT("\n"));
	}
	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Text), Title);
}

void SHyperAIStudioSkillsPanel::ImportFrom(const FString& Source)
{
	FString Error;
	const FString Name = HyperAIStudio::Skills::Import(FHyperAIStudioService::GetProjectRoot(), Source, Error);
	if (Name.IsEmpty())
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return;
	}
	HyperAIStudio::Skills::SyncClaudeMirror(FHyperAIStudioService::GetProjectRoot());
	Reload();
}

TSharedRef<SWidget> SHyperAIStudioSkillsPanel::BuildExportMenu(FString Folder)
{
	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	auto AddTarget = [&Menu, Folder](const FText& Label, const FString& Destination)
	{
		Menu.AddMenuEntry(Label, FText::FromString(Destination), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([Folder, Destination]()
		{
			FString Error;
			FMessageDialog::Open(EAppMsgType::Ok, HyperAIStudio::Skills::Export(Folder, Destination, Error)
				? FText::Format(LOCTEXT("Exported", "Copied to {0}."), FText::FromString(Destination))
				: FText::FromString(Error));
		})));
	};
	AddTarget(LOCTEXT("ExportClaude", "To my Claude Code skills"), HyperAIStudio::Skills::PersonalClaudeDirectory());
	AddTarget(LOCTEXT("ExportCodex", "To my Codex skills"), HyperAIStudio::Skills::PersonalAgentsDirectories()[0]);
	return Menu.MakeWidget();
}

TSharedRef<ITableRow> SHyperAIStudioSkillsPanel::GenerateRow(FSkillPtr Skill, const TSharedRef<STableViewBase>& OwnerTable)
{
	using namespace HyperAIStudio::SkillsPanel;
	const FHyperAIStudioSkill Copy = *Skill;
	const bool bProject = Copy.Scope == TEXT("project");
	const bool bPersonal = Copy.Scope.StartsWith(TEXT("personal"));

	TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Copy.Name))
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			Badge(ScopeLabel(Copy.Scope), FSlateColor::UseSubduedForeground())
		];
	if (Copy.bBuiltIn)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			Badge(LOCTEXT("BuiltIn", "built in"), FSlateColor(FStyleColors::AccentBlue), LOCTEXT("BuiltInTooltip", "Installed by HyperAIStudio and updated with the plugin until you edit it."))
		];
	}
	if (!Copy.Problem.IsEmpty())
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			Badge(LOCTEXT("Invalid", "not valid"), FSlateColor(FStyleColors::Error), FText::FromString(Copy.Problem))
		];
	}
	if (Copy.MirrorState == TEXT("edited") || Copy.MirrorState == TEXT("taken"))
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			Badge(Copy.MirrorState == TEXT("edited") ? LOCTEXT("MirrorEdited", "Claude copy edited") : LOCTEXT("MirrorTaken", "name taken in Claude Code"),
				FSlateColor(FStyleColors::AccentYellow),
				Copy.MirrorState == TEXT("edited")
					? LOCTEXT("MirrorEditedTooltip", "Someone edited this skill in .claude/skills. Promote keeps those edits for both agents.")
					: LOCTEXT("MirrorTakenTooltip", "A Claude Code skill of this name exists that HyperAIStudio did not write; Claude Code uses that one."))
		];
	}
	if (!bPersonal)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SCheckBox)
			.IsChecked(Copy.bMenuOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
			.ToolTipText(LOCTEXT("MenuOnlyTooltip", "Only you start it, from the menu or by typing its command; agents never pick it on their own."))
			.OnCheckStateChanged_Lambda([this, Folder = Copy.Folder](const ECheckBoxState State)
			{
				FString Error;
				if (!HyperAIStudio::Skills::SetMenuOnly(Folder, State == ECheckBoxState::Checked, Error))
				{
					FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
				}
				HyperAIStudio::Skills::SyncClaudeMirror(FHyperAIStudioService::GetProjectRoot());
				Reload();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("MenuOnly", "Menu only"))
			]
		];
	}
	if (Copy.Scope == TEXT("claude") || Copy.MirrorState == TEXT("edited"))
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Promote", "Promote"))
			.ToolTipText(LOCTEXT("PromoteTooltip", "Make the Claude Code version the project's skill, so Codex gets it too."))
			.OnClicked_Lambda([this, Name = Copy.Name]()
			{
				FString Error;
				if (!HyperAIStudio::Skills::Promote(FHyperAIStudioService::GetProjectRoot(), Name, Error))
				{
					FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
				}
				Reload();
				return FReply::Handled();
			})
		];
	}
	if (bProject)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SComboButton)
			.OnGetMenuContent(this, &SHyperAIStudioSkillsPanel::BuildExportMenu, Copy.Folder)
			.ButtonContent()[ SNew(STextBlock).Text(LOCTEXT("Export", "Export")) ]
		];
	}
	if (bPersonal)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("AddToProject", "Add to Project"))
			.ToolTipText(LOCTEXT("AddToProjectTooltip", "Copy this personal skill into the project, for both agents and your team."))
			.OnClicked_Lambda([this, Folder = Copy.Folder]()
			{
				ImportFrom(Folder);
				return FReply::Handled();
			})
		];
	}
	Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
	[
		SNew(SButton)
		.Text(LOCTEXT("EditSkill", "Edit"))
		.ToolTipText(LOCTEXT("EditSkillTooltip", "Open SKILL.md in your editor. A project skill's change reaches Claude Code's copy at the next Sync."))
		.OnClicked_Lambda([Folder = Copy.Folder]()
		{
			FPlatformProcess::LaunchFileInDefaultExternalApplication(*FPaths::Combine(Folder, TEXT("SKILL.md")));
			return FReply::Handled();
		})
	];
	Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
	[
		SNew(SButton)
		.Text(LOCTEXT("OpenFolder", "Open"))
		.ToolTipText(LOCTEXT("OpenFolderTooltip", "Show the skill's folder."))
		.OnClicked_Lambda([Folder = Copy.Folder]()
		{
			FPlatformProcess::ExploreFolder(*Folder);
			return FReply::Handled();
		})
	];
	if (bProject)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Delete", "Delete"))
			.OnClicked_Lambda([this, Name = Copy.Name]()
			{
				if (FMessageDialog::Open(EAppMsgType::YesNo, FText::Format(LOCTEXT("DeleteConfirm", "Delete the skill {0} from this project, for both agents?"),
					FText::FromString(Name))) == EAppReturnType::Yes)
				{
					FString Error;
					if (!HyperAIStudio::Skills::Delete(FHyperAIStudioService::GetProjectRoot(), Name, Error))
					{
						FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
					}
					Reload();
				}
				return FReply::Handled();
			})
		];
	}

	return SNew(STableRow<FSkillPtr>, OwnerTable)
		.Padding(FMargin(6.0f, 5.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Line ]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Copy.Problem.IsEmpty() ? Copy.Description : Copy.Problem))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

#undef LOCTEXT_NAMESPACE
