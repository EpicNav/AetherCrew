// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioImageGen : ModuleRules
{
	public HyperAIStudioImageGen(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"Json",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
