// Games by Hyper 2026.

#include "SHyperAIStudioMemoryPanel.h"

#include "HyperAIStudioStyle.h"
#include "Dialog/SCustomDialog.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioMemoryPanel"

namespace HyperAIStudio::MemoryPanel
{
	FText ModeLabel(const EHyperAIStudioAgentMemoryMode Mode)
	{
		return StaticEnum<EHyperAIStudioAgentMemoryMode>()->GetDisplayNameTextByValue(static_cast<int64>(Mode));
	}

	TSharedRef<SWidget> Badge(const FText& Label, const FSlateColor& Color)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(5.0f, 1.0f))
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(Color)
			];
	}

	/** A Claude Code memory file for this project: frontmatter name, description and metadata type, then the text. */
	struct FClaudeMemoryFile
	{
		FString Path;
		FString Title;
		FString Type = TEXT("fact");
		FString Body;
		FString Raw;
		bool bChosen = false;
	};

	FString ClaudeMemoryDirectory(const FString& ProjectRoot)
	{
		FString ConfigDirectory = FPlatformMisc::GetEnvironmentVariable(TEXT("CLAUDE_CONFIG_DIR"));
		if (ConfigDirectory.IsEmpty())
		{
			FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
			ConfigDirectory = FPaths::Combine(Home.IsEmpty() ? FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")) : Home, TEXT(".claude"));
		}
		return FPaths::Combine(ConfigDirectory, TEXT("projects"), HyperAIStudio::ChatHistory::ClaudeProjectDirectoryName(ProjectRoot), TEXT("memory"));
	}

	FClaudeMemoryFile ParseClaudeMemory(const FString& Path, const FString& Raw)
	{
		FClaudeMemoryFile File;
		File.Path = Path;
		File.Raw = Raw;
		File.Title = FPaths::GetBaseFilename(Path);
		TArray<FString> Lines;
		Raw.Replace(TEXT("\r"), TEXT("")).ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty=*/false);
		int32 BodyStart = 0;
		if (Lines.Num() > 0 && Lines[0].TrimEnd() == TEXT("---"))
		{
			for (int32 Line = 1; Line < Lines.Num(); ++Line)
			{
				const FString Trimmed = Lines[Line].TrimStartAndEnd();
				if (Trimmed == TEXT("---"))
				{
					BodyStart = Line + 1;
					break;
				}
				FString Key;
				FString Value;
				if (Trimmed.Split(TEXT(":"), &Key, &Value))
				{
					Value.TrimStartAndEndInline();
					Value.RemoveFromStart(TEXT("\""));
					Value.RemoveFromEnd(TEXT("\""));
					if (Key == TEXT("description") && !Value.IsEmpty())
					{
						File.Title = Value;
					}
					else if (Key == TEXT("type"))
					{
						// Claude's kinds: feedback is how to work here; the rest are facts.
						File.Type = Value == TEXT("feedback") ? TEXT("convention") : TEXT("fact");
					}
				}
			}
		}
		TArray<FString> Body(Lines.GetData() + BodyStart, Lines.Num() - BodyStart);
		File.Body = FString::Join(Body, TEXT("\n")).TrimStartAndEnd();
		return File;
	}
}

void SHyperAIStudioMemoryPanel::Construct(const FArguments& InArgs)
{
	ChangedHandle = HyperAIStudio::Memory::OnChanged().AddSP(this, &SHyperAIStudioMemoryPanel::Reload);

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Title", "Project Memory"))
			.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.PageTitle")
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("SubtitleAutomatic", "What agents know about this project, from .hyperai/memory (shared through Git). Agents save what they learn on their own, each message a chat sends carries the notes that match it, and a note that reads like instructions to agents waits here for you."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SSegmentedControl<bool>)
				.Value_Lambda([this]() { return bShowTasks; })
				.OnValueChanged_Lambda([this](const bool bTasks)
				{
					bShowTasks = bTasks;
					RebuildFiltered();
				})
				+ SSegmentedControl<bool>::Slot(false)
					.Text(LOCTEXT("NotesTab", "Notes"))
					.ToolTip(LOCTEXT("NotesTabTooltip", "What agents know about this project."))
				+ SSegmentedControl<bool>::Slot(true)
					.Text(LOCTEXT("TasksTab", "Past tasks"))
					.ToolTip(LOCTEXT("TasksTabTooltip", "Every task a chat finished on this machine: what was asked and how it ended. Agents find these when a new task looks like an old one."))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Search memory"))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					Filter = Text.ToString();
					RebuildFiltered();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(10.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]() { return bShowTasks ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text(LOCTEXT("ModeLabel", "Agent-saved memories:"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.Visibility_Lambda([this]() { return bShowTasks ? EVisibility::Collapsed : EVisibility::Visible; })
				.OnGetMenuContent(this, &SHyperAIStudioMemoryPanel::BuildModeMenu)
				.ToolTipText(LOCTEXT("ModeTooltip", "How a fact an agent saves on its own reaches future chats. Facts you add always apply in full."))
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text_Lambda([]() { return HyperAIStudio::MemoryPanel::ModeLabel(GetDefault<UHyperAIStudioSettings>()->AgentMemory); })
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Visibility_Lambda([this]() { return bShowTasks ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text(LOCTEXT("AddFact", "Add Fact"))
				.ToolTipText(LOCTEXT("AddFactTooltip", "Write a fact every agent should know about this project."))
				.OnClicked_Lambda([]()
				{
					OpenFactDialog();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Visibility_Lambda([this]() { return bShowTasks ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text(LOCTEXT("ImportClaude", "Import from Claude Code..."))
				.ToolTipText(LOCTEXT("ImportClaudeTooltip", "Copy chosen facts from Claude Code's own memory for this project. You see each one before it is copied."))
				.OnClicked_Lambda([]()
				{
					OpenClaudeImportDialog();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Visibility_Lambda([this]() { return (bShowTasks ? Tasks.IsEmpty() : Filtered.IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed; })
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
			.Text_Lambda([this]()
			{
				if (bShowTasks)
				{
					return Filter.IsEmpty()
						? LOCTEXT("NoTasks", "No finished tasks yet. Each task a chat finishes on this machine is kept here, in .hyperai/runtime/memory/history.")
						: LOCTEXT("NoTaskMatch", "No past task matches.");
				}
				return AllFacts.IsEmpty()
					? LOCTEXT("Empty", "Nothing remembered yet. Add a fact, or let agents save what they learn.")
					: LOCTEXT("NoMatch", "No fact matches.");
			})
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return bShowTasks || Filtered.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					SAssignNew(List, SListView<FFactPtr>)
					.ListItemsSource(&Filtered)
					.SelectionMode(ESelectionMode::None)
					.OnGenerateRow(this, &SHyperAIStudioMemoryPanel::GenerateRow)
				]
			]
			+ SOverlay::Slot()
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return !bShowTasks || Tasks.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					SAssignNew(TaskList, SListView<FTaskPtr>)
					.ListItemsSource(&Tasks)
					.SelectionMode(ESelectionMode::None)
					.OnGenerateRow(this, &SHyperAIStudioMemoryPanel::GenerateTaskRow)
				]
			]
		]
	];
	Reload();
}

SHyperAIStudioMemoryPanel::~SHyperAIStudioMemoryPanel()
{
	HyperAIStudio::Memory::OnChanged().Remove(ChangedHandle);
}

void SHyperAIStudioMemoryPanel::Reload()
{
	AllFacts = HyperAIStudio::Memory::LoadFacts(FHyperAIStudioService::GetProjectRoot());
	RebuildFiltered();
}

bool SHyperAIStudioMemoryPanel::NeedsUser(const FHyperAIStudioMemoryFact& Fact)
{
	return !Fact.Held.IsEmpty() || (!Fact.bConfirmed && GetDefault<UHyperAIStudioSettings>()->AgentMemory != EHyperAIStudioAgentMemoryMode::Automatic);
}

void SHyperAIStudioMemoryPanel::RebuildFiltered()
{
	if (bShowTasks)
	{
		// Read afresh: chats add a task each time they finish one. A search ranks them as agents' recalls do.
		Tasks.Reset();
		for (const FHyperAIStudioMemoryTask& Task : HyperAIStudio::Memory::RecallTasks(FHyperAIStudioService::GetProjectRoot(), Filter, 50))
		{
			Tasks.Add(MakeShared<FHyperAIStudioMemoryTask>(Task));
		}
		if (TaskList.IsValid())
		{
			TaskList->RequestListRefresh();
		}
		return;
	}
	Filtered.Reset();
	for (const FHyperAIStudioMemoryFact& Fact : AllFacts)
	{
		if (Filter.IsEmpty() || Fact.Title.Contains(Filter) || Fact.Text.Contains(Filter) || Fact.Name.Contains(Filter))
		{
			Filtered.Add(MakeShared<FHyperAIStudioMemoryFact>(Fact));
		}
	}
	// What waits for the user comes first; otherwise newest first, as loaded.
	Filtered.StableSort([](const FFactPtr& A, const FFactPtr& B) { return NeedsUser(*A) && !NeedsUser(*B); });
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

TSharedRef<SWidget> SHyperAIStudioMemoryPanel::BuildModeMenu()
{
	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	auto AddMode = [&Menu](const EHyperAIStudioAgentMemoryMode Mode, const FText& ToolTip)
	{
		Menu.AddMenuEntry(
			HyperAIStudio::MemoryPanel::ModeLabel(Mode),
			ToolTip,
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Mode]()
				{
					UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
					Settings->AgentMemory = Mode;
					Settings->SaveConfig();
					// AGENTS.md follows the new rule.
					HyperAIStudio::Memory::OnChanged().Broadcast();
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([Mode]() { return GetDefault<UHyperAIStudioSettings>()->AgentMemory == Mode; })),
			NAME_None,
			EUserInterfaceActionType::RadioButton);
	};
	AddMode(EHyperAIStudioAgentMemoryMode::Automatic,
		LOCTEXT("ModeAutomaticTooltip", "Straight into every future chat; review or delete it here later. A note that reads like instructions to agents still waits for you."));
	AddMode(EHyperAIStudioAgentMemoryMode::TitleUntilReviewed,
		LOCTEXT("ModeTitleTooltip", "Future chats see the title at once and can look up the text; the text joins every chat once you confirm it."));
	AddMode(EHyperAIStudioAgentMemoryMode::AfterReview,
		LOCTEXT("ModeReviewTooltip", "Future chats never see it until you confirm it."));
	return Menu.MakeWidget();
}

TSharedRef<ITableRow> SHyperAIStudioMemoryPanel::GenerateRow(FFactPtr Fact, const TSharedRef<STableViewBase>& OwnerTable)
{
	using namespace HyperAIStudio::MemoryPanel;
	const FHyperAIStudioMemoryFact Copy = *Fact;
	const FString Preview = Copy.Text.Len() > 240 ? Copy.Text.Left(240) + TEXT("...") : Copy.Text;

	TSharedRef<SHorizontalBox> TitleLine = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Copy.Title))
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			Badge(FText::FromString(Copy.Type), FSlateColor::UseSubduedForeground())
		];
	if (NeedsUser(Copy))
	{
		const bool bHeld = !Copy.Held.IsEmpty();
		TitleLine->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.ToolTipText(bHeld ? FText::Format(LOCTEXT("HeldTooltip", "No chat sees this note: {0}. Allow it once you have checked it is a note about the project, not instructions."),
				FText::FromString(Copy.Held)) : FText::GetEmpty())
			[
				Badge(bHeld ? LOCTEXT("Held", "held for you") : LOCTEXT("AwaitingReview", "awaiting review"),
					FSlateColor(bHeld ? FStyleColors::AccentOrange : FStyleColors::AccentYellow))
			]
		];
		TitleLine->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(bHeld ? LOCTEXT("Allow", "Allow") : LOCTEXT("Confirm", "Confirm"))
			.ToolTipText(bHeld ? LOCTEXT("AllowTooltip", "Let every future chat use this note.") : LOCTEXT("ConfirmTooltip", "Let every future chat see this fact in full."))
			.OnClicked_Lambda([Name = Copy.Name]()
			{
				HyperAIStudio::Memory::Confirm(FHyperAIStudioService::GetProjectRoot(), Name);
				return FReply::Handled();
			})
		];
	}
	TitleLine->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(4.0f, 0.0f, 0.0f, 0.0f)
	[
		SNew(SButton)
		.Text(LOCTEXT("Edit", "Edit"))
		.OnClicked_Lambda([Copy]()
		{
			OpenFactDialog(&Copy);
			return FReply::Handled();
		})
	];
	TitleLine->AddSlot()
	.AutoWidth()
	.VAlign(VAlign_Center)
	.Padding(4.0f, 0.0f, 0.0f, 0.0f)
	[
		SNew(SButton)
		.Text(LOCTEXT("Delete", "Delete"))
		.OnClicked_Lambda([Name = Copy.Name, Title = Copy.Title]()
		{
			if (FMessageDialog::Open(EAppMsgType::YesNo, FText::Format(LOCTEXT("DeleteConfirm", "Delete the memory \"{0}\"? Future chats will no longer see it."),
				FText::FromString(Title))) == EAppReturnType::Yes)
			{
				FString Error;
				if (!HyperAIStudio::Memory::Forget(FHyperAIStudioService::GetProjectRoot(), Name, /*bByAgent=*/false, Error))
				{
					FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
				}
			}
			return FReply::Handled();
		})
	];

	return SNew(STableRow<FFactPtr>, OwnerTable)
		.Padding(FMargin(6.0f, 6.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				TitleLine
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Visibility(Preview.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.Text(FText::FromString(Preview))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::Format(LOCTEXT("RowMeta", "{0}, updated {1}. Name: {2}"),
					Copy.Source == TEXT("agent") ? LOCTEXT("FromAgent", "Saved by an agent") : LOCTEXT("FromUser", "Written by you"),
					FText::FromString(Copy.Updated), FText::FromString(Copy.Name)))
			]
		];
}

TSharedRef<ITableRow> SHyperAIStudioMemoryPanel::GenerateTaskRow(FTaskPtr Task, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FHyperAIStudioMemoryTask& Copy = *Task;
	const FString Asked = Copy.Request.Len() > 200 ? Copy.Request.Left(200) + TEXT("...") : Copy.Request;
	const FString Ended = Copy.Outcome.Len() > 320 ? Copy.Outcome.Left(320) + TEXT("...") : Copy.Outcome;
	return SNew(STableRow<FTaskPtr>, OwnerTable)
		.Padding(FMargin(6.0f, 6.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Asked))
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Visibility(Ended.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.Text(FText::FromString(Ended))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
				.Text(FText::Format(LOCTEXT("TaskMeta", "{0}, {1}.{2}"), FText::AsDateTime(Copy.Utc, EDateTimeStyle::Medium, EDateTimeStyle::Short),
					FText::FromString(Copy.Agent),
					Copy.Tools.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("TaskTools", " Used {0}."), FText::FromString(FString::Join(Copy.Tools, TEXT(", "))))))
			]
		];
}

void SHyperAIStudioMemoryPanel::OpenFactDialog(const FHyperAIStudioMemoryFact* Existing)
{
	const TSharedRef<SEditableTextBox> TitleBox = SNew(SEditableTextBox)
		.Text(Existing ? FText::FromString(Existing->Title) : FText::GetEmpty())
		.HintText(LOCTEXT("TitleHint", "One line: what this is about"));
	const TSharedRef<SMultiLineEditableTextBox> TextBox = SNew(SMultiLineEditableTextBox)
		.Text(Existing ? FText::FromString(Existing->Text) : FText::GetEmpty())
		.HintText(LOCTEXT("TextHint", "What every future chat should know, and why."))
		.AutoWrapText(true);
	const TSharedRef<FString> Type = MakeShared<FString>(Existing ? Existing->Type : FString(TEXT("fact")));
	TSharedRef<SHorizontalBox> TypeRow = SNew(SHorizontalBox);
	for (const TCHAR* Option : {TEXT("convention"), TEXT("decision"), TEXT("fact"), TEXT("gotcha")})
	{
		const FString Value = Option;
		TypeRow->AddSlot()
		.AutoWidth()
		.Padding(0.0f, 0.0f, 12.0f, 0.0f)
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([Type, Value]() { return *Type == Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Type, Value](ECheckBoxState) { *Type = Value; })
			[
				SNew(STextBlock).Text(FText::FromString(Value))
			]
		];
	}
	const TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
		.Title(Existing ? LOCTEXT("EditTitle", "Edit Memory") : LOCTEXT("AddTitle", "Add to Project Memory"))
		.Content()
		[
			SNew(SBox)
			.WidthOverride(560.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(LOCTEXT("TitleLabel", "Title")) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 10.0f)[ TitleBox ]
				+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(LOCTEXT("TypeLabel", "Kind")) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 10.0f)[ TypeRow ]
				+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(LOCTEXT("TextLabel", "Text")) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 10.0f)
				[
					SNew(SBox).HeightOverride(180.0f)[ TextBox ]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("SharedNote", "Saved in .hyperai/memory: every agent gets it, and Git shares it with your team."))
				]
			]
		]
		.Buttons({SCustomDialog::FButton(LOCTEXT("Save", "Save")), SCustomDialog::FButton(LOCTEXT("Cancel", "Cancel"))});
	if (Dialog->ShowModal() != 0)
	{
		return;
	}
	FHyperAIStudioMemoryFact Fact;
	Fact.Name = Existing ? Existing->Name : FString();
	Fact.Title = TitleBox->GetText().ToString();
	Fact.Text = TextBox->GetText().ToString();
	Fact.Type = *Type;
	Fact.Source = TEXT("user");
	FString Error;
	if (HyperAIStudio::Memory::Remember(FHyperAIStudioService::GetProjectRoot(), Fact, Error).IsEmpty())
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
	}
}

void SHyperAIStudioMemoryPanel::OpenClaudeImportDialog()
{
	using namespace HyperAIStudio::MemoryPanel;
	const FString Root = FHyperAIStudioService::GetProjectRoot();
	const FString Directory = ClaudeMemoryDirectory(Root);
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *FPaths::Combine(Directory, TEXT("*.md")), /*Files=*/true, /*Directories=*/false);
	Names.Remove(TEXT("MEMORY.md"));
	Names.Sort();
	const TSharedRef<TArray<FClaudeMemoryFile>> Files = MakeShared<TArray<FClaudeMemoryFile>>();
	for (const FString& Name : Names)
	{
		FString Raw;
		if (FFileHelper::LoadFileToString(Raw, *FPaths::Combine(Directory, Name)))
		{
			Files->Add(ParseClaudeMemory(FPaths::Combine(Directory, Name), Raw));
		}
	}
	if (Files->IsEmpty())
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("NoClaudeMemory", "No Claude Code memory was found for this project in {0}."),
			FText::FromString(Directory)));
		return;
	}

	const TSharedRef<int32> Shown = MakeShared<int32>(0);
	TSharedRef<SVerticalBox> Choices = SNew(SVerticalBox);
	for (int32 Index = 0; Index < Files->Num(); ++Index)
	{
		Choices->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([Files, Index]() { return (*Files)[Index].bChosen ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Files, Shown, Index](const ECheckBoxState State)
				{
					(*Files)[Index].bChosen = State == ECheckBoxState::Checked;
					*Shown = Index;
				})
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked_Lambda([Shown, Index]()
				{
					*Shown = Index;
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(FText::FromString((*Files)[Index].Title))
					.ColorAndOpacity_Lambda([Shown, Index]() { return *Shown == Index ? FSlateColor(FStyleColors::AccentBlue) : FSlateColor::UseForeground(); })
				]
			]
		];
	}

	const TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
		.Title(LOCTEXT("ImportTitle", "Import from Claude Code Memory"))
		.Content()
		[
			SNew(SBox)
			.WidthOverride(880.0f)
			.HeightOverride(480.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT("ImportNote", "These are your private Claude Code memories for this project. Imported facts go to .hyperai/memory, which Git can share with your team, so read each one before you tick it."))
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(0.42f)
					[
						SNew(SScrollBox) + SScrollBox::Slot()[ Choices ]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(0.58f)
					.Padding(10.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SMultiLineEditableTextBox)
						.IsReadOnly(true)
						.AutoWrapText(true)
						.Text_Lambda([Files, Shown]() { return FText::FromString((*Files)[*Shown].Raw); })
					]
				]
			]
		]
		.Buttons({SCustomDialog::FButton(LOCTEXT("ImportSelected", "Import Selected")), SCustomDialog::FButton(LOCTEXT("ImportCancel", "Cancel"))});
	if (Dialog->ShowModal() != 0)
	{
		return;
	}
	int32 Imported = 0;
	TArray<FString> Errors;
	for (const FClaudeMemoryFile& File : *Files)
	{
		if (!File.bChosen)
		{
			continue;
		}
		FHyperAIStudioMemoryFact Fact;
		Fact.Title = File.Title;
		Fact.Text = File.Body;
		Fact.Type = File.Type;
		Fact.Source = TEXT("user");
		FString Error;
		if (HyperAIStudio::Memory::Remember(Root, Fact, Error).IsEmpty())
		{
			Errors.Add(FString::Printf(TEXT("%s: %s"), *File.Title, *Error));
		}
		else
		{
			++Imported;
		}
	}
	if (Imported > 0 || !Errors.IsEmpty())
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("ImportResult", "Imported {0} {0}|plural(one=fact,other=facts).{1}"),
			Imported, FText::FromString(Errors.IsEmpty() ? FString() : TEXT("\n\n") + FString::Join(Errors, TEXT("\n")))));
	}
}

#undef LOCTEXT_NAMESPACE
