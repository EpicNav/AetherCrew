// Games by Hyper 2026.

using UnrealBuildTool;

public class HyperAIStudioScripting : ModuleRules
{
	public HyperAIStudioScripting(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"Json",
			"Projects",
			"PythonScriptPlugin",
			"ToolsetRegistry",
			"UnrealEd"
		});
	}
}
