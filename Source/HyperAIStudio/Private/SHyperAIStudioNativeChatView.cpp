// Games by Hyper 2026.

#include "SHyperAIStudioNativeChatView.h"

#include "DesktopPlatformModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioAgents.h"
#include "HyperAIStudioChatModes.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "SHyperAIStudioModesPanel.h"
#include "HyperAIStudioMarkdown.h"
#include "HyperAIStudioPreviewPanel.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioWidgets.h"
#include "IDesktopPlatform.h"
#include "ImageUtils.h"
#include "Input/DragAndDrop.h"
#include "LevelEditorViewport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SHyperAIStudioPromptPicker.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioNativeChatView"

namespace HyperAIStudio::NativeChatView
{
	using EKind = FHyperAIStudioChatItem::EKind;

	FSlateFontInfo Mono()
	{
		return FCoreStyle::GetDefaultFontStyle("Mono", 9);
	}

	/** Editable text takes its colour from a style it keeps a pointer to, so the four variants live for good. */
	const FTextBlockStyle& LiveStyle(const bool bMono, const bool bMuted)
	{
		static FTextBlockStyle Styles[4];
		static bool bMade = false;
		if (!bMade)
		{
			bMade = true;
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Styles[Index] = FAppStyle::GetWidgetStyle<FTextBlockStyle>("NormalText");
				Styles[Index].SetFont((Index & 1) ? Mono() : FCoreStyle::GetDefaultFontStyle("Regular", 10));
				Styles[Index].SetColorAndOpacity((Index & 2) ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground());
			}
		}
		return Styles[(bMono ? 1 : 0) + (bMuted ? 2 : 0)];
	}

	/** Selectable, wrapping text that follows the item as it streams in. */
	TSharedRef<SWidget> LiveText(const TSharedPtr<FHyperAIStudioChatItem>& Item, const bool bMono = false, const bool bMuted = false)
	{
		return SNew(SMultiLineEditableText)
			.IsReadOnly(true)
			.AutoWrapText(true)
			.TextStyle(&LiveStyle(bMono, bMuted))
			.Text_Lambda([Item]() { return FText::FromString(Item->Text); });
	}

	FSlateColor StatusColor(const FString& Status)
	{
		if (Status == TEXT("completed"))
		{
			return FSlateColor(FStyleColors::AccentGreen);
		}
		if (Status == TEXT("failed") || Status == TEXT("declined") || Status.StartsWith(TEXT("exit code")))
		{
			return FSlateColor(FStyleColors::AccentRed);
		}
		return FSlateColor::UseSubduedForeground();
	}

	bool IsPictureFile(const FString& Path)
	{
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		return Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("webp");
	}

	FText KindLabel(const EKind Kind)
	{
		switch (Kind)
		{
		case EKind::Command: return LOCTEXT("KindCommand", "Command");
		case EKind::FileChange: return LOCTEXT("KindFiles", "Files");
		case EKind::Reasoning: return LOCTEXT("KindThinking", "Thinking");
		default: return LOCTEXT("KindTool", "Tool");
		}
	}

	FName KindIcon(const EKind Kind)
	{
		switch (Kind)
		{
		case EKind::Command: return TEXT("HyperAIStudio.Icon.Terminal");
		case EKind::FileChange: return TEXT("HyperAIStudio.Icon.File");
		case EKind::Reasoning: return TEXT("HyperAIStudio.Icon.Thinking");
		default: return TEXT("HyperAIStudio.Icon.Tools");
		}
	}

	HyperAIStudio::UI::EStatus StepStatus(const FString& Status)
	{
		using HyperAIStudio::UI::EStatus;
		if (Status == TEXT("completed"))
		{
			return EStatus::Good;
		}
		if (Status == TEXT("failed") || Status == TEXT("declined") || Status.StartsWith(TEXT("exit code")))
		{
			return EStatus::Error;
		}
		return Status == TEXT("running") || Status == TEXT("inProgress") ? EStatus::Busy : EStatus::Neutral;
	}

	FString PrettyJson(const FString& Json)
	{
		TSharedPtr<FJsonValue> Value;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Value) || !Value.IsValid())
		{
			return Json;
		}
		FString Out;
		FJsonSerializer::Serialize(Value, FString(), TJsonWriterFactory<>::Create(&Out));
		return Out;
	}

	/** A tool's details, as the chat keeps them: "Arguments: {...}", then an error, then what it answered. */
	FString ToolMarkdown(const FString& Text)
	{
		FString Rest = Text;
		FString Arguments;
		if (Rest.StartsWith(TEXT("Arguments: ")))
		{
			const int32 Split = Rest.Find(TEXT("\n\n"));
			Arguments = Split == INDEX_NONE ? Rest.Mid(11) : Rest.Mid(11, Split - 11);
			Rest = Split == INDEX_NONE ? FString() : Rest.Mid(Split + 2);
		}
		FString Out;
		if (Rest.StartsWith(TEXT("Error: ")))
		{
			const int32 Split = Rest.Find(TEXT("\n\n"));
			const FString Error = Split == INDEX_NONE ? Rest.Mid(7) : Rest.Mid(7, Split - 7);
			Out += TEXT("> **") + LOCTEXT("ToolError", "Error").ToString() + TEXT(":** ") + Error.Replace(TEXT("\n"), TEXT(" ")) + TEXT("\n\n");
			Rest = Split == INDEX_NONE ? FString() : Rest.Mid(Split + 2);
		}
		// What it answered comes first; a JSON answer reads as a report.
		if (!Rest.TrimStartAndEnd().IsEmpty())
		{
			const FString Report = HyperAIStudio::Markdown::FromJson(Rest);
			Out += (Report.IsEmpty() ? Rest.TrimStartAndEnd() : Report) + TEXT("\n\n");
		}
		if (!Arguments.IsEmpty() && Arguments != TEXT("{}") && Arguments != TEXT("null"))
		{
			Out += TEXT("**") + LOCTEXT("ToolArguments", "Arguments").ToString() + TEXT("**\n\n```json\n") + PrettyJson(Arguments) + TEXT("\n```\n");
		}
		return Out.TrimEnd();
	}
}

FString SHyperAIStudioNativeChatView::StepMarkdown(const FHyperAIStudioChatItem& Item)
{
	using namespace HyperAIStudio::NativeChatView;
	switch (Item.Kind)
	{
	case EKind::Reasoning:
		return Item.Text;
	case EKind::Command:
		return Item.Text.IsEmpty() ? FString() : TEXT("```\n") + Item.Text.TrimEnd() + TEXT("\n```");
	case EKind::FileChange:
		return Item.Text.IsEmpty() ? FString() : TEXT("```diff\n") + Item.Text.TrimEnd() + TEXT("\n```");
	default:
		return ToolMarkdown(Item.Text);
	}
}

void SHyperAIStudioNativeChatView::Construct(const FArguments& InArgs)
{
	Chat = InArgs._Chat;
	OnRoute = InArgs._OnRoute;
	OnModeChosen = InArgs._OnModeChosen;
	ChatLabel = InArgs._ChatLabel;
	check(Chat.IsValid());
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this]()
				{
					if (!Chat->IsRunning())
					{
						return Chat->IsReady() ? LOCTEXT("StateReady", "Ready") : LOCTEXT("StateStopped", "Stopped");
					}
					if (!Chat->IsReady())
					{
						return LOCTEXT("StateStarting", "Starting...");
					}
					if (Chat->IsBusy())
					{
						return Chat->NumQueued() > 0
							? FText::Format(LOCTEXT("StateWorkingQueued", "Working... ({0} queued)"), Chat->NumQueued())
							: LOCTEXT("StateWorking", "Working...");
					}
					return LOCTEXT("StateReady", "Ready");
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]() { return FText::FromString(Chat->GetUsage()); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Stop", "Stop"))
				.ToolTipText(LOCTEXT("StopTooltip", "Stop the agent's current turn and drop queued messages."))
				.Visibility_Lambda([this]() { return Chat->IsBusy() ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked_Lambda([this]()
				{
					Chat->Interrupt();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SAssignNew(PlanBox, SVerticalBox)
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(Scroll, SScrollBox)
			.OnUserScrolled_Lambda([this](const float Offset)
			{
				// Reading back up stops following the conversation; scrolling to its end picks it up again.
				bFollow = Scroll.IsValid() && Offset >= Scroll->GetScrollOffsetOfEnd() - 4.0f;
			})
			+ SScrollBox::Slot()
			[
				SAssignNew(Transcript, SVerticalBox)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(AttachmentBox, SWrapBox)
			.UseAllottedSize(true)
			.InnerSlotPadding(FVector2D(6.0f, 6.0f))
			.Visibility_Lambda([this]() { return Attached.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SBox)
				.MaxDesiredHeight(160.0f)
				[
					SAssignNew(Composer, SMultiLineEditableTextBox)
					.AutoWrapText(true)
					.HintText(FText::Format(LOCTEXT("ComposerHint", "Message {0}. Enter sends; Shift+Enter starts a new line."), FText::FromString(Chat->GetAgentName())))
					.OnTextChanged_Lambda([this](const FText& Text)
					{
						// A composer typed empty starts over: the preset no longer describes what is in it, and what it turned on
						// and said goes with it.
						if (Text.IsEmpty())
						{
							if (PendingPreset.IsSet())
							{
								bTeam = bTeamFromPreset ? false : bTeam;
								Notice = Notice == PresetNotice ? FString() : Notice;
							}
							PendingPreset.Reset();
							PendingRoute.Reset();
							PendingBlanks.Reset();
							bBlanksNoted = false;
							bTeamFromPreset = false;
						}
					})
					.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& KeyEvent)
					{
						if (KeyEvent.GetKey() == EKeys::Enter && !KeyEvent.IsShiftDown() && !KeyEvent.IsControlDown() && !KeyEvent.IsAltDown())
						{
							Submit();
							return FReply::Handled();
						}
						// Ctrl+Space, not "/": a slash starts the agent's own commands and skills.
						if (KeyEvent.GetKey() == EKeys::SpaceBar && KeyEvent.IsControlDown() && !KeyEvent.IsShiftDown() && !KeyEvent.IsAltDown())
						{
							OpenPrompts();
							return FReply::Handled();
						}
						return FReply::Unhandled();
					})
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SComboButton)
				.ToolTipText_Lambda([this]()
				{
					const FHyperAIStudioChatMode& Mode = Chat->GetMode();
					return FText::Format(LOCTEXT("ModeTooltip", "The chat's mode: {0}. {1}"), Mode.Name.IsEmpty() ? LOCTEXT("ModeBuildName", "Build") : FText::FromString(Mode.Name),
						FText::FromString(Mode.Description));
				})
				.MenuPlacement(MenuPlacement_AboveAnchor)
				.OnGetMenuContent(this, &SHyperAIStudioNativeChatView::BuildModeMenu)
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						const FHyperAIStudioChatMode& Mode = Chat->GetMode();
						return Mode.Name.IsEmpty() ? LOCTEXT("ModeBuild", "Build") : FText::FromString(Mode.Name);
					})
					.ColorAndOpacity_Lambda([this]()
					{
						return Chat->GetMode().IsReadOnly() ? FSlateColor(FStyleColors::AccentYellow) : FSlateColor::UseForeground();
					})
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.ToolTipText(LOCTEXT("TeamTooltip", "Send your messages to HyperAI's team, not to this agent alone. The director plans the work, the programmer, level designer, lighting artist, technical artist and UI designer build their parts at once, the QA tester proves each part, and the director reviews it all against the team's quality bar. The first time, this adds the team to the project."))
				.IsChecked_Lambda([this]() { return bTeam ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState State)
				{
					bTeam = State == ECheckBoxState::Checked;
					// The user's own choice from here on.
					bTeamFromPreset = false;
				})
				[
					SNew(SBox)
					.Padding(FMargin(8.0f, 2.0f))
					[
						SNew(STextBlock).Text(LOCTEXT("Team", "Team"))
					]
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SAssignNew(PromptsButton, SComboButton)
				.ToolTipText(LOCTEXT("PromptsTooltip", "Start from a tested prompt for any kind of work; it fills in what you have selected. Ctrl+Space opens it too."))
				.MenuPlacement(MenuPlacement_AboveAnchor)
				.OnGetMenuContent(this, &SHyperAIStudioNativeChatView::BuildPromptPicker)
				.ButtonContent()
				[
					SNew(STextBlock).Text(LOCTEXT("Prompts", "Prompts"))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("AttachImage", "Image..."))
				.ToolTipText(LOCTEXT("AttachImageTooltip", "Attach a picture for the agent to look at. Dropping image files onto the chat does the same."))
				.OnClicked_Lambda([this]()
				{
					IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
					TArray<FString> Files;
					if (Desktop && Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared()),
						LOCTEXT("AttachImageTitle", "Attach an image").ToString(), FPaths::ProjectDir(), FString(),
						TEXT("Images (*.png;*.jpg;*.jpeg;*.webp)|*.png;*.jpg;*.jpeg;*.webp"), EFileDialogFlags::Multiple, Files))
					{
						for (const FString& File : Files)
						{
							Attach(FPaths::ConvertRelativePathToFull(File));
						}
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("AttachViewport", "Viewport"))
				.ToolTipText(LOCTEXT("AttachViewportTooltip", "Attach what the level viewport shows now."))
				.OnClicked_Lambda([this]()
				{
					FString Error;
					const FString File = CaptureViewport(Error);
					Notice = File.IsEmpty() ? Error : FString();
					if (!File.IsEmpty())
					{
						Attach(File);
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text_Lambda([this]() { return Chat->IsBusy() || !Chat->IsReady() ? LOCTEXT("Queue", "Queue") : LOCTEXT("Send", "Send"); })
				.ToolTipText(LOCTEXT("SendTooltip", "Send now, or after the running turn."))
				.OnClicked_Lambda([this]()
				{
					Submit();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Visibility_Lambda([this]() { return Notice.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this]() { return FText::FromString(Notice); })
		]
	];
	ChangedHandle = Chat->OnChanged.AddSP(this, &SHyperAIStudioNativeChatView::Refresh);
	Refresh();
}

SHyperAIStudioNativeChatView::~SHyperAIStudioNativeChatView()
{
	if (Chat.IsValid())
	{
		Chat->OnChanged.Remove(ChangedHandle);
	}
}

void SHyperAIStudioNativeChatView::InsertText(const FString& Text)
{
	if (Composer.IsValid())
	{
		const FString Current = Composer->GetText().ToString();
		Composer->SetText(FText::FromString(Current.IsEmpty() ? Text : Current + TEXT(" ") + Text));
		FocusComposer();
	}
}

void SHyperAIStudioNativeChatView::InsertPreset(const FHyperAIStudioPromptPreset& Preset, const FString& Text, const TArray<FString>& Blanks)
{
	if (!Composer.IsValid() || Text.TrimStartAndEnd().IsEmpty())
	{
		return;
	}
	const FString Current = Composer->GetText().ToString().TrimEnd();
	Composer->SetText(FText::FromString(Current.TrimStart().IsEmpty() ? Text : Current + TEXT("\n\n") + Text));
	// After the text, which may have reset it.
	PendingPreset = Preset;
	const HyperAIStudio::PromptPresets::FLibrary& Library = HyperAIStudio::PromptPresets::Get(FHyperAIStudioService::GetProjectRoot());
	const FHyperAIStudioPromptCategory* Category = Library.FindCategory(Preset.Category);
	PendingRoute = Category ? Category->Route : FString();
	PendingBlanks = Blanks;
	bBlanksNoted = false;
	HyperAIStudio::PromptPresets::NoteUsed(Preset.Id);
	TArray<FString> Lines;
	if (Preset.Agent == TEXT("team") && !bTeam)
	{
		// Visibly, so the user can turn it off and keep the work with this agent.
		bTeam = true;
		bTeamFromPreset = true;
		Lines.Add(LOCTEXT("PresetTeamOn", "Team is on: this prompt is for HyperAI's team. Turn it off to send it to this agent alone.").ToString());
	}
	FocusComposer();
	if (!Blanks.IsEmpty())
	{
		Composer->GoTo(ETextLocation::BeginningOfDocument);
		Composer->BeginSearch(FText::FromString(TEXT("[") + Blanks[0] + TEXT("]")), ESearchCase::CaseSensitive);
		TArray<FString> Bracketed;
		for (const FString& Blank : Blanks)
		{
			Bracketed.Add(TEXT("[") + Blank + TEXT("]"));
		}
		Lines.Add(FText::Format(LOCTEXT("PresetBlanks", "Fill in: {0}"), FText::FromString(FString::Join(Bracketed, TEXT(", ")))).ToString());
	}
	Notice = FString::Join(Lines, TEXT(" "));
	PresetNotice = Notice;
}

void SHyperAIStudioNativeChatView::OpenPrompts()
{
	if (!PromptsButton.IsValid())
	{
		return;
	}
	PromptsButton->SetIsOpen(true);
	if (const TSharedPtr<SHyperAIStudioPromptPicker> Opened = Picker.Pin())
	{
		FSlateApplication::Get().SetKeyboardFocus(Opened->GetSearchBox(), EFocusCause::SetDirectly);
	}
}

TSharedRef<SWidget> SHyperAIStudioNativeChatView::BuildPromptPicker()
{
	const TSharedRef<SHyperAIStudioPromptPicker> Built = SNew(SHyperAIStudioPromptPicker)
		.ProjectRoot(FHyperAIStudioService::GetProjectRoot())
		.OnPresetChosen_Lambda([this](const FHyperAIStudioPromptPreset& Preset, const FString& Text, const TArray<FString>& Blanks)
		{
			InsertPreset(Preset, Text, Blanks);
			if (PromptsButton.IsValid())
			{
				PromptsButton->SetIsOpen(false);
			}
			FocusComposer();
		})
		.OnClose_Lambda([this]()
		{
			if (PromptsButton.IsValid())
			{
				PromptsButton->SetIsOpen(false);
			}
			FocusComposer();
		});
	Picker = Built;
	if (PromptsButton.IsValid())
	{
		PromptsButton->SetMenuContentWidgetToFocus(Built->GetSearchBox());
	}
	return SNew(SBox)
		.WidthOverride(720.0f)
		.HeightOverride(460.0f)
		[
			Built
		];
}

TSharedRef<SWidget> SHyperAIStudioNativeChatView::BuildModeMenu()
{
	FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);
	Menu.BeginSection(NAME_None, LOCTEXT("ModesSection", "Chat mode"));
	for (const FHyperAIStudioChatMode& Mode : HyperAIStudio::ChatModes::List(FHyperAIStudioService::GetProjectRoot()))
	{
		const FText Label = Mode.IsReadOnly() ? FText::Format(LOCTEXT("ModeReadOnlyEntry", "{0} (read only)"), FText::FromString(Mode.Name))
			: Mode.Approval == EHyperAIStudioModeApproval::Ask ? FText::Format(LOCTEXT("ModeAskEntry", "{0} (asks for edits)"), FText::FromString(Mode.Name))
			: FText::FromString(Mode.Name);
		Menu.AddMenuEntry(Label, FText::FromString(Mode.Description), FSlateIcon(), FUIAction(
			FExecuteAction::CreateLambda([this, Mode]() { ChooseMode(Mode); }),
			FCanExecuteAction(),
			FIsActionChecked::CreateLambda([this, Id = Mode.Id]() { return Chat->GetMode().Id == Id; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	}
	Menu.EndSection();
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(LOCTEXT("ManageModes", "Manage Modes..."), LOCTEXT("ManageModesTooltip", "Make, edit, import and export chat modes. The project's are in .hyperai/modes."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() { SHyperAIStudioModesPanel::OpenWindow(); })));
	return Menu.MakeWidget();
}

void SHyperAIStudioNativeChatView::ChooseMode(const FHyperAIStudioChatMode& Mode)
{
	const bool bChanged = Chat->GetMode().Id != Mode.Id;
	Chat->SetMode(Mode, ChatLabel.IsEmpty() ? Chat->GetAgentName() + TEXT(" chat") : ChatLabel);
	if (bChanged)
	{
		Notice = Mode.IsReadOnly()
			? FText::Format(LOCTEXT("ModeReadOnlyNotice", "{0} mode: read only. HyperAI refuses edit plans while this chat is in it."), FText::FromString(Mode.Name)).ToString()
			: FText::Format(LOCTEXT("ModeNotice", "{0} mode. The agent is told with your next message."), FText::FromString(Mode.Name)).ToString();
	}
	OnModeChosen.ExecuteIfBound(Mode);
}

void SHyperAIStudioNativeChatView::FocusComposer()
{
	if (Composer.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(Composer, EFocusCause::SetDirectly);
	}
}

FString SHyperAIStudioNativeChatView::GetComposerText() const
{
	return Composer.IsValid() ? Composer->GetText().ToString() : FString();
}

FHyperAIStudioMessageHints SHyperAIStudioNativeChatView::GetPendingHints() const
{
	FHyperAIStudioMessageHints Hints;
	if (PendingPreset.IsSet())
	{
		Hints.PresetId = PendingPreset->Id;
		Hints.Packs = PendingPreset->Packs;
		Hints.Tools = PendingPreset->Tools;
		Hints.Skills = PendingPreset->Skills;
		Hints.Agent = PendingPreset->Agent;
		Hints.Route = PendingRoute;
	}
	return Hints;
}

void SHyperAIStudioNativeChatView::Submit()
{
	const FString Text = Composer.IsValid() ? Composer->GetText().ToString().TrimStartAndEnd() : FString();
	if (Text.IsEmpty())
	{
		return;
	}
	// A preset's blanks: the first send says what is left to fill, the next sends it as it is.
	if (PendingPreset.IsSet() && !bBlanksNoted)
	{
		TArray<FString> Left;
		for (const FString& Blank : PendingBlanks)
		{
			if (Text.Contains(TEXT("[") + Blank + TEXT("]"), ESearchCase::CaseSensitive))
			{
				Left.Add(TEXT("[") + Blank + TEXT("]"));
			}
		}
		if (!Left.IsEmpty())
		{
			bBlanksNoted = true;
			Notice = FText::Format(LOCTEXT("PresetBlanksLeft", "Fill in: {0}. Send again to send it as it is."), FText::FromString(FString::Join(Left, TEXT(", ")))).ToString();
			PresetNotice = Notice;
			return;
		}
	}
	FHyperAIStudioMessageHints Hints = GetPendingHints();
	if (!bTeam && Hints.Agent == TEXT("team"))
	{
		// Team turned off: the work stays with this agent, so no hand-off to the team goes with it.
		Hints.Agent.Reset();
	}
	if (!Hints.IsEmpty())
	{
		Notice.Reset();
	}
	// Forgotten before the composer empties, or emptying it would also turn off the Team the preset turned on.
	PendingPreset.Reset();
	PendingRoute.Reset();
	PendingBlanks.Reset();
	bBlanksNoted = false;
	bTeamFromPreset = false;
	Composer->SetText(FText::GetEmpty());
	bFollow = true;
	FString Message = Text;
	if (bTeam)
	{
		// A chat that started before the team was added does not know its skill yet, so it gets the skill's file.
		const FString Root = FHyperAIStudioService::GetProjectRoot();
		const bool bAdded = !HyperAIStudio::Agents::IsTeamInstalled(Root);
		if (bAdded)
		{
			HyperAIStudio::Agents::InstallTeam(Root, /*bUpdateOnly=*/false);
			Notice = TEXT("Added HyperAI's team to this project, in .hyperai/agents, with its skill and workflow.");
		}
		Message = HyperAIStudio::Agents::TeamRequest(bAdded ? FString() : Chat->GetAgentName(), Text);
	}
	// A request to the team is the team's to share out; anything else may first go to the agent its task fits.
	else if (OnRoute.IsBound() && OnRoute.Execute(Message, Attached, Hints))
	{
		Attached.Reset();
		RebuildAttachments();
		return;
	}
	Chat->Send(Message, Attached, Hints);
	Attached.Reset();
	RebuildAttachments();
}

bool SHyperAIStudioNativeChatView::Attach(const FString& Path)
{
	using namespace HyperAIStudio::NativeChatView;
	const FString Full = FPaths::ConvertRelativePathToFull(Path);
	if (!IsPictureFile(Full) || !IFileManager::Get().FileExists(*Full))
	{
		Notice = FString::Printf(TEXT("%s is not a PNG, JPG or WebP file."), *FPaths::GetCleanFilename(Path));
		return false;
	}
	Attached.AddUnique(Full);
	Notice.Reset();
	RebuildAttachments();
	FocusComposer();
	return true;
}

void SHyperAIStudioNativeChatView::RebuildAttachments()
{
	if (!AttachmentBox.IsValid())
	{
		return;
	}
	AttachmentBox->ClearChildren();
	for (const FString& Path : Attached)
	{
		AttachmentBox->AddSlot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				Picture([Path]() { return Path; }, FVector2D(96.0f, 72.0f))
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Text(LOCTEXT("RemoveAttachment", "Remove"))
				.ToolTipText(FText::FromString(Path))
				.OnClicked_Lambda([this, Path]()
				{
					Attached.Remove(Path);
					RebuildAttachments();
					return FReply::Handled();
				})
			]
		];
	}
}

FReply SHyperAIStudioNativeChatView::OnDragOver(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	const TSharedPtr<FExternalDragOperation> Files = Event.GetOperationAs<FExternalDragOperation>();
	return Files.IsValid() && Files->HasFiles() && Files->GetFiles().ContainsByPredicate(&HyperAIStudio::NativeChatView::IsPictureFile)
		? FReply::Handled() : FReply::Unhandled();
}

FReply SHyperAIStudioNativeChatView::OnDrop(const FGeometry& Geometry, const FDragDropEvent& Event)
{
	const TSharedPtr<FExternalDragOperation> Files = Event.GetOperationAs<FExternalDragOperation>();
	if (!Files.IsValid() || !Files->HasFiles())
	{
		// Assets and actors belong to the chat tab, which puts their paths in the prompt.
		return FReply::Unhandled();
	}
	bool bAny = false;
	for (const FString& File : Files->GetFiles())
	{
		bAny |= HyperAIStudio::NativeChatView::IsPictureFile(File) && Attach(File);
	}
	return bAny ? FReply::Handled() : FReply::Unhandled();
}

FString SHyperAIStudioNativeChatView::CaptureViewport(FString& OutError)
{
	FViewport* Viewport = GCurrentLevelEditingViewportClient ? GCurrentLevelEditingViewportClient->Viewport : nullptr;
	if (!Viewport && GEditor)
	{
		Viewport = GEditor->GetActiveViewport();
	}
	const FIntPoint Size = Viewport ? Viewport->GetSizeXY() : FIntPoint::ZeroValue;
	TArray<FColor> Pixels;
	if (Size.X < 1 || Size.Y < 1 || !Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
	{
		OutError = TEXT("There is no level viewport to capture, or it could not be read.");
		return FString();
	}
	for (FColor& Pixel : Pixels)
	{
		Pixel.A = 255;
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
	IFileManager::Get().MakeDirectory(*FHyperAIStudioNativeChat::ImageFolder(), /*Tree=*/true);
	const FString File = FHyperAIStudioNativeChat::ImageFolder() / FString::Printf(TEXT("viewport-%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
	if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *File))
	{
		OutError = FString::Printf(TEXT("Could not write %s."), *File);
		return FString();
	}
	return File;
}

bool SHyperAIStudioNativeChatView::SaveToProject(const FHyperAIStudioChatItem& Item, const FString& AgentName, FString& OutProjectPath, FString& OutError)
{
	const FString Source = Item.Images.IsEmpty() ? FString() : Item.Images[0];
	if (Source.IsEmpty() || !IFileManager::Get().FileExists(*Source))
	{
		OutError = TEXT("The picture is no longer on disk.");
		return false;
	}
	FString Folder = GetDefault<UHyperAIStudioSettings>()->ImageOutputFolder;
	if (Folder.IsEmpty())
	{
		Folder = TEXT("Content/References/Generated");
	}
	const FString Batch = FString::Printf(TEXT("chat_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d")));
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Folder / Batch);
	IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);
	FString Target;
	for (int32 Index = 1; Target.IsEmpty() || IFileManager::Get().FileExists(*Target); ++Index)
	{
		Target = Directory / FString::Printf(TEXT("%s_%02d.%s"), *Batch, Index, *FPaths::GetExtension(Source).ToLower());
	}
	if (IFileManager::Get().Copy(*Target, *Source) != COPY_OK)
	{
		OutError = FString::Printf(TEXT("Could not copy the picture to %s."), *Target);
		return false;
	}
	// ponytail: the same sidecar fields the image tools' batches write, so the Images tab and hyper_image_inspect
	// list it; share a writer if a third place ever needs one.
	const TSharedRef<FJsonObject> Sidecar = MakeShared<FJsonObject>();
	Sidecar->SetStringField(TEXT("image"), FPaths::GetCleanFilename(Target));
	Sidecar->SetStringField(TEXT("batch_id"), Batch);
	Sidecar->SetStringField(TEXT("prompt"), Item.Text);
	Sidecar->SetStringField(TEXT("model_id"), AgentName.ToLower() + TEXT("_chat"));
	Sidecar->SetStringField(TEXT("service_id"), TEXT("agent:") + AgentName.ToLower());
	Sidecar->SetNumberField(TEXT("seed"), -1);
	Sidecar->SetStringField(TEXT("generated_utc"), FDateTime::UtcNow().ToIso8601());
	FString Json;
	FJsonSerializer::Serialize(Sidecar, TJsonWriterFactory<>::Create(&Json));
	FFileHelper::SaveStringToFile(Json, *FPaths::ChangeExtension(Target, TEXT("json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	OutProjectPath = Target;
	FPaths::MakePathRelativeTo(OutProjectPath, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	return true;
}

TSharedRef<SWidget> SHyperAIStudioNativeChatView::Picture(TFunction<FString()> Path, const FVector2D& MaxSize)
{
	// Read every paint, so a row made before its picture arrived shows it once it does.
	const TSharedRef<FHyperAIStudioPreviewImage> Image = MakeShared<FHyperAIStudioPreviewImage>();
	return SNew(SBox)
		.MaxDesiredWidth(MaxSize.X)
		.MaxDesiredHeight(MaxSize.Y)
		.ToolTipText_Lambda([Path]() { return FText::FromString(Path()); })
		[
			SNew(SScaleBox)
			.Stretch(EStretch::ScaleToFit)
			[
				SNew(SImage).Image_Lambda([Image, Path]() { return Image->Get(Path()); })
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioNativeChatView::ImageActions(const FItemPtr& Item)
{
	auto Action = [](const FText& Label, const FText& Tooltip, TFunction<void()> Run)
	{
		return SNew(SButton)
			.Text(Label)
			.ToolTipText(Tooltip)
			.OnClicked_Lambda([Run]()
			{
				Run();
				return FReply::Handled();
			});
	};
	const TWeakPtr<FHyperAIStudioChatItem> Weak = Item;
	return SNew(SHorizontalBox)
		.Visibility_Lambda([Item]() { return Item->Images.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			Action(LOCTEXT("OpenInMatch", "Open in Match"), LOCTEXT("OpenInMatchTooltip", "Show it in the Match page, beside what the level looks like."),
				[this, Weak]()
				{
					const TSharedPtr<FHyperAIStudioChatItem> Pinned = Weak.Pin();
					if (!Pinned.IsValid() || Pinned->Images.IsEmpty())
					{
						return;
					}
					// Match only opens pictures inside the project, and Codex keeps its own elsewhere.
					FString Path = Pinned->Images[0];
					if (!FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Path), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())))
					{
						const FString Copy = FHyperAIStudioNativeChat::ImageFolder() / FPaths::GetCleanFilename(Path);
						IFileManager::Get().MakeDirectory(*FHyperAIStudioNativeChat::ImageFolder(), /*Tree=*/true);
						if (IFileManager::Get().Copy(*Copy, *Path) == COPY_OK)
						{
							Path = Copy;
						}
					}
					FHyperAIStudioPreviewPanel::Open(NAME_None, Path);
				})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			Action(LOCTEXT("SaveToProject", "Save to project"), LOCTEXT("SaveToProjectTooltip", "Copy it into the project's generated images, where the Images tab and the image tools find it."),
				[this, Weak]()
				{
					const TSharedPtr<FHyperAIStudioChatItem> Pinned = Weak.Pin();
					FString Saved;
					FString Error;
					if (Pinned.IsValid())
					{
						Notice = SaveToProject(*Pinned, Chat->GetAgentName(), Saved, Error) ? TEXT("Saved to ") + Saved : Error;
					}
				})
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			Action(LOCTEXT("ShowInFolder", "Show in folder"), LOCTEXT("ShowInFolderTooltip", "Open the folder the picture is in."),
				[Weak]()
				{
					if (const TSharedPtr<FHyperAIStudioChatItem> Pinned = Weak.Pin(); Pinned.IsValid() && !Pinned->Images.IsEmpty())
					{
						FPlatformProcess::ExploreFolder(*Pinned->Images[0]);
					}
				})
		];
}

void SHyperAIStudioNativeChatView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// Rows grow as text streams in, so following is checked every frame, not only when a row arrives.
	if (bFollow && Scroll.IsValid() && Scroll->GetScrollOffset() < Scroll->GetScrollOffsetOfEnd())
	{
		Scroll->ScrollToEnd();
	}
}

bool SHyperAIStudioNativeChatView::IsAtEnd() const
{
	return Scroll.IsValid() && Scroll->GetScrollOffset() >= Scroll->GetScrollOffsetOfEnd() - 1.0f;
}

void SHyperAIStudioNativeChatView::SetOpen(const FString& ItemId, const bool bOpen)
{
	if (bOpen)
	{
		Open.Add(ItemId);
	}
	else
	{
		Open.Remove(ItemId);
	}
}

void SHyperAIStudioNativeChatView::Refresh()
{
	// Items are only ever added to a transcript; anything else is a new one, built again.
	const TArray<FItemPtr>& Items = Chat->GetItems();
	if (Items.Num() < Rows.Num() || (!Rows.IsEmpty() && Items[Rows.Num() - 1] != Rows.Last()))
	{
		Rows.Reset();
		if (Transcript.IsValid())
		{
			Transcript->ClearChildren();
		}
	}
	for (int32 Index = Rows.Num(); Index < Items.Num(); ++Index)
	{
		Rows.Add(Items[Index]);
		if (Transcript.IsValid())
		{
			Transcript->AddSlot().AutoHeight().Padding(FMargin(2.0f, 5.0f))
			[
				MakeRow(Items[Index])
			];
		}
	}
	// The plan is small and changes rarely; rebuilding it whole is simpler than diffing it.
	if (PlanBox.IsValid())
	{
		PlanBox->ClearChildren();
		const TArray<FHyperAIStudioChatPlanStep>& Plan = Chat->GetPlan();
		if (!Plan.IsEmpty())
		{
			TSharedRef<SVerticalBox> Steps = SNew(SVerticalBox);
			for (const FHyperAIStudioChatPlanStep& Step : Plan)
			{
				const bool bDone = Step.Status == TEXT("completed");
				const bool bNow = Step.Status == TEXT("inProgress");
				Steps->AddSlot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("%s %s"), bDone ? TEXT("[x]") : bNow ? TEXT("[>]") : TEXT("[ ]"), *Step.Text)))
					.ColorAndOpacity(bNow ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				];
			}
			PlanBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(FMargin(8.0f, 6.0f))
				[
					Steps
				]
			];
		}
	}
}

TSharedRef<SWidget> SHyperAIStudioNativeChatView::MakeStep(const FItemPtr& Item)
{
	using namespace HyperAIStudio::NativeChatView;
	using namespace HyperAIStudio::UI;
	const FString Id = Item->Id;
	const bool bReasoning = Item->Kind == EKind::Reasoning;
	return SNew(SBorder)
		.BorderImage(Style().GetBrush("HyperAIStudio.Card"))
		.Padding(FMargin(8.0f, 4.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(0.0f, 2.0f))
				.ToolTipText(LOCTEXT("ToggleDetails", "Show or hide the details."))
				.OnClicked_Lambda([this, Id]()
				{
					SetOpen(Id, !Open.Contains(Id));
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SImage)
						.Image_Lambda([this, Id]() { return FAppStyle::GetBrush(Open.Contains(Id) ? "Icons.ChevronDown" : "Icons.ChevronRight"); })
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(SImage)
						.Image(HyperAIStudio::UI::Icon(KindIcon(Item->Kind)))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(KindLabel(Item->Kind))
						.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(STextBlock)
						.Font(bReasoning ? FCoreStyle::GetDefaultFontStyle("Italic", 9) : Mono())
						.ColorAndOpacity(bReasoning ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.Text_Lambda([Item, bReasoning]()
						{
							const FString& Source = bReasoning ? Item->Text : Item->Title;
							int32 Newline = INDEX_NONE;
							return FText::FromString((Source.FindChar(TEXT('\n'), Newline) ? Source.Left(Newline) : Source).Left(200));
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.0f, 0.0f, 2.0f, 0.0f))
					[
						SNew(SBox)
						.Visibility_Lambda([Item]() { return Item->Status.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
						[
							Chip(TAttribute<FText>::CreateLambda([Item]() { return FText::FromString(Item->Status); }),
								TAttribute<FSlateColor>::CreateLambda([Item]() { return StatusColor(StepStatus(Item->Status)); }))
						]
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(22.0f, 4.0f, 2.0f, 4.0f))
			[
				SNew(SBox)
				.MaxDesiredHeight(340.0f)
				.Visibility_Lambda([this, Id]() { return Open.Contains(Id) ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SHyperAIStudioMarkdown)
						.Markdown_Lambda([Item]() { return StepMarkdown(*Item); })
					]
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioNativeChatView::MakeRow(const FItemPtr& Item)
{
	using namespace HyperAIStudio::NativeChatView;
	TSharedRef<SWidget> Content = SNullWidget::NullWidget;
	const FString Id = Item->Id;
	switch (Item->Kind)
	{
	case EKind::User:
	{
		TSharedRef<SWrapBox> Attachments = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.0f, 6.0f));
		for (const FString& Path : Item->Images)
		{
			Attachments->AddSlot()[Picture([Path]() { return Path; }, FVector2D(160.0f, 120.0f))];
		}
		Content = SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(10.0f, 7.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(LOCTEXT("You", "You")).TextStyle(&HyperAIStudio::UI::Style(), "HyperAIStudio.Text.Caption")
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					LiveText(Item)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					Attachments
				]
			];
		break;
	}
	case EKind::Agent:
		// The reply as a report: headings, tables with status chips, code and pictures, as it streams in.
		Content = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHyperAIStudioMarkdown)
				.Markdown_Lambda([Item]() { return Item->Text; })
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
			[
				SNew(SBox)
				.Visibility_Lambda([this, Item]()
				{
					const bool bStreaming = Chat->IsBusy() && !Rows.IsEmpty() && Rows.Last() == Item;
					return bStreaming || Item->Text.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
				})
				[
					HyperAIStudio::Markdown::ReportActions(
						[this]() { return FText::Format(LOCTEXT("ReplyTitle", "{0} reply"), FText::FromString(Chat->GetAgentName())).ToString(); },
						[Item]() { return Item->Text; })
				]
			];
		break;
	case EKind::Reasoning:
	case EKind::Tool:
	case EKind::Command:
	case EKind::FileChange:
		Content = MakeStep(Item);
		break;
	case EKind::Image:
		Content = SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("KindImage", "Image"))
					.Font(FAppStyle::GetFontStyle("SmallFontBold"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text_Lambda([Item]() { return FText::FromString(Item->Title); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.Text_Lambda([Item]() { return FText::FromString(Item->Status == TEXT("running") ? TEXT("drawing...") : Item->Status); })
					.ColorAndOpacity_Lambda([Item]() { return StatusColor(Item->Status); })
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				Picture([Item]() { return Item->Images.IsEmpty() ? FString() : Item->Images[0]; }, FVector2D(420.0f, 300.0f))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				LiveText(Item, /*bMono=*/false, /*bMuted=*/true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				ImageActions(Item)
			];
		break;
	case EKind::Approval:
	{
		TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
		for (int32 Index = 0; Index < Item->Choices.Num(); ++Index)
		{
			Buttons->AddSlot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton)
				.Text(FText::FromString(Item->Choices[Index]))
				.IsEnabled_Lambda([Item]() { return !Item->bAnswered; })
				.OnClicked_Lambda([this, Id, Index]()
				{
					Chat->Answer(Id, Index);
					return FReply::Handled();
				})
			];
		}
		Content = SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.BorderBackgroundColor(FSlateColor(FStyleColors::AccentYellow))
			.Padding(FMargin(10.0f, 8.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(FText::FromString(Item->Title)).Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
				[
					LiveText(Item, /*bMono=*/true)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					Buttons
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Visibility_Lambda([Item]() { return Item->bAnswered ? EVisibility::Visible : EVisibility::Collapsed; })
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([Item]() { return FText::Format(LOCTEXT("Answered", "Answered: {0}"), FText::FromString(Item->Status)); })
				]
			];
		break;
	}
	case EKind::Error:
		Content = SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(FStyleColors::AccentRed))
			.Text_Lambda([Item]() { return FText::FromString(Item->Text); });
		break;
	default:
		Content = SNew(STextBlock)
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([Item]() { return FText::FromString(Item->Text); });
		break;
	}
	return Content;
}

#undef LOCTEXT_NAMESPACE
