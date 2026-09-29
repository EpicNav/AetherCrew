// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;

/**
 * Lays out the Blueprint nodes agents add, so a graph an agent wrote reads left to right instead of landing in one
 * overlapping row. HyperAI remembers each Blueprint graph's nodes when it is loaded or made; when the Blueprint
 * compiles (Epic's graph DSL compiles after every write), the nodes added since are arranged on the next tick with
 * Epic's layered graph formatter: exec and data flow in columns, crossings reduced, node sizes respected, and the
 * nodes that were already there left where they are as anchors. A Blueprint open in its editor is where a person
 * places nodes by hand, so there the new nodes move only when they overlap something. Game thread only.
 */
class HYPERAISTUDIO_API FHyperAIStudioBlueprintTidy
{
public:
	static void Startup();
	static void Shutdown();
	/** Remembers every node of Blueprint's graphs as already placed. */
	static void Remember(UBlueprint* Blueprint);
	/** Arranges the nodes added to Blueprint since it was remembered; the number of nodes moved. */
	static int32 Tidy(UBlueprint* Blueprint);
};
