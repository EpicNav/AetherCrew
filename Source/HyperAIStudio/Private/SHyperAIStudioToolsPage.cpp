// Games by Hyper 2026.

#include "SHyperAIStudioToolsPage.h"

#include "HyperAIStudioCapabilityInventoryClient.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioWidgets.h"
#include "SHyperAIStudioToolForm.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Styling/StyleDefaults.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioToolsPage"

void SHyperAIStudioToolsPage::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::UI;
	const TSharedRef<FHyperAIStudioService> Service = MakeShared<FHyperAIStudioService>();
	Endpoint = Service->GetMcpRuntimeStatusFast().Endpoint;
	ChildSlot
	[
		SNew(SSplitter)
		.PhysicalSplitterHandleSize(1.0f)
		+ SSplitter::Slot().Value(0.42f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(20.0f, 20.0f, 14.0f, 12.0f))
			[
				PageHeader(LOCTEXT("Title", "Tools"),
					TAttribute<FText>::CreateLambda([this]()
					{
						if (bRefreshing && All.Num() == 0)
						{
							return LOCTEXT("Loading", "Reading the tools from Unreal MCP...");
						}
						return FText::Format(LOCTEXT("Summary", "{0} tools agents can call: {1} from HyperAI and {2} from Epic."),
							View.GetAvailableToolCount(), View.AvailableHyperToolCount, View.LiveEpicToolCount);
					}),
					IconButton(FAppStyle::GetBrush("Icons.Refresh"), LOCTEXT("RefreshTooltip", "Ask Unreal MCP for its tools again."),
						FOnClicked::CreateLambda([this]()
						{
							Refresh();
							return FReply::Handled();
						})))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(20.0f, 0.0f, 14.0f, 8.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SSearchBox)
					.HintText(LOCTEXT("SearchHint", "Search tools by name, description or toolset"))
					.OnTextChanged_Lambda([this](const FText& Text) { SetQuery(Text.ToString()); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(20.0f, 0.0f, 14.0f, 8.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SourceButton(LOCTEXT("All", "All"), {})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4.0f, 0.0f))
				[
					SourceButton(LOCTEXT("HyperAI", "HyperAI"), EHyperAIStudioToolInventorySource::HyperAI)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SourceButton(LOCTEXT("Epic", "Epic"), EHyperAIStudioToolInventorySource::Epic)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					.Text_Lambda([this]() { return FText::Format(LOCTEXT("Shown", "{0} shown"), Shown.Num()); })
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(FMargin(14.0f, 0.0f, 8.0f, 8.0f))
			[
				SAssignNew(List, SListView<FItem>)
				.ListItemsSource(&Shown)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SHyperAIStudioToolsPage::GenerateRow)
				.OnSelectionChanged_Lambda([this](FItem Item, ESelectInfo::Type)
				{
					if (Item.IsValid())
					{
						ShowTool(Item);
					}
				})
			]
		]
		+ SSplitter::Slot().Value(0.58f)
		[
			SAssignNew(FormHost, SBox)
			[
				EmptyState(Icon("HyperAIStudio.Icon.Tools"), LOCTEXT("PickTitle", "Pick a tool"),
					LOCTEXT("PickBody", "Its request opens here as a form. Run calls it the way an agent's MCP call would, and its answer shows below."))
			]
		]
	];
	if (const TOptional<FHyperAIStudioCapabilityInventoryResult> Cached = FHyperAIStudioCapabilityInventoryClient::GetLastDetailedSuccess(Endpoint))
	{
		SetView(FHyperAIStudioToolInventoryViewModel::Build(Cached.GetValue()));
	}
	Refresh();
}

void SHyperAIStudioToolsPage::Refresh()
{
	if (bRefreshing || Endpoint.IsEmpty())
	{
		return;
	}
	bRefreshing = true;
	FHyperAIStudioCapabilityInventoryClient::RefreshDetailedAsync(Endpoint, /*bForce=*/false, EHyperAIStudioDetailedInventoryScope::EpicToolsets,
		[this, Alive = bAlive](const FHyperAIStudioCapabilityInventoryResult& Result)
		{
			if (!*Alive)
			{
				return;
			}
			bRefreshing = false;
			FHyperAIStudioToolInventoryView Fresh = FHyperAIStudioToolInventoryViewModel::Build(Result);
			// A failed refresh keeps the last list it had.
			if (Fresh.bExactLiveInventory || All.Num() == 0)
			{
				SetView(MoveTemp(Fresh));
			}
		});
}

void SHyperAIStudioToolsPage::SetView(FHyperAIStudioToolInventoryView InView)
{
	View = MoveTemp(InView);
	All.Reset(View.Rows.Num());
	for (const FHyperAIStudioToolInventoryRow& Row : View.Rows)
	{
		All.Add(MakeShared<FHyperAIStudioToolInventoryRow>(Row));
	}
	// HyperAI's tools first, then by name.
	All.StableSort([](const FItem& A, const FItem& B)
	{
		return A->Source != B->Source ? A->Source == EHyperAIStudioToolInventorySource::HyperAI : A->Name < B->Name;
	});
	Filter();
}

void SHyperAIStudioToolsPage::Filter()
{
	Shown.Reset(All.Num());
	for (const FItem& Row : All)
	{
		if (FHyperAIStudioToolInventoryViewModel::Matches(*Row, Query, Source))
		{
			Shown.Add(Row);
		}
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

void SHyperAIStudioToolsPage::SetQuery(const FString& InQuery)
{
	Query = InQuery;
	Filter();
}

void SHyperAIStudioToolsPage::SetSource(const TOptional<EHyperAIStudioToolInventorySource> InSource)
{
	Source = InSource;
	Filter();
}

bool SHyperAIStudioToolsPage::SelectTool(const FString& Name)
{
	const FItem* Found = All.FindByPredicate([&Name](const FItem& Row) { return Row->Name == Name || Row->QualifiedName == Name; });
	if (!Found)
	{
		return false;
	}
	ShowTool(*Found);
	if (List.IsValid() && Shown.Contains(*Found))
	{
		List->SetSelection(*Found, ESelectInfo::Direct);
		List->RequestScrollIntoView(*Found);
	}
	return true;
}

void SHyperAIStudioToolsPage::ShowTool(const FItem& Item)
{
	using namespace HyperAIStudio::UI;
	if (!FormHost.IsValid() || Item == Selected)
	{
		return;
	}
	Selected = Item;
	FormHost->SetContent(
		Item->bAvailableNow
			? StaticCastSharedRef<SWidget>(SNew(SHyperAIStudioToolForm).Toolset(Item->Toolset).Tool(Item->Name).Description(Item->Description))
			: EmptyState(FAppStyle::GetBrush("Icons.Warning"), FText::FromString(Item->Name),
				LOCTEXT("NotLoadedBody", "This tool's toolset is not loaded in this editor, so it cannot run here. Its plugin may be disabled.")));
}

TSharedRef<SWidget> SHyperAIStudioToolsPage::SourceButton(const FText& Label, const TOptional<EHyperAIStudioToolInventorySource> InSource)
{
	using namespace HyperAIStudio::UI;
	return SNew(SButton)
		.ButtonStyle(&Style(), "HyperAIStudio.NavItem")
		.ContentPadding(0.0f)
		.OnClicked_Lambda([this, InSource]()
		{
			SetSource(InSource);
			return FReply::Handled();
		})
		[
			SNew(SBorder)
			.BorderImage_Lambda([this, InSource]() { return Source == InSource ? HyperAIStudio::UI::Style().GetBrush("HyperAIStudio.NavItem.Selected") : FStyleDefaults::GetNoBrush(); })
			.Padding(FMargin(10.0f, 3.0f))
			[
				SNew(STextBlock)
				.Text(Label)
				.TextStyle(&Style(), "HyperAIStudio.Text.Body")
				.ColorAndOpacity_Lambda([this, InSource]() { return Source == InSource ? FStyleColors::ForegroundHover : FStyleColors::Foreground; })
			]
		];
}

TSharedRef<ITableRow> SHyperAIStudioToolsPage::GenerateRow(FItem Item, const TSharedRef<STableViewBase>& Owner)
{
	using namespace HyperAIStudio::UI;
	const bool bHyper = Item->Source == EHyperAIStudioToolInventorySource::HyperAI;
	return SNew(STableRow<FItem>, Owner)
		.Padding(FMargin(8.0f, 6.0f))
		.ToolTipText(FText::FromString(Item->QualifiedName + TEXT("\n") + Item->Toolset))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->Name))
					.TextStyle(&Style(), "HyperAIStudio.Text.CardTitle")
					.ColorAndOpacity(Item->bAvailableNow ? FSlateColor(FStyleColors::ForegroundHover) : FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
				[
					Chip(bHyper ? LOCTEXT("ChipHyper", "HYPERAI") : LOCTEXT("ChipEpic", "EPIC"),
						bHyper ? StatusColor(EStatus::Info) : StatusColor(EStatus::Neutral))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->bAvailableNow ? Item->Description : TEXT("Not loaded in this editor. ") + Item->Description))
				.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
				.AutoWrapText(true)
			]
		];
}

#undef LOCTEXT_NAMESPACE
