// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioDestruction : ModuleRules
{
	public HyperAIStudioDestruction(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			// The collection's managed arrays, facades (hierarchy, bounds, anchoring) and clustering utilities.
			"Chaos",
			"Core",
			"CoreUObject",
			// FDataflowTransformSelection, the selection the Fracture plugin's cutters take.
			"DataflowCore",
			"Engine",
			// FFractureEngineFracturing, FFractureEngineClustering, FFractureEngineMaterials, FFractureEngineUtility:
			// the functions the Dataflow fracture nodes call (the Fracture plugin).
			"FractureEngine",
			// UGeometryCollection, AGeometryCollectionActor and FGeometryCollectionEngineConversion.
			"GeometryCollectionEngine",
			"HyperAIStudio",
			// The tests drive the playtest bot's Damage, Fire and AssertBroken steps in a game world.
			"HyperAIStudioBot",
			// The verification case the plan hands back is JSON; the tests read hyper_verify_run's report.
			"Json",
			"JsonUtilities",
			// The exploded preview is drawn from a temporary static mesh built from the pieces.
			"MeshDescription",
			// UPhysicalMaterial for the mass op.
			"PhysicsCore",
			// CutMultipleWithPlanarCells, FPlanarCells and FNoiseSettings (the PlanarCut plugin under FractureEngine).
			"PlanarCut",
			// The tests read the destruction skill shipped with the plugin.
			"Projects",
			"StaticMeshDescription",
			"ToolsetRegistry",
			"UnrealEd",
			// FVoronoiDiagram, from which the Voronoi cutters build their cells.
			"Voronoi"
		});
	}
}
