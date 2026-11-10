// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class RewindDebuggerToolsetTests : ModuleRules
	{
		public RewindDebuggerToolsetTests(ReadOnlyTargetRules Target) : base(Target)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"CQTest",
					"Engine",
					"RewindDebuggerToolset",
				}
			);
		}
	}
}
