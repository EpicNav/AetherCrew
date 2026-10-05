// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioSceneSpatial : ModuleRules
{
	public HyperAIStudioSceneSpatial(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// Foliage instances are counted apart, and are not ground unless a plan asks.
			"Foliage",
			"HyperAIStudio",
			"Landscape",
			// Collision shapes for sweeps, overlaps and clear-spot searches.
			"PhysicsCore",
			// Nanite is decided per component as the renderer decides it, and the last frame's draw calls are read.
			"RenderCore",
			"RHI",
			"ToolsetRegistry",
			"UnrealEd"
		});

		// Headers only, for the test that holds PCG's generated-component tag equal to the text this module matches: the
		// module itself never links PCG, so its tools stay available in projects without the PCG plugin.
		PrivateIncludePathModuleNames.Add("PCG");
	}
}
