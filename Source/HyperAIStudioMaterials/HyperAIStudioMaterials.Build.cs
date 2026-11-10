// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioMaterials : ModuleRules
{
	public HyperAIStudioMaterials(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"Json",
			// Landscape layer blend and coordinates nodes for the landscape_layers recipe.
			"Landscape",
			"MaterialEditor",
			"RenderCore",
			"RHI",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
