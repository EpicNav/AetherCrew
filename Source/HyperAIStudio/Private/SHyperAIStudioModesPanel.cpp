// Games by Hyper 2026.

#include "SHyperAIStudioModesPanel.h"

#include "Dialog/SCustomDialog.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioStyle.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioModesPanel"

namespace HyperAIStudio::ModesPanel
{
	FText ApprovalLabel(const EHyperAIStudioModeApproval Approval)
	{
		switch (Approval)
		{
		case EHyperAIStudioModeApproval::ReadOnly: return LOCTEXT("ApprovalReadOnly", "read only");
		case EHyperAIStudioModeApproval::Ask: return LOCTEXT("ApprovalAsk", "asks for edits");
		default: return LOCTEXT("ApprovalNormal", "normal approvals");
		}
	}

	TArray<FString> SplitList(const FString& Text)
	{
		TArray<FString> Items;
		Text.ParseIntoArray(Items, TEXT(","), /*InCullEmpty=*/true);
		TArray<FString> Out;
		for (FString& Item : Items)
		{
			Item.TrimStartAndEndInline();
			if (!Item.IsEmpty())
			{
				Out.AddUnique(Item);
			}
		}
		return Out;
	}

	/** A mode id from a name: "Art pass" is art-pass. */
	FString IdFromName(const FString& Name)
	{
		FString Id;
		for (const TCHAR C : Name.ToLower())
		{
			Id.AppendChar((C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9')) ? C : TEXT('-'));
		}
		while (Id.ReplaceInline(TEXT("--"), TEXT("-")) > 0) {}
		Id.RemoveFromStart(TEXT("-"));
		Id.RemoveFromEnd(TEXT("-"));
		return Id.Left(64);
	}

	const void* ParentWindow()
	{
		return FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	}

	/** One edit of Draft: false when it saved or was cancelled, true to show the dialog again after an error. */
	bool EditOnce(FHyperAIStudioChatMode& Draft, const FString& PreviousId, bool& bOutSaved)
	{
		auto Box = [](const FString& Value, const FText& Hint)
		{
			return SNew(SEditableTextBox).Text(FText::FromString(Value)).HintText(Hint);
		};
		const TSharedRef<SEditableTextBox> NameBox = Box(Draft.Name, LOCTEXT("NameHint", "Such as Level blockout"));
		const TSharedRef<SEditableTextBox> DescriptionBox = Box(Draft.Description, LOCTEXT("DescriptionHint", "One line the mode menu shows"));
		const TSharedRef<SEditableTextBox> PacksBox = Box(FString::Join(Draft.Packs, TEXT(", ")), LOCTEXT("PacksHint", "Catalog packs, such as level_design, lighting_lookdev"));
		const TSharedRef<SEditableTextBox> ToolsBox = Box(FString::Join(Draft.Tools, TEXT(", ")), LOCTEXT("ToolsHint", "HyperAI tools to name first, such as hyper_lighting_inspect"));
		const TSharedRef<SEditableTextBox> SkillsBox = Box(FString::Join(Draft.Skills, TEXT(", ")), LOCTEXT("SkillsHint", "Skills, such as unreal-level-design"));
		const TSharedRef<SEditableTextBox> AgentBox = Box(Draft.Agent, LOCTEXT("AgentHint", "Empty keeps the chat's agent; or Claude Code, Codex, Gemini..."));
		const TSharedRef<SEditableTextBox> ModelBox = Box(Draft.Model, LOCTEXT("ModelHint", "Empty keeps the chat's model"));
		const TSharedRef<SMultiLineEditableTextBox> InstructionsBox = SNew(SMultiLineEditableTextBox)
			.Text(FText::FromString(Draft.Instructions))
			.HintText(LOCTEXT("InstructionsHint", "What the agent is told when the chat enters this mode."))
			.AutoWrapText(true);
		const TSharedRef<EHyperAIStudioModeApproval> Approval = MakeShared<EHyperAIStudioModeApproval>(Draft.Approval);
		auto Radio = [Approval](const EHyperAIStudioModeApproval Value, const FText& Label, const FText& ToolTip)
		{
			return SNew(SCheckBox)
				.Style(FAppStyle::Get(), "RadioButton")
				.ToolTipText(ToolTip)
				.IsChecked_Lambda([Approval, Value]() { return *Approval == Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Approval, Value](const ECheckBoxState) { *Approval = Value; })
				[
					SNew(STextBlock).Text(Label)
				];
		};
		auto Label = [](const FText& Text) { return SNew(STextBlock).Text(Text); };
		auto Pair = [Label](const FText& LeftLabel, const TSharedRef<SWidget>& Left, const FText& RightLabel, const TSharedRef<SWidget>& Right)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ Label(LeftLabel) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)[ Left ]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ Label(RightLabel) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)[ Right ]
				];
		};
		const TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
			.Title(PreviousId.IsEmpty() ? LOCTEXT("NewTitle", "New Chat Mode") : LOCTEXT("EditTitle", "Edit Chat Mode"))
			.Content()
			[
				SNew(SBox)
				.WidthOverride(640.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[ Pair(LOCTEXT("NameLabel", "Name"), NameBox, LOCTEXT("DescriptionLabel", "Description"), DescriptionBox) ]
					+ SVerticalBox::Slot().AutoHeight()[ Label(LOCTEXT("ApprovalLabel", "HyperAI's tools in this mode")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 14.0f, 0.0f)
						[
							Radio(EHyperAIStudioModeApproval::Normal, LOCTEXT("RadioNormal", "Normal"), LOCTEXT("RadioNormalTip", "Your approval settings and auto-approval apply as usual."))
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 14.0f, 0.0f)
						[
							Radio(EHyperAIStudioModeApproval::Ask, LOCTEXT("RadioAsk", "Ask for edits"), LOCTEXT("RadioAskTip", "Every edit plan waits for your click, even with approvals or auto-approval off."))
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							Radio(EHyperAIStudioModeApproval::ReadOnly, LOCTEXT("RadioReadOnly", "Read only"), LOCTEXT("RadioReadOnlyTip", "HyperAI refuses every edit plan while a chat is in this mode."))
						]
					]
					+ SVerticalBox::Slot().AutoHeight()[ Pair(LOCTEXT("PacksLabel", "Packs"), PacksBox, LOCTEXT("ToolsLabel", "Tools"), ToolsBox) ]
					+ SVerticalBox::Slot().AutoHeight()[ Label(LOCTEXT("SkillsLabel", "Skills")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)[ SkillsBox ]
					+ SVerticalBox::Slot().AutoHeight()[ Pair(LOCTEXT("AgentLabel", "Preferred agent"), AgentBox, LOCTEXT("ModelLabel", "Preferred model"), ModelBox) ]
					+ SVerticalBox::Slot().AutoHeight()[ Label(LOCTEXT("InstructionsLabel", "Instructions")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
					[
						SNew(SBox).HeightOverride(200.0f)[ InstructionsBox ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(LOCTEXT("SharedNote", "Saved in .hyperai/modes as JSON, so Git shares it with your team. Never put a key here: keys go in Settings > Agents."))
					]
				]
			]
			.Buttons({SCustomDialog::FButton(LOCTEXT("Save", "Save")), SCustomDialog::FButton(LOCTEXT("Cancel", "Cancel"))});
		bOutSaved = false;
		if (Dialog->ShowModal() != 0)
		{
			return false;
		}
		Draft.Name = NameBox->GetText().ToString().TrimStartAndEnd();
		// A built-in mode keeps its id, so the project's version replaces it; a new one takes its id from its name.
		if (!Draft.bBuiltIn)
		{
			Draft.Id = IdFromName(Draft.Name);
		}
		Draft.Description = DescriptionBox->GetText().ToString().TrimStartAndEnd();
		Draft.Approval = *Approval;
		Draft.Packs = SplitList(PacksBox->GetText().ToString());
		Draft.Tools = SplitList(ToolsBox->GetText().ToString());
		Draft.Skills = SplitList(SkillsBox->GetText().ToString());
		Draft.Agent = AgentBox->GetText().ToString().TrimStartAndEnd();
		Draft.Model = ModelBox->GetText().ToString().TrimStartAndEnd();
		Draft.Instructions = InstructionsBox->GetText().ToString();
		FString Error;
		if (HyperAIStudio::ChatModes::Save(FHyperAIStudioService::GetProjectRoot(), Draft, PreviousId, Error))
		{
			bOutSaved = true;
			return false;
		}
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
		return true;
	}
}

bool SHyperAIStudioModesPanel::EditMode(const FHyperAIStudioChatMode* Existing)
{
	FHyperAIStudioChatMode Draft = Existing ? *Existing : FHyperAIStudioChatMode();
	const FString PreviousId = Existing ? Existing->Id : FString();
	bool bSaved = false;
	while (HyperAIStudio::ModesPanel::EditOnce(Draft, PreviousId, bSaved))
	{
	}
	return bSaved;
}

void SHyperAIStudioModesPanel::OpenWindow()
{
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("WindowTitle", "HyperAI Chat Modes"))
		.ClientSize(FVector2D(760.0f, 560.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(16.0f)
			[
				SNew(SHyperAIStudioModesPanel)
			]
		];
	FSlateApplication::Get().AddWindow(Window);
}

void SHyperAIStudioModesPanel::Construct(const FArguments& InArgs)
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
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Title", "Chat modes"))
			.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.PageTitle")
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("Subtitle", "A mode is a preset for a chat: what the agent is told, the packs, tools and skills its messages go with, and how far HyperAI's tools may go. Pick one in the chat's composer. A read-only mode is enforced: HyperAI refuses edit plans while any chat is in it."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				Button(LOCTEXT("New", "New Mode..."), LOCTEXT("NewTip", "Make a mode for this project."), [this]()
				{
					if (EditMode(nullptr))
					{
						Reload();
					}
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				Button(LOCTEXT("Import", "Import..."), LOCTEXT("ImportTip", "Copy a mode's .json file into this project after checking it."), [this]()
				{
					TArray<FString> Files;
					IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
					if (Desktop && Desktop->OpenFileDialog(HyperAIStudio::ModesPanel::ParentWindow(), LOCTEXT("PickMode", "Chat mode").ToString(),
						FString(), FString(), TEXT("Chat modes (*.json)|*.json"), EFileDialogFlags::None, Files) && Files.Num() == 1)
					{
						FString Id;
						FString Error;
						if (!HyperAIStudio::ChatModes::Import(FHyperAIStudioService::GetProjectRoot(), Files[0], Id, Error))
						{
							FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
						}
						Reload();
					}
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				Button(LOCTEXT("OpenFolder", "Open Folder"), LOCTEXT("OpenFolderTip", "Show .hyperai/modes."), []()
				{
					const FString Folder = HyperAIStudio::ChatModes::Directory(FHyperAIStudioService::GetProjectRoot());
					IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
					FPlatformProcess::ExploreFolder(*Folder);
				})
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(List, SListView<FModePtr>)
			.ListItemsSource(&Modes)
			.SelectionMode(ESelectionMode::None)
			.OnGenerateRow(this, &SHyperAIStudioModesPanel::GenerateRow)
		]
	];
	Reload();
}

void SHyperAIStudioModesPanel::Reload()
{
	Modes.Reset();
	for (FHyperAIStudioChatMode& Mode : HyperAIStudio::ChatModes::List(FHyperAIStudioService::GetProjectRoot()))
	{
		Modes.Add(MakeShared<FHyperAIStudioChatMode>(MoveTemp(Mode)));
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SHyperAIStudioModesPanel::GenerateRow(FModePtr Mode, const TSharedRef<STableViewBase>& OwnerTable)
{
	using namespace HyperAIStudio::ModesPanel;
	const FHyperAIStudioChatMode Copy = *Mode;
	const TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("RowTitle", "{0}  ({1}{2})"), FText::FromString(Copy.Name), ApprovalLabel(Copy.Approval),
				Copy.bBuiltIn ? (Copy.File.IsEmpty() ? LOCTEXT("BuiltInSuffix", ", built in") : LOCTEXT("ChangedSuffix", ", built in, changed here")) : FText::GetEmpty()))
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.ColorAndOpacity(Copy.IsReadOnly() ? FSlateColor(FStyleColors::AccentYellow) : FSlateColor::UseForeground())
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Edit", "Edit"))
			.ToolTipText(Copy.bBuiltIn ? LOCTEXT("EditBuiltInTip", "Change this built-in mode for this project; Delete goes back to how it shipped.") : LOCTEXT("EditTip", "Change this mode."))
			.OnClicked_Lambda([this, Copy]()
			{
				if (EditMode(&Copy))
				{
					Reload();
				}
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("Export", "Export..."))
			.ToolTipText(LOCTEXT("ExportTip", "Save this mode as a .json file to share."))
			.OnClicked_Lambda([Id = Copy.Id]()
			{
				TArray<FString> Files;
				IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
				if (Desktop && Desktop->SaveFileDialog(HyperAIStudio::ModesPanel::ParentWindow(), LOCTEXT("ExportTitle", "Export chat mode").ToString(),
					FString(), Id + TEXT(".json"), TEXT("Chat modes (*.json)|*.json"), EFileDialogFlags::None, Files) && Files.Num() == 1)
				{
					FString Error;
					if (!HyperAIStudio::ChatModes::Export(FHyperAIStudioService::GetProjectRoot(), Id, Files[0], Error))
					{
						FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
					}
				}
				return FReply::Handled();
			})
		];
	if (!Copy.File.IsEmpty())
	{
		Line->AddSlot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(Copy.bBuiltIn ? LOCTEXT("Restore", "Restore") : LOCTEXT("Delete", "Delete"))
			.OnClicked_Lambda([this, Copy]()
			{
				const FText Question = Copy.bBuiltIn
					? FText::Format(LOCTEXT("RestoreConfirm", "Put {0} back as it shipped, dropping this project's changes to it?"), FText::FromString(Copy.Name))
					: FText::Format(LOCTEXT("DeleteConfirm", "Delete the mode {0} from this project?"), FText::FromString(Copy.Name));
				if (FMessageDialog::Open(EAppMsgType::YesNo, Question) == EAppReturnType::Yes)
				{
					FString Error;
					if (!HyperAIStudio::ChatModes::Delete(FHyperAIStudioService::GetProjectRoot(), Copy.Id, Error))
					{
						FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
					}
					Reload();
				}
				return FReply::Handled();
			})
		];
	}
	return SNew(STableRow<FModePtr>, OwnerTable)
		.Padding(FMargin(6.0f, 5.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Line ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::FromString(Copy.Description))
			]
		];
}

#undef LOCTEXT_NAMESPACE
