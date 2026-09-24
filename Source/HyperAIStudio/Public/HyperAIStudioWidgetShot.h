// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

class SWidget;

/** UI screenshots for the step reports, drawn by tests off screen. */
namespace HyperAIStudio::Tests
{
	/** Where the UI screenshots go: Saved/HyperAIStudio/Screens. */
	HYPERAISTUDIO_API FString ScreensFolder();

	/**
	 * Draws Widget off screen at Size on the editor's panel background and writes it as a PNG, for the step reports.
	 * It is drawn a few times first, so lists, text and pictures that arrive on first paint are all there. Returns
	 * the file, or empty when this editor cannot render.
	 */
	HYPERAISTUDIO_API FString SaveWidgetShot(const TSharedRef<SWidget>& Widget, FIntPoint Size, const FString& Name, int32 Frames = 8);

	/** A dusk-scene picture of Size, so screenshots show something that reads as an image, not a flat tile. */
	HYPERAISTUDIO_API void WriteScenePng(const FString& Path, FIntPoint Size, FLinearColor Top = FLinearColor(0.95f, 0.45f, 0.18f),
		FLinearColor Bottom = FLinearColor(0.25f, 0.12f, 0.35f));
}

#endif
