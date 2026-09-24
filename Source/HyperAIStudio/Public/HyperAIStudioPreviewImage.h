// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FSlateBrush;
struct FSlateDynamicImageBrush;

/**
 * One image file shown in Slate. Slate has no brush for an arbitrary file on disk, so this reads the file and
 * hands the pixels to the renderer, reloading whenever the file's timestamp moves.
 */
class HYPERAISTUDIO_API FHyperAIStudioPreviewImage
{
public:
	/** Longest edge kept in memory; a viewport capture is far larger than any panel that shows it. */
	static constexpr int32 MaxEdgePixels = 2048;

	/** The brush for Path, or null while the file is missing or unreadable. */
	const FSlateBrush* Get(const FString& Path);
	/** The file's own size in pixels, before the panel scaled it down. */
	FIntPoint GetSourceSize() const { return SourceSize; }
	const FString& GetError() const { return Error; }
	void Forget();

private:
	FString LoadedPath;
	FDateTime LoadedStamp = FDateTime::MinValue();
	FIntPoint SourceSize = FIntPoint::ZeroValue;
	FString Error;
	TSharedPtr<FSlateDynamicImageBrush> Brush;
};
