// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class SlateInspectorToolset59 : ModuleRules
	{
		public SlateInspectorToolset59(ReadOnlyTargetRules Target) : base(Target)
		{
			// SlateInspectorToolsetInputScope.cpp calls into FMacApplication, whose headers include Cocoa.
			if (Target.Platform == UnrealTargetPlatform.Mac)
			{
				bRequiresPlatformSDK = true;
			}

			PublicDependencyModuleNames.AddRange(
				new string[] {
					"ToolsetCore",
					"ToolsetRegistry",
					"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
					"ToolsetRegistryEditor",
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"ApplicationCore",
					"Core",
					"CoreUObject",
					"EditorSubsystem",
					"Engine",
					"ImageWrapper",
					"InputCore",
					"Slate",
					"SlateCore",
					"UnrealEd",
					"WidgetHelper",
				}
			);
		}
	}
}
