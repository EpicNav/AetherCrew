// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HyperAIStudioVideoWriter.h"

class SWidget;
class UWorld;

/**
 * Records a play session's game view as an MP4 a person can watch: the bot's current step, and each step's result,
 * are drawn over the game while it records, and a timeline of the steps is written beside the video.
 */
class FHyperAIStudioPlaytestRecorder
{
public:
	~FHyperAIStudioPlaytestRecorder();

	/** Records World's game viewport to Folder/playtest.mp4; false with the reason when there is nothing to record. */
	bool Start(UWorld& World, const FString& Folder, int32 FramesPerSecond, FString& OutError);
	/** Puts a step on the overlay and opens its span on the timeline. */
	void BeginStep(const FString& Label);
	/** Closes the open span with its status (completed, failed...) and a short detail, which the overlay shows a moment. */
	void EndStep(const FString& Status, const FString& Detail);
	/** Finishes the video and writes timeline.json; returns the video's path, or empty when no frame was recorded. */
	FString Stop(FString& OutSummary);

	bool IsRecording() const { return Writer.IsOpen(); }

private:
	bool Tick(float DeltaTime);
	bool Capture();
	double Elapsed() const;
	FText ClockText() const;
	FText StepText() const;

	struct FSpan
	{
		FString Label;
		double Start = 0.0;
		double End = -1.0;
		FString Status;
		FString Detail;
	};

	TWeakObjectPtr<UWorld> World;
	FHyperAIStudioVideoWriter Writer;
	FTSTicker::FDelegateHandle Ticker;
	/** Slate over the game view, so the capture carries it and a person watching the window sees it too. */
	TSharedPtr<SWidget> Overlay;
	FString Folder;
	FString VideoPath;
	double StartedSeconds = 0.0;
	int32 Fps = 10;
	TArray<FColor> Frame;
	TArray<FSpan> Timeline;
	FString WriteError;
};
