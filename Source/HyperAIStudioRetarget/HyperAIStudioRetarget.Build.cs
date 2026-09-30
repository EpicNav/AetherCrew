// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioRetarget : ModuleRules
{
	public HyperAIStudioRetarget(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Loaded only when the IKRig plugin is enabled (HyperAIStudioModule's optional module table requires it).
		// Slate and SlateCore: the IK Rig editor headers bring in widget types.
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"IKRig",
			"IKRigDeveloper",
			"IKRigEditor",
			"Slate",
			"SlateCore",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
