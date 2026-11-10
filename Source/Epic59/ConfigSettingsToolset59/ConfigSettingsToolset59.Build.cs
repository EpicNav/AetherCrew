// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ConfigSettingsToolset59 : ModuleRules
{
	public ConfigSettingsToolset59(ReadOnlyTargetRules Target) : base(Target)
	{
		// UE 5.9 compat: selects the 5.9 side of this module's compat hunks
		PrivateDefinitions.Add("UE59_COMPAT=1");

		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Kismet",
				"Projects",
				"Settings",
				"SourceControl",
				"ToolsetRegistry",
				"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
				"ToolsetRegistryEditor",
				"UnrealEd",
			}
			);
	}
}
