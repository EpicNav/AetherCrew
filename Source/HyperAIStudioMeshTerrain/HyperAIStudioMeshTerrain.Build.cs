// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioMeshTerrain : ModuleRules
{
	public HyperAIStudioMeshTerrain(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Mesh Partition's public headers include Engine Internal headers (the material cache), which UnrealBuildTool
		// shares only with engine-scoped modules. Mesh Partition is experimental, so this module is the one place that reaches in.
		bTreatAsEngineModule = true;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GeometryCore",
			"HyperAIStudio",
			"MeshPartition",
			"MeshPartitionEditor",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
