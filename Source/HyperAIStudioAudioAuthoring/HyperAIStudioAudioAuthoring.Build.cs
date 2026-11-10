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
			// Audio parameters for the offline renders and the preview.
			"AudioExtensions",
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
			// The MetaSound generator, run without an audio device to render and measure a source.
			"MetasoundGenerator",
			"MetasoundGraphCore",
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
