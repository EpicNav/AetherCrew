// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class RewindDebuggerToolset : ModuleRules
	{
		public RewindDebuggerToolset(ReadOnlyTargetRules Target) : base(Target)
		{
			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"ToolsetCore",
					"ToolsetRegistry",
					"AsyncFunctionResult", // UE 5.8: compat layer for 5.9 code
					"ToolsetRegistryEditor",
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"EditorSubsystem",
					"Engine",
					"GameplayInsights",
					"Json",
					"RewindDebuggerInterface",
					"TraceServices",
					"UnrealEd",
				}
			);
		}
	}
}
