// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioImageGenToolset.h"
#include "HyperAIStudioPreviewImage.h"
#include "Widgets/SCompoundWidget.h"

class SVerticalBox;
class SWrapBox;
template <typename OptionType> class SComboBox;

/**
 * The Images tab: generate reference images with any model the image pack knows, Codex among them, and browse what
 * the project has generated. A preset fills in the words around the prompt, the shape and the count, so most images
 * need only a prompt and Generate; everything else waits under More options. Generate runs the tool's own dry run
 * first, and the click is the approval the tool asks for, since the person pressing it has seen the prompt and the cost.
 */
class SHyperAIStudioImagesPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioImagesPanel) {}
	SLATE_END_ARGS()

	/** What a preset sets: words before and after the prompt, what to keep out, a shape and a count. */
	struct FPreset
	{
		FString Id;
		FText Label;
		/** Shown in the empty prompt box: what to describe. */
		FText Hint;
		FString Before;
		FString After;
		FString Negative;
		int32 Shape = 0;
		int32 Count = 1;
		/** Saved by someone on the team, in .hyperai/image-presets.json. */
		bool bUser = false;
		/** Changes a picture of the level, taken through HyperAI_MatchCamera when Generate is pressed, rather than drawing from words. */
		bool bFromView = false;
	};

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/**
	 * Opens the Images page on a preset, as the Relight this view action does. A prompt fills the box, a model id picks the
	 * model, and bGenerate presses Generate, which spends as the button does.
	 */
	static void OpenOnPreset(const FString& PresetId, const FString& Prompt = FString(), const FString& ModelId = FString(), bool bGenerate = false);
	/** For a preset that changes a picture of the level: start matching the level's lighting to the picture once it lands. */
	void SetMatchWhenDone(const bool bInMatchWhenDone) { bMatchWhenDone = bInMatchWhenDone; }

	/** Reads models, templates, presets, generated images and batches again. */
	void Reload();
	void SetPrompt(const FString& Text) { Prompt = Text; }
	void SetCount(const int32 InCount) { Count = FMath::Clamp(InCount, 1, FHyperAIStudioImageGenContracts::MaxImagesPerBatch); }
	bool SelectModel(const FString& Id);
	/** Picks a preset by id, or none with an empty id: its shape, count and words replace the current ones. */
	bool SelectPreset(const FString& Id);
	void SelectImage(int32 Index);
	/** The free dry run: the composed prompt and what it would cost. */
	FHyperAIImageGenerateReport PreviewPrompt();
	/** Dry run, submit, and approve: the batch starts. */
	FHyperAIImageGenerateReport Generate();
	const FHyperAIImageInspectReport& GetInspect() const { return Inspect; }
	const TArray<FPreset>& GetPresets() const { return Presets; }
	/** Saves the current preset words, avoid list, shape and count as a new preset for the team. */
	bool SaveAsPreset(const FString& Label, FString& OutError);

	/** The built-in presets, and where the team's own are kept. */
	static TArray<FPreset> BuiltInPresets();
	static FString UserPresetsFile();
	/** Prompt with a preset's words around it; the avoid lists joined. Exposed for tests. */
	static FString ComposePrompt(const FPreset* Preset, const FString& Prompt);
	static FString ComposeNegative(const FPreset* Preset, const FString& Avoid);

private:
	using FModelPtr = TSharedPtr<FHyperAIImageModelInfo>;
	using FTemplatePtr = TSharedPtr<FHyperAIImageTemplateInfo>;

	FHyperAIImageGenerateRequest BuildRequest(bool bDryRun) const;
	TSharedRef<SWidget> BuildForm();
	TSharedRef<SWidget> BuildPresets();
	TSharedRef<SWidget> BuildMoreOptions();
	TSharedRef<SWidget> BuildBrowser();
	void RebuildPresetButtons();
	void RebuildSlots();
	void RebuildGallery();
	void LoadUserPresets();
	const FPreset* SelectedPreset() const;
	const FHyperAIImageRecord* SelectedRecord() const;
	FString NewBatchName() const;
	/** Match it from a generated image: a job for the preferred agent, followed on the Match page. */
	void StartMatch(const FName ActionId);
	/** The level's view through HyperAI_MatchCamera at the chosen shape, for a preset that changes it; false with Status set. */
	bool CaptureView();
	/** Once the batch's picture lands, the lighting is matched to it, wherever the user is by then. */
	void MatchWhenDone(const FString& BatchId);
	/** An OpenOnPreset waiting for the page to show. */
	void TakePending();

	FHyperAIImageInspectReport Inspect;
	TArray<FModelPtr> Models;
	FModelPtr Model;
	TArray<FTemplatePtr> Templates;
	FTemplatePtr Template;
	TMap<FString, FString> SlotValues;
	TArray<FPreset> Presets;
	int32 PresetIndex = INDEX_NONE;
	FString Prompt;
	FString Negative;
	FString BatchName;
	FString NewPresetLabel;
	int32 Count = 1;
	int32 Shape = 0;
	/** A size typed under More options wins over the shape; 0 uses the shape. */
	int32 Width = 0;
	int32 Height = 0;
	int32 Seed = -1;
	FString Status;
	bool bStatusIsError = false;
	FString ComposedPrompt;
	int32 Selected = INDEX_NONE;
	double NextReload = 0.0;
	/** The picture of the level a preset that changes one sends, captured when Generate is pressed. */
	FString InputImage;
	bool bMatchWhenDone = true;

	TSharedPtr<SComboBox<FModelPtr>> ModelCombo;
	TSharedPtr<SComboBox<FTemplatePtr>> TemplateCombo;
	TSharedPtr<SWrapBox> PresetBox;
	TSharedPtr<SVerticalBox> SlotBox;
	TSharedPtr<SWrapBox> Gallery;
	TArray<TSharedPtr<FHyperAIStudioPreviewImage>> Thumbnails;
	FHyperAIStudioPreviewImage Large;
};
