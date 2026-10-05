// Games by Hyper 2026.

#include "SHyperAIStudioPromptPicker.h"

#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioWidgets.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioPromptPicker"

const TCHAR* const SHyperAIStudioPromptPicker::ForSelection = TEXT(":selection");
const TCHAR* const SHyperAIStudioPromptPicker::RecentCategory = TEXT(":recent");

namespace HyperAIStudio::PromptPicker
{
	FText DifficultyLabel(const FString& Difficulty)
	{
		if (Difficulty == TEXT("starter"))
		{
			return LOCTEXT("Starter", "Starter");
		}
		return Difficulty == TEXT("advanced") ? LOCTEXT("Advanced", "Advanced") : LOCTEXT("Standard", "Standard");
	}

	HyperAIStudio::UI::EStatus DifficultyStatus(const FString& Difficulty)
	{
		using HyperAIStudio::UI::EStatus;
		return Difficulty == TEXT("starter") ? EStatus::Good : Difficulty == TEXT("advanced") ? EStatus::Warning : EStatus::Info;
	}

	/** An agent, skill or tool the preset goes with, as a small grey tag. */
	TSharedRef<SWidget> MakeTag(const FString& Text, const FText& ToolTip)
	{
		return SNew(SBorder)
			.BorderImage(HyperAIStudio::UI::Style().GetBrush("HyperAIStudio.Badge"))
			.Padding(FMargin(6.0f, 1.0f))
			.ToolTipText(ToolTip)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Text))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
	}
}

void SHyperAIStudioPromptPicker::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::PromptPresets;
	using namespace HyperAIStudio::UI;
	OnPresetChosen = InArgs._OnPresetChosen;
	OnClose = InArgs._OnClose;
	// A copy: the library reloads when its files change, and this list must not change under the user.
	Library = Get(InArgs._ProjectRoot.IsEmpty() ? FHyperAIStudioService::GetProjectRoot() : InArgs._ProjectRoot);
	Context = InArgs._Context.IsSet() ? InArgs._Context.GetValue() : CaptureContext();
	// Once per opening: which packs are on decides which presets are greyed.
	TArray<FHyperAIStudioRuntimeToolBinding> Bindings;
	TArray<FString> Diagnostics;
	FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(Bindings, Diagnostics);
	EnabledPacks = HyperAIStudio::PromptPresets::EnabledPacks(Bindings);
	for (const FHyperAIStudioPromptPreset& Preset : Library.Presets)
	{
		All.Add(MakeShared<FHyperAIStudioPromptPreset>(Preset));
	}
	// What fits the selection comes first when anything does.
	if (CountIn(ForSelection) > 0)
	{
		CategoryId = ForSelection;
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(170.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SAssignNew(SearchBox, SSearchBox)
						.HintText(LOCTEXT("SearchHint", "Search prompts"))
						.OnTextChanged_Lambda([this](const FText& Text)
						{
							if (Text.ToString() != Query)
							{
								Query = Text.ToString();
								Refresh();
							}
						})
						.OnKeyDownHandler(this, &SHyperAIStudioPromptPicker::OnSearchKey)
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							BuildCategories()
						]
					]
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.5f)
			.Padding(8.0f, 0.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SAssignNew(List, SListView<FPresetPtr>)
					.ListItemsSource(&Shown)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SHyperAIStudioPromptPicker::MakeRow)
					.OnSelectionChanged_Lambda([this](FPresetPtr Preset, ESelectInfo::Type)
					{
						if (Preset.IsValid() && Preset != Selected)
						{
							Select(Preset);
						}
					})
					.OnMouseButtonDoubleClick_Lambda([this](FPresetPtr Preset)
					{
						Select(Preset);
						Choose();
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]() { return Shown.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					.AutoWrapText(true)
					.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					.Text_Lambda([this]()
					{
						return All.IsEmpty() ? LOCTEXT("NoLibrary", "No prompts are installed. The plugin's Resources/Prompts holds them.")
							: LOCTEXT("NoMatch", "No prompt matches. Try other words or another category.");
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					// A prompt file that could not be read, such as a project's own with a typo, says so instead of leaving its presets out quietly.
					SNew(STextBlock)
					.Visibility(Library.Errors.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					.AutoWrapText(true)
					.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					.ColorAndOpacity(StatusColor(EStatus::Warning))
					.ToolTipText(FText::FromString(FString::Join(Library.Errors, TEXT("\n"))))
					.Text(Library.Errors.IsEmpty() ? FText::GetEmpty()
						: FText::Format(LOCTEXT("LibraryErrors", "Some prompts could not be read: {0}"), FText::FromString(Library.Errors[0])))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(0.5f)
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([this]() { return Selected.IsValid() ? EVisibility::Visible : EVisibility::Hidden; })
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.TextStyle(&Style(), "HyperAIStudio.Text.CardTitle")
					.AutoWrapText(true)
					.Text_Lambda([this]() { return Selected.IsValid() ? Selected->DisplayTitle() : FText::GetEmpty(); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					.AutoWrapText(true)
					.Text_Lambda([this]() { return Selected.IsValid() ? Selected->DisplaySummary() : FText::GetEmpty(); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SAssignNew(Tags, SWrapBox)
					.UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(4.0f, 4.0f))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.MaxDesiredHeight(150.0f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SAssignNew(Fields, SVerticalBox)
						]
					]
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SBorder)
					.BorderImage(Style().GetBrush("HyperAIStudio.CodeBlock"))
					.Padding(FMargin(8.0f, 6.0f))
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
							.ToolTipText(LOCTEXT("PreviewTooltip", "The prompt as it goes in the composer. Edit it there before you send."))
							.Text_Lambda([this]() { return FText::FromString(GetFilledText()); })
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						Button(LOCTEXT("InsertBlanks", "Insert and edit blanks"), FOnClicked::CreateLambda([this]()
						{
							Choose(/*bUseDefaults=*/false);
							return FReply::Handled();
						}), nullptr, false, LOCTEXT("InsertBlanksTooltip", "Put the prompt in the composer with every field you did not type left as a [blank] to fill there."))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						Button(LOCTEXT("Insert", "Insert"), FOnClicked::CreateLambda([this]()
						{
							Choose();
							return FReply::Handled();
						}), nullptr, /*bPrimary=*/true, LOCTEXT("InsertTooltip", "Put the prompt in the composer as shown. Nothing is sent until you send it."))
					]
				]
			]
		]
	];
	Refresh();
}

TSharedRef<SWidget> SHyperAIStudioPromptPicker::BuildCategories()
{
	const TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	Box->AddSlot().AutoHeight()[CategoryChip(ForSelection, LOCTEXT("ForSelection", "For your selection"), CountIn(ForSelection))];
	Box->AddSlot().AutoHeight()[CategoryChip(RecentCategory, LOCTEXT("Recent", "Recent"), CountIn(RecentCategory))];
	Box->AddSlot().AutoHeight()[CategoryChip(FString(), LOCTEXT("AllPrompts", "All prompts"), All.Num())];
	for (const FHyperAIStudioPromptCategory& Category : Library.Categories)
	{
		Box->AddSlot().AutoHeight()[CategoryChip(Category.Id, Category.DisplayTitle(), CountIn(Category.Id))];
	}
	return Box;
}

TSharedRef<SWidget> SHyperAIStudioPromptPicker::CategoryChip(const FString& Id, const FText& Label, const int32 Count)
{
	using namespace HyperAIStudio::UI;
	// The two lists that are not categories show only when they have something; so do empty categories.
	return SNew(SBox)
		.Visibility(Count > 0 || Id.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed)
		[
			SNew(SBorder)
			.BorderImage_Lambda([this, Id]() { return CategoryId == Id ? Style().GetBrush("HyperAIStudio.NavItem.Selected") : FAppStyle::GetNoBrush(); })
			.Padding(0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(6.0f, 3.0f))
				.OnClicked_Lambda([this, Id]()
				{
					SetCategory(Id);
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(Label)
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.TextStyle(&Style(), "HyperAIStudio.Text.Body")
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(FText::AsNumber(Count))
						.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					]
				]
			]
		];
}

TSharedRef<ITableRow> SHyperAIStudioPromptPicker::MakeRow(FPresetPtr Preset, const TSharedRef<STableViewBase>& Owner)
{
	using namespace HyperAIStudio::UI;
	const FString Missing = HyperAIStudio::PromptPresets::MissingPack(*Preset, EnabledPacks);
	const bool bFits = Fits(*Preset);
	return SNew(STableRow<FPresetPtr>, Owner)
		.Padding(FMargin(6.0f, 4.0f))
		.ToolTipText(Missing.IsEmpty() ? Preset->DisplaySummary()
			: FText::Format(LOCTEXT("NeedsPack", "Needs the {0} pack, which is not loaded in this project."), FText::FromString(Missing)))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetNoBrush())
			.Padding(0.0f)
			.ColorAndOpacity(Missing.IsEmpty() ? FLinearColor::White : FLinearColor(1.0f, 1.0f, 1.0f, 0.45f))
			[
				// The title has the row's whole width; the difficulty sits beside the summary.
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 5.0f, 0.0f)
					[
						SNew(SBox)
						.Visibility(bFits ? EVisibility::Visible : EVisibility::Collapsed)
						.ToolTipText(LOCTEXT("FitsTooltip", "Made for what you have selected."))
						[
							Dot(StatusColor(EStatus::Good))
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(Preset->DisplayTitle())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.TextStyle(&Style(), "HyperAIStudio.Text.Body")
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Preset->DisplaySummary())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.TextStyle(&Style(), "HyperAIStudio.Text.Caption")
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						Chip(HyperAIStudio::PromptPicker::DifficultyLabel(Preset->Difficulty),
							StatusColor(HyperAIStudio::PromptPicker::DifficultyStatus(Preset->Difficulty)))
					]
				]
			]
		];
}

void SHyperAIStudioPromptPicker::Refresh()
{
	using namespace HyperAIStudio::PromptPresets;
	Shown.Reset();
	const bool bOwnList = CategoryId == ForSelection || CategoryId == RecentCategory;
	const TArray<int32> Found = Search(Library, Query, bOwnList ? FString() : CategoryId, &Context, 0);
	if (CategoryId == RecentCategory)
	{
		// Newest first, and only those the search keeps.
		TSet<int32> Kept;
		Kept.Append(Found);
		for (const FString& Id : Recent())
		{
			const int32 Index = Library.Presets.IndexOfByPredicate([&Id](const FHyperAIStudioPromptPreset& Each) { return Each.Id == Id; });
			if (Index != INDEX_NONE && Kept.Contains(Index))
			{
				Shown.Add(All[Index]);
			}
		}
	}
	else
	{
		for (const int32 Index : Found)
		{
			if (CategoryId != ForSelection || Fits(*All[Index]))
			{
				Shown.Add(All[Index]);
			}
		}
	}
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
	Select(Shown.Contains(Selected) ? Selected : Shown.IsEmpty() ? FPresetPtr() : Shown[0]);
}

void SHyperAIStudioPromptPicker::Select(const FPresetPtr& Preset)
{
	if (Preset != Selected)
	{
		Selected = Preset;
		Values.Reset();
		Edited.Reset();
		if (Selected.IsValid())
		{
			// Each field starts with what the preset would use: its default, or what is selected for a context token.
			for (const FHyperAIStudioPromptPlaceholder& Placeholder : Selected->Placeholders)
			{
				Values.Add(Placeholder.Name, Placeholder.Default);
			}
			for (const FString& Name : HyperAIStudio::PromptPresets::UsedNames(Selected->Prompt))
			{
				if (!Values.Contains(Name))
				{
					Values.Add(Name, HyperAIStudio::PromptPresets::ContextValue(Name, Context));
				}
			}
		}
		RebuildDetails();
	}
	if (List.IsValid())
	{
		if (Preset.IsValid())
		{
			List->SetSelection(Preset, ESelectInfo::Direct);
			List->RequestScrollIntoView(Preset);
		}
		else
		{
			List->ClearSelection();
		}
	}
}

void SHyperAIStudioPromptPicker::RebuildDetails()
{
	using namespace HyperAIStudio::PromptPicker;
	if (!Tags.IsValid() || !Fields.IsValid())
	{
		return;
	}
	Tags->ClearChildren();
	Fields->ClearChildren();
	if (!Selected.IsValid())
	{
		return;
	}
	if (!Selected->Agent.IsEmpty())
	{
		Tags->AddSlot()[MakeTag(Selected->Agent == TEXT("team") ? FString(TEXT("team")) : TEXT("@") + Selected->Agent,
			Selected->Agent == TEXT("team") ? LOCTEXT("TeamTag", "Goes to HyperAI's team: the Team switch turns on.")
				: LOCTEXT("AgentTag", "The project agent this task is handed to, when the project has it."))];
	}
	for (const FString& Skill : Selected->Skills)
	{
		Tags->AddSlot()[MakeTag(Skill,LOCTEXT("SkillTag", "A skill the agent is pointed at."))];
	}
	for (const FString& Tool : Selected->Tools)
	{
		Tags->AddSlot()[MakeTag(Tool,LOCTEXT("ToolTag", "A HyperAI tool the prompt uses."))];
	}
	auto AddField = [this](const FString& Name, const FText& Label, const FText& Hint)
	{
		Fields->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(110.0f)
				[
					SNew(STextBlock)
					.Text(Label)
					.ToolTipText(Hint)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					.TextStyle(&HyperAIStudio::UI::Style(), "HyperAIStudio.Text.Caption")
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SEditableTextBox)
				.Text(FText::FromString(Values.FindRef(Name)))
				.HintText(Hint.IsEmpty() ? Label : Hint)
				.ToolTipText(Hint)
				.OnTextChanged_Lambda([this, Name](const FText& Text)
				{
					Values.Add(Name, Text.ToString());
					Edited.Add(Name);
				})
			]
		];
	};
	for (const FHyperAIStudioPromptPlaceholder& Placeholder : Selected->Placeholders)
	{
		AddField(Placeholder.Name, Selected->DisplayLabel(Placeholder), Selected->DisplayHint(Placeholder));
	}
	for (const FString& Name : HyperAIStudio::PromptPresets::UsedNames(Selected->Prompt))
	{
		if (!Selected->Placeholders.ContainsByPredicate([&Name](const FHyperAIStudioPromptPlaceholder& Each) { return Each.Name == Name; }))
		{
			AddField(Name, HyperAIStudio::PromptPresets::TokenLabel(Name), LOCTEXT("TokenHint", "From what you have selected in the editor."));
		}
	}
}

void SHyperAIStudioPromptPicker::Move(const int32 Step)
{
	if (Shown.IsEmpty())
	{
		return;
	}
	const int32 Index = Shown.IndexOfByKey(Selected);
	Select(Shown[FMath::Clamp(Index == INDEX_NONE ? 0 : Index + Step, 0, Shown.Num() - 1)]);
}

FReply SHyperAIStudioPromptPicker::OnSearchKey(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	return OnKeyDown(Geometry, KeyEvent);
}

FReply SHyperAIStudioPromptPicker::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Down || Key == EKeys::Up)
	{
		Move(Key == EKeys::Down ? 1 : -1);
		return FReply::Handled();
	}
	if (Key == EKeys::Enter)
	{
		Choose();
		return FReply::Handled();
	}
	if (Key == EKeys::Escape)
	{
		OnClose.ExecuteIfBound();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

TSharedPtr<SWidget> SHyperAIStudioPromptPicker::GetSearchBox() const
{
	return SearchBox;
}

void SHyperAIStudioPromptPicker::SetSearchText(const FString& InQuery)
{
	Query = InQuery;
	if (SearchBox.IsValid())
	{
		SearchBox->SetText(FText::FromString(InQuery));
	}
	Refresh();
}

void SHyperAIStudioPromptPicker::SetCategory(const FString& InCategoryId)
{
	CategoryId = InCategoryId;
	Refresh();
}

const FHyperAIStudioPromptPreset* SHyperAIStudioPromptPicker::GetShown(const int32 Index) const
{
	return Shown.IsValidIndex(Index) ? Shown[Index].Get() : nullptr;
}

const FHyperAIStudioPromptPreset* SHyperAIStudioPromptPicker::GetSelected() const
{
	return Selected.Get();
}

FString SHyperAIStudioPromptPicker::GetFilledText(TArray<FString>* OutBlanks, const bool bUseDefaults) const
{
	if (!Selected.IsValid())
	{
		return FString();
	}
	TMap<FString, FString> Use;
	for (const TPair<FString, FString>& Value : Values)
	{
		// Without defaults only what was typed counts; context tokens still fill from the selection.
		if (bUseDefaults || Edited.Contains(Value.Key))
		{
			Use.Add(Value.Key, Value.Value);
		}
	}
	TArray<FString> Blanks;
	const FString Text = HyperAIStudio::PromptPresets::Fill(*Selected, Context, Use, Blanks, bUseDefaults);
	if (OutBlanks)
	{
		*OutBlanks = MoveTemp(Blanks);
	}
	return Text;
}

bool SHyperAIStudioPromptPicker::Choose(const bool bUseDefaults)
{
	if (!Selected.IsValid())
	{
		return false;
	}
	// The owner usually closes the picker as it takes the prompt; this keeps it alive until the call returns.
	const TSharedRef<SWidget> KeepAlive = AsShared();
	const FPresetPtr Chosen = Selected;
	TArray<FString> Blanks;
	const FString Text = GetFilledText(&Blanks, bUseDefaults);
	OnPresetChosen.ExecuteIfBound(*Chosen, Text, Blanks);
	return true;
}

int32 SHyperAIStudioPromptPicker::CountIn(const FString& InCategoryId) const
{
	if (InCategoryId.IsEmpty())
	{
		return All.Num();
	}
	if (InCategoryId == RecentCategory)
	{
		int32 Count = 0;
		for (const FString& Id : HyperAIStudio::PromptPresets::Recent())
		{
			Count += Library.Find(Id) ? 1 : 0;
		}
		return Count;
	}
	int32 Count = 0;
	for (const FPresetPtr& Preset : All)
	{
		Count += (InCategoryId == ForSelection ? Fits(*Preset) : Preset->Category == InCategoryId) ? 1 : 0;
	}
	return Count;
}

bool SHyperAIStudioPromptPicker::Fits(const FHyperAIStudioPromptPreset& Preset) const
{
	return HyperAIStudio::PromptPresets::FitsSelection(Preset, Context);
}

#undef LOCTEXT_NAMESPACE
