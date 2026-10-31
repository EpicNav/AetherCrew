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
			"DeveloperSettings",
			// The model creator's checks and free fixes: meshes read as dynamic meshes, holes capped, UVs unwrapped (xatlas).
			"DynamicMesh",
			"Engine",
			"GeometryAlgorithms",
			"GeometryCore",
			"HTTP",
			// The stand-in services the tests answer from.
			"HTTPServer",
			"HyperAIStudio",
			"ImageCore",
			"InterchangeCore",
			"InterchangeEngine",
			"InterchangePipelines",
			"Json",
			"JsonUtilities",
			"MeshConversion",
			"MeshDescription",
			"PhysicsCore",
			"PhysicsUtilities",
			"Projects",
			// The tests ask whether a material compiled for the editor's shader platform.
			"RHI",
			// The Tripo account panel in the creator's settings, and the notification offering to fix a Tripo Studio model.
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"StaticMeshDescription",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
