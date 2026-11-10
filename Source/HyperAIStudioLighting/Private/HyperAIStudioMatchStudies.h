// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioLightingToolset.h"

/**
 * What each Match job learned, kept so the next one starts from it: one Markdown file per job under
 * .hyperai/memory/studies, written when the job ends and shared through Git like the rest of the project's memory.
 * The next Match job's prompt carries the studies that bear on it, and the rounds of a job carry what raised and what
 * lowered its own score.
 */
namespace HyperAIStudio::Lighting::Studies
{
	struct FStudy
	{
		/** lighting or level. */
		FString Goal;
		/** The reference image, project-relative. */
		FString Reference;
		/** The level's package. */
		FString Level;
		FString Session;
		/** matched, stopped or failed. */
		FString Result;
		/** The job's last status line. */
		FString Status;
		float Best = 0.f;
		float Target = 0.f;
		int32 Attempts = 0;
		/** YYYY-MM-DD. */
		FString Date;
		/** Attempts that raised the best score, biggest gain first: "+9: sun pitch -12, 3800 K (attempt 3)". */
		TArray<FString> Helped;
		/** Attempts that lost two points or more against the best before them, biggest loss first. */
		TArray<FString> Hurt;
		/** Where the best attempt still differed when the job ended, and what was suggested next. */
		TArray<FString> StillDiffered;
		TArray<FString> NextSteps;
	};

	/** .hyperai/memory/studies under the project folder. */
	FString Directory(const FString& ProjectDir);
	/** What raised and what lowered a session's score: each attempt's note against the best before it, biggest change first. */
	void Learn(const TArray<FHyperAIImageIteration>& History, TArray<FString>& OutHelped, TArray<FString>& OutHurt);
	FString Format(const FStudy& Study);
	bool Parse(const FString& Text, FStudy& OutStudy);
	/** Writes the study as <date>-<goal>-<reference>-<time>.md under Directory. */
	bool Write(const FString& ProjectDir, const FStudy& Study, FString& OutPath, FString& OutError);
	/** The newest studies first, up to Limit. */
	TArray<FStudy> Load(const FString& ProjectDir, int32 Limit);
	/**
	 * The earlier studies that bear on a job, condensed for its prompt: this level's for the same goal first, then this
	 * level's for the other goal, then other levels' for the same goal, newest first within each. Empty without any.
	 */
	FString Lessons(const TArray<FStudy>& Studies, const FString& Goal, const FString& Level, int32 MaxStudies = 4);
}
