// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class PerfToolset : ModuleRules
{
	public PerfToolset(ReadOnlyTargetRules Target) : base(Target)
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
				"AsyncFunctionResult",
				"CoreUObject",
				"CSVUtils",
				"Engine",
				"Json",
				"Projects",
				"RHI",
				"RenderCore",
				"ToolsetRegistry",
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
				"ToolsetRegistryEditor", // UE 5.8: compat layer for 5.9 code
			}
			);

		if (Target.bBuildEditor)
		{
			// UnrealEd: only needed to work around EDA focus stealing during perf capture
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
