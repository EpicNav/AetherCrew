// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioPCGScatter : ModuleRules
{
	public HyperAIStudioPCGScatter(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			// Landscapes PCG passes over are named when a scatter finds no ground.
			"Landscape",
			"PCG",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
