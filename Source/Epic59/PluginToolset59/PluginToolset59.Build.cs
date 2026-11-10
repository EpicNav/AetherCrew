// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class PluginToolset59 : ModuleRules
{
	public PluginToolset59(ReadOnlyTargetRules Target) : base(Target)
	{
		// UE 5.9 compat: selects the 5.9 side of this module's compat hunks
		PrivateDefinitions.Add("UE59_COMPAT=1");

		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Kismet",
				"PluginUtils",
				"Projects",
				"SourceControl",
				"ToolsetRegistry",
				"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
				"ToolsetRegistryEditor",
				"UnrealEd",
				// ... add private dependencies that you statically link with here ...	
			}
			);
	}
}
