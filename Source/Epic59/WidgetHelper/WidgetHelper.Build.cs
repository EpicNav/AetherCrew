// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class WidgetHelper : ModuleRules
{
	public WidgetHelper(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"SlateCore",
		});
	}
}
