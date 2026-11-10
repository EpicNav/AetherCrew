// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

// UE 5.8: compatibility stand-in for UE 5.9's Engine/Source/Runtime/AsyncFunctionResult module. It
// is an editor module here because its base class, 5.8's UToolCallAsyncResult, lives in the
// editor-only ToolsetRegistry plugin. See Source/Epic59/PORTING.md.
public class AsyncFunctionResult : ModuleRules
{
	public AsyncFunctionResult(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Json",
				"JsonUtilities",
				// UE 5.8: UAsyncFunctionResult derives from 5.8's UToolCallAsyncResult.
				"ToolsetRegistry",
			}
			);
	}
}
