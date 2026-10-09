// Games by Hyper 2026.

#include "SHyperAIStudioAutoApprovalDialog.h"

#include "CoreGlobals.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioStyle.h"
#include "Misc/App.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#define LOCTEXT_NAMESPACE "HyperAIStudioAutoApproval"

namespace HyperAIStudio::AutoApprovalDialog
{
	constexpr float Width = 600.0f;
	constexpr int32 ShownPending = 5;

	int32 IndexOf(const EHyperAIStudioApprovalCategory Category)
	{
		return static_cast<int32>(Category);
	}

	FText AssetName(const FString& Target)
	{
		FString Name = Target;
		int32 Dot = INDEX_NONE;
		if (Name.FindLastChar(TEXT('.'), Dot))
		{
			Name.LeftInline(Dot);
		}
		int32 Slash = INDEX_NONE;
		return FText::FromString(Name.FindLastChar(TEXT('/'), Slash) ? Name.RightChop(Slash + 1) : Name);
	}

	TSharedRef<SWidget> Paragraph(const FText& Text, const bool bBold = false)
	{
		return SNew(STextBlock)
			.Text(Text)
			.AutoWrapText(true)
			.Font(bBold ? FCoreStyle::GetDefaultFontStyle("Bold", 10) : FCoreStyle::GetDefaultFontStyle("Regular", 10));
	}

	/**
	 * The most the scrolling part may take: 70% of the screen the editor works on, in Slate units, so the buttons
	 * fit on a 768-pixel-high screen or a 1080p one at 150%. Unlimited where no screen is known, as in a headless test.
	 */
	FOptionalSize MaxBodyHeight()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return FOptionalSize();
		}
		const FSlateRect Area = FSlateApplication::Get().GetPreferredWorkArea();
		const float Scale = FMath::Max(FPlatformApplicationMisc::GetDPIScaleFactorAtPoint(Area.Left, Area.Top), 0.5f);
		const float Height = Area.GetSize().Y / Scale;
		return Height > 200.0f ? FOptionalSize(FMath::Max(240.0f, Height * 0.7f)) : FOptionalSize();
	}
}

void SHyperAIStudioAutoApprovalDialog::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::AutoApprovalDialog;
	Window = InArgs._Window;
	PendingAtOpen = FHyperAIStudioApprovalGate::GetPending();
	ConfirmNatively = [this](const FText& Caption, const FText& Body)
	{
		return ShowNativeConfirmation(Window.Pin(), Caption, Body);
	};

	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const bool bEditsAlreadyRun = Settings && !Settings->bRequireApprovalForAgentEdits;
	FNumberFormattingOptions Money;
	Money.MinimumFractionalDigits = 2;
	Money.MaximumFractionalDigits = 2;
	const FText SpendCap = FText::AsNumber(Settings ? Settings->ExternalServiceSessionSpendCap : 0.0f, &Money);

	const TSharedRef<SWidget> EditsNote = SNew(STextBlock)
		.Visibility(bEditsAlreadyRun ? EVisibility::Visible : EVisibility::Collapsed)
		.AutoWrapText(true)
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		.Text(LOCTEXT("EditsAlreadyRun", "Edits already run without asking: Require Approval For Agent Edits is off in Project Settings."));
	const TSharedRef<SWidget> ScriptsNote = SNew(STextBlock)
		.Visibility_Lambda([this]() { return bTicked[IndexOf(EHyperAIStudioApprovalCategory::Scripts)] ? EVisibility::Visible : EVisibility::Collapsed; })
		.AutoWrapText(true)
		.ColorAndOpacity(FStyleColors::AccentRed)
		.Text(LOCTEXT("ScriptsWarning", "Ticking this lets agents run any code on this computer. It is as strong as ticking everything."));

	// Everything between the heading and the buttons scrolls, so Cancel and Turn On stay on a short screen.
	const TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Paragraph(LOCTEXT("Intro",
				"While auto-approval is on, HyperAI approves each new request in the categories you tick as if you had clicked Approve in Activity: it runs through the same queue, with the same checks. Requests in other categories still wait for you."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			Paragraph(LOCTEXT("Scope",
				"This applies to every agent and MCP client connected to this editor, not just HyperAI chats: Claude Code, Codex, Gemini, or any other program that reaches this editor's Unreal MCP server."), true)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			Paragraph(FText::Format(LOCTEXT("EndingUnstarted",
				"It is never saved. It ends when its time runs out, when you press Stop, when the editor closes, when the spend cap or an external service's or image model's settings change, or by itself after {0} approvals, {1} external-effect requests, or {2} failures in a row. What it approved that has not started by then waits for your approval again."),
				FHyperAIStudioAutoApproval::MaxApprovals, FHyperAIStudioAutoApproval::MaxExternalEffects,
				FHyperAIStudioAutoApproval::MaxConsecutiveFailures))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 14.0f, 0.0f, 0.0f)
		[
			BuildCategory(EHyperAIStudioApprovalCategory::Edits, LOCTEXT("EditsMostlyLabel", "Edits (mostly undoable)"),
				LOCTEXT("EditsChecksBody",
					"Changes agents plan to assets and levels: Blueprints and Animation Blueprints, materials, animation, gameplay abilities, Niagara, PCG and scatter, landscape and mesh terrain, foliage, lighting, level blockout, arranging actors, sequences, World Partition, UI, modeling and placing models, retargeting, pose search, texture graphs and sound. Also playtests (they start Play In Editor and may record a video), movie renders written to disk, and Python scripts in which the runner's best-effort checks found nothing risky: they run inside one undo step behind a guard that is not a sandbox, and one may open another level when nothing is unsaved. Each change is saved to the project when it finishes; most can be taken back with Undo."),
				EditsNote)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			BuildCategory(EHyperAIStudioApprovalCategory::Scripts, LOCTEXT("ScriptsLabel", "Scripts that need approval (cannot be undone)"),
				LOCTEXT("ScriptsBody",
					"Editor Python from hyper_script_run that the runner could not prove safe: it deletes, renames or merges assets or folders, destroys actors or components, imports files from disk, opens another level while this one has unsaved changes, runs console commands or more Python, writes files outside its own folder, closes the editor, or imports modules such as os, subprocess, socket, ctypes or requests. These scripts run without an undo step and without the guard, with your account's rights: they can delete or overwrite any of your files, start programs, use the network, and read keys kept in environment variables."),
				ScriptsNote)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			BuildCategory(EHyperAIStudioApprovalCategory::Destructive, LOCTEXT("DestructiveLabel", "Destructive tools (cannot be undone)"),
				LOCTEXT("DestructiveBody",
					"Plans that delete or replace things, such as UI plans (hyper_ui_apply_plan) that delete or replace widgets, bindings or animations, and any other plan its tool marks destructive. The one-time permission the editor requires for each of these is issued without your click. Agent scripts are not part of this; they are under Scripts."),
				SNullWidget::NullWidget)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			BuildCategory(EHyperAIStudioApprovalCategory::ExternalEffects, LOCTEXT("ExternalLabel", "External effects (spend money, leave this computer)"),
				FText::Format(LOCTEXT("ExternalCapBody",
					"Requests that reach outside the editor: generating images (hyper_image_generate, with Codex, Gemini or a configured image service), generating 3D models (hyper_mesh_generate, with Tripo, Meshy or Hi3D), and submitting files to source control (hyper_source_control_submit, through Perforce, Git, Plastic or Lore, where your team receives them). Prompts and pictures are sent to those services. One image request may make up to 8 paid calls. Paid calls still stop at this session's spend cap of {0}, and auto-approval turns off if the cap or a service's or model's price changes while it is on; agent models use your agent plan."),
					SpendCap),
				SNullWidget::NullWidget)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 14.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 12.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("TurnOffAfter", "Turn off after"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
			+ SHorizontalBox::Slot().AutoWidth()[BuildDuration(EHyperAIStudioAutoApprovalDuration::FifteenMinutes, LOCTEXT("FifteenMinutes", "15 minutes"))]
			+ SHorizontalBox::Slot().AutoWidth()[BuildDuration(EHyperAIStudioAutoApprovalDuration::OneHour, LOCTEXT("OneHour", "1 hour"))]
			+ SHorizontalBox::Slot().AutoWidth()[BuildDuration(EHyperAIStudioAutoApprovalDuration::FourHours, LOCTEXT("FourHours", "4 hours"))]
			+ SHorizontalBox::Slot().AutoWidth()[BuildDuration(EHyperAIStudioAutoApprovalDuration::UntilEditorCloses, LOCTEXT("WhenEditorCloses", "When the editor closes"))]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			BuildPending()
		];

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Panel"))
		.Padding(16.0f)
		[
			SNew(SBox)
			.WidthOverride(Width)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush(TEXT("Icons.WarningWithColor")))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Heading", "Agents' requests will run without asking you."))
						.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.Title")
						.AutoWrapText(true)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.MaxDesiredHeight_Static(&MaxBodyHeight)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							Body
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Right)
				.Padding(0.0f, 16.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SAssignNew(CancelButton, SButton)
						.Text(LOCTEXT("Cancel", "Cancel"))
						.OnClicked(this, &SHyperAIStudioAutoApprovalDialog::OnCancelClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("TurnOn", "Turn On Auto-Approval"))
						.ButtonColorAndOpacity(FStyleColors::AccentRed)
						.IsEnabled_Lambda([this]() { return CanTurnOn(); })
						.ToolTipText_Lambda([this]() { return CanTurnOn() ? FText::GetEmpty() : LOCTEXT("TickOne", "Tick at least one category."); })
						.OnClicked(this, &SHyperAIStudioAutoApprovalDialog::OnTurnOnClicked)
					]
				]
			]
		]
	];
}

TSharedRef<SWidget> SHyperAIStudioAutoApprovalDialog::BuildCategory(
	const EHyperAIStudioApprovalCategory Category,
	const FText& Label,
	const FText& Body,
	const TSharedRef<SWidget>& Note)
{
	using namespace HyperAIStudio::AutoApprovalDialog;
	const int32 Index = IndexOf(Category);
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this, Index]() { return bTicked[Index] ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Index](const ECheckBoxState State) { bTicked[Index] = State == ECheckBoxState::Checked; })
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(24.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(Body)
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(24.0f, 2.0f, 0.0f, 0.0f)
		[
			Note
		];
}

TSharedRef<SWidget> SHyperAIStudioAutoApprovalDialog::BuildDuration(const EHyperAIStudioAutoApprovalDuration Choice, const FText& Label)
{
	return SNew(SBox)
		.Padding(FMargin(0.0f, 0.0f, 12.0f, 0.0f))
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "RadioButton")
			.IsChecked_Lambda([this, Choice]() { return Duration == Choice ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Choice](const ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					Duration = Choice;
				}
			})
			[
				SNew(STextBlock)
				.Text(Label)
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioAutoApprovalDialog::BuildPending() const
{
	using namespace HyperAIStudio::AutoApprovalDialog;
	if (PendingAtOpen.IsEmpty())
	{
		return SNullWidget::NullWidget;
	}
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Paragraph(FText::Format(LOCTEXT("PendingIntro",
				"{0} {0}|plural(one=request is,other=requests are) waiting now. They keep waiting for your click: auto-approval covers only requests made after you turn it on."),
				PendingAtOpen.Num()))
		];
	for (int32 Index = 0; Index < PendingAtOpen.Num() && Index < ShownPending; ++Index)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(12.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("PendingLine", "{0} on {1}"),
				FText::FromString(PendingAtOpen[Index].Summary.ToolName), AssetName(PendingAtOpen[Index].Summary.EffectTarget)))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}
	if (PendingAtOpen.Num() > ShownPending)
	{
		Box->AddSlot()
		.AutoHeight()
		.Padding(12.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("PendingMore", "and {0} more"), PendingAtOpen.Num() - ShownPending))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}
	return SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Badge"))
		.Padding(FMargin(8.0f, 6.0f))
		[
			Box
		];
}

TSharedPtr<SWidget> SHyperAIStudioAutoApprovalDialog::FocusOnOpen() const
{
	return CancelButton;
}

FReply SHyperAIStudioAutoApprovalDialog::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		return OnCancelClicked();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

TArray<EHyperAIStudioApprovalCategory> SHyperAIStudioAutoApprovalDialog::GetChosen() const
{
	TArray<EHyperAIStudioApprovalCategory> Chosen;
	for (int32 Index = 0; Index < CategoryCount; ++Index)
	{
		if (bTicked[Index])
		{
			Chosen.Add(static_cast<EHyperAIStudioApprovalCategory>(Index));
		}
	}
	return Chosen;
}

bool SHyperAIStudioAutoApprovalDialog::CanTurnOn() const
{
	return !bClosed && !GetChosen().IsEmpty();
}

FText SHyperAIStudioAutoApprovalDialog::BuildConfirmation() const
{
	FText When;
	switch (Duration)
	{
	case EHyperAIStudioAutoApprovalDuration::FifteenMinutes: When = LOCTEXT("ConfirmFifteenMinutes", "For the next 15 minutes"); break;
	case EHyperAIStudioAutoApprovalDuration::FourHours: When = LOCTEXT("ConfirmFourHours", "For the next 4 hours"); break;
	case EHyperAIStudioAutoApprovalDuration::UntilEditorCloses: When = LOCTEXT("ConfirmUntilClose", "Until the editor closes"); break;
	default: When = LOCTEXT("ConfirmOneHour", "For the next hour"); break;
	}
	TArray<FText> Lines;
	for (const EHyperAIStudioApprovalCategory Category : GetChosen())
	{
		switch (Category)
		{
		case EHyperAIStudioApprovalCategory::Scripts:
			Lines.Add(LOCTEXT("ConfirmScripts", "- Scripts that need approval (can run any code on this computer)"));
			break;
		case EHyperAIStudioApprovalCategory::Destructive:
			Lines.Add(LOCTEXT("ConfirmDestructive", "- Destructive tools (cannot be undone)"));
			break;
		case EHyperAIStudioApprovalCategory::ExternalEffects:
			Lines.Add(LOCTEXT("ConfirmExternal", "- External effects (can spend money and send data off this computer)"));
			break;
		default:
			Lines.Add(LOCTEXT("ConfirmEditsMostly", "- Edits (mostly undoable asset and level changes, and scripts the runner's checks passed)"));
			break;
		}
	}
	return FText::Format(LOCTEXT("ConfirmBody",
		"{0}, agents' requests in these categories will run without asking you:\n\n{1}\n\nThis covers every agent and MCP client connected to this editor, not only HyperAI chats.\n\nIf you did not open this yourself, press Cancel."),
		When, FText::Join(FText::FromString(TEXT("\n")), Lines));
}

FReply SHyperAIStudioAutoApprovalDialog::OnTurnOnClicked()
{
	if (!CanTurnOn())
	{
		return FReply::Handled();
	}
	// A second, native confirmation. An MCP-only client can synthesize Slate input but cannot reach a Windows message
	// box. It is no barrier to a program with input of its own at the OS level (an agent with a shell can send it
	// keys); such agents are held by their own client's permission prompts.
	++NativeConfirmations;
	if (!ConfirmNatively || !ConfirmNatively(LOCTEXT("ConfirmCaption", "HyperAI Studio: turn on auto-approval?"), BuildConfirmation()))
	{
		return FReply::Handled();
	}
#if WITH_DEV_AUTOMATION_TESTS
	if (TestScope.IsSet())
	{
		FHyperAIStudioAutoApproval::TestScopeForNextActivation() = TestScope;
	}
#endif
	FString Error;
	if (FHyperAIStudioAutoApproval::Activate(GetChosen(), Duration, Error))
	{
		Close();
	}
	return FReply::Handled();
}

FReply SHyperAIStudioAutoApprovalDialog::OnCancelClicked()
{
	Close();
	return FReply::Handled();
}

void SHyperAIStudioAutoApprovalDialog::Close()
{
	bClosed = true;
	if (const TSharedPtr<SWindow> Pinned = Window.Pin())
	{
		Pinned->RequestDestroyWindow();
	}
}

bool SHyperAIStudioAutoApprovalDialog::ShowNativeConfirmation(const TSharedPtr<SWindow>& Owner, const FText& Caption, const FText& Body)
{
#if PLATFORM_WINDOWS
	// Fails closed wherever nobody can answer it; tests answer through their own seam.
	if (FApp::IsUnattended() || GIsAutomationTesting || IsRunningCommandlet() || !FSlateApplication::IsInitialized())
	{
		return false;
	}
	HWND OwnerHandle = nullptr;
	if (Owner.IsValid() && Owner->GetNativeWindow().IsValid())
	{
		OwnerHandle = static_cast<HWND>(Owner->GetNativeWindow()->GetOSWindowHandle());
	}
	return ::MessageBoxW(OwnerHandle, *Body.ToString(), *Caption.ToString(),
		MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDOK;
#else
	// HyperAI Studio runs on Windows; anywhere else nothing can confirm, so nothing turns on.
	return false;
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
TSharedRef<SHyperAIStudioAutoApprovalDialog> FHyperAIStudioAutoApprovalTestAccess::MakeDialog()
{
	return SNew(SHyperAIStudioAutoApprovalDialog);
}

void FHyperAIStudioAutoApprovalTestAccess::SetNativeAnswer(SHyperAIStudioAutoApprovalDialog& Dialog, const bool bOk)
{
	Dialog.ConfirmNatively = [bOk](const FText&, const FText&) { return bOk; };
	if (!Dialog.TestScope.IsSet())
	{
		Dialog.TestScope.Emplace();
	}
}

void FHyperAIStudioAutoApprovalTestAccess::SetTicked(SHyperAIStudioAutoApprovalDialog& Dialog, const EHyperAIStudioApprovalCategory Category, const bool bTicked)
{
	Dialog.bTicked[static_cast<int32>(Category)] = bTicked;
}

bool FHyperAIStudioAutoApprovalTestAccess::IsTicked(const SHyperAIStudioAutoApprovalDialog& Dialog, const EHyperAIStudioApprovalCategory Category)
{
	return Dialog.bTicked[static_cast<int32>(Category)];
}

void FHyperAIStudioAutoApprovalTestAccess::SetDuration(SHyperAIStudioAutoApprovalDialog& Dialog, const EHyperAIStudioAutoApprovalDuration Duration)
{
	Dialog.Duration = Duration;
}

EHyperAIStudioAutoApprovalDuration FHyperAIStudioAutoApprovalTestAccess::GetDuration(const SHyperAIStudioAutoApprovalDialog& Dialog)
{
	return Dialog.Duration;
}

bool FHyperAIStudioAutoApprovalTestAccess::CanTurnOn(const SHyperAIStudioAutoApprovalDialog& Dialog)
{
	return Dialog.CanTurnOn();
}

void FHyperAIStudioAutoApprovalTestAccess::PressTurnOn(SHyperAIStudioAutoApprovalDialog& Dialog, const TSet<FString>& OperationIds)
{
	Dialog.TestScope = OperationIds;
	Dialog.OnTurnOnClicked();
}

void FHyperAIStudioAutoApprovalTestAccess::PressCancel(SHyperAIStudioAutoApprovalDialog& Dialog)
{
	Dialog.OnCancelClicked();
}

void FHyperAIStudioAutoApprovalTestAccess::PressKey(SHyperAIStudioAutoApprovalDialog& Dialog, const FKey& Key)
{
	Dialog.OnKeyDown(FGeometry(), FKeyEvent(Key, FModifierKeysState(), 0, false, 0, 0));
}

bool FHyperAIStudioAutoApprovalTestAccess::FocusesCancel(const SHyperAIStudioAutoApprovalDialog& Dialog)
{
	return Dialog.FocusOnOpen().IsValid() && Dialog.FocusOnOpen() == Dialog.CancelButton;
}

bool FHyperAIStudioAutoApprovalTestAccess::TurnOn(
	const TArray<EHyperAIStudioApprovalCategory>& Categories,
	const TSet<FString>& OperationIds,
	const EHyperAIStudioAutoApprovalDuration Duration)
{
	const TSharedRef<SHyperAIStudioAutoApprovalDialog> Dialog = MakeDialog();
	for (int32 Index = 0; Index < SHyperAIStudioAutoApprovalDialog::CategoryCount; ++Index)
	{
		Dialog->bTicked[Index] = Categories.Contains(static_cast<EHyperAIStudioApprovalCategory>(Index));
	}
	Dialog->Duration = Duration;
	SetNativeAnswer(*Dialog, true);
	PressTurnOn(*Dialog, OperationIds);
	return FHyperAIStudioAutoApproval::IsActive();
}

bool FHyperAIStudioAutoApprovalTestAccess::WasClosed(const SHyperAIStudioAutoApprovalDialog& Dialog)
{
	return Dialog.bClosed;
}

int32 FHyperAIStudioAutoApprovalTestAccess::GetPendingShown(const SHyperAIStudioAutoApprovalDialog& Dialog)
{
	return Dialog.PendingAtOpen.Num();
}

int32 FHyperAIStudioAutoApprovalTestAccess::GetNativeConfirmations(const SHyperAIStudioAutoApprovalDialog& Dialog)
{
	return Dialog.NativeConfirmations;
}
#endif

#undef LOCTEXT_NAMESPACE
