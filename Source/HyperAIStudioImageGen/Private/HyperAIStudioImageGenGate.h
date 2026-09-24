// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioImageGenToolset.h"
#include "HyperAIStudioSettings.h"

/** Everything the image generation tools do to prompts, endpoints and files, away from the contract layer. */
namespace HyperAIStudio::ImageGen::Gate
{
	bool FindModel(const FString& Id, FHyperAIStudioImageModelSettings& OutModel);
	/**
	 * True for a model an agent CLI draws on the user's own plan, codex_image_gen or gemini_image_gen, rather than
	 * an external service; OutAgent is codex or gemini. These are built in, so they are there without settings.
	 */
	bool IsAgentModel(const FHyperAIStudioImageModelSettings& Model, FString& OutAgent);
	TArray<FHyperAIImageModelInfo> ReadModels();
	TArray<FHyperAIImageTemplateInfo> ReadTemplates();
	/** Slot names a template takes, in the order they first appear. */
	TArray<FString> ReadTemplateSlots(const FString& Template);

	/** Project-relative, e.g. Content/References/Generated. */
	FString OutputFolder();
	FString OutputRootOnDisk();
	/** Generated images read back from their sidecars, newest first. */
	TArray<FHyperAIImageRecord> ReadGenerated(const FString& BatchId, int32 Max);

	/**
	 * Fills a template from slots, then appends any extra text. A clause holding an unfilled slot is
	 * dropped, and a sentence left empty goes with it, so a half-filled template still reads.
	 */
	bool ComposePrompt(const FString& TemplateId, const TArray<FHyperAIImageSlot>& Slots, const FString& Extra,
		FString& OutPrompt, TArray<FString>& OutUnfilled, FString& OutError);

	/**
	 * What one image from Model costs at Width by Height: its price for that size (1K, 2K or 4K by the longest side),
	 * or the service's cost per call when it has none. Zero for an agent model, which draws on the user's own plan.
	 */
	double CostPerImage(const FHyperAIStudioImageModelSettings& Model, int32 Width, int32 Height);

	/** The nearest aspect ratio image APIs take, from 1:1, 2:3, 3:2, 3:4, 4:3, 4:5, 5:4, 9:16, 16:9 and 21:9. */
	FString AspectRatio(int32 Width, int32 Height);
	/** 1K, 2K or 4K from the longest side. */
	FString ImageSize(int32 Width, int32 Height);

	/** The model's request template with its placeholders substituted, each value JSON-escaped. */
	bool BuildRequestBody(const FHyperAIStudioImageModelSettings& Model, const FString& Prompt, const FString& Negative,
		int32 Width, int32 Height, int32 Seed, FString& OutBody, FString& OutError);

	/** Walks a dotted path into a JSON reply and base64-decodes what it finds; numbers index arrays. */
	bool ExtractImageBytes(const TArray<uint8>& Response, const FString& DottedPath, TArray<uint8>& OutBytes, FString& OutError);

	/** png, jpg or webp from the leading bytes; empty when they are not an image this pack recognises. */
	FString SniffImageExtension(const TArray<uint8>& Bytes);

	bool StartBatch(const FString& BatchId, const FHyperAIStudioImageModelSettings& Model, const FString& TemplateId,
		const FString& Prompt, const FString& Negative, int32 Count, int32 Width, int32 Height, int32 Seed, FString& OutError);
	bool FindBatch(const FString& BatchId, FHyperAIImageBatchInfo& OutInfo);
	TArray<FHyperAIImageBatchInfo> ReadBatches(const FString& BatchId);
}
