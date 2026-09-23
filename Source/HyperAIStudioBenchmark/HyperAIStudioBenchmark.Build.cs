// Games by Hyper 2026.

using UnrealBuildTool;

// Verifies benchmark tasks against the engine directly, never through HyperAIStudio's own tools, so a
// defect in a tool cannot also be the thing that passes its work. Loaded only by the harness.
public class HyperAIStudioBenchmark : ModuleRules
{
	public HyperAIStudioBenchmark(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AnimGraph",
			"AnimGraphRuntime",
			"BlueprintGraph",
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"MovieScene",
			"MovieSceneTracks",
			"Niagara",
			"RHI",
			"RenderCore",
			"SlateCore",
			"UMG",
			"UMGEditor",
			"UnrealEd"
		});
	}
}
