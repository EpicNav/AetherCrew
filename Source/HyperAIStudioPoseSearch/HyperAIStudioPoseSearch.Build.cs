// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioPoseSearch : ModuleRules
{
	public HyperAIStudioPoseSearch(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Loaded only when the PoseSearch plugin is enabled (HyperAIStudioModule's optional module table requires it).
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"PoseSearch",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
