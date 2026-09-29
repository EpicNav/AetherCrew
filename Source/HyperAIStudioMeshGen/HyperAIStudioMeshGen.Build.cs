// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioMeshGen : ModuleRules
{
	public HyperAIStudioMeshGen(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetTools",
			"Core",
			"CoreUObject",
			"Engine",
			"HTTP",
			// The stand-in services the tests answer from.
			"HTTPServer",
			"HyperAIStudio",
			"Json",
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
