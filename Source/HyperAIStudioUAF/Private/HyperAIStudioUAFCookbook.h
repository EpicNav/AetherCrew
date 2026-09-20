// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/** UAF knowledge agents read before building a UAF animation graph, kept in the project's .hyperai/docs. */
namespace HyperAIStudio::UAF
{
	FString GetCookbook();
	FString GetCookbookPath();
	/** Writes the cookbook when it is missing or out of date. */
	bool EnsureCookbook();
}
