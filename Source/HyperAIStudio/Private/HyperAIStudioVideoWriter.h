// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * An H.264 MP4 writer on Windows Media Foundation, which ships with Windows, so playtest recordings need no ffmpeg:
 * BGRA frames in, top row first; a video-only file out. Frames are timed at a fixed rate, not by when they arrive.
 */
class FHyperAIStudioVideoWriter
{
public:
	FHyperAIStudioVideoWriter();
	~FHyperAIStudioVideoWriter();

	/** Width and height are rounded down to even numbers, as H.264 needs; frames must then be exactly that size. */
	bool Open(const FString& Path, int32 InWidth, int32 InHeight, int32 FramesPerSecond, FString& OutError);
	bool WriteFrame(TConstArrayView<FColor> Pixels, FString& OutError);
	/** Finishes the file so players can read it; the destructor closes an open writer too. */
	bool Close(FString& OutError);

	bool IsOpen() const;
	int32 GetWidth() const { return Width; }
	int32 GetHeight() const { return Height; }
	int64 GetFrameCount() const { return FrameCount; }

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
	int32 Width = 0;
	int32 Height = 0;
	int32 Fps = 0;
	int64 FrameCount = 0;
};
