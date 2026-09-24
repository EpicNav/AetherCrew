// Games by Hyper 2026.

#include "SHyperAIStudioImagesPanel.h"

#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioPreviewPanel.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioImagesPanel"

namespace HyperAIStudio::ImagesPanel
{
	constexpr int32 MaxShown = 60;

	FString FullPath(const FString& Path)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::ProjectDir() / Path : Path);
	}

	FSlateColor HealthColor(const FString& Health)
	{
		return Health == TEXT("ready") ? FSlateColor(FStyleColors::AccentGreen)
			: Health == TEXT("unknown") ? FSlateColor::UseSubduedForeground() : FSlateColor(FStyleColors::AccentRed);
	}

	TSharedRef<SWidget> Label(const FText& Text)
	{
		return SNew(STextBlock).Text(Text).Font(FAppStyle::GetFontStyle("SmallFontBold")).ColorAndOpacity(FSlateColor::UseSubduedForeground());
	}

	TSharedRef<SWidget> Header(const FText& Text)
	{
		return SNew(STextBlock).Text(Text).Font(FCoreStyle::GetDefaultFontStyle("Bold", 11));
	}
}

void SHyperAIStudioImagesPanel::Construct(const FArguments& InArgs)
{
	BatchName = NewBatchName();
	ChildSlot
	[
		SNew(SSplitter)
		.Orientation(Orient_Horizontal)
		+ SSplitter::Slot().Value(0.42f)
		[
			BuildForm()
		]
		+ SSplitter::Slot().Value(0.58f)
		[
			BuildBrowser()
		]
	];
	Reload();
}

FString SHyperAIStudioImagesPanel::NewBatchName() const
{
	return TEXT("images_") + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
}

TSharedRef<SWidget> SHyperAIStudioImagesPanel::BuildForm()
{
	using namespace HyperAIStudio::ImagesPanel;
	auto Number = [](const FText& Name, const int32 Min, const int32 Max, TFunction<int32()> Get, TFunction<void(int32)> Set, const FText& Tooltip)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[Label(Name)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 6.0f, 0.0f)
			[
				SNew(SSpinBox<int32>)
				.MinValue(Min)
				.MaxValue(Max)
				.ToolTipText(Tooltip)
				.Value_Lambda(MoveTemp(Get))
				.OnValueChanged_Lambda(MoveTemp(Set))
			];
	};
	return SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[Header(LOCTEXT("Generate", "Generate"))]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)[Label(LOCTEXT("Model", "Model"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(ModelCombo, SComboBox<FModelPtr>)
				.OptionsSource(&Models)
				.OnGenerateWidget_Lambda([](const FModelPtr& Item)
				{
					return SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)[SNew(STextBlock).Text(FText::FromString(Item->Label.IsEmpty() ? Item->Id : Item->Label))]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock).Text(FText::FromString(Item->Health)).ColorAndOpacity(HealthColor(Item->Health))
						];
				})
				.OnSelectionChanged_Lambda([this](const FModelPtr& Item, ESelectInfo::Type)
				{
					if (Item.IsValid())
					{
						Model = Item;
					}
				})
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return Model.IsValid()
							? FText::Format(LOCTEXT("ModelShown", "{0} ({1})"), FText::FromString(Model->Label.IsEmpty() ? Model->Id : Model->Label), FText::FromString(Model->Health))
							: LOCTEXT("NoModel", "No image model");
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor(FStyleColors::AccentRed))
				.Visibility_Lambda([this]() { return Model.IsValid() && !Model->Issue.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text_Lambda([this]() { return FText::FromString(Model.IsValid() ? Model->Issue : FString()); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)[Label(LOCTEXT("Template", "Prompt template"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(TemplateCombo, SComboBox<FTemplatePtr>)
				.OptionsSource(&Templates)
				.OnGenerateWidget_Lambda([](const FTemplatePtr& Item)
				{
					return SNew(STextBlock).Text(Item.IsValid() ? FText::FromString(Item->Label.IsEmpty() ? Item->Id : Item->Label)
						: LOCTEXT("NoTemplate", "None: send the prompt as written"));
				})
				.OnSelectionChanged_Lambda([this](const FTemplatePtr& Item, ESelectInfo::Type)
				{
					Template = Item;
					RebuildSlots();
				})
				[
					SNew(STextBlock).Text_Lambda([this]()
					{
						return Template.IsValid() ? FText::FromString(Template->Label.IsEmpty() ? Template->Id : Template->Label)
							: LOCTEXT("NoTemplate", "None: send the prompt as written");
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SAssignNew(SlotBox, SVerticalBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)[Label(LOCTEXT("Prompt", "Prompt"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.MinDesiredHeight(84.0f)
				[
					SNew(SMultiLineEditableTextBox)
					.AutoWrapText(true)
					.HintText(LOCTEXT("PromptHint", "What to draw. With a template, this is added after it."))
					.Text_Lambda([this]() { return FText::FromString(Prompt); })
					.OnTextChanged_Lambda([this](const FText& Text) { Prompt = Text.ToString(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)[Label(LOCTEXT("Negative", "Avoid"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("NegativeHint", "What to keep out, such as text or people."))
				.Text_Lambda([this]() { return FText::FromString(Negative); })
				.OnTextChanged_Lambda([this](const FText& Text) { Negative = Text.ToString(); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					Number(LOCTEXT("Count", "Count"), 1, FHyperAIStudioImageGenContracts::MaxImagesPerBatch, [this]() { return Count; }, [this](const int32 Value) { Count = Value; },
						LOCTEXT("CountTooltip", "Images in the batch."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					Number(LOCTEXT("Width", "Width"), 0, FHyperAIStudioImageGenContracts::MaxImageSize, [this]() { return Width; }, [this](const int32 Value) { Width = Value; },
						LOCTEXT("SizeTooltip", "0 for the model's own size."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					Number(LOCTEXT("Height", "Height"), 0, FHyperAIStudioImageGenContracts::MaxImageSize, [this]() { return Height; }, [this](const int32 Value) { Height = Value; },
						LOCTEXT("SizeTooltip", "0 for the model's own size."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					Number(LOCTEXT("Seed", "Seed"), -1, MAX_int32, [this]() { return Seed; }, [this](const int32 Value) { Seed = Value; },
						LOCTEXT("SeedTooltip", "-1 for a random seed. A fixed seed re-renders the same image, where the model takes one."))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 2.0f)[Label(LOCTEXT("Batch", "Batch name"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SEditableTextBox)
				.ToolTipText(LOCTEXT("BatchTooltip", "Letters, digits and underscore: it names the folder the images land in."))
				.Text_Lambda([this]() { return FText::FromString(BatchName); })
				.OnTextChanged_Lambda([this](const FText& Text) { BatchName = Text.ToString(); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("PreviewPrompt", "Preview prompt"))
					.ToolTipText(LOCTEXT("PreviewPromptTooltip", "A free dry run: the exact prompt that would be sent, and what it costs."))
					.OnClicked_Lambda([this]()
					{
						PreviewPrompt();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("GenerateButton", "Generate"))
					.ToolTipText(LOCTEXT("GenerateTooltip", "Generate the batch. This spends money, or your agent's plan for an agent model; pressing it is the approval."))
					.IsEnabled_Lambda([this]() { return Model.IsValid() && Model->Issue.IsEmpty(); })
					.OnClicked_Lambda([this]()
					{
						Generate();
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this]() { return FText::FromString(Status); })
				.ColorAndOpacity_Lambda([this]() { return bStatusIsError ? FSlateColor(FStyleColors::AccentRed) : FSlateColor::UseForeground(); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]() { return ComposedPrompt.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text_Lambda([this]() { return FText::Format(LOCTEXT("Composed", "Will send: {0}"), FText::FromString(ComposedPrompt)); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 4.0f)[Header(LOCTEXT("Batches", "Batches this session"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.Text_Lambda([this]()
				{
					TArray<FString> Lines;
					for (const FHyperAIImageBatchInfo& Batch : Inspect.Batches)
					{
						Lines.Add(FString::Printf(TEXT("%s: %s, %s%s"), *Batch.BatchId, *Batch.State, *Batch.Progress,
							Batch.Diagnostic.IsEmpty() ? TEXT("") : *(TEXT(" - ") + Batch.Diagnostic)));
					}
					return Lines.IsEmpty() ? LOCTEXT("NoBatches", "None yet.") : FText::FromString(FString::Join(Lines, TEXT("\n")));
				})
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioImagesPanel::BuildBrowser()
{
	using namespace HyperAIStudio::ImagesPanel;
	auto Action = [](const FText& Text, const FText& Tooltip, TFunction<void()> Run)
	{
		return SNew(SButton).Text(Text).ToolTipText(Tooltip).OnClicked_Lambda([Run]()
		{
			Run();
			return FReply::Handled();
		});
	};
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8.0f, 8.0f, 8.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				.Text_Lambda([this]()
				{
					return FText::Format(LOCTEXT("Generated", "Generated images ({0}), in {1}"), FText::AsNumber(Inspect.Images.Num()), FText::FromString(Inspect.OutputFolder));
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "Refresh"))
				.OnClicked_Lambda([this]()
				{
					Reload();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().FillHeight(0.45f).Padding(8.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(6.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(Gallery, SWrapBox)
					.UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(6.0f, 6.0f))
				]
			]
		]
		+ SVerticalBox::Slot().FillHeight(0.55f).Padding(8.0f, 8.0f)
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this]() { return SelectedRecord() ? EVisibility::Visible : EVisibility::Hidden; })
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SScaleBox)
				.Stretch(EStretch::ScaleToFit)
				[
					SNew(SImage).Image_Lambda([this]()
					{
						const FHyperAIImageRecord* Record = SelectedRecord();
						return Record ? Large.Get(FullPath(Record->File)) : nullptr;
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this]()
				{
					const FHyperAIImageRecord* Record = SelectedRecord();
					return Record ? FText::FromString(Record->Prompt.IsEmpty() ? TEXT("(no prompt recorded)") : Record->Prompt) : FText::GetEmpty();
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]()
				{
					const FHyperAIImageRecord* Record = SelectedRecord();
					return Record ? FText::FromString(FString::Printf(TEXT("%s | %s | seed %d | %dx%d | %s | %s"), *Record->ModelId, *Record->BatchId, Record->Seed,
						Record->Width, Record->Height, *Record->GeneratedUtc.Left(19), *Record->File)) : FText::GetEmpty();
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(6.0f, 6.0f))
				+ SWrapBox::Slot()
				[
					Action(LOCTEXT("OpenPreview", "Open in Preview"), LOCTEXT("OpenPreviewTooltip", "Measure the level viewport's lighting against it in the HyperAI Preview tab."),
						[this]() { if (const FHyperAIImageRecord* Record = SelectedRecord()) FHyperAIStudioPreviewPanel::Open(FName(TEXT("lighting")), FullPath(Record->File)); })
				]
				+ SWrapBox::Slot()
				[
					Action(LOCTEXT("UseMatch", "Use as match reference"), LOCTEXT("UseMatchTooltip", "Open the Image match source with it: score attempts against it, iteration by iteration."),
						[this]() { if (const FHyperAIImageRecord* Record = SelectedRecord()) FHyperAIStudioPreviewPanel::Open(FName(TEXT("image_match")), FullPath(Record->File)); })
				]
				+ SWrapBox::Slot()
				[
					Action(LOCTEXT("Reuse", "Reuse prompt"), LOCTEXT("ReuseTooltip", "Put its prompt, model and seed back in the form."),
						[this]()
						{
							if (const FHyperAIImageRecord* Record = SelectedRecord())
							{
								Prompt = Record->Prompt;
								Seed = Record->Seed;
								SelectModel(Record->ModelId);
							}
						})
				]
				+ SWrapBox::Slot()
				[
					Action(LOCTEXT("CopyPath", "Copy path"), LOCTEXT("CopyPathTooltip", "Copy its project-relative path, to paste into a chat."),
						[this]() { if (const FHyperAIImageRecord* Record = SelectedRecord()) FPlatformApplicationMisc::ClipboardCopy(*Record->File); })
				]
				+ SWrapBox::Slot()
				[
					Action(LOCTEXT("ShowFolder", "Show in folder"), LOCTEXT("ShowFolderTooltip", "Open the folder it is in."),
						[this]() { if (const FHyperAIImageRecord* Record = SelectedRecord()) FPlatformProcess::ExploreFolder(*FullPath(Record->File)); })
				]
			]
		];
}

void SHyperAIStudioImagesPanel::Reload()
{
	FHyperAIImageInspectRequest Request;
	Request.MaxOutputBytes = FHyperAIStudioImageGenContracts::MaxOutputBytes;
	Inspect = FHyperAIStudioImageGenContracts::Inspect(Request);
	const FString KeptModel = Model.IsValid() ? Model->Id : FString();
	const FString KeptTemplate = Template.IsValid() ? Template->Id : FString();
	Models.Reset();
	for (const FHyperAIImageModelInfo& Info : Inspect.Models)
	{
		Models.Add(MakeShared<FHyperAIImageModelInfo>(Info));
	}
	Model.Reset();
	if (!SelectModel(KeptModel))
	{
		// The first model that can generate now, or the first at all.
		const FModelPtr* Usable = Models.FindByPredicate([](const FModelPtr& Item) { return Item->Issue.IsEmpty(); });
		Model = Usable ? *Usable : Models.IsEmpty() ? nullptr : Models[0];
	}
	Templates.Reset();
	Templates.Add(nullptr);
	Template.Reset();
	for (const FHyperAIImageTemplateInfo& Info : Inspect.Templates)
	{
		Templates.Add(MakeShared<FHyperAIImageTemplateInfo>(Info));
		if (Info.Id == KeptTemplate)
		{
			Template = Templates.Last();
		}
	}
	if (ModelCombo.IsValid())
	{
		ModelCombo->RefreshOptions();
		ModelCombo->SetSelectedItem(Model);
	}
	if (TemplateCombo.IsValid())
	{
		TemplateCombo->RefreshOptions();
		TemplateCombo->SetSelectedItem(Template);
	}
	if (Selected >= Inspect.Images.Num())
	{
		Selected = Inspect.Images.IsEmpty() ? INDEX_NONE : 0;
	}
	RebuildSlots();
	RebuildGallery();
}

bool SHyperAIStudioImagesPanel::SelectModel(const FString& Id)
{
	for (const FModelPtr& Item : Models)
	{
		if (!Id.IsEmpty() && Item->Id.Equals(Id, ESearchCase::IgnoreCase))
		{
			Model = Item;
			if (ModelCombo.IsValid())
			{
				ModelCombo->SetSelectedItem(Model);
			}
			return true;
		}
	}
	return false;
}

void SHyperAIStudioImagesPanel::SelectImage(const int32 Index)
{
	Selected = Inspect.Images.IsValidIndex(Index) ? Index : INDEX_NONE;
}

const FHyperAIImageRecord* SHyperAIStudioImagesPanel::SelectedRecord() const
{
	return Inspect.Images.IsValidIndex(Selected) ? &Inspect.Images[Selected] : nullptr;
}

void SHyperAIStudioImagesPanel::RebuildSlots()
{
	using namespace HyperAIStudio::ImagesPanel;
	if (!SlotBox.IsValid())
	{
		return;
	}
	SlotBox->ClearChildren();
	if (!Template.IsValid())
	{
		return;
	}
	for (const FString& Slot : Template->Slots)
	{
		SlotBox->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(110.0f)[Label(FText::FromString(Slot.Replace(TEXT("_"), TEXT(" "))))]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SEditableTextBox)
				.Text_Lambda([this, Slot]() { const FString* Value = SlotValues.Find(Slot); return FText::FromString(Value ? *Value : FString()); })
				.OnTextChanged_Lambda([this, Slot](const FText& Text) { SlotValues.Add(Slot, Text.ToString()); })
			]
		];
	}
}

void SHyperAIStudioImagesPanel::RebuildGallery()
{
	using namespace HyperAIStudio::ImagesPanel;
	if (!Gallery.IsValid())
	{
		return;
	}
	Gallery->ClearChildren();
	Thumbnails.Reset();
	for (int32 Index = 0; Index < Inspect.Images.Num() && Index < MaxShown; ++Index)
	{
		const FString Path = FullPath(Inspect.Images[Index].File);
		const TSharedPtr<FHyperAIStudioPreviewImage> Image = Thumbnails.Add_GetRef(MakeShared<FHyperAIStudioPreviewImage>());
		Gallery->AddSlot()
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(FText::FromString(Inspect.Images[Index].Prompt.Left(300)))
			.OnClicked_Lambda([this, Index]()
			{
				SelectImage(Index);
				return FReply::Handled();
			})
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.BorderBackgroundColor_Lambda([this, Index]() { return Selected == Index ? FLinearColor(0.85f, 0.55f, 0.1f) : FLinearColor(0.16f, 0.16f, 0.18f); })
				.Padding(2.0f)
				[
					SNew(SBox).WidthOverride(132.0f).HeightOverride(88.0f)
					[
						SNew(SScaleBox)
						.Stretch(EStretch::ScaleToFit)
						[
							SNew(SImage).Image_Lambda([Image, Path]() { return Image->Get(Path); })
						]
					]
				]
			]
		];
	}
}

FHyperAIImageGenerateRequest SHyperAIStudioImagesPanel::BuildRequest(const bool bDryRun) const
{
	FHyperAIImageGenerateRequest Request;
	Request.bDryRun = bDryRun;
	Request.ModelId = Model.IsValid() ? Model->Id : FString();
	Request.TemplateId = Template.IsValid() ? Template->Id : FString();
	for (const TPair<FString, FString>& Slot : SlotValues)
	{
		if (Template.IsValid() && Template->Slots.Contains(Slot.Key) && !Slot.Value.TrimStartAndEnd().IsEmpty())
		{
			FHyperAIImageSlot& Filled = Request.Slots.AddDefaulted_GetRef();
			Filled.Name = Slot.Key;
			Filled.Value = Slot.Value.TrimStartAndEnd();
		}
	}
	Request.Prompt = Prompt.TrimStartAndEnd();
	Request.Negative = Negative.TrimStartAndEnd();
	Request.BatchName = BatchName.TrimStartAndEnd();
	Request.Count = Count;
	Request.Width = Width;
	Request.Height = Height;
	Request.Seed = Seed;
	return Request;
}

FHyperAIImageGenerateReport SHyperAIStudioImagesPanel::PreviewPrompt()
{
	const FHyperAIImageGenerateReport Plan = FHyperAIStudioImageGenContracts::BuildPlan(BuildRequest(/*bDryRun=*/true));
	ComposedPrompt = Plan.ComposedPrompt;
	Status = Plan.Diagnostic;
	bStatusIsError = !Plan.bOk;
	return Plan;
}

FHyperAIImageGenerateReport SHyperAIStudioImagesPanel::Generate()
{
	FHyperAIImageGenerateRequest Request = BuildRequest(/*bDryRun=*/true);
	const FHyperAIImageGenerateReport Plan = FHyperAIStudioImageGenContracts::BuildPlan(Request);
	ComposedPrompt = Plan.ComposedPrompt;
	if (!Plan.bOk)
	{
		Status = Plan.Diagnostic;
		bStatusIsError = true;
		return Plan;
	}
	Request.bDryRun = false;
	Request.OperationId = TEXT("images-panel-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(16).ToLower();
	Request.ExpectedPlanHash = Plan.PlanHash;
	FHyperAIImageGenerateReport Submitted = FHyperAIStudioImageGenContracts::BuildPlan(Request);
	if (!Submitted.bOk)
	{
		Status = Submitted.Diagnostic;
		bStatusIsError = true;
		return Submitted;
	}
	if (Submitted.Status == TEXT("awaiting_user_approval"))
	{
		// The click is the approval: whoever pressed Generate has read the prompt and the cost above it.
		FString Error;
		if (!FHyperAIStudioApprovalGate::Approve(Request.OperationId, Error))
		{
			Status = Error;
			bStatusIsError = true;
			return Submitted;
		}
		Submitted.Status = TEXT("approved");
	}
	Status = FString::Printf(TEXT("Generating %d image%s into %s with %s."), Submitted.Count, Submitted.Count == 1 ? TEXT("") : TEXT("s"),
		*Submitted.Folder, *Submitted.ModelId);
	bStatusIsError = false;
	BatchName = NewBatchName();
	NextReload = 0.0;
	Reload();
	return Submitted;
}

void SHyperAIStudioImagesPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// Only while a batch runs: the gallery fills in as its images land.
	const bool bRunning = Inspect.Batches.ContainsByPredicate([](const FHyperAIImageBatchInfo& Batch) { return Batch.State == TEXT("running"); });
	if (bRunning && FPlatformTime::Seconds() >= NextReload)
	{
		NextReload = FPlatformTime::Seconds() + 1.0;
		const int32 Before = Inspect.Images.Num();
		Reload();
		if (Inspect.Images.Num() > Before)
		{
			Selected = 0;
		}
	}
}

#undef LOCTEXT_NAMESPACE
