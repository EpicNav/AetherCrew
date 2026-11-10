// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class NavMeshToolset : ModuleRules
{
	public NavMeshToolset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AIModule",
				"CoreUObject",
				"Engine",
				"NavigationSystem",
				"ToolsetRegistry",
				"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
				"ToolsetRegistryEditor", // UE 5.8: compat layer for 5.9 headers
			}
		);

		// The tile and poly code consumes WITH_RECAST-only APIs. Targets with
		// bCompileRecast=false compile every implementation file down to no-recast stubs (the
		// tools raise a script error and the module never registers the toolset).
		if (Target.bCompileRecast)
		{
			PrivateDependencyModuleNames.Add("Navmesh");
		}
	}
}
