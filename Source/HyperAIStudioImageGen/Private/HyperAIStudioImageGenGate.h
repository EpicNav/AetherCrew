// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioImageGenToolset.h"
#include "HyperAIStudioSettings.h"

/** Everything the image generation tools do to prompts, endpoints and files, away from the contract layer. */
namespace HyperAIStudio::ImageGen::Gate
{
	bool FindModel(const FString& Id, FHyperAIStudioImageModelSettings& OutModel);
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
