// Games by Hyper 2026.

using UnrealBuildTool;

// The playtest bot: steps that play the game through its own input and movement, then check the result.
// Runtime modules only, so a -game worker process can load it as well as the editor's PIE.
public class HyperAIStudioBot : ModuleRules
{
	public HyperAIStudioBot(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"EnhancedInput",
			"InputCore",
			"NavigationSystem",
			"Slate",
			"SlateCore",
			"UMG"
		});
	}
}
