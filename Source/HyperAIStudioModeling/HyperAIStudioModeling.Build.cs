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
			// FEditorModeInfo: whether Modeling Mode is registered, asked by name so its Beta plugin is not linked.
			"EditorFramework",
			"Engine",
			// FDynamicMesh3, UDynamicMesh and the transient mesh pool the plan builds in.
			"GeometryCore",
			"GeometryFramework",
			// The mesh ops themselves: primitives, booleans, modelling, warps, remeshing, UVs, collision.
			"GeometryScriptingCore",
			"HyperAIStudio",
			// A mode's tools context, to see whether Modeling Mode has a tool open and whether a named tool would start.
			"InteractiveToolsFramework",
			// The tests read the modeling skill's example plans and run them.
			"Json",
			"JsonUtilities",
			// The level editor's tab and viewport, brought in front of HyperAI Studio for a hand-off.
			"LevelEditor",
			// Static mesh source data, read back to check what was written.
			"MeshDescription",
			// UE::AssetUtils::CreateStaticMeshAsset, which writes a new asset without opening a transaction of its own.
			"ModelingComponentsEditorOnly",
			// ECollisionTraceFlag for complex-as-simple collision.
			"PhysicsCore",
			"Projects",
			// Tabs, and the notification a hand-off shows the person.
			"Slate",
			"SlateCore",
			// The first UV set, hashed into a mesh's geometry fingerprint.
			"StaticMeshDescription",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
