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
 * the project has generated. Generate runs the tool's own dry run first, and the click is the approval the tool asks
 * for, since the person pressing it has seen the prompt and the cost.
 */
class SHyperAIStudioImagesPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioImagesPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	static FName GetTabName();

	/** Reads models, templates, generated images and batches again. */
	void Reload();
	void SetPrompt(const FString& Text) { Prompt = Text; }
	void SetCount(const int32 InCount) { Count = FMath::Clamp(InCount, 1, FHyperAIStudioImageGenContracts::MaxImagesPerBatch); }
	bool SelectModel(const FString& Id);
	void SelectImage(int32 Index);
	/** The free dry run: the composed prompt and what it would cost. */
	FHyperAIImageGenerateReport PreviewPrompt();
	/** Dry run, submit, and approve: the batch starts. */
	FHyperAIImageGenerateReport Generate();
	const FHyperAIImageInspectReport& GetInspect() const { return Inspect; }

private:
	using FModelPtr = TSharedPtr<FHyperAIImageModelInfo>;
	using FTemplatePtr = TSharedPtr<FHyperAIImageTemplateInfo>;

	FHyperAIImageGenerateRequest BuildRequest(bool bDryRun) const;
	TSharedRef<SWidget> BuildForm();
	TSharedRef<SWidget> BuildBrowser();
	void RebuildSlots();
	void RebuildGallery();
	const FHyperAIImageRecord* SelectedRecord() const;
	FString NewBatchName() const;

	FHyperAIImageInspectReport Inspect;
	TArray<FModelPtr> Models;
	FModelPtr Model;
	TArray<FTemplatePtr> Templates;
	FTemplatePtr Template;
	TMap<FString, FString> SlotValues;
	FString Prompt;
	FString Negative;
	FString BatchName;
	int32 Count = 1;
	int32 Width = 0;
	int32 Height = 0;
	int32 Seed = -1;
	FString Status;
	bool bStatusIsError = false;
	FString ComposedPrompt;
	int32 Selected = INDEX_NONE;
	double NextReload = 0.0;

	TSharedPtr<SComboBox<FModelPtr>> ModelCombo;
	TSharedPtr<SComboBox<FTemplatePtr>> TemplateCombo;
	TSharedPtr<SVerticalBox> SlotBox;
	TSharedPtr<SWrapBox> Gallery;
	TArray<TSharedPtr<FHyperAIStudioPreviewImage>> Thumbnails;
	FHyperAIStudioPreviewImage Large;
};
