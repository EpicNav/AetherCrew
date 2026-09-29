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
			// The remote page's server, and the stand-in servers tests talk to, such as Ollama's.
			"HTTPServer",
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
			"SourceControl",
			"Terminal",
			"FileSandboxCore",
			"ToolsetRegistry",
			"ToolMenus",
			"ToolWidgets",
			// The off-screen widget renderer the UI screenshot tests draw with.
			"UMG",
			"UnrealEd"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Playtest videos are H.264 through Media Foundation, which ships with Windows; delay-loaded, so an
			// N edition without the Media Feature Pack still loads the plugin and only recording is unavailable.
			PublicSystemLibraries.AddRange(new[] { "mfplat.lib", "mfreadwrite.lib", "mfuuid.lib" });
			PublicDelayLoadDLLs.AddRange(new[] { "mfplat.dll", "mfreadwrite.dll" });
		}
	}
}
