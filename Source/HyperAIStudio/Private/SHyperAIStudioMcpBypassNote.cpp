// Games by Hyper 2026.

#include "SHyperAIStudioMcpBypassNote.h"

#include "HyperAIStudioMarkdown.h"
#include "HyperAIStudioMcpBypassMonitor.h"
#include "HyperAIStudioStyle.h"
#include "HyperAIStudioWidgets.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioMcpBypass"

void SHyperAIStudioMcpBypassNote::Construct(const FArguments& InArgs)
{
	ChangedHandle = FHyperAIStudioMcpBypassMonitor::OnChanged().AddSP(this, &SHyperAIStudioMcpBypassNote::Refresh);
	ChildSlot
	[
		SNew(SBorder)
		.Visibility_Lambda([this]() { return Text.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(0.0f, 1.0f, 6.0f, 0.0f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush(TEXT("Icons.WarningWithColor")))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return Text; })
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("NoteDetailsTooltip", "Which servers, where they are configured, and how to remove them."))
				.OnClicked_Lambda([]()
				{
					FHyperAIStudioMcpBypassMonitor::OpenDetails();
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("NoteDetails", "Details"))
				]
			]
		]
	];
	Refresh();
}

SHyperAIStudioMcpBypassNote::~SHyperAIStudioMcpBypassNote()
{
	FHyperAIStudioMcpBypassMonitor::OnChanged().Remove(ChangedHandle);
}

void SHyperAIStudioMcpBypassNote::Refresh()
{
	Text = HyperAIStudio::McpBypass::BuildActivityNote(
		HyperAIStudio::McpBypass::FilterBypass(FHyperAIStudioMcpBypassMonitor::GetFindings(), /*bUnsilencedOnly=*/true));
}

void SHyperAIStudioMcpBypassPanel::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::UI;
	ChangedHandle = FHyperAIStudioMcpBypassMonitor::OnChanged().AddSP(this, &SHyperAIStudioMcpBypassPanel::Rebuild);
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PanelTitle", "Other MCP servers"))
					.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.CardTitle")
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("PanelBody",
						"MCP servers these agents have besides HyperAI's Unreal MCP, and this editor's plugins and settings that act like one. Those that change this editor do it with no HyperAI approval, no auto-approval, no undo grouping and no HyperAI checks. HyperAI only reads their files; remove an entry yourself as shown."))
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("PanelCheckAgain", "Check Again"))
				.ToolTipText(LOCTEXT("PanelCheckAgainTooltip", "Read the agents' config files and this editor's plugins again."))
				.OnClicked_Lambda([]()
				{
					FHyperAIStudioMcpBypassMonitor::Rescan();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("PanelDetails", "Details"))
				.ToolTipText(LOCTEXT("PanelDetailsTooltip", "Everything below as one report, in its own window."))
				.OnClicked_Lambda([]()
				{
					FHyperAIStudioMcpBypassMonitor::OpenDetails();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(List, SVerticalBox)
		]
	];
	Rebuild();
}

SHyperAIStudioMcpBypassPanel::~SHyperAIStudioMcpBypassPanel()
{
	FHyperAIStudioMcpBypassMonitor::OnChanged().Remove(ChangedHandle);
}

void SHyperAIStudioMcpBypassPanel::Rebuild()
{
	using namespace HyperAIStudio::UI;
	if (!List.IsValid())
	{
		return;
	}
	List->ClearChildren();
	const TArray<FHyperAIStudioMcpFinding> Findings = FHyperAIStudioMcpBypassMonitor::GetFindings();
	if (Findings.IsEmpty())
	{
		List->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PanelNone", "No other MCP servers found in the agents' configs, and no editor plugin or setting that acts like one."))
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}
	for (const FHyperAIStudioMcpFinding& Finding : Findings)
	{
		const TSharedRef<SVerticalBox> Places = SNew(SVerticalBox);
		for (const FHyperAIStudioMcpServerSource& Source : Finding.Sources)
		{
			Places->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("PanelPlace", "- {0}"), HyperAIStudio::McpBypass::DescribeSource(Source)))
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
			];
			if (Finding.ChangesEditor())
			{
				Places->AddSlot()
				.AutoHeight()
				.Padding(10.0f, 1.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("ToRemove", "To remove: {0}"), HyperAIStudio::McpBypass::DescribeRemoval(Source, Finding.bWrittenByHyperAI)))
					.AutoWrapText(true)
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				];
			}
		}
		const FString Id = Finding.Id;
		const bool bSilenced = Finding.bSilenced;
		List->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 5.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(9.0f, 7.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Finding.DisplayName))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						Chip(Finding.ChangesEditor() ? LOCTEXT("ChipBypass", "Bypasses HyperAI") : LOCTEXT("ChipOther", "Other tools"),
							StatusColor(Finding.ChangesEditor() ? EStatus::Error : EStatus::Neutral))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SBox)
						.Visibility(bSilenced ? EVisibility::Visible : EVisibility::Collapsed)
						[
							Chip(LOCTEXT("ChipSilenced", "Not warned about"), StatusColor(EStatus::Neutral))
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNullWidget::NullWidget
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SBox)
						.Visibility(Finding.ChangesEditor() ? EVisibility::Visible : EVisibility::Collapsed)
						[
							SNew(SButton)
							.Text(bSilenced ? LOCTEXT("WarnAgain", "Warn Again") : LOCTEXT("PanelDontWarn", "Don't Warn Again"))
							.ToolTipText(bSilenced
								? LOCTEXT("WarnAgainTooltip", "Warn about it again at editor start and in Activity.")
								: LOCTEXT("PanelDontWarnTooltip", "Stop warning about it in this project, for you. Agents can still use it, and it stays listed here."))
							.OnClicked_Lambda([Id, bSilenced]()
							{
								FHyperAIStudioMcpBypassMonitor::SetWarningSilenced(Id, !bSilenced);
								return FReply::Handled();
							})
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(HyperAIStudio::McpBypass::DescribeWhy(Finding))
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					Places
				]
			]
		];
	}
	for (const FString& Problem : FHyperAIStudioMcpBypassMonitor::GetProblems())
	{
		List->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Problem))
			.AutoWrapText(true)
			.ColorAndOpacity(FStyleColors::Warning)
		];
	}
}

void SHyperAIStudioMcpBypassDetails::Construct(const FArguments& InArgs)
{
	Window = InArgs._Window;
	ChangedHandle = FHyperAIStudioMcpBypassMonitor::OnChanged().AddSP(this, &SHyperAIStudioMcpBypassDetails::Refresh);
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Panel"))
		.Padding(16.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(Markdown, SHyperAIStudioMarkdown)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("PanelCheckAgain", "Check Again"))
					.ToolTipText(LOCTEXT("PanelCheckAgainTooltip", "Read the agents' config files and this editor's plugins again."))
					.OnClicked_Lambda([]()
					{
						FHyperAIStudioMcpBypassMonitor::Rescan();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("DetailsClose", "Close"))
					.OnClicked_Lambda([this]()
					{
						if (const TSharedPtr<SWindow> Owner = Window.Pin())
						{
							Owner->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]
	];
	Refresh();
}

SHyperAIStudioMcpBypassDetails::~SHyperAIStudioMcpBypassDetails()
{
	FHyperAIStudioMcpBypassMonitor::OnChanged().Remove(ChangedHandle);
}

void SHyperAIStudioMcpBypassDetails::Refresh()
{
	if (Markdown.IsValid())
	{
		Markdown->SetMarkdown(HyperAIStudio::McpBypass::BuildDetailsMarkdown(FHyperAIStudioMcpBypassMonitor::GetFindings(),
			FHyperAIStudioMcpBypassMonitor::GetProblems()));
	}
}

#undef LOCTEXT_NAMESPACE
