// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FHyperAIStudioRuntimeToolBinding;

/** A blank in a preset's prompt, written {{Name}}: filled from the picker, else the selection, else Default. */
struct FHyperAIStudioPromptPlaceholder
{
	FString Name;
	FString Label;
	FString Default;
	/** One line on what to put there, shown beside the field. */
	FString Hint;
};

/**
 * One prompt from the library: a tested request for one kind of work, and the packs, skills and project agent it goes
 * with, so the message arrives at the right tools without the task router guessing from its words.
 */
struct FHyperAIStudioPromptPreset
{
	/** "<category>.<kebab>", unique in the library. */
	FString Id;
	FString Category;
	FString Title;
	FString Summary;
	/** A project agent's name, "team", or empty for the chat's own agent. */
	FString Agent;
	/** English, as skills and agent files are: it is written for the agent. */
	FString Prompt;
	/** starter, standard or advanced. */
	FString Difficulty;
	/** The file it came from, for errors. */
	FString SourceFile;
	/** Catalog pack ids, the main one first. */
	TArray<FString> Packs;
	TArray<FString> Skills;
	/** The hyper_* tools the prompt names. */
	TArray<FString> Tools;
	/** Plan op kinds the prompt asks for; only the offline checker can check them. */
	TArray<FString> Ops;
	TArray<FString> Tags;
	/** What the preset works on: actors, assets or none; empty when it says nothing. */
	FString SelectionKind;
	/** The selection is of one of these classes, or a class made from one ("Landscape", "PCGVolume"). */
	TArray<FString> SelectionClasses;
	/** How many such things it needs selected. */
	int32 SelectionMin = 0;
	TArray<FHyperAIStudioPromptPlaceholder> Placeholders;
	/** Shipped with the plugin, so its text is translated; a project's own preset shows as written. */
	bool bBuiltIn = true;
	/** A project file's way to take a built-in preset out of the library. */
	bool bHidden = false;

	FText DisplayTitle() const;
	FText DisplaySummary() const;
	FText DisplayLabel(const FHyperAIStudioPromptPlaceholder& Placeholder) const;
	FText DisplayHint(const FHyperAIStudioPromptPlaceholder& Placeholder) const;
};

/** A kind of work the library groups presets by, as the picker lists them. */
struct FHyperAIStudioPromptCategory
{
	FString Id;
	FString Title;
	/** The task route (a default route's category) a new chat started from one of its presets takes; empty for none. */
	FString Route;
	FName Icon;
	int32 Order = 0;
	/** How many presets the shipped library holds for it at least. */
	int32 Min = 0;
	bool bBuiltIn = true;

	FText DisplayTitle() const;
};

/**
 * The prompt library: Resources/Prompts/<category>.json in the plugin, with the project's own presets in
 * .hyperai/prompts/*.json, which add presets, replace a built-in one by its id or hide it. Placeholders are {{name}},
 * so the JSON plans prompts often hold stay as written. Titles, summaries and labels are translated under the
 * HyperAIStudioPrompts namespace, which Tools/Localization/gather.py keeps; prompt bodies stay English. Game thread only.
 */
namespace HyperAIStudio::PromptPresets
{
	/** Packs every preset may use tools from, whatever its own packs are. */
	extern const TCHAR* const FoundationPacks[4];
	/** The {{tokens}} filled from the editor rather than declared by a preset. */
	extern const TCHAR* const ContextTokens[7];

	struct FLibrary
	{
		/** By Order. */
		TArray<FHyperAIStudioPromptCategory> Categories;
		/** By category, then as their files list them. */
		TArray<FHyperAIStudioPromptPreset> Presets;
		/** Each names its file, and the preset's index, id and field when it is about one. */
		TArray<FString> Errors;
		/** What a project file says that HyperAI ignores, such as a field it does not know. */
		TArray<FString> Warnings;

		const FHyperAIStudioPromptPreset* Find(const FString& Id) const;
		const FHyperAIStudioPromptCategory* FindCategory(const FString& Id) const;
	};

	/** What a preset is filled from: the editor's selection, level and Content Browser folder when the picker opened. */
	struct FPromptContext
	{
		TArray<FString> ActorLabels;
		TArray<FString> ActorPaths;
		/** Per selected actor, its class and each parent class up to Actor, comma-separated: "Landscape,LandscapeProxy,...,Actor". */
		TArray<FString> ActorClasses;
		TArray<FString> AssetPaths;
		/** Per selected asset, its class and each parent class it has, comma-separated, as for actors. */
		TArray<FString> AssetClasses;
		/** The current map's package, such as /Game/Maps/Arena. */
		FString Level;
		/** The Content Browser's current folder, else the first selected asset's, else /Game/HyperAI. */
		FString ContentFolder;
		FString Project;
	};

	/** The plugin's Resources/Prompts. */
	FString BuiltInDirectory();
	/** <Root>/.hyperai/prompts. */
	FString ProjectDirectory(const FString& Root);

	/** Reads a categories.json; adds its categories, or errors that name SourceName. False when it is not one at all. */
	bool ParseCategories(const FString& JsonText, const FString& SourceName, bool bBuiltIn, FLibrary& InOut);
	/**
	 * Reads one <category>.json and adds its presets as they are written. Unknown fields are errors in a built-in file
	 * and warnings in a project's. False when the file is not a preset file at all.
	 */
	bool ParseFile(const FString& JsonText, const FString& SourceName, bool bBuiltIn, FLibrary& InOut);
	/**
	 * The library from a built-in folder and a project folder (either may be missing): the project's presets replace
	 * built-ins with their id, "hidden" ones take them out, and new category ids add categories. A broken project file
	 * adds an error and nothing else; the built-ins always load.
	 */
	FLibrary Load(const FString& BuiltInFolder, const FString& ProjectFolder);
	/** Load for the project, kept until a file in either folder is added, removed or changes its time stamp. */
	const FLibrary& Get(const FString& ProjectRoot);
#if WITH_DEV_AUTOMATION_TESTS
	/** Reads built-in presets from Directory instead of the plugin's; empty restores it. */
	void SetBuiltInDirectoryForTests(const FString& Directory);
#endif

	/**
	 * Everything wrong with a library against what the plugin has: the schema rules, each pack, tool, skill, agent and
	 * route a preset or category names, and the {{placeholders}} of each prompt. A pure function, so tests can feed it.
	 */
	TArray<FString> Validate(const FLibrary& Library, const TSet<FString>& KnownPacks, const TMap<FString, FString>& PackOfTool,
		const TSet<FString>& KnownSkills, const TSet<FString>& KnownAgents, const TSet<FString>& KnownRoutes);
	/** Validate against the compiled catalog, the plugin's skills and team, and the default task routes. */
	TArray<FString> ValidateInstalled(const FLibrary& Library);

	/** The selection, level, folder and project now. */
	FPromptContext CaptureContext();
	/**
	 * The preset's prompt with each {{name}} filled: from Values (typed in the picker), else the selection or level for a
	 * context token, else the placeholder's default (with bUseDefaults). Anything left becomes "[Label]", listed in
	 * OutBlanks. Braces that are not a {{name}}, as in a JSON plan, and {{names}} the preset does not know stay as written.
	 */
	FString Fill(const FHyperAIStudioPromptPreset& Preset, const FPromptContext& Context, const TMap<FString, FString>& Values,
		TArray<FString>& OutBlanks, bool bUseDefaults = true);
	/** The value a context token takes from Context; empty when the editor has none. */
	FString ContextValue(const FString& Token, const FPromptContext& Context);
	/** How a context token's blank reads, such as "Selected actor". */
	FText TokenLabel(const FString& Token);
	/** The {{names}} the prompt uses, each once, in order. */
	TArray<FString> UsedNames(const FString& Prompt);
	/** The preset works on a selection, and enough of what it needs is selected. */
	bool FitsSelection(const FHyperAIStudioPromptPreset& Preset, const FPromptContext& Context);
	/**
	 * Indexes into Library.Presets that match Query, best first: memory's word search over title (twice), summary, tags
	 * and packs, in English and the shown language, with presets that fit the selection first among equals. An empty
	 * Query lists them in library order. CategoryId empty for all; Context may be null; at most Limit (0 for all).
	 */
	TArray<int32> Search(const FLibrary& Library, const FString& Query, const FString& CategoryId, const FPromptContext* Context, int32 Limit);

	/** The packs that have at least one tool enabled now. */
	TSet<FString> EnabledPacks(const TArray<FHyperAIStudioRuntimeToolBinding>& Bindings);
	/** The first of the preset's packs with no tool enabled, or empty when it can run. */
	FString MissingPack(const FHyperAIStudioPromptPreset& Preset, const TSet<FString>& Enabled);
	bool IsAvailable(const FHyperAIStudioPromptPreset& Preset, const TArray<FHyperAIStudioRuntimeToolBinding>& Bindings);

	/** Presets used last, newest first, at most 12 (kept per user). */
	TArray<FString> Recent();
	void NoteUsed(const FString& Id);
}
