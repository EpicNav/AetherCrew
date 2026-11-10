// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioMovieRender : ModuleRules
{
	public HyperAIStudioMovieRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			// The tests cut a short sequence to a camera to render.
			"CinematicCamera",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"LevelSequence",
			"MovieRenderPipelineCore",
			"MovieRenderPipelineEditor",
			"MovieRenderPipelineSettings",
			"MovieScene",
			"MovieSceneTracks",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
