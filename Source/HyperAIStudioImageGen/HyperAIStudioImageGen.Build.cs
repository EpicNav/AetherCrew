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
			// The tests play Google's image endpoint on a local port.
			"HTTPServer",
			"HyperAIStudio",
			// The size of a picture to change.
			"ImageCore",
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
