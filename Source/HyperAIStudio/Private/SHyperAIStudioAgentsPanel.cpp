// Games by Hyper 2026.

#include "SHyperAIStudioAgentsPanel.h"

#include "HyperAIStudioStyle.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioService.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Dialog/SCustomDialog.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioAgentsPanel"

namespace HyperAIStudio::AgentsPanel
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

	/** A warning badge for a generated file that is not HyperAIStudio's to update, or nothing. */
	TSharedPtr<SWidget> StateBadge(const FString& State, const FText& Cli)
	{
		if (State == TEXT("edited"))
		{
			return Badge(FText::Format(LOCTEXT("StateEdited", "{0} file edited"), Cli), FSlateColor(FStyleColors::AccentYellow),
				FText::Format(LOCTEXT("StateEditedTooltip", "Someone edited this subagent's {0} file directly, so HyperAIStudio no longer updates it. Edit the definition here and delete that file to regenerate it."), Cli));
		}
		if (State == TEXT("taken"))
		{
			return Badge(FText::Format(LOCTEXT("StateTaken", "name taken in {0}"), Cli), FSlateColor(FStyleColors::AccentYellow),
				FText::Format(LOCTEXT("StateTakenTooltip", "{0} already has a subagent of this name that HyperAIStudio did not write; {0} uses that one."), Cli));
		}
		return nullptr;
	}
}

void SHyperAIStudioAgentsPanel::Construct(const FArguments& InArgs)
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
			.Text(LOCTEXT("Title", "Subagents"))
			.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.PageTitle")
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("SubtitleTeam", "Each subagent is defined once in .hyperai/agents and written out for Claude Code (.claude/agents) and Codex (.codex/agents). A chat hands work to one by name, or the whole team takes a request: the Team switch in the chat, /hyperai-team in Claude Code, $team in Codex."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				Button(LOCTEXT("New", "New Subagent..."), LOCTEXT("NewTooltip", "Define a subagent for both Claude Code and Codex."), [this]()
				{
					OpenAgentDialog();
					Reload();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				Button(LOCTEXT("AddTheTeam", "Add the Team"),
					LOCTEXT("AddTheTeamTooltip", "Add HyperAI's team: a director, a programmer, a level designer, a lighting artist, a technical artist and a QA tester, each with its quality bar, and the workflow that runs them together. Nothing you already have is overwritten."),
					[this]()
				{
					const HyperAIStudio::Skills::FSyncReport Report = HyperAIStudio::Agents::InstallTeam(FHyperAIStudioService::GetProjectRoot(), /*bUpdateOnly=*/false);
					ShowNotes(LOCTEXT("AddTeamTitle", "Gameplay team"), Report.Notes, Report.Changed);
					Reload();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				Button(LOCTEXT("Sync", "Sync"), LOCTEXT("SyncTooltip", "Write every definition out for Claude Code and Codex again. Nothing edited there is overwritten."), [this]()
				{
					const HyperAIStudio::Skills::FSyncReport Report = HyperAIStudio::Agents::Sync(FHyperAIStudioService::GetProjectRoot());
					ShowNotes(LOCTEXT("SyncTitle", "Subagents synced"), Report.Notes, Report.Changed);
					Reload();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Visibility_Lambda([this]() { return Agents.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("EmptyTeam", "No subagents yet. Add the team, or define your own."))
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.Visibility_Lambda([this]() { return Agents.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				SAssignNew(List, SListView<FAgentPtr>)
				.ListItemsSource(&Agents)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SHyperAIStudioAgentsPanel::GenerateRow)
			]
		]
	];
	Reload();
}

void SHyperAIStudioAgentsPanel::Reload()
{
	Agents.Reset();
	for (FHyperAIStudioAgentDefinition& Agent : HyperAIStudio::Agents::List(FHyperAIStudioService::GetProjectRoot()))
	{
		Agents.Add(MakeShared<FHyperAIStudioAgentDefinition>(MoveTemp(Agent)));
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

void SHyperAIStudioAgentsPanel::ShowNotes(const FText& Title, const TArray<FString>& Notes, const int32 Changed)
{
	FString Text = FText::Format(LOCTEXT("Changed", "{0} {0}|plural(one=file,other=files) written."), Changed).ToString();
	if (!Notes.IsEmpty())
	{
		Text += TEXT("\n\n") + FString::Join(Notes, TEXT("\n"));
	}
	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Text), Title);
}

TSharedRef<ITableRow> SHyperAIStudioAgentsPanel::GenerateRow(FAgentPtr Agent, const TSharedRef<STableViewBase>& OwnerTable)
{
	using namespace HyperAIStudio::AgentsPanel;
	const FHyperAIStudioAgentDefinition Copy = *Agent;
	const bool bProject = Copy.Scope == TEXT("project");

	TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Copy.Name))
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
		];
	auto AddBadge = [&Line](const TSharedRef<SWidget>& Widget)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)[ Widget ];
	};
	AddBadge(Badge(bProject ? LOCTEXT("ScopeProject", "Claude Code + Codex") : LOCTEXT("ScopeClaude", "Claude Code only"), FSlateColor::UseSubduedForeground()));
	if (bProject && Copy.Tools == TEXT("read"))
	{
		AddBadge(Badge(LOCTEXT("ReadOnly", "read only"), FSlateColor::UseSubduedForeground(),
			LOCTEXT("ReadOnlyTooltip", "It can read files and call Unreal tools, but not write files or run commands.")));
	}
	if (Copy.bBuiltIn)
	{
		AddBadge(Badge(LOCTEXT("BuiltInTeam", "team"), FSlateColor(FStyleColors::AccentBlue),
			LOCTEXT("BuiltInTeamTooltip", "Installed with HyperAI's team and updated with the plugin until you edit it.")));
	}
	if (const TSharedPtr<SWidget> Claude = StateBadge(Copy.ClaudeState, LOCTEXT("Claude", "Claude Code")))
	{
		AddBadge(Claude.ToSharedRef());
	}
	if (const TSharedPtr<SWidget> Codex = StateBadge(Copy.CodexState, LOCTEXT("Codex", "Codex")))
	{
		AddBadge(Codex.ToSharedRef());
	}
	auto AddButton = [&Line](const FText& Label, const FText& ToolTip, TFunction<void()> OnClick)
	{
		Line->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(Label)
			.ToolTipText(ToolTip)
			.OnClicked_Lambda([OnClick = MoveTemp(OnClick)]()
			{
				OnClick();
				return FReply::Handled();
			})
		];
	};
	if (bProject)
	{
		AddButton(LOCTEXT("Edit", "Edit"), LOCTEXT("EditTooltip", "Change this subagent for both Claude Code and Codex."), [this, Copy]()
		{
			OpenAgentDialog(&Copy);
			Reload();
		});
	}
	else
	{
		AddButton(LOCTEXT("Promote", "Share with Codex"), LOCTEXT("PromoteTooltip", "Make this Claude Code subagent a project definition, so Codex gets it too."), [this, Name = Copy.Name]()
		{
			FString Error;
			if (!HyperAIStudio::Agents::Promote(FHyperAIStudioService::GetProjectRoot(), Name, Error))
			{
				FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
			}
			Reload();
		});
	}
	AddButton(LOCTEXT("Open", "Open"), LOCTEXT("OpenTooltip", "Show its file."), [File = Copy.File]()
	{
		FPlatformProcess::ExploreFolder(*File);
	});
	if (bProject)
	{
		AddButton(LOCTEXT("Delete", "Delete"), FText::GetEmpty(), [this, Name = Copy.Name]()
		{
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::Format(LOCTEXT("DeleteConfirm", "Delete the subagent {0}, for both Claude Code and Codex?"),
				FText::FromString(Name))) == EAppReturnType::Yes)
			{
				FString Error;
				if (!HyperAIStudio::Agents::Delete(FHyperAIStudioService::GetProjectRoot(), Name, Error))
				{
					FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
				}
				Reload();
			}
		});
	}

	return SNew(STableRow<FAgentPtr>, OwnerTable)
		.Padding(FMargin(6.0f, 5.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Line ]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Copy.Description))
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

namespace HyperAIStudio::AgentsPanel
{
	/** Shows the form once. True when a save failed and the form should open again with what was typed. */
	bool EditOnce(FHyperAIStudioAgentDefinition& Draft, const FString& PreviousName)
	{
		auto Box = [](const FString& Value, const FText& Hint)
		{
			return SNew(SEditableTextBox).Text(FText::FromString(Value)).HintText(Hint);
		};
		const TSharedRef<SEditableTextBox> NameBox = Box(Draft.Name, LOCTEXT("NameHint", "lowercase-with-hyphens, such as quest-designer"));
		const TSharedRef<SEditableTextBox> DescriptionBox = Box(Draft.Description, LOCTEXT("DescriptionHint", "What it does and when to use it; agents pick it by this"));
		const TSharedRef<SEditableTextBox> ClaudeModelBox = Box(Draft.ModelClaude, LOCTEXT("ClaudeModelHint", "Empty keeps the chat's model; or sonnet, opus, haiku"));
		const TSharedRef<SEditableTextBox> CodexModelBox = Box(Draft.ModelCodex, LOCTEXT("CodexModelHint", "Empty keeps the chat's model"));
		const TSharedRef<SEditableTextBox> SkillsBox = Box(FString::Join(Draft.Skills, TEXT(", ")), LOCTEXT("SkillsHint", "Skills it should use, such as unreal-blueprint, hyperai-memory"));
		const TSharedRef<SMultiLineEditableTextBox> InstructionsBox = SNew(SMultiLineEditableTextBox)
			.Text(FText::FromString(Draft.Instructions))
			.HintText(LOCTEXT("InstructionsHint", "Its instructions: what it is for, how to work in this project, and what to report back."))
			.AutoWrapText(true);
		const TSharedRef<bool> bReadOnly = MakeShared<bool>(Draft.Tools == TEXT("read"));

		auto Label = [](const FText& Text) { return SNew(STextBlock).Text(Text); };
		const TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
			.Title(PreviousName.IsEmpty() ? LOCTEXT("NewTitle", "New Subagent") : LOCTEXT("EditTitle", "Edit Subagent"))
			.Content()
			[
				SNew(SBox)
				.WidthOverride(620.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ Label(LOCTEXT("NameLabel", "Name")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)[ NameBox ]
					+ SVerticalBox::Slot().AutoHeight()[ Label(LOCTEXT("DescriptionLabel", "Description")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)[ DescriptionBox ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([bReadOnly]() { return *bReadOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([bReadOnly](const ECheckBoxState State) { *bReadOnly = State == ECheckBoxState::Checked; })
						.ToolTipText(LOCTEXT("ReadOnlyBoxTooltip", "It can still call Unreal tools; this stops it writing files and running commands."))
						[
							Label(LOCTEXT("ReadOnlyLabel", "Read only: it inspects and reports, and never writes files"))
						]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()[ Label(LOCTEXT("ClaudeModelLabel", "Claude Code model")) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)[ ClaudeModelBox ]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()[ Label(LOCTEXT("CodexModelLabel", "Codex model")) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)[ CodexModelBox ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight()[ Label(LOCTEXT("SkillsLabel", "Skills")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)[ SkillsBox ]
					+ SVerticalBox::Slot().AutoHeight()[ Label(LOCTEXT("InstructionsLabel", "Instructions")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
					[
						SNew(SBox).HeightOverride(240.0f)[ InstructionsBox ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(LOCTEXT("SharedNote", "Saved in .hyperai/agents and written out for both agents; Git shares it with your team."))
					]
				]
			]
			.Buttons({SCustomDialog::FButton(LOCTEXT("Save", "Save")), SCustomDialog::FButton(LOCTEXT("Cancel", "Cancel"))});
		if (Dialog->ShowModal() != 0)
		{
			return false;
		}
		Draft.Name = NameBox->GetText().ToString().TrimStartAndEnd();
		Draft.Description = DescriptionBox->GetText().ToString().TrimStartAndEnd();
		Draft.Tools = *bReadOnly ? TEXT("read") : TEXT("edit");
		Draft.ModelClaude = ClaudeModelBox->GetText().ToString().TrimStartAndEnd();
		Draft.ModelCodex = CodexModelBox->GetText().ToString().TrimStartAndEnd();
		Draft.Skills.Reset();
		SkillsBox->GetText().ToString().ParseIntoArray(Draft.Skills, TEXT(","), /*InCullEmpty=*/true);
		for (FString& Skill : Draft.Skills)
		{
			Skill.TrimStartAndEndInline();
		}
		Draft.Instructions = InstructionsBox->GetText().ToString();
		FString Error;
		if (HyperAIStudio::Agents::Save(FHyperAIStudioService::GetProjectRoot(), Draft, PreviousName, Error))
		{
			return false;
		}
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return true;
	}
}

void SHyperAIStudioAgentsPanel::OpenAgentDialog(const FHyperAIStudioAgentDefinition* Existing)
{
	FHyperAIStudioAgentDefinition Draft = Existing ? *Existing : FHyperAIStudioAgentDefinition();
	const FString PreviousName = Existing ? Existing->Name : FString();
	while (HyperAIStudio::AgentsPanel::EditOnce(Draft, PreviousName))
	{
	}
}

#undef LOCTEXT_NAMESPACE
