// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * A generated map of every enabled HyperAIStudio toolset and tool, with each tool's top-level fields
 * spelled exactly as the MCP schema spells them, kept in the project's .hyperai/docs.
 *
 * Discovery is the most expensive part of a short task: one list_toolsets over ~95 toolsets, then a
 * describe_toolset per guess, some of them tens of kilobytes. One local file read replaces most of it.
 * Built from live reflection, so it can never drift from the tools actually registered.
 */
namespace HyperAIStudio::ToolGuide
{
	FString Build();
	FString GetPath();
	/** Epic's toolsets the same way, from the schemas the toolset registry serves, in Epic-ToolGuide.md beside it. */
	FString BuildEpic();
	FString GetEpicPath();
	/** Writes both guides when missing or different from what the running editor would write. */
	bool Ensure();
}
