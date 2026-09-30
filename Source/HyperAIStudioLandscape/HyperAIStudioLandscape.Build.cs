// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioLandscape : ModuleRules
{
	public HyperAIStudioLandscape(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			"Engine",
			// LandscapeEdit.h includes the foliage actor's header.
			"Foliage",
			"HyperAIStudio",
			"ImageCore",
			"Landscape",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
