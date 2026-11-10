// Games by Hyper 2026.

#include "SHyperAIStudioKeysPanel.h"

#include "Dialog/SCustomDialog.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioStyle.h"
#include "HyperAIStudioApiOptions.h"
#include "HyperAIStudioService.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioKeysPanel"

void SHyperAIStudioKeysPanel::Construct(const FArguments& InArgs)
{
	const FString ProjectRoot = FHyperAIStudioService::GetProjectRoot();
	const TSharedRef<int32> OutputTokens = MakeShared<int32>(HyperAIStudio::ApiOptions::MaxOutputTokens(ProjectRoot));
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Title", "API keys & authentication"))
			.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.PageTitle")
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("Subtitle", "Set a key below, then choose Claude API, OpenAI API or Gemini API in Chat. These routes send generation requests directly to the provider; no agent CLI or balance check is required. Stored keys take precedence over environment variables and stay in Windows Credential Manager. Provider billing and eligible promotional credits apply to the key's account. HyperAI's tool spend cap does not limit agent API usage."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock).AutoWrapText(true)
			.Text(LOCTEXT("OutputLimitHelp", "Direct API output: 0 selects Auto. Claude uses its model's maximum; OpenAI and Gemini use provider defaults. A positive number sets a custom limit. Changes apply to the next request in either view. Provider limits still apply. Uncheck Native View in Chat to use the API terminal."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[ SNew(STextBlock).Text(LOCTEXT("OutputLimit", "Max output tokens (0 = Auto)")) ]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SNumericEntryBox<int32>).MinValue(0).AllowSpin(false).MinDesiredValueWidth(120.0f)
				.Value_Lambda([OutputTokens]() { return TOptional<int32>(*OutputTokens); })
				.OnValueCommitted_Lambda([ProjectRoot, OutputTokens](int32 Value, ETextCommit::Type)
				{
					if (HyperAIStudio::ApiOptions::SetMaxOutputTokens(ProjectRoot, Value)) { *OutputTokens = Value; }
					else { FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("OutputSaveFailed", "The API output limit could not be saved. Check project folder permissions.")); }
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock).AutoWrapText(true)
			.Text(LOCTEXT("DesktopAuth", "Desktop editors: VS Code/Copilot > Chat: Manage Language Models > Add Models; Cursor > Settings > Models. Enter provider keys there. These editors manage their own credentials; the Cursor Agent CLI key below is a separate Cursor account key."))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).Text(LOCTEXT("VSCodeAuth", "VS Code API setup"))
				.OnClicked_Lambda([]() { FPlatformProcess::LaunchURL(TEXT("https://code.visualstudio.com/docs/agent-customization/language-models"), nullptr, nullptr); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f)
			[
				SNew(SButton).Text(LOCTEXT("CursorAuth", "Cursor API setup"))
				.OnClicked_Lambda([]() { FPlatformProcess::LaunchURL(TEXT("https://cursor.com/docs/settings/api-keys"), nullptr, nullptr); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(Rows, SVerticalBox)
		]
	];
	Rebuild();
}

void SHyperAIStudioKeysPanel::Rebuild()
{
	if (!Rows.IsValid())
	{
		return;
	}
	Rows->ClearChildren();
	FString Group = TEXT("\x01");
	for (const FHyperAIStudioKeySlot& Slot : HyperAIStudio::Credentials::Slots())
	{
		const FString SlotGroup = Slot.Agent.IsEmpty() ? FString(TEXT("Providers for HyperAI's tools")) : Slot.Agent;
		if (SlotGroup != Group)
		{
			Group = SlotGroup;
			Rows->AddSlot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
					.Text(Slot.Agent.IsEmpty() ? LOCTEXT("ProvidersGroup", "Providers for HyperAI's tools") : FText::FromString(
						Slot.Agent == TEXT("Claude Code") ? TEXT("Claude / Anthropic") : Slot.Agent == TEXT("Codex") ? TEXT("OpenAI / Codex")
						: Slot.Agent == TEXT("Gemini") ? TEXT("Google / Gemini") : Slot.Agent))
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			];
		}
		FRow Row;
		Row.Slot = Slot;
		const FString Stored = HyperAIStudio::Credentials::ReadSlot(Slot.Id).TrimStartAndEnd();
		Row.bStored = !Stored.IsEmpty();
		Row.bFromEnvironment = !Row.bStored && !FPlatformMisc::GetEnvironmentVariable(*Slot.Variable).TrimStartAndEnd().IsEmpty();
		Row.Shown = Row.bStored ? (Slot.bSecret ? HyperAIStudio::Credentials::Mask(Stored) : Stored) : FString();
		Rows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
		[
			BuildRow(Row)
		];
	}
}

TSharedRef<SWidget> SHyperAIStudioKeysPanel::BuildRow(const FRow& Row)
{
	const FHyperAIStudioKeySlot Slot = Row.Slot;
	FString KeyUrl;
	if (Slot.Variable == TEXT("ANTHROPIC_API_KEY")) { KeyUrl = TEXT("https://platform.claude.com/settings/keys"); }
	else if (Slot.Variable == TEXT("OPENAI_API_KEY")) { KeyUrl = TEXT("https://platform.openai.com/api-keys"); }
	else if (Slot.Variable == TEXT("GEMINI_API_KEY")) { KeyUrl = TEXT("https://aistudio.google.com/apikey"); }
	else if (Slot.Variable == TEXT("CURSOR_API_KEY")) { KeyUrl = TEXT("https://cursor.com/dashboard?tab=integrations"); }
	const FText State = Row.bStored ? FText::FromString(Row.Shown)
		: Row.bFromEnvironment ? FText::Format(LOCTEXT("FromEnvironment", "{0} is set in your environment"), FText::FromString(Slot.Variable))
		: LOCTEXT("NotSet", "Not set");
	FText Tip = FText::Format(LOCTEXT("RowTip", "{0}\nPassed as {1}."), FText::FromString(Slot.Note), FText::FromString(Slot.Variable));
	if (!Slot.bVerified)
	{
		Tip = FText::Format(LOCTEXT("RowTipUnverified", "{0}\nThe agent's documentation does not name this variable; it may not read it."), Tip);
	}
	const TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.34f).VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(FText::FromString(Slot.Label)).ToolTipText(Tip) ]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Slot.Variable))
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(FText::FromString(Slot.bVerified ? Slot.Note : Slot.Note + TEXT(" Verify the provider setup in your installed agent."))).AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.0f, 3.0f)
			[
				SNew(SButton).Text(LOCTEXT("GetKey", "Get key"))
				.Visibility(KeyUrl.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				.OnClicked_Lambda([KeyUrl]() { FPlatformProcess::LaunchURL(*KeyUrl, nullptr, nullptr); return FReply::Handled(); })
			]
		]
		+ SHorizontalBox::Slot().FillWidth(0.36f).VAlign(VAlign_Center).Padding(8.0f, 0.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(State)
				.ColorAndOpacity(Row.bStored ? FSlateColor(FStyleColors::AccentGreen) : FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.Visibility_Lambda([this, Id = Slot.Id]() { return Results.Contains(Id) ? EVisibility::Visible : EVisibility::Collapsed; })
				.ColorAndOpacity_Lambda([this, Id = Slot.Id]()
				{
					const TPair<bool, FString>* Result = Results.Find(Id);
					return Result && Result->Key ? FSlateColor(FStyleColors::AccentGreen) : FSlateColor(FStyleColors::AccentYellow);
				})
				.Text_Lambda([this, Id = Slot.Id]()
				{
					const TPair<bool, FString>* Result = Results.Find(Id);
					return Result ? FText::FromString(Result->Value) : FText::GetEmpty();
				})
			]
		]
		+ SHorizontalBox::Slot().FillWidth(0.30f).VAlign(VAlign_Center).HAlign(HAlign_Right)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
			[
				SNew(SButton)
				.Text(Row.bStored ? LOCTEXT("Change", "Change...") : LOCTEXT("Set", "Set..."))
				.ToolTipText(Slot.bSecret ? LOCTEXT("SetKeyTip", "Store a key in Windows Credential Manager.") : LOCTEXT("SetUrlTip", "Store an address for this agent."))
				.OnClicked_Lambda([this, Slot]()
				{
					EditSlot(Slot);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Test", "Check key"))
				.Visibility(!Slot.Test.IsEmpty() && (Row.bStored || Row.bFromEnvironment) ? EVisibility::Visible : EVisibility::Collapsed)
				.ToolTipText(LOCTEXT("TestTip", "Make one free authenticated call (a model list or an account read) to see whether the key works. Nothing is generated or billed."))
				.OnClicked_Lambda([this, Id = Slot.Id]()
				{
					Results.Add(Id, TPair<bool, FString>(false, LOCTEXT("Testing", "Testing...").ToString()));
					const TWeakPtr<SWidget> Weak = AsShared();
					HyperAIStudio::Credentials::TestSlot(Id, [this, Weak, Id](const bool bOk, const FString& Message)
					{
						if (Weak.IsValid())
						{
							Results.Add(Id, TPair<bool, FString>(bOk, Message));
						}
					});
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Remove", "Remove"))
				.Visibility(Row.bStored ? EVisibility::Visible : EVisibility::Collapsed)
				.ToolTipText(LOCTEXT("RemoveTip", "Delete the stored value from Windows Credential Manager."))
				.OnClicked_Lambda([this, Slot]()
				{
					if (FMessageDialog::Open(EAppMsgType::YesNo, FText::Format(LOCTEXT("RemoveConfirm", "Remove the stored {0} for {1}?"),
						FText::FromString(Slot.Label), FText::FromString(Slot.Agent.IsEmpty() ? Slot.Variable : Slot.Agent))) == EAppReturnType::Yes)
					{
						HyperAIStudio::Credentials::Remove(HyperAIStudio::Credentials::TargetName(Slot.Id));
						Results.Remove(Slot.Id);
						Rebuild();
					}
					return FReply::Handled();
				})
			]
		];
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(9.0f, 6.0f))
		[
			Line
		];
}

void SHyperAIStudioKeysPanel::EditSlot(const FHyperAIStudioKeySlot& Slot)
{
	const TSharedRef<SEditableTextBox> Box = SNew(SEditableTextBox)
		.IsPassword(Slot.bSecret)
		.HintText(Slot.bSecret ? LOCTEXT("KeyHint", "Paste the key") : LOCTEXT("UrlHint", "https://..."))
		.Text(Slot.bSecret ? FText::GetEmpty() : FText::FromString(HyperAIStudio::Credentials::ReadSlot(Slot.Id)));
	const TSharedRef<SCustomDialog> Dialog = SNew(SCustomDialog)
		.Title(FText::Format(LOCTEXT("EditTitle", "{0}: {1}"), FText::FromString(Slot.Agent.IsEmpty() ? FString(TEXT("HyperAI")) : Slot.Agent), FText::FromString(Slot.Label)))
		.Content()
		[
			SNew(SBox)
			.WidthOverride(520.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Slot.Note))
				]
				+ SVerticalBox::Slot().AutoHeight()[ Box ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(FText::Format(LOCTEXT("EditWhere", "Kept in Windows Credential Manager as {0}, and given to the agent as {1}."),
						FText::FromString(HyperAIStudio::Credentials::TargetName(Slot.Id)), FText::FromString(Slot.Variable)))
				]
			]
		]
		.Buttons({SCustomDialog::FButton(LOCTEXT("Save", "Save")), SCustomDialog::FButton(LOCTEXT("Cancel", "Cancel"))});
	if (Dialog->ShowModal() != 0)
	{
		return;
	}
	const FString Value = Box->GetText().ToString().TrimStartAndEnd();
	if (!Slot.bSecret && !Value.IsEmpty() && !Value.StartsWith(TEXT("http://")) && !Value.StartsWith(TEXT("https://")))
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NotUrl", "An address starts with http:// or https://."));
		return;
	}
	FString Error;
	if (Value.IsEmpty())
	{
		HyperAIStudio::Credentials::Remove(HyperAIStudio::Credentials::TargetName(Slot.Id));
	}
	else if (!HyperAIStudio::Credentials::Write(HyperAIStudio::Credentials::TargetName(Slot.Id), Value, Error))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
	}
	Results.Remove(Slot.Id);
	Rebuild();
}

#undef LOCTEXT_NAMESPACE
