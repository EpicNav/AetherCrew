// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** Dialogue knowledge agents read before writing or rendering lines, kept in the project's .hyperai/docs. */
namespace HyperAIStudio::Dialogue
{
	FString GetCookbook();
	FString GetCookbookPath();
	/** Writes the cookbook when it is missing or out of date. */
	bool EnsureCookbook();
}
