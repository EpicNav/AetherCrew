// Games by Hyper 2026.

#include "SHyperAIStudioHomePage.h"

#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioShell.h"
#include "HyperAIStudioWidgets.h"
#include "SHyperAIStudioActivitySidebar.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioHomePage"

namespace HyperAIStudio::Home
{
	constexpr float CardWidth = 268.0f;
	constexpr float TileWidth = 268.0f;
}

void SHyperAIStudioHomePage::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::Home;
	Service = MakeShared<FHyperAIStudioService>();
	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(FMargin(28.0f, 22.0f, 28.0f, 28.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				PageHeader(LOCTEXT("Title", "Welcome to HyperAI Studio"),
					LOCTEXT("Description", "Agents, tools and tests for this project, in one place."),
					IconButton(FAppStyle::GetBrush("Icons.Refresh"), LOCTEXT("RefreshTooltip", "Check the status again."),
						FOnClicked::CreateLambda([this]()
						{
							Refresh();
							return FReply::Handled();
						})))
			]
			// Until setup is done, finishing it is the one thing that matters.
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 18.0f, 0.0f, 0.0f))
			[
				SNew(SBox)
				.Visibility_Lambda([this]() { return bHasStatus && !bReady ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					Card(
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 14.0f, 0.0f))
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.WarningWithColor.Large"))
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("SetupTitle", "Finish setting up HyperAI"))
								.TextStyle(&Style(), "HyperAIStudio.Text.CardTitle")
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
							[
								SNew(STextBlock)
								.Text_Lambda([this]()
								{
									return Status.Summary.IsEmpty()
										? LOCTEXT("SetupBody", "Unreal MCP and the agent files are not ready yet, so agents cannot work in this editor.")
										: FText::FromString(Status.Summary);
								})
								.TextStyle(&Style(), "HyperAIStudio.Text.Subdued")
								.AutoWrapText(true)
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(14.0f, 0.0f, 0.0f, 0.0f))
						[
							Button(LOCTEXT("OpenSetup", "Open setup"), FOnClicked::CreateLambda([]()
							{
								FHyperAIStudioShell::Open(TEXT("Settings"));
								return FReply::Handled();
							}), nullptr, /*bPrimary=*/true)
						])
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 22.0f, 0.0f, 8.0f))
			[
				SectionLabel(LOCTEXT("StatusSection", "Status"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildStatus()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 24.0f, 0.0f, 8.0f))
			[
				SectionLabel(LOCTEXT("StartSection", "Start something"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				BuildActions()
			]
		]
	];
	Refresh();
}

void SHyperAIStudioHomePage::Refresh()
{
	if (!Service.IsValid())
	{
		return;
	}
	Status = Service->GetMcpRuntimeStatusFast();
	bHasStatus = true;
	Prerequisites = Service->GetPrerequisites();
	Approvals = SHyperAIStudioChatActivitySidebar::GetPendingApprovalCount();
	Running = 0;
	for (const FHyperAIStudioAsyncJobStatus& Job : FHyperAIStudioAsyncJobHost::GetSnapshot())
	{
		Running += Job.State == EHyperAIStudioAsyncJobState::Running;
	}
	// The full check, with the agent files and the tool probe, answers later.
	TWeakPtr<SWidget> Weak = AsShared();
	Service->RefreshStatusAsync([this, Weak](const FHyperAIStudioStatus& Full)
	{
		if (Weak.IsValid())
		{
			Status = Full;
			bReady = Full.IsReady();
		}
	});
}

TSharedRef<SWidget> SHyperAIStudioHomePage::BuildStatus()
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::Home;
	const TSharedRef<SWrapBox> Row = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12.0f, 12.0f));
	Row->AddSlot()
	[
		SNew(SBox).WidthOverride(CardWidth)
		[
			Card(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
					[
						Dot(TAttribute<FSlateColor>::CreateLambda([this]() { return StatusColor(!bHasStatus ? EStatus::Neutral : Status.bServerRunning ? EStatus::Good : EStatus::Warning); }))
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(STextBlock)
						.TextStyle(&Style(), "HyperAIStudio.Text.Body")
						.Text_Lambda([this]()
						{
							return !bHasStatus ? LOCTEXT("McpChecking", "Checking...")
								: Status.bServerRunning ? LOCTEXT("McpRunning", "Running") : LOCTEXT("McpOff", "Not running");
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
				[
					Fact(LOCTEXT("Port", "Port"), TAttribute<FText>::CreateLambda([this]()
					{
						const uint32 Port = Status.ActivePort != 0 ? Status.ActivePort : Status.ConfiguredPort;
						return Port != 0 ? FText::AsNumber(Port, &FNumberFormattingOptions::DefaultNoGrouping()) : LOCTEXT("None", "none");
					}))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					Fact(LOCTEXT("Endpoint", "Endpoint"), TAttribute<FText>::CreateLambda([this]()
					{
						return Status.Endpoint.IsEmpty() ? LOCTEXT("NoEndpoint", "not set") : FText::FromString(Status.Endpoint);
					}))
				],
				LOCTEXT("McpCard", "Unreal MCP"))
		]
	];
	Row->AddSlot()
	[
		SNew(SBox).WidthOverride(CardWidth)
		[
			Card(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					BuildAgent(TEXT("codex"), LOCTEXT("Codex", "Codex"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					BuildAgent(TEXT("claude"), LOCTEXT("Claude", "Claude Code"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					BuildAgent(TEXT("gemini"), LOCTEXT("Gemini", "Gemini"))
				],
				LOCTEXT("AgentsCard", "Agents"))
		]
	];
	Row->AddSlot()
	[
		SNew(SBox).WidthOverride(CardWidth)
		[
			Card(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
					[
						SNew(STextBlock)
						.TextStyle(&Style(), "HyperAIStudio.Text.Metric")
						.Text_Lambda([this]() { return FText::AsNumber(Approvals); })
						.ColorAndOpacity_Lambda([this]() { return Approvals > 0 ? FStyleColors::Warning : FStyleColors::ForegroundHover; })
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom).Padding(FMargin(8.0f, 0.0f, 0.0f, 2.0f))
					[
						SNew(STextBlock)
						.TextStyle(&Style(), "HyperAIStudio.Text.Subdued")
						.Text(LOCTEXT("ApprovalsWaiting", "waiting for your approval"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					.Text_Lambda([this]()
					{
						return Running > 0 ? FText::Format(LOCTEXT("JobsRunning", "{0} {0}|plural(one=job,other=jobs) running"), Running)
							: LOCTEXT("NoJobs", "No jobs running");
					})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f)).HAlign(HAlign_Left)
				[
					Button(LOCTEXT("OpenActivity", "Open Activity"), FOnClicked::CreateLambda([]()
					{
						FHyperAIStudioShell::Open(TEXT("Activity"));
						return FReply::Handled();
					}))
				],
				LOCTEXT("NeedsYouCard", "Needs you"))
		]
	];
	return Row;
}

TSharedRef<SWidget> SHyperAIStudioHomePage::BuildAgent(const FString& Id, const FText& Name) const
{
	using namespace HyperAIStudio::UI;
	auto Found = [this, Id]()
	{
		const FHyperAIStudioPrerequisiteStatus* Entry = Prerequisites.FindByPredicate([&Id](const FHyperAIStudioPrerequisiteStatus& Known)
		{
			return Known.Id.Equals(Id, ESearchCase::IgnoreCase);
		});
		return Entry && Entry->bAvailable;
	};
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
		[
			Dot(TAttribute<FSlateColor>::CreateLambda([Found]() { return StatusColor(Found() ? EStatus::Good : EStatus::Neutral); }))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(Name).TextStyle(&Style(), "HyperAIStudio.Text.Body")
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
			.Text_Lambda([Found]() { return Found() ? LOCTEXT("AgentReady", "Installed") : LOCTEXT("AgentMissing", "Not installed"); })
		];
}

TSharedRef<SWidget> SHyperAIStudioHomePage::Tile(const FName Icon, const FText& Title, const FText& Body, TFunction<void()> Run) const
{
	using namespace HyperAIStudio::UI;
	return SNew(SBox)
		.WidthOverride(HyperAIStudio::Home::TileWidth)
		[
			SNew(SButton)
			.ButtonStyle(&Style(), "HyperAIStudio.Tile")
			.ContentPadding(FMargin(14.0f, 12.0f))
			.OnClicked_Lambda([Run]()
			{
				Run();
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(FMargin(0.0f, 2.0f, 12.0f, 0.0f))
				[
					SNew(SBox).WidthOverride(20.0f).HeightOverride(20.0f)
					[
						SNew(SImage)
						.Image(HyperAIStudio::UI::Icon(Icon))
						.ColorAndOpacity(FStyleColors::AccentBlue)
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(Title).TextStyle(&Style(), "HyperAIStudio.Text.CardTitle")
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
					[
						SNew(SBox).MinDesiredHeight(30.0f)
						[
							SNew(STextBlock).Text(Body).TextStyle(&Style(), "HyperAIStudio.Text.Caption").AutoWrapText(true)
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioHomePage::BuildActions()
{
	const TSharedRef<SWrapBox> Tiles = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(12.0f, 12.0f));
	struct FStart
	{
		FName Page;
		FName Icon;
		FText Title;
		FText Body;
	};
	const FStart Starts[] = {
		{TEXT("Chat"), TEXT("HyperAIStudio.Icon.Chat"), LOCTEXT("ChatTile", "Chat with an agent"), LOCTEXT("ChatTileBody", "Codex, Claude Code or Gemini, working in this editor.")},
		{TEXT("Images"), TEXT("HyperAIStudio.Icon.Images"), LOCTEXT("ImagesTile", "Generate an image"), LOCTEXT("ImagesTileBody", "Concept art and references, with Nano Banana or Codex.")},
		{TEXT("Match"), TEXT("HyperAIStudio.Icon.Match"), LOCTEXT("MatchTile", "Match a reference"), LOCTEXT("MatchTileBody", "Compare the level with a target image, side by side.")},
		{TEXT("Playtest"), TEXT("HyperAIStudio.Icon.Playtest"), LOCTEXT("PlaytestTile", "Playtest the level"), LOCTEXT("PlaytestTileBody", "A bot plays like a person, many cases at once.")},
		{TEXT("Memory"), TEXT("HyperAIStudio.Icon.Memory"), LOCTEXT("MemoryTile", "Remember a fact"), LOCTEXT("MemoryTileBody", "What agents should know about this project.")},
		{TEXT("Tools"), TEXT("HyperAIStudio.Icon.Tools"), LOCTEXT("ToolsTile", "Run a tool"), LOCTEXT("ToolsTileBody", "Any HyperAI or Unreal tool, as a form.")}};
	for (const FStart& Start : Starts)
	{
		// A pack that is not loaded has no page, and so no tile.
		if (!FHyperAIStudioShell::FindPage(Start.Page))
		{
			continue;
		}
		const FName Page = Start.Page;
		Tiles->AddSlot()
		[
			Tile(Start.Icon, Start.Title, Start.Body, [Page]() { FHyperAIStudioShell::Open(Page); })
		];
	}
	return Tiles;
}

#undef LOCTEXT_NAMESPACE
