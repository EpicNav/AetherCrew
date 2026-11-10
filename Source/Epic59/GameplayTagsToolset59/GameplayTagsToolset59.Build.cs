// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GameplayTagsToolset59 : ModuleRules
{
	public GameplayTagsToolset59(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;
		bUseUnity = true;

		PublicDependencyModuleNames.Add("Core");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"CoreUObject",
			"DeveloperSettings",
			"EditorScriptingUtilities",
			"Engine",
			"GameplayTags",
			"GameplayTagsEditor",
			"Kismet",
			"SourceControl",
			"ToolsetRegistry",
			"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
			"ToolsetCore", // UE 5.8: compat layer for 5.9 code
			"ToolsetRegistryEditor",
			"UnrealEd",
		});
	}
}
