// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioFoliage : ModuleRules
{
	public HyperAIStudioFoliage(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			"Engine",
			"Foliage",
			"HyperAIStudio",
			// Paint layer weights under a scatter's spots.
			"Landscape",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
