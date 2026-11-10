// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class NavMeshToolsetTests : ModuleRules
{
	public NavMeshToolsetTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AIModule",
				"AsyncFunctionResult",
				"Core",
				"CoreUObject",
				"Engine",
				"NavigationSystem",
				"NavMeshToolset",
				"ToolsetRegistry",
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
				"ToolsetRegistryEditor", // UE 5.8: compat layer for 5.9 headers
				"UnrealEd",
			}
		);

		// The toolset module keeps every header private; these tests are its only consumer and
		// reach in directly, the same bridge PCGTests uses into PCG's Private.
		PrivateIncludePaths.Add(Path.Combine(GetModuleDirectory("NavMeshToolset"), "Private"));
	}
}
