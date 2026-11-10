// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CheatsToolset : ModuleRules
{
	public CheatsToolset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.Add("Core");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Kismet",
			"ToolsetRegistry",
			"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
			"ToolsetCore", // UE 5.8: compat layer for 5.9 code
			"ToolsetRegistryEditor",
		});
	}
}
