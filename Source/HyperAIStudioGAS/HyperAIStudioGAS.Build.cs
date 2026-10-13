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
			// The bot's GAS assertions are tested here, where GAS is linked.
			"HyperAIStudioBot",
			"ToolsetRegistry",
			"GameplayAbilities",
			"GameplayTags",
			"UnrealEd"
		});
	}
}
