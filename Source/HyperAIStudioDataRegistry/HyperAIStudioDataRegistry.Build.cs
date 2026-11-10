// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioDataRegistry : ModuleRules
{
	public HyperAIStudioDataRegistry(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			// UDataRegistry, its table and meta sources, UDataRegistrySettings and UDataRegistrySubsystem (the Data Registry plugin).
			"DataRegistry",
			// UDataRegistrySettings is a UDeveloperSettings.
			"DeveloperSettings",
			"Engine",
			// A registry's ids can be gameplay tags (IdFormat's base tag); the tag is checked against the tags the project has.
			"GameplayTags",
			"HyperAIStudio",
			// Lookup can hand back each item as a JSON object.
			"Json",
			"JsonUtilities",
			// The tests read the data registry skill shipped with the plugin.
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
