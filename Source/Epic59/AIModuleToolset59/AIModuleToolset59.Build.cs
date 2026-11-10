// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class AIModuleToolset59 : ModuleRules
{
	public AIModuleToolset59(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"AIModule",
				"ToolsetRegistry",
				"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Engine",
				"AIGraph",
				"AssetTools",
				"BehaviorTreeEditor",
				"BlueprintGraph",
				"EnvironmentQueryEditor",
				"ToolsetRegistryEditor",
				"UnrealEd",
			}
		);
	}
}
