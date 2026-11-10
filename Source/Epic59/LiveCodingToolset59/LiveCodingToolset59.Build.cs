// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class LiveCodingToolset59 : ModuleRules
	{
		public LiveCodingToolset59(ReadOnlyTargetRules Target) : base(Target)
		{
			PublicDependencyModuleNames.AddRange(
				new string[] {
					"AsyncFunctionResult",
					"ToolsetCore",
					"ToolsetRegistry",
					"ToolsetRegistryEditor",
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"Core",
					"CoreUObject",
					"EditorSubsystem",
					"Engine",
					"UnrealEd",
				}
			);

			if (Target.bWithLiveCoding)
			{
				PrivateDependencyModuleNames.Add("LiveCoding");
			}
		}
	}
}
