// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioMeshGen : ModuleRules
{
	public HyperAIStudioMeshGen(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Interchange's generic pipeline imports animations onto a chosen skeleton; the module loads only with the Interchange
		// plugin (HyperAIStudioModule's optional module table requires it). PhysicsUtilities makes a physics asset when an
		// import made none. The animation_rigging pack is reached through reflection, so it stays optional.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"AssetTools",
			"Core",
			"CoreUObject",
			"Engine",
			"HTTP",
			// The stand-in services the tests answer from.
			"HTTPServer",
			"HyperAIStudio",
			"InterchangeCore",
			"InterchangeEngine",
			"InterchangePipelines",
			"Json",
			"PhysicsUtilities",
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
