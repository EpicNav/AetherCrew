// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * HyperAI's own text in the language chosen in its settings, independent of the editor's language. Translations are
 * Resources/Localization/<culture>.json files that Tools/Localization/gather.py keeps in step with the source.
 */
namespace HyperAIStudio::Localization
{
	struct FEntry
	{
		FString Namespace;
		FString Key;
		FString Source;
		FString Text;
	};

	/** Registers HyperAI's text source once. */
	void Startup();
	/** Applies the language setting now; HyperAI's text changes in place. */
	void Refresh();
	/** Cultures HyperAI has a translation file for, such as ja or zh-Hans. */
	TArray<FString> GetTranslatedCultures();
	/** The culture HyperAI shows, or empty for English. */
	FString ResolveCulture(const FString& Setting, TArrayView<const FString> EditorCultures);
	/** The translated entries of one file. */
	bool LoadEntries(const FString& File, TArray<FEntry>& OutEntries, FString& OutError);
#if WITH_DEV_AUTOMATION_TESTS
	/** Reads translation files from Directory instead of the plugin's; empty restores it. */
	void SetDirectoryForTests(const FString& Directory);
#endif
}
