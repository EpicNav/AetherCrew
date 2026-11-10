// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class DataRegistryToolset59 : ModuleRules
{
	public DataRegistryToolset59(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.Add("Core");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"DataRegistry",
			"Engine",
			"GameplayTags",
			"Json",
			"ToolsetRegistry",
			"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
			"ToolsetCore", // UE 5.8: compat layer for 5.9 code
			"ToolsetRegistryEditor",
		});
	}
}
