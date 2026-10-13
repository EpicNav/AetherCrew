// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioAudioAuthoring : ModuleRules
{
	public HyperAIStudioAudioAuthoring(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"AssetTools",
			// The submix effect presets: reverb, EQ and the dynamics processor.
			"AudioMixer",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			// MetaSound Sources for the recipes and the generated clips' players.
			"MetasoundEditor",
			"MetasoundEngine",
			"MetasoundFrontend",
			"MetasoundGraphCore",
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
