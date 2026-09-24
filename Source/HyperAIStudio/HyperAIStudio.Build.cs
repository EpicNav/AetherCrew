// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudio : ModuleRules
{
	public HyperAIStudio(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"DeveloperSettings",
			"Engine",
			"Slate",
			"SlateCore"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"ApplicationCore",
			"AssetRegistry",
			"BlueprintGraph",
			"ContentBrowser",
			"DesktopPlatform",
			"EnhancedInput",
			"GraphEditor",
			"ImageCore",
			"HTTP",
			"HyperAIStudioBot",
			"InputCore",
			"Json",
			"JsonUtilities",
			"LevelEditor",
			"MainFrame",
			"ModelContextProtocol",
			"ModelContextProtocolEngine",
			"Projects",
			"PropertyEditor",
			"RenderCore",
			"Settings",
			"Sockets",
			"Terminal",
			"FileSandboxCore",
			"ToolsetRegistry",
			"ToolMenus",
			"ToolWidgets",
			// The off-screen widget renderer the UI screenshot tests draw with.
			"UMG",
			"UnrealEd"
		});
	}
}
