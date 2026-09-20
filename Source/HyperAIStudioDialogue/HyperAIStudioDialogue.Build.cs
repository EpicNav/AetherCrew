// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioDialogue : ModuleRules
{
	public HyperAIStudioDialogue(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"AssetTools",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"Json",
			"RenderCore",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
