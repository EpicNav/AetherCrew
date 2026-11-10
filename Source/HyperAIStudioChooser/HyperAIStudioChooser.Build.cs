// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioChooser : ModuleRules
{
	public HyperAIStudioChooser(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Loaded only when the Chooser plugin is enabled (HyperAIStudioModule's optional module table requires it). Pose
		// Match columns live in the PoseSearch plugin, which is not linked: their column type is found at run time.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			// UChooserTable, its column, parameter and result structs, and UChooserTable::EvaluateChooser.
			"Chooser",
			"Core",
			"CoreUObject",
			"Engine",
			// Gameplay tag columns hold FGameplayTagContainer and FGameplayTagQuery cells.
			"GameplayTags",
			"HyperAIStudio",
			// The tests read the chooser skill shipped with the plugin.
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
