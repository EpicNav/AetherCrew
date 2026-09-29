// Games by Hyper 2026.

using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using UnrealBuildTool;

public class HyperAIStudioDecibel : ModuleRules
{
	public HyperAIStudioDecibel(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new[]
		{
			"AssetRegistry",
			"AssetTools",
			"Core",
			"CoreUObject",
			"Engine",
			"HyperAIStudio",
			"Json",
			"MetasoundEditor",
			"MetasoundEngine",
			"MetasoundFrontend",
			"Projects",
			"ToolsetRegistry",
			"UnrealEd"
		});

		// Decibel is a project or Fab plugin, not the engine's. This module builds against it only where it is installed and
		// enabled; everywhere else it compiles to nothing, so HyperAI Studio still builds in projects without it.
		bool bDecibel = IsDecibelInstalled(Target);
		PublicDefinitions.Add("WITH_DECIBEL=" + (bDecibel ? "1" : "0"));
		if (bDecibel)
		{
			PrivateDependencyModuleNames.AddRange(new[] { "DecibelCore", "DecibelRuntime" });
		}
	}

	bool IsDecibelInstalled(ReadOnlyTargetRules Target)
	{
		var Roots = new System.Collections.Generic.List<string> { Path.Combine(EngineDirectory, "Plugins", "Marketplace") };
		if (Target.ProjectFile != null)
		{
			string Project = File.ReadAllText(Target.ProjectFile.FullName);
			if (Regex.IsMatch(Project, "\"Name\"\\s*:\\s*\"Decibel\"\\s*,\\s*\"Enabled\"\\s*:\\s*false", RegexOptions.IgnoreCase))
			{
				return false;
			}
			Roots.Insert(0, Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins"));
		}
		// Plugins/Decibel, or one folder down (Plugins/Audio/Decibel): a full search would walk every plugin's Intermediate.
		return Roots.Where(Directory.Exists).Any(Root =>
			File.Exists(Path.Combine(Root, "Decibel", "Decibel.uplugin"))
			|| Directory.EnumerateDirectories(Root).Any(Folder => File.Exists(Path.Combine(Folder, "Decibel", "Decibel.uplugin"))));
	}
}
