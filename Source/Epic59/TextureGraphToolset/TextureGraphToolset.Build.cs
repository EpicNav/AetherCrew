// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class TextureGraphToolset : ModuleRules
{
	public TextureGraphToolset(ReadOnlyTargetRules Target) : base(Target)
	{
		// UE 5.9 compat: selects the 5.9 side of this module's compat hunks
		PrivateDefinitions.Add("UE59_COMPAT=1");

		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Must match TextureGraph/TextureGraphEditor/TextureGraphEngine. FTG_HelperFunctions
		// returns cti::continuable_base, whose error type is std::exception_ptr with exceptions
		// enabled and std::error_condition without. A mismatch changes the mangled name, so the
		// link fails on targets that do not enable exceptions.
		bEnableExceptions = true;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"AsyncFunctionResult",
			"Core",
			"CoreUObject",
			"Engine",
			"ToolsetRegistry",
			"ToolsetCore", // UE 5.8: compat layer for 5.9 code
			"TextureGraph",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"AssetTools",
			"AssetRegistry",
			"TextureGraphEditor",
			"TextureGraphEngine",
			"EditorScriptingUtilities",
			"ImageCore",
			"ToolsetRegistryEditor",
		});
	}
}
