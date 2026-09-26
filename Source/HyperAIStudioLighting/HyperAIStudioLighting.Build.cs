// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioLighting : ModuleRules
{
	public HyperAIStudioLighting(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			// Registers the LUT textures and cloud material instances a plan makes.
			"AssetRegistry",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"ImageCore",
			"Json",
			"RenderCore",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
