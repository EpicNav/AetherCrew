// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MVVMToolset59 : ModuleRules
{
	public MVVMToolset59(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.Add("Core");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"BlueprintEditorLibrary",
			"BlueprintGraph",
			"CoreUObject",
			"Engine",
			"Json",
			"Kismet",
			"ModelViewViewModel",
			"ModelViewViewModelBlueprint",
			"ModelViewViewModelEditor",
			"ToolsetRegistry",
			"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
			"ToolsetCore", // UE 5.8: compat layer for 5.9 code
			"ToolsetRegistryEditor",
			"UnrealEd",
			"UMG",
			"UMGEditor",
		});
	}
}
