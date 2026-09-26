// Games by Hyper 2026.

#include "SHyperAIStudioImagesPanel.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioImageGenGate.h"
#include "HyperAIStudioPreviewPanel.h"
#include "HyperAIStudioShell.h"
#include "HyperAIStudioWidgets.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
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

	/**
	 * Shapes a preset or the row picks. The sizes are what the request carries: the model takes an aspect ratio and a
	 * size class from them, so a longest side of 1024 keeps every model at its 1K price. Width and Height under More
	 * options ask for more.
	 */
	struct FShape
	{
		const TCHAR* Label;
		int32 Width;
		int32 Height;
	};
	const FShape Shapes[] = {
		{TEXT("16:9"), 1024, 576}, {TEXT("1:1"), 1024, 1024}, {TEXT("9:16"), 576, 1024}, {TEXT("4:3"), 1024, 768},
		{TEXT("3:4"), 768, 1024}, {TEXT("21:9"), 1024, 439}};
	constexpr int32 ShapeCount = UE_ARRAY_COUNT(Shapes);

	int32 ShapeIndexOf(const FString& Label)
	{
		for (int32 Index = 0; Index < ShapeCount; ++Index)
		{
			if (Label == Shapes[Index].Label) return Index;
		}
		return 0;
	}

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

	/** One sentence joined to the next: a full stop between them unless the first already ends one. */
	FString Join(const FString& First, const FString& Second)
	{
		const FString A = First.TrimStartAndEnd();
		const FString B = Second.TrimStartAndEnd();
		if (A.IsEmpty() || B.IsEmpty()) return A.IsEmpty() ? B : A;
		const TCHAR Last = A[A.Len() - 1];
		return A + (Last == TEXT('.') || Last == TEXT('!') || Last == TEXT('?') || Last == TEXT(':') ? TEXT(" ") : TEXT(". ")) + B;
	}

	/** What OpenOnPreset asked for, until the page shows and takes it. */
	struct FPendingOpen
	{
		FString Preset;
		FString Prompt;
		FString ModelId;
		bool bGenerate = false;
	};

	TOptional<FPendingOpen>& Pending()
	{
		static TOptional<FPendingOpen> Value;
		return Value;
	}
}

TArray<SHyperAIStudioImagesPanel::FPreset> SHyperAIStudioImagesPanel::BuiltInPresets()
{
	using namespace HyperAIStudio::ImagesPanel;
	auto Make = [](const TCHAR* Id, const FText& Label, const FText& Hint, const TCHAR* After, const TCHAR* Negative, const TCHAR* Shape, const int32 Count)
	{
		FPreset Preset;
		Preset.Id = Id;
		Preset.Label = Label;
		Preset.Hint = Hint;
		Preset.After = After;
		Preset.Negative = Negative;
		Preset.Shape = ShapeIndexOf(Shape);
		Preset.Count = Count;
		return Preset;
	};
	// The level itself, relit: the prompt says only how, and the words around it keep everything else as it is.
	FPreset Relight = Make(TEXT("relight_view"), LOCTEXT("PresetRelight", "Relight this view"),
		LOCTEXT("PresetRelightHint", "How the view should be lit: the time of day, where the light comes from, its colour, the weather and the mood. For example: late dusk, warm lamps glowing inside, cool blue moonlight through the windows, a little fog."),
		TEXT("Keep the camera, the framing and every object exactly as they are, with the same shapes, positions and materials; change only the light, the shadows, the sky, the fog and the colour grade. A photorealistic real-time render of the same scene"),
		TEXT("new objects, removed objects, changed shapes, a moved camera, text, watermark, people"), TEXT("16:9"), 1);
	Relight.Before = TEXT("Relight this picture of a game level:");
	Relight.bFromView = true;
	// Each preset names what a good image of its kind needs, so the prompt only has to say what it is of.
	return {
		Make(TEXT("scene_lighting"), LOCTEXT("PresetScene", "Scene lighting"),
			LOCTEXT("PresetSceneHint", "A place at a time of day: where it is, when, the weather, the mood. For example: a mountain monastery at dusk, low sun, thin fog."),
			TEXT("Photographic still of the scene, natural light with a clear key light direction, readable contrast, true-to-life colour"),
			TEXT("text, watermark, logo, people, frame, border"), TEXT("16:9"), 2),
		Relight,
		Make(TEXT("level_concept"), LOCTEXT("PresetLevel", "Level concept"),
			LOCTEXT("PresetLevelHint", "The level as a player sees it: the layout, the landmarks, the paths, the scale. For example: a ruined courtyard with a broken bridge over a gorge."),
			TEXT("Game environment concept art seen from eye height, a clear foreground, midground and background, readable paths and landmarks, real-world scale"),
			TEXT("text, watermark, interface, characters, frame"), TEXT("16:9"), 2),
		Make(TEXT("tileable_texture"), LOCTEXT("PresetTexture", "Tileable texture"),
			LOCTEXT("PresetTextureHint", "The surface: its material, age, wear and colour. For example: mossy cobblestones, wet after rain."),
			TEXT("Seamless tileable texture filling the frame, flat top-down orthographic view, even diffuse light, no shadows, no perspective"),
			TEXT("perspective, horizon, shadows, objects, text, border, visible seams"), TEXT("1:1"), 2),
		Make(TEXT("skybox"), LOCTEXT("PresetSky", "Sky panorama"),
			LOCTEXT("PresetSkyHint", "The sky: the time of day, the clouds, the colour. For example: a stormy sunset, towering clouds lit orange from below."),
			TEXT("Wide sky panorama, horizon on the lower third, sky only, no ground objects"),
			TEXT("buildings, people, text, frame"), TEXT("21:9"), 1),
		Make(TEXT("character"), LOCTEXT("PresetCharacter", "Character"),
			LOCTEXT("PresetCharacterHint", "Who they are: their silhouette, clothing, gear and colours. For example: a desert scout in layered cloth with a brass spyglass."),
			TEXT("Full-body character concept, neutral standing pose, front view, plain light background, readable silhouette"),
			TEXT("busy background, cropped limbs, text, watermark"), TEXT("3:4"), 2),
		Make(TEXT("prop"), LOCTEXT("PresetProp", "Prop"),
			LOCTEXT("PresetPropHint", "The object: what it is, its material, size and style. For example: an iron lantern with cracked green glass."),
			TEXT("A single isolated prop, three-quarter view, plain neutral background, soft studio light"),
			TEXT("background clutter, hands, text, watermark"), TEXT("1:1"), 2),
		Make(TEXT("ui_icon"), LOCTEXT("PresetIcon", "UI icon"),
			LOCTEXT("PresetIconHint", "The icon's subject and style. For example: a glowing fire rune on a stone disc."),
			TEXT("Game interface icon, one bold centred shape that reads at small sizes, plain flat background"),
			TEXT("text, letters, numbers, photographic background, frame"), TEXT("1:1"), 4),
		Make(TEXT("decal"), LOCTEXT("PresetDecal", "Decal"),
			LOCTEXT("PresetDecalHint", "The mark: cracks, stains, scorch marks, footprints, graffiti. For example: a scorch mark with soot spray."),
			TEXT("Isolated decal seen straight on, top-down with no perspective, on a plain black background"),
			TEXT("perspective, objects, text unless asked, frame"), TEXT("1:1"), 2),
	};
}

FString SHyperAIStudioImagesPanel::UserPresetsFile()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".hyperai"), TEXT("image-presets.json")));
}

FString SHyperAIStudioImagesPanel::ComposePrompt(const FPreset* Preset, const FString& Text)
{
	using namespace HyperAIStudio::ImagesPanel;
	return Preset ? Join(Join(Preset->Before, Text), Preset->After) : Text.TrimStartAndEnd();
}

FString SHyperAIStudioImagesPanel::ComposeNegative(const FPreset* Preset, const FString& Avoid)
{
	const FString Base = Preset ? Preset->Negative.TrimStartAndEnd() : FString();
	const FString Extra = Avoid.TrimStartAndEnd();
	return Base.IsEmpty() || Extra.IsEmpty() ? Base + Extra : Base + TEXT(", ") + Extra;
}

void SHyperAIStudioImagesPanel::Construct(const FArguments& InArgs)
{
	BatchName = NewBatchName();
	// A page of HyperAI Studio, headed like the others.
	ChildSlot
	[
		SNew(SBox)
		.Padding(FMargin(24.0f, 20.0f, 24.0f, 16.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				HyperAIStudio::UI::PageHeader(LOCTEXT("PageTitle", "Images"),
					LOCTEXT("PageBody", "Reference images for the look you are after: pick a preset, say what it is of, and Generate. From Nano Banana, Codex or a model you configure, written into the project with the prompt that made them."))
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Horizontal)
				+ SSplitter::Slot().Value(0.40f)
				[
					BuildForm()
				]
				+ SSplitter::Slot().Value(0.60f)
				[
					BuildBrowser()
				]
			]
		]
	];
	Reload();
	SelectPreset(TEXT("scene_lighting"));
	TakePending();
}

void SHyperAIStudioImagesPanel::OpenOnPreset(const FString& PresetId, const FString& InPrompt, const FString& ModelId, const bool bGenerate)
{
	// Taken by the page as it shows: a new one takes it as it is built, an open one on its next tick.
	HyperAIStudio::ImagesPanel::Pending() = HyperAIStudio::ImagesPanel::FPendingOpen{PresetId, InPrompt, ModelId, bGenerate};
	FHyperAIStudioShell::Open(TEXT("Images"));
}

void SHyperAIStudioImagesPanel::TakePending()
{
	TOptional<HyperAIStudio::ImagesPanel::FPendingOpen>& Waiting = HyperAIStudio::ImagesPanel::Pending();
	if (!Waiting.IsSet())
	{
		return;
	}
	const HyperAIStudio::ImagesPanel::FPendingOpen Open = Waiting.GetValue();
	Waiting.Reset();
	SelectPreset(Open.Preset);
	if (!Open.Prompt.IsEmpty())
	{
		Prompt = Open.Prompt;
	}
	if (!Open.ModelId.IsEmpty() && !SelectModel(Open.ModelId))
	{
		Status = FString::Printf(TEXT("There is no image model %s; hyper_image_inspect lists them."), *Open.ModelId);
		bStatusIsError = true;
		return;
	}
	if (Open.bGenerate)
	{
		Generate();
	}
}

FString SHyperAIStudioImagesPanel::NewBatchName() const
{
	return TEXT("images_") + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
}

TSharedRef<SWidget> SHyperAIStudioImagesPanel::BuildPresets()
{
	return SAssignNew(PresetBox, SWrapBox)
		.UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(6.0f, 6.0f));
}

void SHyperAIStudioImagesPanel::RebuildPresetButtons()
{
	if (!PresetBox.IsValid())
	{
		return;
	}
	PresetBox->ClearChildren();
	for (int32 Index = 0; Index < Presets.Num(); ++Index)
	{
		const FPreset& Preset = Presets[Index];
		PresetBox->AddSlot()
		[
			SNew(SCheckBox)
			.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.ToolTipText(FText::Format(LOCTEXT("PresetTip", "{0}\nAdds: {1}"), Preset.Hint, FText::FromString(Preset.After.IsEmpty() ? Preset.Before : Preset.After)))
			.IsChecked_Lambda([this, Index]() { return PresetIndex == Index ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			// Pressing the picked preset again leaves the prompt as written.
			.OnCheckStateChanged_Lambda([this, Id = Preset.Id](const ECheckBoxState State) { SelectPreset(State == ECheckBoxState::Checked ? Id : FString()); })
			[
				SNew(SBox)
				.Padding(FMargin(10.0f, 4.0f))
				[
					SNew(STextBlock)
					.Text(Preset.bUser ? FText::Format(LOCTEXT("UserPresetLabel", "{0} (yours)"), Preset.Label) : Preset.Label)
				]
			]
		];
	}
}

TSharedRef<SWidget> SHyperAIStudioImagesPanel::BuildForm()
{
	using namespace HyperAIStudio::ImagesPanel;
	const auto FromView = [this]() { const FPreset* Preset = SelectedPreset(); return Preset && Preset->bFromView; };
	return SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(8.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[Header(LOCTEXT("StartFrom", "Start from a preset"))]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)[BuildPresets()]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFontBold"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([FromView]() { return FromView() ? LOCTEXT("PromptLight", "How it should be lit") : LOCTEXT("Prompt", "What it is of"); })
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox)
				.MinDesiredHeight(110.0f)
				[
					SNew(SMultiLineEditableTextBox)
					.AutoWrapText(true)
					.HintText_Lambda([this]()
					{
						const FPreset* Preset = SelectedPreset();
						return Preset ? Preset->Hint : LOCTEXT("PromptHint", "What to draw, as you would tell an artist.");
					})
					.Text_Lambda([this]() { return FText::FromString(Prompt); })
					.OnTextChanged_Lambda([this](const FText& Text) { Prompt = Text.ToString(); })
				]
			]
			// Where the picture comes from, and what happens once it is back.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([FromView]() { return FromView() ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("FromViewNote", "Generate takes the picture through HyperAI_MatchCamera, placing that camera where the level viewport looks if the level has none, and the model changes only its light. Move the camera to relight another view."))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]() { return bMatchWhenDone ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](const ECheckBoxState State) { bMatchWhenDone = State == ECheckBoxState::Checked; })
					.IsEnabled_Lambda([this]() { return Count == 1; })
					.ToolTipText(LOCTEXT("MatchWhenDoneTip", "When the picture arrives, your preferred agent relights the level until it matches, as Match the lighting to this does. With more than one image, pick one and use Match the lighting to this."))
					[
						SNew(STextBlock).Text(LOCTEXT("MatchWhenDone", "Then match the level's lighting to it"))
					]
				]
			]

			// The three choices most images need; the rest is under More options.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 2.0f)[Label(LOCTEXT("Model", "Model"))]
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
						return Model.IsValid() ? FText::FromString(Model->Label.IsEmpty() ? Model->Id : Model->Label) : LOCTEXT("NoModel", "No image model");
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)[Label(LOCTEXT("Shape", "Shape"))]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SSegmentedControl<int32>)
						.Value_Lambda([this]() { return Shape; })
						.OnValueChanged_Lambda([this](const int32 In) { Shape = In; })
						+ SSegmentedControl<int32>::Slot(0).Text(FText::FromString(Shapes[0].Label))
						+ SSegmentedControl<int32>::Slot(1).Text(FText::FromString(Shapes[1].Label))
						+ SSegmentedControl<int32>::Slot(2).Text(FText::FromString(Shapes[2].Label))
						+ SSegmentedControl<int32>::Slot(3).Text(FText::FromString(Shapes[3].Label))
						+ SSegmentedControl<int32>::Slot(4).Text(FText::FromString(Shapes[4].Label))
						+ SSegmentedControl<int32>::Slot(5).Text(FText::FromString(Shapes[5].Label))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 2.0f)[Label(LOCTEXT("Count", "Images"))]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SSegmentedControl<int32>)
						.Value_Lambda([this]() { return Count; })
						.OnValueChanged_Lambda([this](const int32 In) { Count = In; })
						+ SSegmentedControl<int32>::Slot(1).Text(FText::AsNumber(1))
						+ SSegmentedControl<int32>::Slot(2).Text(FText::AsNumber(2))
						+ SSegmentedControl<int32>::Slot(3).Text(FText::AsNumber(3))
						+ SSegmentedControl<int32>::Slot(4).Text(FText::AsNumber(4))
					]
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
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor(FStyleColors::AccentRed))
				.Visibility_Lambda([this, FromView]() { return FromView() && Model.IsValid() && !Model->bTakesInputImage ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text(LOCTEXT("NoPictureModel", "This model draws from words only, so it cannot relight a view. Pick a Nano Banana model or Codex."))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("GenerateButton", "Generate"))
					.ToolTipText(LOCTEXT("GenerateTooltip", "Generate the batch. This spends money, or your agent's plan for an agent model; pressing it is the approval."))
					.IsEnabled_Lambda([this, FromView]()
					{
						return Model.IsValid() && Model->Issue.IsEmpty() && !Prompt.TrimStartAndEnd().IsEmpty() && (!FromView() || Model->bTakesInputImage);
					})
					.OnClicked_Lambda([this]()
					{
						Generate();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
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
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("SmallFont"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this]()
					{
						const HyperAIStudio::ImagesPanel::FShape& Chosen = HyperAIStudio::ImagesPanel::Shapes[FMath::Clamp(Shape, 0, HyperAIStudio::ImagesPanel::ShapeCount - 1)];
						return FText::Format(LOCTEXT("WillMake", "{0} x {1}, {2} image(s)"), FText::AsNumber(Width > 0 ? Width : Chosen.Width),
							FText::AsNumber(Height > 0 ? Height : Chosen.Height), FText::AsNumber(Count));
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Visibility_Lambda([this]() { return Status.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
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
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.Visibility_Lambda([this]() { return Inspect.Batches.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text_Lambda([this]()
				{
					TArray<FString> Lines;
					for (const FHyperAIImageBatchInfo& Batch : Inspect.Batches)
					{
						Lines.Add(FString::Printf(TEXT("%s: %s, %s%s"), *Batch.BatchId, *Batch.State, *Batch.Progress,
							Batch.Diagnostic.IsEmpty() ? TEXT("") : *(TEXT(" - ") + Batch.Diagnostic)));
					}
					return FText::FromString(FString::Join(Lines, TEXT("\n")));
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 14.0f, 0.0f, 0.0f)
			[
				BuildMoreOptions()
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioImagesPanel::BuildMoreOptions()
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
	return SNew(SExpandableArea)
		.InitiallyCollapsed(true)
		.AreaTitle(LOCTEXT("MoreOptions", "More options"))
		.BodyContent()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 2.0f)[Label(LOCTEXT("Negative", "Also keep out"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SEditableTextBox)
				.HintText_Lambda([this]()
				{
					const FPreset* Preset = SelectedPreset();
					return Preset && !Preset->Negative.IsEmpty() ? FText::Format(LOCTEXT("NegativeHintPreset", "The preset keeps out: {0}"), FText::FromString(Preset->Negative))
						: LOCTEXT("NegativeHint", "What to keep out, such as text or people.");
				})
				.Text_Lambda([this]() { return FText::FromString(Negative); })
				.OnTextChanged_Lambda([this](const FText& Text) { Negative = Text.ToString(); })
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
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					Number(LOCTEXT("Width", "Width"), 0, FHyperAIStudioImageGenContracts::MaxImageSize, [this]() { return Width; }, [this](const int32 Value) { Width = Value; },
						LOCTEXT("SizeTooltip", "0 uses the shape above."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					Number(LOCTEXT("Height", "Height"), 0, FHyperAIStudioImageGenContracts::MaxImageSize, [this]() { return Height; }, [this](const int32 Value) { Height = Value; },
						LOCTEXT("SizeTooltip", "0 uses the shape above."))
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
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 2.0f)[Label(LOCTEXT("SavePresetLabel", "Save these settings as a preset for the team"))]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SEditableTextBox)
					.HintText(LOCTEXT("PresetNameHint", "Preset name, such as Ember concept"))
					.ToolTipText(LOCTEXT("PresetNameTip", "The prompt you have written becomes the preset's style, kept with the preset's own words, the shape, the count and what to keep out. Presets live in .hyperai/image-presets.json, shared with the team."))
					.Text_Lambda([this]() { return FText::FromString(NewPresetLabel); })
					.OnTextChanged_Lambda([this](const FText& Text) { NewPresetLabel = Text.ToString(); })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("SavePreset", "Save preset"))
					.IsEnabled_Lambda([this]() { return !NewPresetLabel.TrimStartAndEnd().IsEmpty(); })
					.OnClicked_Lambda([this]()
					{
						FString Error;
						Status = SaveAsPreset(NewPresetLabel, Error) ? FString::Printf(TEXT("Saved the preset %s."), *NewPresetLabel.TrimStartAndEnd()) : Error;
						bStatusIsError = !Error.IsEmpty();
						NewPresetLabel.Reset();
						return FReply::Handled();
					})
				]
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioImagesPanel::BuildBrowser()
{
	using namespace HyperAIStudio::ImagesPanel;
	auto Action = [](const FText& Text, const FText& Tooltip, TFunction<void()> Run, const bool bPrimary = false)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), bPrimary ? "PrimaryButton" : "Button")
			.Text(Text)
			.ToolTipText(Tooltip)
			.OnClicked_Lambda([Run]()
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
		+ SVerticalBox::Slot().FillHeight(0.42f).Padding(8.0f, 0.0f)
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
		+ SVerticalBox::Slot().FillHeight(0.58f).Padding(8.0f, 8.0f)
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
				.AutoWrapText(true)
				.Text_Lambda([this]()
				{
					const FHyperAIImageRecord* Record = SelectedRecord();
					return Record ? FText::FromString(FString::Printf(TEXT("%s | %s | seed %d | %dx%d | %s | %s%s"), *Record->ModelId, *Record->BatchId, Record->Seed,
						Record->Width, Record->Height, *Record->GeneratedUtc.Left(19), *Record->File,
						*(Record->InputImage.IsEmpty() ? FString() : TEXT(" | relit from ") + Record->InputImage))) : FText::GetEmpty();
				})
			]
			// What to do with a picture: make the level look like it first, then the smaller moves.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(6.0f, 6.0f))
				+ SWrapBox::Slot()
				[
					Action(LOCTEXT("MatchLighting", "Match the lighting to this"),
						LOCTEXT("MatchLightingTooltip", "Your preferred agent relights the level until it matches this picture, and HyperAI sends it back to work after each round until it does. Follow it on the Match page."),
						[this]() { StartMatch(FName(TEXT("match_lighting"))); }, /*bPrimary=*/true)
				]
				+ SWrapBox::Slot()
				[
					Action(LOCTEXT("MatchLevel", "Build the level like this"),
						LOCTEXT("MatchLevelTooltip", "Your preferred agent builds the level toward this picture as a level designer would, then lights it, until it matches. Follow it on the Match page."),
						[this]() { StartMatch(FName(TEXT("match_level"))); })
				]
				+ SWrapBox::Slot()
				[
					Action(LOCTEXT("UseMatch", "Compare on Match"), LOCTEXT("UseMatchTooltip", "Open the Match page with it: the level's lighting measured against it, and any match session's attempts."),
						[this]() { if (const FHyperAIImageRecord* Record = SelectedRecord()) FHyperAIStudioPreviewPanel::Open(FName(TEXT("lighting")), FullPath(Record->File)); })
				]
				+ SWrapBox::Slot()
				[
					Action(LOCTEXT("Reuse", "Reuse prompt"), LOCTEXT("ReuseTooltip", "Put its prompt, model and seed back in the form."),
						[this]()
						{
							if (const FHyperAIImageRecord* Record = SelectedRecord())
							{
								SelectPreset(FString());
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

void SHyperAIStudioImagesPanel::StartMatch(const FName ActionId)
{
	const FHyperAIImageRecord* Record = SelectedRecord();
	if (!Record)
	{
		return;
	}
	FString Error;
	if (FHyperAIStudioPreviewPanel::StartAgentAction(FName(TEXT("image_match")), ActionId, HyperAIStudio::ImagesPanel::FullPath(Record->File), Error))
	{
		Status = TEXT("Handed to your preferred agent; follow it on the Match page.");
		bStatusIsError = false;
	}
	else
	{
		Status = Error;
		bStatusIsError = true;
	}
}

bool SHyperAIStudioImagesPanel::CaptureView()
{
	using namespace HyperAIStudio::ImagesPanel;
	InputImage.Reset();
	// The lighting pack owns the view the Match jobs work on, and captures it.
	const FHyperAIStudioPreviewPanel::FSource* Source = FHyperAIStudioPreviewPanel::FindSource(FName(TEXT("image_match")));
	if (!Source || !Source->CaptureView)
	{
		Status = TEXT("Relighting a view needs the lighting pack, which takes the picture, and it is not loaded.");
		bStatusIsError = true;
		return false;
	}
	const FShape& Chosen = Shapes[FMath::Clamp(Shape, 0, ShapeCount - 1)];
	FString Error;
	if (!Source->CaptureView(FIntPoint(Width > 0 ? Width : Chosen.Width, Height > 0 ? Height : Chosen.Height), InputImage, Error))
	{
		InputImage.Reset();
		Status = Error;
		bStatusIsError = true;
		return false;
	}
	return true;
}

void SHyperAIStudioImagesPanel::MatchWhenDone(const FString& BatchId)
{
	using namespace HyperAIStudio::ImagesPanel;
	namespace Gate = HyperAIStudio::ImageGen::Gate;
	const TWeakPtr<SHyperAIStudioImagesPanel> Weak = StaticCastSharedRef<SHyperAIStudioImagesPanel>(AsShared());
	// The approved plan starts the batch once the execution queue reaches it, which another edit can hold up a while.
	const double StartBy = FPlatformTime::Seconds() + 120.0;
	// ponytail: a one-second core ticker rather than the page's own tick, which stops while the page is hidden.
	FTSTicker::GetCoreTicker().AddTicker(TEXT("HyperAIRelightThenMatch"), 1.0f, [Weak, BatchId, StartBy](float)
	{
		FHyperAIImageBatchInfo Batch;
		const bool bKnown = Gate::FindBatch(BatchId, Batch);
		if (bKnown ? Batch.State == TEXT("running") : FPlatformTime::Seconds() < StartBy)
		{
			return true;
		}
		const TArray<FHyperAIImageRecord> Landed = bKnown && Batch.State == TEXT("complete") ? Gate::ReadGenerated(BatchId, 1) : TArray<FHyperAIImageRecord>();
		FString Message = !bKnown ? FString(TEXT("The relight never started, so nothing was matched; the Activity panel says why."))
			: Batch.Diagnostic.IsEmpty() ? FString(TEXT("The relit view did not arrive, so nothing was matched.")) : Batch.Diagnostic;
		bool bError = true;
		if (!Landed.IsEmpty())
		{
			const FString Reference = FullPath(Landed[0].File);
			FString Error;
			bError = !FHyperAIStudioPreviewPanel::StartAgentAction(FName(TEXT("image_match")), FName(TEXT("match_lighting")), Reference, Error);
			Message = bError ? Error : FString(TEXT("The view is relit, and your preferred agent is matching the level's lighting to it; follow it on the Match page."));
			if (bError)
			{
				// The picture is still worth having: on the Match page, where Match it can try again.
				FHyperAIStudioPreviewPanel::Open(FName(TEXT("image_match")), Reference);
			}
		}
		if (const TSharedPtr<SHyperAIStudioImagesPanel> Panel = Weak.Pin())
		{
			Panel->Status = Message;
			Panel->bStatusIsError = bError;
		}
		return false;
	});
}

void SHyperAIStudioImagesPanel::LoadUserPresets()
{
	using namespace HyperAIStudio::ImagesPanel;
	FString Text;
	TSharedPtr<FJsonValue> Root;
	if (!FFileHelper::LoadFileToString(Text, *UserPresetsFile()) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Root->TryGetArray(Items) || !Items)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Item : *Items)
	{
		const TSharedPtr<FJsonObject> Object = Item.IsValid() ? Item->AsObject() : nullptr;
		FString Name;
		if (!Object.IsValid() || !Object->TryGetStringField(TEXT("label"), Name) || Name.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}
		FPreset Preset;
		Preset.Id = TEXT("user:") + Name.TrimStartAndEnd();
		Preset.Label = FText::FromString(Name.TrimStartAndEnd().Left(40));
		Preset.bUser = true;
		Object->TryGetStringField(TEXT("before"), Preset.Before);
		Object->TryGetStringField(TEXT("after"), Preset.After);
		Object->TryGetStringField(TEXT("negative"), Preset.Negative);
		FString ShapeLabel;
		Preset.Shape = Object->TryGetStringField(TEXT("shape"), ShapeLabel) ? ShapeIndexOf(ShapeLabel) : 0;
		int32 PresetCount = 1;
		Preset.Count = Object->TryGetNumberField(TEXT("count"), PresetCount) ? FMath::Clamp(PresetCount, 1, 4) : 1;
		Preset.Hint = FText::Format(LOCTEXT("UserPresetHint", "What it is of; the preset {0} adds its style."), Preset.Label);
		Presets.Add(MoveTemp(Preset));
	}
}

bool SHyperAIStudioImagesPanel::SaveAsPreset(const FString& InLabel, FString& OutError)
{
	using namespace HyperAIStudio::ImagesPanel;
	const FString Name = InLabel.TrimStartAndEnd().Left(40);
	if (Name.IsEmpty())
	{
		OutError = TEXT("A preset needs a name.");
		return false;
	}
	FString Text;
	TArray<TSharedPtr<FJsonValue>> Items;
	TSharedPtr<FJsonValue> Root;
	if (FFileHelper::LoadFileToString(Text, *UserPresetsFile()) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
		if (Root->TryGetArray(Existing) && Existing)
		{
			Items = *Existing;
		}
	}
	// The same name replaces the older one.
	Items.RemoveAll([&Name](const TSharedPtr<FJsonValue>& Item)
	{
		const TSharedPtr<FJsonObject> Object = Item.IsValid() ? Item->AsObject() : nullptr;
		FString Other;
		return Object.IsValid() && Object->TryGetStringField(TEXT("label"), Other) && Other.Equals(Name, ESearchCase::IgnoreCase);
	});
	const FPreset* Base = SelectedPreset();
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("label"), Name);
	Object->SetStringField(TEXT("before"), Base ? Base->Before : FString());
	// What the user wrote becomes the style every image of this preset carries.
	Object->SetStringField(TEXT("after"), Join(Base ? Base->After : FString(), Prompt));
	Object->SetStringField(TEXT("negative"), ComposeNegative(Base, Negative));
	Object->SetStringField(TEXT("shape"), Shapes[FMath::Clamp(Shape, 0, ShapeCount - 1)].Label);
	Object->SetNumberField(TEXT("count"), Count);
	Items.Add(MakeShared<FJsonValueObject>(Object));
	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	if (!FJsonSerializer::Serialize(Items, Writer))
	{
		OutError = TEXT("The preset could not be written.");
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(UserPresetsFile()), /*Tree=*/true);
	if (!FFileHelper::SaveStringToFile(Out, *UserPresetsFile(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write %s."), *UserPresetsFile());
		return false;
	}
	const FString Id = TEXT("user:") + Name;
	Reload();
	SelectPreset(Id);
	Prompt.Reset();
	return true;
}

void SHyperAIStudioImagesPanel::Reload()
{
	FHyperAIImageInspectRequest Request;
	Request.MaxOutputBytes = FHyperAIStudioImageGenContracts::MaxOutputBytes;
	Inspect = FHyperAIStudioImageGenContracts::Inspect(Request);
	const FString KeptModel = Model.IsValid() ? Model->Id : FString();
	const FString KeptTemplate = Template.IsValid() ? Template->Id : FString();
	const FString KeptPreset = SelectedPreset() ? SelectedPreset()->Id : FString();
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
	Presets = BuiltInPresets();
	LoadUserPresets();
	PresetIndex = Presets.IndexOfByPredicate([&KeptPreset](const FPreset& Preset) { return !KeptPreset.IsEmpty() && Preset.Id == KeptPreset; });
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
	RebuildPresetButtons();
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

bool SHyperAIStudioImagesPanel::SelectPreset(const FString& Id)
{
	PresetIndex = Id.IsEmpty() ? INDEX_NONE : Presets.IndexOfByPredicate([&Id](const FPreset& Preset) { return Preset.Id == Id; });
	if (const FPreset* Preset = SelectedPreset())
	{
		Shape = Preset->Shape;
		Count = Preset->Count;
		Width = 0;
		Height = 0;
	}
	ComposedPrompt.Reset();
	return Id.IsEmpty() || PresetIndex != INDEX_NONE;
}

const SHyperAIStudioImagesPanel::FPreset* SHyperAIStudioImagesPanel::SelectedPreset() const
{
	return Presets.IsValidIndex(PresetIndex) ? &Presets[PresetIndex] : nullptr;
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
				.BorderBackgroundColor_Lambda([this, Index]() { return Selected == Index ? FStyleColors::Primary : FStyleColors::Header; })
				.Padding(2.0f)
				[
					SNew(SBox).WidthOverride(150.0f).HeightOverride(96.0f)
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
	using namespace HyperAIStudio::ImagesPanel;
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
	const FPreset* Preset = SelectedPreset();
	Request.Prompt = ComposePrompt(Preset, Prompt);
	Request.Negative = ComposeNegative(Preset, Negative);
	Request.BatchName = BatchName.TrimStartAndEnd();
	Request.Count = Count;
	const FShape& Chosen = Shapes[FMath::Clamp(Shape, 0, ShapeCount - 1)];
	Request.Width = Width > 0 ? Width : Chosen.Width;
	Request.Height = Height > 0 ? Height : Chosen.Height;
	Request.Seed = Seed;
	Request.InputImagePath = Preset && Preset->bFromView ? InputImage : FString();
	return Request;
}

FHyperAIImageGenerateReport SHyperAIStudioImagesPanel::PreviewPrompt()
{
	const FPreset* Preset = SelectedPreset();
	const bool bFromView = Preset && Preset->bFromView;
	if (bFromView && Model.IsValid() && !Model->bTakesInputImage)
	{
		FHyperAIImageGenerateReport Refused;
		Refused.Status = TEXT("model_takes_no_picture");
		Refused.Diagnostic = Status = FString::Printf(TEXT("%s draws from words only, so it cannot relight a view. Pick a Nano Banana model or Codex."), *Model->Id);
		bStatusIsError = true;
		return Refused;
	}
	// Free, and so nothing in the level changes: the view is taken, and the camera placed, only when Generate is pressed.
	const FHyperAIImageGenerateReport Plan = FHyperAIStudioImageGenContracts::BuildPlan(BuildRequest(/*bDryRun=*/true));
	ComposedPrompt = Plan.ComposedPrompt;
	const FString Cost = Plan.EstimatedCost > 0.f ? FString::Printf(TEXT(", about %.2f"), Plan.EstimatedCost) : FString();
	Status = Plan.bOk ? FString::Printf(TEXT("Nothing is sent until you press Generate: %d image%s at %d x %d from %s%s.%s"), Plan.Count,
		Plan.Count == 1 ? TEXT("") : TEXT("s"), Plan.Width, Plan.Height, *(Model.IsValid() && !Model->Label.IsEmpty() ? Model->Label : Plan.ModelId), *Cost,
		bFromView ? TEXT(" Generate first takes the view through HyperAI_MatchCamera.") : TEXT(""))
		: Plan.Diagnostic;
	bStatusIsError = !Plan.bOk;
	return Plan;
}

FHyperAIImageGenerateReport SHyperAIStudioImagesPanel::Generate()
{
	const FPreset* Preset = SelectedPreset();
	const bool bFromView = Preset && Preset->bFromView;
	// What stops the model stops the relight before the view is taken, so no camera is placed for nothing.
	if (bFromView && Model.IsValid() && (!Model->Issue.IsEmpty() || !Model->bTakesInputImage))
	{
		FHyperAIImageGenerateReport Refused;
		Refused.Status = Model->Issue.IsEmpty() ? TEXT("model_takes_no_picture") : TEXT("model_unavailable");
		Refused.Diagnostic = Status = Model->Issue.IsEmpty()
			? FString::Printf(TEXT("%s draws from words only, so it cannot relight a view. Pick a Nano Banana model or Codex."), *Model->Id) : Model->Issue;
		bStatusIsError = true;
		return Refused;
	}
	if (bFromView && !CaptureView())
	{
		FHyperAIImageGenerateReport Failed;
		Failed.Status = TEXT("view_not_captured");
		Failed.Diagnostic = Status;
		return Failed;
	}
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
	if (bFromView && bMatchWhenDone && Submitted.Count == 1)
	{
		MatchWhenDone(Submitted.BatchId);
		Status = FString::Printf(TEXT("Relighting the view into %s with %s. When it lands, your preferred agent matches the level's lighting to it."),
			*Submitted.Folder, *Submitted.ModelId);
	}
	bStatusIsError = false;
	BatchName = NewBatchName();
	NextReload = 0.0;
	Reload();
	return Submitted;
}

void SHyperAIStudioImagesPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	TakePending();
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
