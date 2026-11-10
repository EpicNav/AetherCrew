// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

// UE 5.8: compatibility stand-in for UE 5.9's Engine/Source/Runtime/ToolsetCore module
// (IToolsetProvider, IToolsetPermissionProvider, FToolsetRegistration). See Source/Epic59/PORTING.md.
public class ToolsetCore : ModuleRules
{
	public ToolsetCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
			}
		);
	}
}
