// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioImageGen : ModuleRules
{
	public HyperAIStudioImageGen(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"ApplicationCore",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"InputCore",
			"Json",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
