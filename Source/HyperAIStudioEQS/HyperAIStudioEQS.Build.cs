// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioEQS : ModuleRules
{
	public HyperAIStudioEQS(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			// UEnvQuery, its generators, tests and contexts, the query manager that runs it, and the Behavior Tree
			// Run EQS Query task and Run EQS service.
			"AIModule",
			// UAIGraphNode, which the Behavior Tree editor's nodes derive from (AddSubNode, PostPlacedNewNode).
			"AIGraph",
			"AssetRegistry",
			// UBehaviorTreeGraph and its node classes: a Run EQS task or service goes into the tree's graph, which
			// rebuilds the runtime tree, as the Behavior Tree editor does.
			"BehaviorTreeEditor",
			"Core",
			"CoreUObject",
			"Engine",
			// The Gameplay Tags test's tag query.
			"GameplayTags",
			"HyperAIStudio",
			// The tests read the EQS skill shipped with the plugin.
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
