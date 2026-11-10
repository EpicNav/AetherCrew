// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class SlateInspectorToolsetTests59 : ModuleRules
{
	public SlateInspectorToolsetTests59(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"Core",
			"CoreUObject",
			"CQTest",
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore",
			"SlateInspectorToolset59",
			"UMG",
			"WidgetHelper",
		});
	}
}
