// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioSequencer : ModuleRules
{
	public HyperAIStudioSequencer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"CinematicCamera",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"LevelSequence",
			"MovieScene",
			"MovieSceneTracks",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
