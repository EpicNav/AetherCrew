// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** What an agent needs to know before generating a reference image, kept in the project's .hyperai/docs. */
namespace HyperAIStudio::ImageGen
{
	FString GetCookbook();
	FString GetCookbookPath();
	/** Writes the cookbook when it is missing or out of date. */
	bool EnsureCookbook();
}
