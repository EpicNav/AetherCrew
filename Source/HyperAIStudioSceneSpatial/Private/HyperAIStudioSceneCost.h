// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

class UWorld;
struct FHyperAISceneCostReport;
struct FHyperAISceneCostRequest;

/**
 * What a world's loaded actors cost to render, in one bounded pass over them. Game thread only; nothing is loaded, and
 * meshes still compiling are left out rather than waited for.
 */
namespace HyperAIStudio::SceneSpatial::Cost
{
	/** Findings kept, at most. */
	constexpr int32 MaxFindings = 24;
	/** Local lights compared against every primitive for the shadow estimate, at most. */
	constexpr int32 MaxShadowLights = 512;
	/**
	 * LOD0 triangles over all of a mesh's copies drawn without Nanite past which the mesh is heavy for its instances
	 * together (non_nanite_instances): a sizeable share of a level's whole non-Nanite budget, a few million.
	 */
	constexpr int64 HeavyInstancedTriangles = 2000000;

	/**
	 * Measures World for the request's scope into Out. The request's limits are taken as given: the tool fits them
	 * into range first, and tests set them as they like.
	 */
	void Measure(UWorld& World, const FHyperAISceneCostRequest& Request, FHyperAISceneCostReport& Out);
	/** About how many bytes the report takes as JSON. */
	int64 EstimateBytes(const FHyperAISceneCostReport& Report);
}
