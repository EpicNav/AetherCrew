// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class LiveCodingToolsetTests59 : ModuleRules
	{
		public LiveCodingToolsetTests59(ReadOnlyTargetRules Target) : base(Target)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"AsyncFunctionResult",
				"Core",
				"CoreUObject",
				"CQTest",
				"Engine",
				"LiveCodingToolset59",
				"ToolsetRegistry",
				"ToolsetCore", // UE 5.8: compat layer for 5.9 code
				"ToolsetRegistryEditor",
				"UnrealEd",
			});
		}
	}
}
