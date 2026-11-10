// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

// UE 5.8: compatibility stand-in for UE 5.9's ToolsetRegistryEditor module. It re-exports 5.8's
// ToolsetRegistry plus the compat AsyncFunctionResult and ToolsetCore modules, so a ported toolset
// that depends on ToolsetRegistryEditor sees every 5.9 header it expects. See Source/Epic59/PORTING.md.
public class ToolsetRegistryEditor : ModuleRules
{
	public ToolsetRegistryEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.Full;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"ToolsetRegistry",
				"AsyncFunctionResult",
				"ToolsetCore",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"Projects", // UE 5.8: Epic59Ports.cpp checks which plugins are enabled
				"UnrealEd",
			}
			);
	}
}
