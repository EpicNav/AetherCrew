// Games by Hyper 2026.

#include "SHyperAIStudioActivitySidebar.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "FileHelpers.h"
#include "HyperAIStudioAutoApproval.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioStyle.h"
#include "IContentBrowserSingleton.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SHyperAIStudioAutoApprovalBanner.h"
#include "SHyperAIStudioMcpBypassNote.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioChatActivitySidebar"

namespace HyperAIStudio::ActivitySidebar
{
	FText RelativeTime(const FDateTime& Utc)
	{
		const FTimespan Age = FDateTime::UtcNow() - Utc;
		if (Age.GetTotalSeconds() < 60.0)
		{
			return LOCTEXT("JustNow", "just now");
		}
		if (Age.GetTotalMinutes() < 60.0)
		{
			return FText::Format(LOCTEXT("MinutesAgo", "{0}m ago"), FMath::FloorToInt(Age.GetTotalMinutes()));
		}
		return FText::Format(LOCTEXT("HoursAgo", "{0}h ago"), FMath::FloorToInt(Age.GetTotalHours()));
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

	FString AgentText(const FString& Agent, const FString& Model)
	{
		return Model.IsEmpty() ? Agent : FString::Printf(TEXT("%s (%s)"), *Agent, *Model);
	}

	FSlateColor StatusColor(const EHyperAIStudioActivityKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioActivityKind::Failed: return FStyleColors::AccentRed;
		case EHyperAIStudioActivityKind::ApprovalRequested: return FStyleColors::AccentYellow;
		case EHyperAIStudioActivityKind::Completed:
		case EHyperAIStudioActivityKind::JobFinished: return FStyleColors::AccentGreen;
		case EHyperAIStudioActivityKind::AutoApproval: return FStyleColors::AccentOrange;
		default: return FSlateColor::UseSubduedForeground();
		}
	}

	/** The asset a held file saves, for the Content Browser; empty for files that are not assets. */
	FString ObjectPathOfFile(const FString& ProjectRelativePath)
	{
		FString PackageName;
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / ProjectRelativePath);
		if (!FPackageName::IsPackageFilename(File) || !FPackageName::TryConvertFilenameToLongPackageName(File, PackageName))
		{
			return FString();
		}
		return PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	}
}

void SHyperAIStudioChatActivitySidebar::Construct(const FArguments& InArgs)
{
	OnClose = InArgs._OnClose;
	OnApprovalResolved = InArgs._OnApprovalResolved;

	ActivityChangedHandle = FHyperAIStudioAgentActivityLog::OnChanged().AddSP(this, &SHyperAIStudioChatActivitySidebar::Rebuild);
	ApprovalChangedHandle = FHyperAIStudioApprovalGate::OnChanged().AddSP(this, &SHyperAIStudioChatActivitySidebar::Rebuild);
	JobChangedHandle = FHyperAIStudioAsyncJobHost::OnChanged().AddSP(this, &SHyperAIStudioChatActivitySidebar::Rebuild);
	SandboxChangedHandle = HyperAIStudio::Sandbox::OnChanged().AddSP(this, &SHyperAIStudioChatActivitySidebar::Rebuild);
	AutoApprovalChangedHandle = FHyperAIStudioAutoApproval::OnChanged().AddSP(this, &SHyperAIStudioChatActivitySidebar::Rebuild);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Panel"))
		.Padding(8.0f)
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
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "Activity"))
					.TextStyle(FHyperAIStudioStyle::Get(), "HyperAIStudio.Text.Title")
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText_Lambda([]()
					{
						return FHyperAIStudioAutoApproval::IsActive()
							? LOCTEXT("AutoApproveOnTooltip", "Auto-approval is on. Open to start a new one in its place; use Stop to end it.")
							: LOCTEXT("AutoApproveTooltip", "Let agents' requests run without asking you, for a while. It shows what it covers and asks before it turns on.");
					})
					.OnClicked_Lambda([this]()
					{
						FHyperAIStudioAutoApproval::OpenDialog(AsShared());
						return FReply::Handled();
					})
					[
						SNew(STextBlock)
						.Text_Lambda([]()
						{
							return FHyperAIStudioAutoApproval::IsActive()
								? LOCTEXT("AutoApprovalOnButton", "Auto-approval on") : LOCTEXT("AutoApproveButton", "Auto-approve...");
						})
						.ColorAndOpacity_Lambda([]()
						{
							return FHyperAIStudioAutoApproval::IsActive() ? FSlateColor(FStyleColors::AccentOrange) : FSlateColor::UseForeground();
						})
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("ClearTooltip", "Clear the finished entries. Plans waiting for approval stay."))
					.OnClicked_Lambda([]()
					{
						FHyperAIStudioAgentActivityLog::Clear();
						return FReply::Handled();
					})
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush(TEXT("Icons.Delete")))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("CloseTooltip", "Hide the Activity panel"))
					.OnClicked_Lambda([this]()
					{
						OnClose.ExecuteIfBound();
						return FReply::Handled();
					})
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush(TEXT("Icons.X")))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				// A chat window shows its own banner above the chat; one there is enough.
				SNew(SBox)
				.Visibility(InArgs._bShowAutoApprovalBanner ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed)
				[
					SNew(SHyperAIStudioAutoApprovalBanner)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				// Other MCP servers change this editor without coming here; it says so while any is found.
				SNew(SHyperAIStudioMcpBypassNote)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]() { return Rows.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text(LOCTEXT("Empty", "No agent edits yet. Plans appear here for approval before anything changes."))
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SAssignNew(List, SListView<FRowPtr>)
				.ListItemsSource(&Rows)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SHyperAIStudioChatActivitySidebar::GenerateRow)
			]
		]
	];

	Rebuild();
}

SHyperAIStudioChatActivitySidebar::~SHyperAIStudioChatActivitySidebar()
{
	FHyperAIStudioAgentActivityLog::OnChanged().Remove(ActivityChangedHandle);
	FHyperAIStudioApprovalGate::OnChanged().Remove(ApprovalChangedHandle);
	FHyperAIStudioAsyncJobHost::OnChanged().Remove(JobChangedHandle);
	HyperAIStudio::Sandbox::OnChanged().Remove(SandboxChangedHandle);
	FHyperAIStudioAutoApproval::OnChanged().Remove(AutoApprovalChangedHandle);
}

int32 SHyperAIStudioChatActivitySidebar::GetPendingApprovalCount()
{
	// Held sandbox files wait on the user too.
	return FHyperAIStudioApprovalGate::GetPending().Num() + HyperAIStudio::Sandbox::GetHeldCount();
}

void SHyperAIStudioChatActivitySidebar::Rebuild()
{
	Rows.Reset();
	auto AddHeading = [this](const FText& Heading)
	{
		FRow Row;
		Row.Heading = Heading;
		Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
	};

	const TArray<FHyperAIStudioPendingApproval> Pending = FHyperAIStudioApprovalGate::GetPending();
	if (!Pending.IsEmpty())
	{
		AddHeading(LOCTEXT("PendingHeading", "Waiting for you"));
		for (const FHyperAIStudioPendingApproval& Approval : Pending)
		{
			FRow Row;
			Row.Approval = MakeShared<FHyperAIStudioPendingApproval>(Approval);
			Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
		}
	}

	HyperAIStudio::Sandbox::FStatus Sandbox = HyperAIStudio::Sandbox::GetStatus();
	if (Sandbox.State != HyperAIStudio::Sandbox::EState::Off)
	{
		AddHeading(LOCTEXT("SandboxHeading", "Sandbox"));
		FRow Row;
		Row.Sandbox = MakeShared<HyperAIStudio::Sandbox::FStatus>(MoveTemp(Sandbox));
		Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
	}

	TArray<FHyperAIStudioAsyncJobStatus> Jobs = FHyperAIStudioAsyncJobHost::GetSnapshot();
	Jobs.RemoveAll([](const FHyperAIStudioAsyncJobStatus& Job) { return Job.State != EHyperAIStudioAsyncJobState::Running; });
	if (!Jobs.IsEmpty())
	{
		AddHeading(LOCTEXT("RunningHeading", "Running"));
		for (const FHyperAIStudioAsyncJobStatus& Job : Jobs)
		{
			FRow Row;
			Row.Job = MakeShared<FHyperAIStudioAsyncJobStatus>(Job);
			Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
		}
	}

	// The scoreboard outlives the session; the busiest rows are the ones routing leans on.
	const TArray<FHyperAIStudioAgentScore> Scores = FHyperAIStudioAgentScoreboard::GetScores();
	if (!Scores.IsEmpty())
	{
		AddHeading(LOCTEXT("RecordHeading", "Track record"));
		for (int32 Index = 0; Index < FMath::Min(Scores.Num(), 8); ++Index)
		{
			FRow Row;
			Row.Score = MakeShared<FHyperAIStudioAgentScore>(Scores[Index]);
			Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
		}
	}

	const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
	if (!Entries.IsEmpty())
	{
		AddHeading(LOCTEXT("HistoryHeading", "History"));
		for (const FHyperAIStudioActivityEntry& Entry : Entries)
		{
			FRow Row;
			Row.Entry = MakeShared<FHyperAIStudioActivityEntry>(Entry);
			Rows.Add(MakeShared<FRow>(MoveTemp(Row)));
		}
	}

	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SHyperAIStudioChatActivitySidebar::GenerateRow(FRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	TSharedPtr<SWidget> Content;
	if (Row->Approval.IsValid())
	{
		Content = BuildApprovalCard(*Row->Approval);
	}
	else if (Row->Job.IsValid())
	{
		Content = BuildJobRow(*Row->Job);
	}
	else if (Row->Entry.IsValid())
	{
		Content = BuildEntryRow(*Row->Entry);
	}
	else if (Row->Score.IsValid())
	{
		Content = BuildScoreRow(*Row->Score);
	}
	else if (Row->Sandbox.IsValid())
	{
		Content = BuildSandboxCard(*Row->Sandbox);
	}
	else
	{
		Content = SNew(STextBlock)
			.Text(Row->Heading)
			.Font(FAppStyle::GetFontStyle("SmallFontBold"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground());
	}

	return SNew(STableRow<FRowPtr>, OwnerTable)
		.Style(FAppStyle::Get(), "TableView.NoHoverTableRow")
		.ShowSelection(false)
		.Padding(FMargin(2.0f, Row->Heading.IsEmpty() ? 3.0f : 10.0f, 2.0f, 3.0f))
		[
			Content.ToSharedRef()
		];
}

TSharedRef<SWidget> SHyperAIStudioChatActivitySidebar::BuildApprovalCard(const FHyperAIStudioPendingApproval& Approval)
{
	using namespace HyperAIStudio::ActivitySidebar;
	const FString OperationId = Approval.OperationId;
	const FString Target = Approval.Summary.EffectTarget;

	TSharedRef<SVerticalBox> Effects = SNew(SVerticalBox);
	for (int32 Index = 0; Index < Approval.Summary.Effects.Num() && Index < 8; ++Index)
	{
		Effects->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TEXT("- ") + Approval.Summary.Effects[Index]))
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
		];
	}
	if (Approval.Summary.Effects.Num() > 8)
	{
		Effects->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("MoreEffects", "- and {0} more"), Approval.Summary.Effects.Num() - 8))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}

	return SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Badge"))
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Text(FText::FromString(Approval.Summary.ToolName))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.Text(AssetName(Target))
				.ToolTipText(FText::FromString(Target))
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				Effects
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Visibility(Approval.bAutoApproving ? EVisibility::Visible : EVisibility::Collapsed)
				// Read live: what holds it back (an open undo step, a mouse capture) changes without the list rebuilding.
				.Text_Lambda([]()
				{
					const FText Wait = FHyperAIStudioApprovalGate::DescribeAutoApprovalWait();
					return Wait.IsEmpty()
						? LOCTEXT("AutoApprovingNote", "Auto-approval runs this as soon as the editor is idle.")
						: FText::Format(LOCTEXT("AutoApprovingWaitNote", "Auto-approval runs this at the next quiet moment; it is waiting because {0}."), Wait);
				})
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity_Lambda([]()
				{
					return FHyperAIStudioApprovalGate::DescribeAutoApprovalWait().IsEmpty()
						? FSlateColor::UseSubduedForeground() : FSlateColor(FStyleColors::AccentOrange);
				})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				// While auto-approval is on, a request it leaves for a click says why, so "it approves nothing" can be read.
				SNew(STextBlock)
				.Visibility_Lambda([OperationId]()
				{
					return FHyperAIStudioApprovalGate::DescribeWhyNotAutoApproved(OperationId).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
				})
				.Text_Lambda([OperationId]()
				{
					return FText::Format(LOCTEXT("NotAutoApprovedNote", "Auto-approval is on but leaves this for your click: {0}."),
						FText::FromString(FHyperAIStudioApprovalGate::DescribeWhyNotAutoApproved(OperationId)));
				})
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Approve", "Approve"))
					.ToolTipText(LOCTEXT("ApproveTooltip", "Run this plan now."))
					.OnClicked_Lambda([this, OperationId]()
					{
						FString Error;
						FHyperAIStudioApprovalGate::Approve(OperationId, Error);
						OnApprovalResolved.ExecuteIfBound();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Reject", "Reject"))
					.ToolTipText(LOCTEXT("RejectTooltip", "Discard this plan. Nothing changes."))
					.OnClicked_Lambda([this, OperationId]()
					{
						FHyperAIStudioApprovalGate::Reject(OperationId, TEXT("Rejected in the Activity panel."));
						OnApprovalResolved.ExecuteIfBound();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(RelativeTime(Approval.RequestedUtc))
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioChatActivitySidebar::BuildJobRow(const FHyperAIStudioAsyncJobStatus& Job)
{
	using namespace HyperAIStudio::ActivitySidebar;
	const FString JobId = Job.JobId;
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("JobLine", "{0} - {1}"), AssetName(Job.Target), FText::FromString(Job.Progress)))
				.ToolTipText(FText::FromString(Job.ToolName))
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Visibility(Job.bCancellable ? EVisibility::Visible : EVisibility::Collapsed)
				.Text(LOCTEXT("CancelJob", "Cancel"))
				.ToolTipText(LOCTEXT("CancelJobTooltip", "Stop this job. The agent that started it sees it as cancelled."))
				.OnClicked_Lambda([JobId]()
				{
					FString Error;
					FHyperAIStudioAsyncJobHost::Cancel(JobId, TEXT("Cancelled from the Activity panel."), Error);
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("JobElapsed", "running for {0}s"), static_cast<int32>(Job.ElapsedMs / 1000)))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
}

TSharedRef<SWidget> SHyperAIStudioChatActivitySidebar::BuildEntryRow(const FHyperAIStudioActivityEntry& Entry)
{
	using namespace HyperAIStudio::ActivitySidebar;
	const FString Target = Entry.Target;
	// What nobody clicked for says so: the user's auto-approval, with the category that covered it, or a running job.
	// So does whatever such an approval led to in the queue: a wait, or a failure.
	const bool bAutoApproved = Entry.Kind == EHyperAIStudioActivityKind::ApprovalApproved && Entry.AutoApprovedAs.IsSet();
	const bool bAutoApprovedOutcome = !bAutoApproved && Entry.AutoApprovedAs.IsSet();
	// An auto-approval that came back on by itself after a restart is told apart from one turned on in this editor.
	const FText KindText = bAutoApproved && Entry.bKeptOnAfterRestart
		? FText::Format(LOCTEXT("AutoApprovedKeptOnKind", "Auto-approved (kept on after restart, {0})"), FHyperAIStudioAutoApproval::GetCategoryName(*Entry.AutoApprovedAs))
		: bAutoApproved
		? FText::Format(LOCTEXT("AutoApprovedKind", "Auto-approved ({0})"), FHyperAIStudioAutoApproval::GetCategoryName(*Entry.AutoApprovedAs))
		: bAutoApprovedOutcome && Entry.bKeptOnAfterRestart
		? FText::Format(LOCTEXT("AutoApprovedKeptOnOutcomeKind", "{0} (auto-approved, kept on after restart, {1})"),
			FText::FromString(FHyperAIStudioAgentActivityLog::LexToString(Entry.Kind)), FHyperAIStudioAutoApproval::GetCategoryName(*Entry.AutoApprovedAs))
		: bAutoApprovedOutcome
		? FText::Format(LOCTEXT("AutoApprovedOutcomeKind", "{0} (auto-approved, {1})"),
			FText::FromString(FHyperAIStudioAgentActivityLog::LexToString(Entry.Kind)), FHyperAIStudioAutoApproval::GetCategoryName(*Entry.AutoApprovedAs))
		: Entry.bApprovedForJob
		? FText::Format(LOCTEXT("JobApprovedKind", "{0} (for a job)"), FText::FromString(FHyperAIStudioAgentActivityLog::LexToString(Entry.Kind)))
		: FText::FromString(FHyperAIStudioAgentActivityLog::LexToString(Entry.Kind));
	TSharedRef<SHorizontalBox> Line = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("EntryLine", "{0}: {1}"), KindText, FText::FromString(Entry.ToolName)))
			.ToolTipText(FText::FromString((Entry.Detail.IsEmpty() ? Entry.StatusCode : Entry.Detail)
				+ (Entry.Agent.IsEmpty() ? FString() : TEXT("\nBy ") + AgentText(Entry.Agent, Entry.Model))))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.ColorAndOpacity(bAutoApproved ? FSlateColor(FStyleColors::AccentOrange) : StatusColor(Entry.Kind))
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Entry.Agent.IsEmpty() ? RelativeTime(Entry.Utc)
				: FText::Format(LOCTEXT("EntryAgentTime", "{0}, {1}"), FText::FromString(Entry.Agent), RelativeTime(Entry.Utc)))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];

	if (Target.IsEmpty())
	{
		return Line;
	}
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			Line
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.HAlign(HAlign_Left)
			.ContentPadding(FMargin(0.0f, 1.0f))
			.ToolTipText(FText::Format(LOCTEXT("ShowAssetTooltip", "Show {0} in the Content Browser"), FText::FromString(Target)))
			.OnClicked_Lambda([Target]()
			{
				ShowTargetInContentBrowser(Target);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(AssetName(Target))
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FStyleColors::AccentBlue)
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioChatActivitySidebar::BuildScoreRow(const FHyperAIStudioAgentScore& Score)
{
	using namespace HyperAIStudio::ActivitySidebar;
	const float Rate = Score.Total() > 0 ? static_cast<float>(Score.Completed) / Score.Total() : 0.0f;
	return SNew(SHorizontalBox)
		.ToolTipText(FText::Format(LOCTEXT("ScoreTooltip", "{0} on {1}: {2} completed, {3} failed, {4} rejected by you. The new-tab menu picks agents by this record."),
			FText::FromString(AgentText(Score.Agent, Score.Model)), FText::FromString(Score.PackId),
			Score.Completed, Score.Failed, Score.Rejected))
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("ScoreLine", "{0} - {1}"), FText::FromString(AgentText(Score.Agent, Score.Model)), FText::FromString(Score.PackId)))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(6.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("ScoreCount", "{0} of {1}"), Score.Completed, Score.Total()))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(Rate >= 0.8f ? FStyleColors::AccentGreen : Rate >= 0.5f ? FStyleColors::AccentYellow : FStyleColors::AccentRed)
		];
}

TSharedRef<SWidget> SHyperAIStudioChatActivitySidebar::BuildSandboxCard(const HyperAIStudio::Sandbox::FStatus& Status)
{
	using namespace HyperAIStudio::ActivitySidebar;
	namespace Sandbox = HyperAIStudio::Sandbox;
	constexpr int32 ShownFiles = 12;

	TSharedRef<SVerticalBox> Card = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(Status.State == Sandbox::EState::Active
				? FText::Format(LOCTEXT("SandboxHeld", "{0}: {1} {1}|plural(one=file,other=files) held for review"),
					FText::FromString(Status.SandboxName), Status.Changes.Num())
				: FText::FromString(Status.SandboxName))
			.Visibility(Status.SandboxName.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
			.AutoWrapText(true)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 2.0f, 0.0f, 4.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Status.Diagnostic))
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(Status.State == Sandbox::EState::Foreign ? FSlateColor(FStyleColors::AccentYellow) : FSlateColor::UseSubduedForeground())
		];

	for (int32 Index = 0; Index < Status.Changes.Num() && Index < ShownFiles; ++Index)
	{
		const Sandbox::FChange& Change = Status.Changes[Index];
		const FString ObjectPath = ObjectPathOfFile(Change.Path);
		Card->AddSlot()
		.AutoHeight()
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.HAlign(HAlign_Left)
			.ContentPadding(FMargin(0.0f, 1.0f))
			.IsEnabled(!ObjectPath.IsEmpty())
			.ToolTipText(FText::FromString(Change.Path))
			.OnClicked_Lambda([ObjectPath]()
			{
				ShowTargetInContentBrowser(ObjectPath);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("SandboxFile", "{0} {1}"), FText::FromString(Change.Kind), FText::FromString(FPaths::GetCleanFilename(Change.Path))))
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(Change.Kind == TEXT("removed") ? FStyleColors::AccentRed : FStyleColors::AccentBlue)
			]
		];
	}
	if (Status.Changes.Num() > ShownFiles)
	{
		Card->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::Format(LOCTEXT("MoreSandboxFiles", "and {0} more"), Status.Changes.Num() - ShownFiles))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}

	if (Status.State == Sandbox::EState::Active && !Status.Changes.IsEmpty())
	{
		// While auto-approval is on: when it presses Approve All for these, what that waits for, or that it does not
		// write them because Sandbox Mode's held saves was not ticked in its dialog.
		Card->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Visibility_Lambda([]() { return FHyperAIStudioAutoApproval::DescribeSandboxWait().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([]() { return FHyperAIStudioAutoApproval::DescribeSandboxWait(); })
			.AutoWrapText(true)
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FStyleColors::AccentOrange)
		];
		TArray<FString> Files;
		for (const Sandbox::FChange& Change : Status.Changes)
		{
			Files.Add(Change.Kind + TEXT(" ") + Change.Path);
		}
		const auto Report = [](const bool bDone, const FString& Error)
		{
			if (!bDone)
			{
				FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error));
			}
		};
		Card->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("ApproveAll", "Approve All"))
				.ToolTipText(LOCTEXT("ApproveAllTooltip", "Write every held file to the project, saving what agents left unsaved first. A new sandbox holds what is saved next."))
				.OnClicked_Lambda([Report]()
				{
					// Closing the sandbox reloads unsaved assets from disk, so unsaved work is offered for saving first, as a map load does.
					FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave=*/true, /*bSaveMapPackages=*/true, /*bSaveContentPackages=*/true,
						/*bFastSave=*/false, /*bNotifyNoPackagesSaved=*/false, /*bCanBeDeclined=*/false);
					FString Error;
					Report(HyperAIStudio::Sandbox::ApproveAll(Error), Error);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("DiscardAll", "Discard All"))
				.ToolTipText(LOCTEXT("DiscardAllTooltip", "Throw every held file away and reload the project's own versions. Your own saves since the sandbox opened go too."))
				.OnClicked_Lambda([Report, Files]()
				{
					const FText Question = FText::Format(LOCTEXT("DiscardAllConfirm",
						"Discard all {0} held {0}|plural(one=file,other=files)? Unsaved changes in any asset are reloaded from the project too. This cannot be undone.\n\n{1}"),
						Files.Num(), FText::FromString(FString::Join(TArrayView<const FString>(Files).Left(20), TEXT("\n"))
							+ (Files.Num() > 20 ? FString::Printf(TEXT("\n... and %d more"), Files.Num() - 20) : FString())));
					if (FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, Question) == EAppReturnType::Yes)
					{
						FString Error;
						Report(HyperAIStudio::Sandbox::DiscardAll(Error), Error);
					}
					return FReply::Handled();
				})
			]
		];
	}

	return SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Badge"))
		.Padding(FMargin(8.0f, 6.0f))
		[
			Card
		];
}

void SHyperAIStudioChatActivitySidebar::ShowTargetInContentBrowser(const FString& Target)
{
	// A target is whatever a tool named for the activity, so an agent's text: one no asset path can be is not looked up.
	if (!FHyperAIStudioNames::IsObjectPath(Target, nullptr))
	{
		return;
	}
	const FSoftObjectPath Path(Target);
	if (Path.IsNull())
	{
		return;
	}
	const FAssetData Asset = IAssetRegistry::GetChecked().GetAssetByObjectPath(Path);
	if (!Asset.IsValid())
	{
		return;
	}
	FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowser.Get().SyncBrowserToAssets({Asset});
}

#undef LOCTEXT_NAMESPACE
