// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioAudioGen : ModuleRules
{
	public HyperAIStudioAudioGen(ReadOnlyTargetRules Target) : base(Target)
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
			// The MetaSound and Sound Cue players the clips are wrapped in.
			"HyperAIStudioAudioAuthoring",
			"Json",
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
