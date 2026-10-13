// Games by Hyper 2026.

#include "SHyperAIStudioKeysPanel.h"

#include "Dialog/SCustomDialog.h"
#include "HAL/PlatformMisc.h"
#include "HyperAIStudioStyle.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioKeysPanel"

void SHyperAIStudioKeysPanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Title", "Your own keys"))
			.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.PageTitle")
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("Subtitle", "Give an agent your own API key or an OpenAI-compatible address, and give HyperAI's image, audio and 3D tools their providers' keys. Keys are kept in Windows Credential Manager for your user (targets named HyperAIStudio/...), never in the project, an ini file, a log or Activity, and go only into the environment of the agent HyperAI starts. A key set here wins over the same environment variable. Agents read a new key when their chat next starts."))
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
				.Text(Slot.Agent.IsEmpty() ? LOCTEXT("ProvidersGroup", "Providers for HyperAI's tools") : FText::FromString(Slot.Agent))
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
				.Text(LOCTEXT("Test", "Test"))
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
