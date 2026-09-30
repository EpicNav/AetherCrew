// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioGAS : ModuleRules
{
	public HyperAIStudioGAS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"ToolsetRegistry",
			"GameplayAbilities",
			"GameplayTags",
			"UnrealEd"
		});
	}
}
