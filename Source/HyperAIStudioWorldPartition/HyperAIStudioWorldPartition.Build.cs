// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioWorldPartition : ModuleRules
{
	public HyperAIStudioWorldPartition(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			// Data layers are made and filled through the editor's own subsystem.
			"DataLayerEditor",
			"Engine",
			"HyperAIStudio",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
