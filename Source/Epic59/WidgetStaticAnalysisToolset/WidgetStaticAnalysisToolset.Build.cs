// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WidgetStaticAnalysisToolset : ModuleRules
{
	public WidgetStaticAnalysisToolset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"ToolsetRegistry",
				"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
				"ToolsetRegistryEditor",
				"UMG",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"Slate",
				"SlateCore",
				"UMGEditor",
				"Kismet",
				"AssetRegistry",
				"DataValidation",
				"FieldNotification",
				"MessageLog",
				"BlueprintGraph",
				"MovieScene",
				"MovieSceneTracks",
				"ModelViewViewModel",
				"ModelViewViewModelBlueprint",
				"ModelViewViewModelEditor",
			}
		);
	}
}
