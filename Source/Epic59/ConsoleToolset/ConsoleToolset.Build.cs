// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ConsoleToolset : ModuleRules
{
	public ConsoleToolset(ReadOnlyTargetRules Target) : base(Target)
	{
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
				"Json",
				"ToolsetRegistry",
				"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
				"ToolsetRegistryEditor", // UE 5.8: compat layer for 5.9 headers
			}
			);
	}
}
