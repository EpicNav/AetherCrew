// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WorldConditionsToolset59 : ModuleRules
{
	public WorldConditionsToolset59(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.Add("Core");
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Json",
			"ToolsetRegistry",
			"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
			"ToolsetCore", // UE 5.8: compat layer for 5.9 code
			"ToolsetRegistryEditor",
			"UnrealEd",
			"WorldConditions",
		});
	}
}
