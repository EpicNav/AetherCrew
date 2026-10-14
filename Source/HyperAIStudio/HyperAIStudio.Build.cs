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
			// The Content Browser's current folder, for the prompt presets' {{content_folder}}.
			"ContentBrowserData",
			"DesktopPlatform",
			"EnhancedInput",
			"GraphEditor",
			"ImageCore",
			"ImageWrapper",
			"HTTP",
			// The remote page's server, and the stand-in servers tests talk to, such as Ollama's.
			"HTTPServer",
			"HyperAIStudioBot",
			"InputCore",
			// An import running keeps the editor open; a restart waits for it before it asks to close again.
			"InterchangeEngine",
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
			"UnrealEd",
			// The Docs page shows HyperAI's own documentation in the editor's web browser (CEF).
			"WebBrowser"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Playtest videos are H.264 through Media Foundation, which ships with Windows; delay-loaded, so an
			// N edition without the Media Feature Pack still loads the plugin and only recording is unavailable.
			PublicSystemLibraries.AddRange(new[] { "mfplat.lib", "mfreadwrite.lib", "mfuuid.lib" });
			PublicDelayLoadDLLs.AddRange(new[] { "mfplat.dll", "mfreadwrite.dll" });
			// A deleted chat's log goes to the Recycle Bin through the shell.
			PublicSystemLibraries.Add("shell32.lib");
			// The ports this editor listens on (GetExtendedTcpTable), to tell an MCP server inside it from one outside.
			PublicSystemLibraries.Add("iphlpapi.lib");
			// Auto-approval kept on after a restart: its record is sealed with DPAPI (CryptProtectData) for this user.
			PublicSystemLibraries.Add("crypt32.lib");
			// Keys the user gives agents and providers live in Windows Credential Manager (CredWrite, CredRead).
			PublicSystemLibraries.Add("advapi32.lib");
		}
	}
}
