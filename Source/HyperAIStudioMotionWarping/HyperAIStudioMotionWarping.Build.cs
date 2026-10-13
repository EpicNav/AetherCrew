// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioMotionWarping : ModuleRules
{
	public HyperAIStudioMotionWarping(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			// UAnimNotifyState_MotionWarping, the root motion modifiers and UMotionWarpingComponent (the Motion Warping plugin).
			"MotionWarping",
			// The tests read the motion warping skill shipped with the plugin.
			"Projects",
			"ToolsetRegistry",
			// FKismetEditorUtilities and FBlueprintEditorUtils for the character Blueprint's component; UAnimMontageFactory in the tests.
			"UnrealEd"
		});
	}
}
