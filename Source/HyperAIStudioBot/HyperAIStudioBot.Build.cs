// Games by Hyper 2026.

using UnrealBuildTool;

// The playtest bot: steps that play the game through its own input and movement, then check the result.
// Runtime modules only, so a -game worker process can load it as well as the editor's PIE.
public class HyperAIStudioBot : ModuleRules
{
	public HyperAIStudioBot(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"ApplicationCore",
			// Damage, Fire and AssertBroken: Chaos geometry collections, their dynamic state and strain fields.
			"Chaos",
			"EnhancedInput",
			"FieldSystemEngine",
			"GeometryCollectionEngine",
			// AssertTag reads gameplay tags; the rest of GAS is read by reflection, so games without it need nothing more.
			"GameplayTags",
			"InputCore",
			// SurveyLevel writes coverage.json.
			"Json",
			"NavigationSystem",
			"Slate",
			"SlateCore",
			"UMG"
		});
	}
}
