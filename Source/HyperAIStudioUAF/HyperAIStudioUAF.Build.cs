// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioUAF : ModuleRules
{
	public HyperAIStudioUAF(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// UAF keeps its graph authoring API in Internal headers, which UnrealBuildTool shares only with modules in the
		// same scope or with engine-scoped ones. UAF is experimental, so this module is the one place that reaches in.
		bTreatAsEngineModule = true;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"HyperAIStudio",
			"RenderCore",
			"RigVM",
			"RigVMDeveloper",
			"ToolsetRegistry",
			"UAF",
			"UAFAnimGraph",
			"UAFAnimGraphEditor",
			"UAFAnimGraphUncookedOnly",
			"UAFUncookedOnly",
			"UnrealEd"
		});
	}
}
