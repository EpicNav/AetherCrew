// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioProfiling : ModuleRules
{
	public HyperAIStudioProfiling(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			// A bot route plays while the session is measured.
			"HyperAIStudioBot",
			"Json",
			"JsonUtilities",
			// Live readout of the running systems, their emitters, particles and effect types.
			"Niagara",
			"Projects",
			// Nanite on this platform, the last frame's draw calls and GPU time.
			"RenderCore",
			"RHI",
			"ToolsetRegistry",
			// Unreal Insights traces, read in the editor for the hottest CPU timers.
			"TraceLog",
			"TraceServices",
			"UnrealEd"
		});
	}
}
