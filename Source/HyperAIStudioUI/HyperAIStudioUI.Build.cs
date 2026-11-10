// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioUI : ModuleRules
{
	public HyperAIStudioUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"ToolsetRegistry",
			"UMG",
			"UMGEditor",
			"UnrealEd",
			// hyper_ui_render_review draws widgets off screen and reads the pixels back.
			"RenderCore",
			"Slate",
			"SlateCore",
			"MovieScene",
			"MovieSceneTracks",
			"ModelViewViewModel",
			"ModelViewViewModelBlueprint"
		});
	}
}
