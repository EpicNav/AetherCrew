// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioModelingGate.h"

/**
 * Handing a static mesh to a person in Unreal's Modeling Mode: the mode's tools need a person's clicks, viewport ticks
 * and an Accept, so the hand-off selects and frames the mesh, opens the mode, and remembers the mesh as it was, so a
 * later inspect can tell whether the person changed it.
 */
namespace HyperAIStudio::Modeling::Handoff
{
	/** The line inspect adds for an asset handed to Modeling Mode this session: when, its fingerprint then, and whether it changed; empty if it was not. */
	FString DescribeSince(const FString& AssetPath, const Gate::FGeometryPrint& Now);
}
