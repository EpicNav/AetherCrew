// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioBot.h"

/** The editor's half of the bot's SurveyLevel step: the bot writes the frames, and the editor makes them a video. */
namespace HyperAIStudio::LevelSurvey
{
	constexpr int32 VideoFps = 8;

	/** Folder's frame_*.png, in order, as Folder/survey.mp4; the video's path, or empty with OutError. */
	FString WriteVideo(const FString& Folder, FString& OutError);

	/** A finished SurveyLevel result with its frames made into survey.mp4 and the detail saying so; other results as they are. */
	FHyperAIBotResult WithVideo(EHyperAIBotAction Action, const FHyperAIBotResult& Result);
}
