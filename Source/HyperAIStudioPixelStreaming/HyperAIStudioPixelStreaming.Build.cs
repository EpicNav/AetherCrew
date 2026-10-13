// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioPixelStreaming : ModuleRules
{
	public HyperAIStudioPixelStreaming(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Loaded by core's phone playtest only when the project enables Pixel Streaming 2; it gives core the plugin's
		// in-editor signalling server and PIE streaming through IHyperAIStudioPixelStreamingBackend.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			// IPixelStreaming2Module, its streamers and the default streamer id.
			"PixelStreaming2",
			"PixelStreaming2Core",
			// The PIE streamer's signalling port.
			"PixelStreaming2Editor",
			// MakeSignallingServer: Epic's C++ signalling server, which runs in this process with nothing downloaded.
			"PixelStreaming2Servers",
			// The editor module's public header includes the settings enums.
			"PixelStreaming2Settings",
			"Projects",
			"UnrealEd"
		});
	}
}
