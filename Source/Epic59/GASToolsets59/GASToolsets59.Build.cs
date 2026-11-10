// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GASToolsets59 : ModuleRules
{
	public GASToolsets59(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.Add("Core");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"CoreUObject",
			"Engine",
			"GameplayAbilities",
			"GameplayAbilitiesEditor",
			"GameplayTags",
			"GameplayTagsEditor",
			"Kismet",
			"BlueprintGraph",
			"ToolsetRegistry",
			"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
			"ToolsetCore", // UE 5.8: compat layer for 5.9 code
			"ToolsetRegistryEditor",
			"UnrealEd",
		});
	}
}
