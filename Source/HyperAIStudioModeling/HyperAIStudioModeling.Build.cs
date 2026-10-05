// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioModeling : ModuleRules
{
	public HyperAIStudioModeling(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			// FGroupTopology: subdivide reads a mesh's polygroups as a cage before OpenSubdiv runs.
			"DynamicMesh",
			"Engine",
			// FDynamicMesh3, UDynamicMesh and the transient mesh pool the plan builds in.
			"GeometryCore",
			"GeometryFramework",
			// The mesh ops themselves: primitives, booleans, modelling, warps, remeshing, UVs, collision.
			"GeometryScriptingCore",
			// Catmull-Clark subdivision (ApplyPolygroupCatmullClarkSubD) lives in Geometry Script's editor module.
			"GeometryScriptingEditor",
			"HyperAIStudio",
			// The tests read the previews' pixels back.
			"ImageCore",
			// The tests read the modeling skill's example plans and run them.
			"Json",
			"JsonUtilities",
			// Static mesh source data, read back to check what was written.
			"MeshDescription",
			// UE::AssetUtils::CreateStaticMeshAsset, which writes a new asset without opening a transaction of its own.
			"ModelingComponentsEditorOnly",
			// ECollisionTraceFlag for complex-as-simple collision.
			"PhysicsCore",
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
