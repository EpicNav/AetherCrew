// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class InputToolset : ModuleRules
{
	public InputToolset(ReadOnlyTargetRules Target) : base(Target)
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
				"ApplicationCore",
				"AsyncFunctionResult",
				"CoreUObject",
				"Engine",
				"EnhancedInput",
				"InputCore",
				"ToolsetCore",
				"ToolsetRegistry",
				"ToolsetRegistryEditor", // UE 5.8: compat layer for 5.9 code
			}
			);

		if (Target.bBuildEditor)
		{
			// UnrealEd: test-only, to drive a real PIE session in automation tests.
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
