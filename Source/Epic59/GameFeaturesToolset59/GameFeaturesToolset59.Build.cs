// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GameFeaturesToolset59 : ModuleRules
{
	public GameFeaturesToolset59(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"AsyncFunctionResult",
				"CoreUObject",
				"Engine",
				"GameFeatures",
				"Json",
				"JsonUtilities",
				"Kismet",
				"PluginUtils",
				"Projects",
				"SourceControl",
				"ToolsetRegistry",
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
				"ToolsetRegistryEditor",
				"UnrealEd",
			}
			);
	}
}
