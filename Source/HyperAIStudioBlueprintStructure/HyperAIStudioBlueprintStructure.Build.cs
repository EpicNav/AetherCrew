// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioBlueprintStructure : ModuleRules
{
	public HyperAIStudioBlueprintStructure(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"BlueprintEditorLibrary",
			"BlueprintGraph",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"Kismet",
			"KismetCompiler",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
