// Games by Hyper 2026.

#include "HyperAIStudioPromptPresets.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserItemPath.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgents.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioChatHistoryPrefs.h"
#include "HyperAIStudioProjectMemory.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSkills.h"
#include "IContentBrowserSingleton.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioPromptPresets"

namespace HyperAIStudio::PromptPresets
{
	const TCHAR* const FoundationPacks[4] = {TEXT("shared_foundation"), TEXT("context_scene_project_search"), TEXT("diagnostics"), TEXT("pie_runtime")};
	const TCHAR* const ContextTokens[7] = {TEXT("selected_actors"), TEXT("selected_actor"), TEXT("selected_assets"), TEXT("selected_asset"), TEXT("level"),
		TEXT("content_folder"), TEXT("project")};

	namespace
	{
		/** The translations' namespace; gather.py writes the same one. */
		const TCHAR* const TextNamespace = TEXT("HyperAIStudioPrompts");
		const TCHAR* const PresetSchema = TEXT("hyperai.prompt-presets.v1");
		const TCHAR* const CategorySchema = TEXT("hyperai.prompt-categories.v1");
		const TCHAR* const CategoriesFile = TEXT("categories.json");
		const TCHAR* const DefaultContentFolder = TEXT("/Game/HyperAI");
		constexpr int32 MaxTitle = 60;
		constexpr int32 MaxSummary = 160;
		constexpr int32 MinPrompt = 80;
		constexpr int32 MaxPrompt = 4000;
		constexpr int32 MaxPacks = 3;
		constexpr int32 MaxSkills = 2;
		/** Project categories list after the plugin's. */
		constexpr int32 ProjectCategoryOrder = 100000;

		FText Localized(const FString& Key, const FString& Source, const bool bBuiltIn)
		{
			// The same namespace, key and English as the translation file's entry, so HyperAI's text source applies it.
			return bBuiltIn ? FText::AsLocalizable_Advanced(FTextKey(TextNamespace), FTextKey(Key), CopyTemp(Source)) : FText::FromString(Source);
		}

		bool IsLowerOrDigit(const TCHAR C)
		{
			return (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9'));
		}

		/** [a-z][a-z0-9_]* */
		bool IsIdentifier(const FString& Name)
		{
			if (Name.IsEmpty() || !(Name[0] >= TEXT('a') && Name[0] <= TEXT('z')))
			{
				return false;
			}
			for (const TCHAR C : Name)
			{
				if (!IsLowerOrDigit(C) && C != TEXT('_'))
				{
					return false;
				}
			}
			return true;
		}

		/** [a-z0-9-]+ */
		bool IsKebab(const FString& Part)
		{
			if (Part.IsEmpty())
			{
				return false;
			}
			for (const TCHAR C : Part)
			{
				if (!IsLowerOrDigit(C) && C != TEXT('-'))
				{
					return false;
				}
			}
			return true;
		}

		/** ^[a-z0-9-]+\.[a-z0-9-]+$ */
		bool IsValidId(const FString& Id)
		{
			FString Category;
			FString Name;
			return Id.Split(TEXT("."), &Category, &Name) && IsKebab(Category) && IsKebab(Name);
		}

		bool IsContextToken(const FString& Name)
		{
			for (const TCHAR* Token : ContextTokens)
			{
				if (Name == Token)
				{
					return true;
				}
			}
			return false;
		}

		bool IsFoundation(const FString& Pack)
		{
			for (const TCHAR* Each : FoundationPacks)
			{
				if (Pack == Each)
				{
					return true;
				}
			}
			return false;
		}

		/** What a {{...}} holds, for every pair with no brace between, as the offline checker's \{\{([^{}]*)\}\} finds them. */
		TArray<FString> BracedContents(const FString& Prompt)
		{
			TArray<FString> Found;
			int32 Index = 0;
			while (Index + 1 < Prompt.Len())
			{
				if (Prompt[Index] == TEXT('{') && Prompt[Index + 1] == TEXT('{'))
				{
					int32 End = Index + 2;
					while (End < Prompt.Len() && Prompt[End] != TEXT('{') && Prompt[End] != TEXT('}'))
					{
						++End;
					}
					if (End + 1 < Prompt.Len() && Prompt[End] == TEXT('}') && Prompt[End + 1] == TEXT('}'))
					{
						Found.Add(Prompt.Mid(Index + 2, End - Index - 2));
						Index = End + 2;
						continue;
					}
				}
				++Index;
			}
			return Found;
		}

		/** A "{{name" whose name runs on without its closing "}}", as a typo such as "{{biome}" leaves. */
		TArray<FString> UnclosedNames(const FString& Prompt)
		{
			TArray<FString> Found;
			for (int32 Index = 0; Index + 2 < Prompt.Len(); ++Index)
			{
				if (Prompt[Index] != TEXT('{') || Prompt[Index + 1] != TEXT('{') || !(Prompt[Index + 2] >= TEXT('a') && Prompt[Index + 2] <= TEXT('z')))
				{
					continue;
				}
				int32 End = Index + 2;
				while (End < Prompt.Len() && (IsLowerOrDigit(Prompt[End]) || Prompt[End] == TEXT('_')))
				{
					++End;
				}
				if (!(End + 1 < Prompt.Len() && Prompt[End] == TEXT('}') && Prompt[End + 1] == TEXT('}')))
				{
					Found.Add(Prompt.Mid(Index + 2, End - Index - 2));
				}
			}
			return Found;
		}

		/** Unreal's prefix-less class name compared with or without a leading A or U ("ALandscape" is "Landscape"). */
		bool SameClass(const FString& A, const FString& B)
		{
			auto Bare = [](const FString& Name)
			{
				return Name.Len() > 1 && (Name[0] == TEXT('A') || Name[0] == TEXT('U')) && FChar::IsUpper(Name[1]) ? Name.Mid(1) : Name;
			};
			return A.Equals(B, ESearchCase::IgnoreCase) || Bare(A).Equals(Bare(B), ESearchCase::IgnoreCase);
		}

		FString ClassChain(const UClass* Class, const UClass* Last)
		{
			TArray<FString> Names;
			for (; Class && Class != UObject::StaticClass(); Class = Class->GetSuperClass())
			{
				Names.Add(Class->GetName());
				if (Class == Last)
				{
					break;
				}
			}
			return FString::Join(Names, TEXT(","));
		}

		/** One file's problems, worded alike: "pcg-foliage.json[3] pcg-foliage.forest: title: ...". */
		struct FReport
		{
			FLibrary& Library;
			FString Source;
			bool bBuiltIn = true;

			void Error(const FString& Message) const
			{
				Library.Errors.Add(FString::Printf(TEXT("%s: %s"), *Source, *Message));
			}
			void At(const int32 Index, const FString& Id, const FString& Message, const bool bWarning = false) const
			{
				(bWarning ? Library.Warnings : Library.Errors).Add(FString::Printf(TEXT("%s[%d] %s: %s"), *Source, Index, *Id, *Message));
			}
			/** A field HyperAI does not know: an error in the plugin's own files, a warning in a project's. */
			void Unknown(const int32 Index, const FString& Id, const FString& Field) const
			{
				At(Index, Id, FString::Printf(TEXT("%s: unknown field"), *Field), /*bWarning=*/!bBuiltIn);
			}
		};

		bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& Out, FString& OutProblem)
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->Type != EJson::String)
			{
				OutProblem = FString::Printf(TEXT("%s: must be a string"), Field);
				return false;
			}
			Out = Value->AsString();
			return true;
		}

		bool ReadStrings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, TArray<FString>& Out, FString& OutProblem)
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(Field);
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->Type != EJson::Array)
			{
				OutProblem = FString::Printf(TEXT("%s: must be an array of strings"), Field);
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (!Item.IsValid() || Item->Type != EJson::String)
				{
					OutProblem = FString::Printf(TEXT("%s: must be an array of strings"), Field);
					return false;
				}
				Out.Add(Item->AsString());
			}
			return true;
		}

		bool ReadPreset(const TSharedPtr<FJsonObject>& Object, const int32 Index, const FString& Category, const FReport& Report, FHyperAIStudioPromptPreset& Out)
		{
			static const TSet<FString> Fields = {TEXT("id"), TEXT("title"), TEXT("summary"), TEXT("packs"), TEXT("skills"), TEXT("agent"), TEXT("tools"),
				TEXT("ops"), TEXT("difficulty"), TEXT("tags"), TEXT("context"), TEXT("placeholders"), TEXT("prompt"), TEXT("hidden")};
			Out.Category = Category;
			Out.SourceFile = Report.Source;
			Out.bBuiltIn = Report.bBuiltIn;
			FString Problem;
			if (!ReadString(Object, TEXT("id"), Out.Id, Problem) || Out.Id.IsEmpty())
			{
				Report.At(Index, Out.Id, Problem.IsEmpty() ? FString(TEXT("id: missing")) : Problem);
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (!Fields.Contains(Pair.Key) || (Pair.Key == TEXT("hidden") && Report.bBuiltIn))
				{
					Report.Unknown(Index, Out.Id, Pair.Key);
				}
			}
			bool bOk = true;
			auto Check = [&](const bool bRead)
			{
				if (!bRead)
				{
					Report.At(Index, Out.Id, Problem);
					bOk = false;
				}
			};
			Object->TryGetBoolField(TEXT("hidden"), Out.bHidden);
			if (Out.bHidden)
			{
				// A project hides a built-in preset by its id alone.
				return true;
			}
			Check(ReadString(Object, TEXT("title"), Out.Title, Problem));
			Check(ReadString(Object, TEXT("summary"), Out.Summary, Problem));
			Check(ReadString(Object, TEXT("prompt"), Out.Prompt, Problem));
			Check(ReadString(Object, TEXT("agent"), Out.Agent, Problem));
			Check(ReadString(Object, TEXT("difficulty"), Out.Difficulty, Problem));
			Check(ReadStrings(Object, TEXT("packs"), Out.Packs, Problem));
			Check(ReadStrings(Object, TEXT("skills"), Out.Skills, Problem));
			Check(ReadStrings(Object, TEXT("tools"), Out.Tools, Problem));
			Check(ReadStrings(Object, TEXT("ops"), Out.Ops, Problem));
			Check(ReadStrings(Object, TEXT("tags"), Out.Tags, Problem));
			for (const TCHAR* Required : {TEXT("title"), TEXT("summary"), TEXT("prompt")})
			{
				if (!Object->HasField(Required))
				{
					Report.At(Index, Out.Id, FString::Printf(TEXT("%s: missing"), Required));
					bOk = false;
				}
			}
			if (const TSharedPtr<FJsonValue> Context = Object->TryGetField(TEXT("context")); Context.IsValid())
			{
				const TSharedPtr<FJsonObject> Fit = Context->Type == EJson::Object ? Context->AsObject() : nullptr;
				if (!Fit.IsValid())
				{
					Report.At(Index, Out.Id, TEXT("context: must be an object"));
					bOk = false;
				}
				else
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Fit->Values)
					{
						if (Pair.Key != TEXT("selection") && Pair.Key != TEXT("classes") && Pair.Key != TEXT("min"))
						{
							Report.Unknown(Index, Out.Id, TEXT("context.") + Pair.Key);
						}
					}
					Check(ReadString(Fit, TEXT("selection"), Out.SelectionKind, Problem));
					Check(ReadStrings(Fit, TEXT("classes"), Out.SelectionClasses, Problem));
					Fit->TryGetNumberField(TEXT("min"), Out.SelectionMin);
				}
			}
			if (const TSharedPtr<FJsonValue> Blanks = Object->TryGetField(TEXT("placeholders")); Blanks.IsValid())
			{
				if (Blanks->Type != EJson::Array)
				{
					Report.At(Index, Out.Id, TEXT("placeholders: must be an array"));
					bOk = false;
				}
				else
				{
					for (const TSharedPtr<FJsonValue>& Item : Blanks->AsArray())
					{
						const TSharedPtr<FJsonObject> Blank = Item.IsValid() && Item->Type == EJson::Object ? Item->AsObject() : nullptr;
						if (!Blank.IsValid())
						{
							Report.At(Index, Out.Id, TEXT("placeholders: each must be an object"));
							bOk = false;
							continue;
						}
						FHyperAIStudioPromptPlaceholder& Placeholder = Out.Placeholders.AddDefaulted_GetRef();
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Blank->Values)
						{
							if (Pair.Key != TEXT("name") && Pair.Key != TEXT("label") && Pair.Key != TEXT("default") && Pair.Key != TEXT("hint"))
							{
								Report.Unknown(Index, Out.Id, TEXT("placeholders.") + Pair.Key);
							}
						}
						Check(ReadString(Blank, TEXT("name"), Placeholder.Name, Problem));
						Check(ReadString(Blank, TEXT("label"), Placeholder.Label, Problem));
						Check(ReadString(Blank, TEXT("default"), Placeholder.Default, Problem));
						Check(ReadString(Blank, TEXT("hint"), Placeholder.Hint, Problem));
					}
				}
			}
			return bOk;
		}

		FString ReadText(const FString& File)
		{
			FString Text;
			FFileHelper::LoadFileToString(Text, *File);
			return Text;
		}

		TArray<FString> JsonFiles(const FString& Folder)
		{
			TArray<FString> Files;
			if (!Folder.IsEmpty())
			{
				IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.json")), /*Files=*/true, /*Directories=*/false);
			}
			Files.Sort();
			return Files;
		}

		/** Reads a folder: its categories.json, then every other .json by name. */
		void LoadFolder(const FString& Folder, const bool bBuiltIn, FLibrary& Out)
		{
			const TArray<FString> Files = JsonFiles(Folder);
			if (Files.Contains(CategoriesFile))
			{
				ParseCategories(ReadText(FPaths::Combine(Folder, CategoriesFile)), CategoriesFile, bBuiltIn, Out);
			}
			else if (bBuiltIn)
			{
				Out.Errors.Add(FString::Printf(TEXT("%s: missing from %s"), CategoriesFile, *Folder));
			}
			for (const FString& File : Files)
			{
				if (File != CategoriesFile)
				{
					ParseFile(ReadText(FPaths::Combine(Folder, File)), File, bBuiltIn, Out);
				}
			}
			if (!bBuiltIn)
			{
				return;
			}
			for (const FHyperAIStudioPromptPreset& Preset : Out.Presets)
			{
				if (!Out.FindCategory(Preset.Category))
				{
					Out.Errors.Add(FString::Printf(TEXT("%s %s: category %s is not in %s"), *Preset.SourceFile, *Preset.Id, *Preset.Category, CategoriesFile));
				}
			}
		}

		/** The project's presets over the plugin's: a replacement keeps the built-in's place, a hidden one leaves. */
		void Merge(const FLibrary& Project, FLibrary& Library)
		{
			for (const FHyperAIStudioPromptCategory& Category : Project.Categories)
			{
				if (!Library.FindCategory(Category.Id))
				{
					Library.Categories.Add(Category);
				}
			}
			for (const FHyperAIStudioPromptPreset& Preset : Project.Presets)
			{
				const int32 Existing = Library.Presets.IndexOfByPredicate([&Preset](const FHyperAIStudioPromptPreset& Each) { return Each.Id == Preset.Id; });
				if (Preset.bHidden)
				{
					if (Existing == INDEX_NONE)
					{
						Library.Warnings.Add(FString::Printf(TEXT("%s %s: hides a preset the library does not have"), *Preset.SourceFile, *Preset.Id));
					}
					else
					{
						Library.Presets.RemoveAt(Existing);
					}
					continue;
				}
				if (Existing == INDEX_NONE)
				{
					Library.Presets.Add(Preset);
					continue;
				}
				const FString Category = Library.Presets[Existing].Category;
				Library.Presets[Existing] = Preset;
				Library.Presets[Existing].Category = Category;
			}
		}

		FString& BuiltInDirectoryForTests()
		{
			static FString Directory;
			return Directory;
		}

		struct FCache
		{
			FString BuiltIn;
			FString Project;
			/** Every file's name, time stamp and size, so an edit, a new file or a removed one loads the library again. */
			FString Stamp;
			bool bLoaded = false;
			FLibrary Library;
		};

		FCache& Cache()
		{
			static FCache Instance;
			return Instance;
		}

		FString StampOf(const FString& Folder)
		{
			FString Stamp;
			for (const FString& File : JsonFiles(Folder))
			{
				const FString Path = FPaths::Combine(Folder, File);
				Stamp += FString::Printf(TEXT("%s@%lld#%lld;"), *File, IFileManager::Get().GetTimeStamp(*Path).GetTicks(), IFileManager::Get().FileSize(*Path));
			}
			return Stamp;
		}

		const TMap<FString, FString>& CatalogPackOfTool()
		{
			static const TMap<FString, FString> PackOfTool = []()
			{
				TMap<FString, FString> Map;
				for (const FHyperAIStudioCapabilityToolDefinition& Tool : FHyperAIStudioCapabilityPackRegistry::GetCatalog().Tools)
				{
					Map.Add(Tool.Name, Tool.PackId);
				}
				return Map;
			}();
			return PackOfTool;
		}

		/** A preset's words for search: its title twice, its summary, tags, packs and id. */
		TArray<FString> PresetWords(const FHyperAIStudioPromptPreset& Preset, const bool bShownLanguage)
		{
			using namespace HyperAIStudio::Memory;
			const FString Title = bShownLanguage ? Preset.DisplayTitle().ToString() : Preset.Title;
			const FString Summary = bShownLanguage ? Preset.DisplaySummary().ToString() : Preset.Summary;
			TArray<FString> Words = SearchWords(Title);
			Words.Append(SearchWords(Title));
			Words.Append(SearchWords(Summary));
			if (!bShownLanguage)
			{
				Words.Append(SearchWords(FString::Join(Preset.Tags, TEXT(" "))));
				Words.Append(SearchWords(FString::Join(Preset.Packs, TEXT(" ")).Replace(TEXT("_"), TEXT(" "))));
				Words.Append(SearchWords(Preset.Id.Replace(TEXT("."), TEXT(" ")).Replace(TEXT("-"), TEXT(" "))));
			}
			return Words;
		}
	}

	const FHyperAIStudioPromptPreset* FLibrary::Find(const FString& Id) const
	{
		return Presets.FindByPredicate([&Id](const FHyperAIStudioPromptPreset& Each) { return Each.Id == Id; });
	}

	const FHyperAIStudioPromptCategory* FLibrary::FindCategory(const FString& Id) const
	{
		return Categories.FindByPredicate([&Id](const FHyperAIStudioPromptCategory& Each) { return Each.Id == Id; });
	}

	FString BuiltInDirectory()
	{
		if (!BuiltInDirectoryForTests().IsEmpty())
		{
			return BuiltInDirectoryForTests();
		}
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Prompts")) : FString();
	}

	FString ProjectDirectory(const FString& Root)
	{
		return FPaths::Combine(Root, TEXT(".hyperai"), TEXT("prompts"));
	}

	bool ParseCategories(const FString& JsonText, const FString& SourceName, const bool bBuiltIn, FLibrary& InOut)
	{
		const FReport Report{InOut, SourceName, bBuiltIn};
		TSharedPtr<FJsonObject> Root;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		FString Schema;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Root) || !Root.IsValid()
			|| !Root->TryGetStringField(TEXT("schema"), Schema) || Schema != CategorySchema || !Root->TryGetArrayField(TEXT("categories"), Items) || !Items)
		{
			Report.Error(FString::Printf(TEXT("not a category list: it needs \"schema\": \"%s\" and a categories array"), CategorySchema));
			return false;
		}
		for (int32 Index = 0; Index < Items->Num(); ++Index)
		{
			// AsObject on anything else logs an error of its own; a bad entry is this file's error alone.
			const TSharedPtr<FJsonObject> Object = (*Items)[Index].IsValid() && (*Items)[Index]->Type == EJson::Object ? (*Items)[Index]->AsObject() : nullptr;
			FHyperAIStudioPromptCategory Category;
			Category.bBuiltIn = bBuiltIn;
			FString Icon;
			if (!Object.IsValid() || !Object->TryGetStringField(TEXT("id"), Category.Id) || !IsKebab(Category.Id))
			{
				Report.At(Index, Category.Id, TEXT("id: must be lower-case words joined by hyphens"));
				continue;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (Pair.Key != TEXT("id") && Pair.Key != TEXT("title") && Pair.Key != TEXT("order") && Pair.Key != TEXT("icon") && Pair.Key != TEXT("min")
					&& Pair.Key != TEXT("route"))
				{
					Report.Unknown(Index, Category.Id, Pair.Key);
				}
			}
			Object->TryGetStringField(TEXT("title"), Category.Title);
			Object->TryGetStringField(TEXT("route"), Category.Route);
			Object->TryGetStringField(TEXT("icon"), Icon);
			Object->TryGetNumberField(TEXT("order"), Category.Order);
			Object->TryGetNumberField(TEXT("min"), Category.Min);
			Category.Icon = Icon.IsEmpty() ? FName(TEXT("HyperAIStudio.Icon.Tools")) : FName(*Icon);
			if (Category.Title.IsEmpty())
			{
				Report.At(Index, Category.Id, TEXT("title: missing"));
			}
			if (InOut.FindCategory(Category.Id))
			{
				Report.At(Index, Category.Id, TEXT("id: used twice"));
				continue;
			}
			InOut.Categories.Add(MoveTemp(Category));
		}
		return true;
	}

	bool ParseFile(const FString& JsonText, const FString& SourceName, const bool bBuiltIn, FLibrary& InOut)
	{
		const FReport Report{InOut, SourceName, bBuiltIn};
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Root) || !Root.IsValid())
		{
			Report.Error(TEXT("not valid JSON"));
			return false;
		}
		FString Schema;
		FString Category;
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		Root->TryGetStringField(TEXT("schema"), Schema);
		Root->TryGetStringField(TEXT("category"), Category);
		if (Schema != PresetSchema || !IsKebab(Category) || !Root->TryGetArrayField(TEXT("presets"), Items) || !Items)
		{
			Report.Error(FString::Printf(TEXT("not a preset file: it needs \"schema\": \"%s\", a category id and a presets array"), PresetSchema));
			return false;
		}
		if (bBuiltIn && SourceName.EndsWith(TEXT(".json")) && FPaths::GetBaseFilename(SourceName) != Category)
		{
			Report.Error(TEXT("category: must be the file's name"));
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
		{
			// A project file may title the category it adds.
			if (Pair.Key != TEXT("schema") && Pair.Key != TEXT("category") && Pair.Key != TEXT("presets") && (bBuiltIn || Pair.Key != TEXT("title")))
			{
				(bBuiltIn ? InOut.Errors : InOut.Warnings).Add(FString::Printf(TEXT("%s: %s: unknown field"), *SourceName, *Pair.Key));
			}
		}
		if (!bBuiltIn && !InOut.FindCategory(Category))
		{
			FHyperAIStudioPromptCategory& Added = InOut.Categories.AddDefaulted_GetRef();
			Added.Id = Category;
			Added.bBuiltIn = false;
			Added.Order = ProjectCategoryOrder + InOut.Categories.Num();
			Added.Icon = TEXT("HyperAIStudio.Icon.File");
			if (!Root->TryGetStringField(TEXT("title"), Added.Title) || Added.Title.IsEmpty())
			{
				Added.Title = Category;
			}
		}
		for (int32 Index = 0; Index < Items->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Object = (*Items)[Index].IsValid() && (*Items)[Index]->Type == EJson::Object ? (*Items)[Index]->AsObject() : nullptr;
			if (!Object.IsValid())
			{
				Report.At(Index, FString(), TEXT("each preset must be an object"));
				continue;
			}
			FHyperAIStudioPromptPreset Preset;
			if (!ReadPreset(Object, Index, Category, Report, Preset))
			{
				continue;
			}
			if (!IsValidId(Preset.Id) || (bBuiltIn && !Preset.Id.StartsWith(Category + TEXT("."))))
			{
				Report.At(Index, Preset.Id, bBuiltIn ? TEXT("id: must be '<category>.<lower-case-words>' with this file's category")
					: TEXT("id: must be '<category>.<lower-case-words>'"));
				continue;
			}
			if (InOut.Find(Preset.Id))
			{
				Report.At(Index, Preset.Id, TEXT("id: used twice"));
				continue;
			}
			InOut.Presets.Add(MoveTemp(Preset));
		}
		return true;
	}

	FLibrary Load(const FString& BuiltInFolder, const FString& ProjectFolder)
	{
		FLibrary Library;
		if (!BuiltInFolder.IsEmpty())
		{
			LoadFolder(BuiltInFolder, /*bBuiltIn=*/true, Library);
		}
		if (!ProjectFolder.IsEmpty() && IFileManager::Get().DirectoryExists(*ProjectFolder))
		{
			FLibrary Project;
			LoadFolder(ProjectFolder, /*bBuiltIn=*/false, Project);
			for (const FString& Error : Project.Errors)
			{
				Library.Errors.Add(TEXT(".hyperai/prompts/") + Error);
			}
			for (const FString& Warning : Project.Warnings)
			{
				Library.Warnings.Add(TEXT(".hyperai/prompts/") + Warning);
			}
			Merge(Project, Library);
		}
		Library.Categories.StableSort([](const FHyperAIStudioPromptCategory& A, const FHyperAIStudioPromptCategory& B) { return A.Order < B.Order; });
		TMap<FString, int32> Place;
		for (int32 Index = 0; Index < Library.Categories.Num(); ++Index)
		{
			Place.Add(Library.Categories[Index].Id, Index);
		}
		Library.Presets.StableSort([&Place](const FHyperAIStudioPromptPreset& A, const FHyperAIStudioPromptPreset& B)
		{
			const int32* PlaceA = Place.Find(A.Category);
			const int32* PlaceB = Place.Find(B.Category);
			return (PlaceA ? *PlaceA : MAX_int32) < (PlaceB ? *PlaceB : MAX_int32);
		});
		return Library;
	}

	const FLibrary& Get(const FString& ProjectRoot)
	{
		check(IsInGameThread());
		FCache& Cached = Cache();
		const FString BuiltIn = BuiltInDirectory();
		const FString Project = ProjectRoot.IsEmpty() ? FString() : ProjectDirectory(ProjectRoot);
		const FString Stamp = StampOf(BuiltIn) + TEXT("|") + StampOf(Project);
		if (!Cached.bLoaded || Cached.BuiltIn != BuiltIn || Cached.Project != Project || Cached.Stamp != Stamp)
		{
			Cached.Library = Load(BuiltIn, Project);
			Cached.BuiltIn = BuiltIn;
			Cached.Project = Project;
			Cached.Stamp = Stamp;
			Cached.bLoaded = true;
		}
		return Cached.Library;
	}

#if WITH_DEV_AUTOMATION_TESTS
	void SetBuiltInDirectoryForTests(const FString& Directory)
	{
		BuiltInDirectoryForTests() = Directory;
	}
#endif

	TArray<FString> Validate(const FLibrary& Library, const TSet<FString>& KnownPacks, const TMap<FString, FString>& PackOfTool,
		const TSet<FString>& KnownSkills, const TSet<FString>& KnownAgents, const TSet<FString>& KnownRoutes)
	{
		TArray<FString> Errors;
		for (const FHyperAIStudioPromptCategory& Category : Library.Categories)
		{
			if (!Category.Route.IsEmpty() && !KnownRoutes.Contains(Category.Route))
			{
				Errors.Add(FString::Printf(TEXT("category %s: route '%s' is not a task route"), *Category.Id, *Category.Route));
			}
			if (Category.Title.IsEmpty() || Category.Title.Len() > MaxTitle)
			{
				Errors.Add(FString::Printf(TEXT("category %s: title must be 1-%d characters"), *Category.Id, MaxTitle));
			}
		}
		TSet<FString> Ids;
		for (const FHyperAIStudioPromptPreset& Preset : Library.Presets)
		{
			auto Problem = [&Errors, &Preset](const FString& Message) { Errors.Add(FString::Printf(TEXT("%s %s: %s"), *Preset.SourceFile, *Preset.Id, *Message)); };
			if (!Library.FindCategory(Preset.Category))
			{
				Problem(FString::Printf(TEXT("category %s does not exist"), *Preset.Category));
			}
			if (!IsValidId(Preset.Id) || (Preset.bBuiltIn && !Preset.Id.StartsWith(Preset.Category + TEXT("."))))
			{
				Problem(TEXT("id must be '<category>.<lower-case-words>'"));
			}
			bool bSeen = false;
			Ids.Add(Preset.Id, &bSeen);
			if (bSeen)
			{
				Problem(TEXT("id is used twice"));
			}
			if (Preset.Title.IsEmpty() || Preset.Title.Len() > MaxTitle)
			{
				Problem(FString::Printf(TEXT("title must be 1-%d characters"), MaxTitle));
			}
			if (Preset.Summary.IsEmpty() || Preset.Summary.Len() > MaxSummary || Preset.Summary.Contains(TEXT("\n")))
			{
				Problem(FString::Printf(TEXT("summary must be one line of 1-%d characters"), MaxSummary));
			}
			if (Preset.Prompt.Len() < MinPrompt || Preset.Prompt.Len() > MaxPrompt)
			{
				Problem(FString::Printf(TEXT("prompt must be %d-%d characters"), MinPrompt, MaxPrompt));
			}
			if (Preset.Difficulty != TEXT("starter") && Preset.Difficulty != TEXT("standard") && Preset.Difficulty != TEXT("advanced"))
			{
				Problem(TEXT("difficulty must be starter, standard or advanced"));
			}
			if (Preset.Packs.IsEmpty() || Preset.Packs.Num() > MaxPacks)
			{
				Problem(FString::Printf(TEXT("packs must name 1-%d packs"), MaxPacks));
			}
			for (const FString& Pack : Preset.Packs)
			{
				if (!KnownPacks.Contains(Pack))
				{
					Problem(FString::Printf(TEXT("pack %s is not in the catalog"), *Pack));
				}
			}
			if (Preset.Skills.Num() > MaxSkills)
			{
				Problem(FString::Printf(TEXT("at most %d skills"), MaxSkills));
			}
			for (const FString& Skill : Preset.Skills)
			{
				if (!KnownSkills.Contains(Skill))
				{
					Problem(FString::Printf(TEXT("skill %s is not shipped"), *Skill));
				}
			}
			if (!Preset.Agent.IsEmpty() && !KnownAgents.Contains(Preset.Agent))
			{
				Problem(FString::Printf(TEXT("agent %s is not one of the team's"), *Preset.Agent));
			}
			for (const FString& Tool : Preset.Tools)
			{
				const FString* Owner = PackOfTool.Find(Tool);
				if (!Owner)
				{
					Problem(FString::Printf(TEXT("tool %s is not in the catalog"), *Tool));
				}
				else if (!Preset.Packs.Contains(*Owner) && !IsFoundation(*Owner))
				{
					Problem(FString::Printf(TEXT("tool %s belongs to pack %s, which is not one of this preset's"), *Tool, **Owner));
				}
				if (!Preset.Prompt.Contains(Tool, ESearchCase::CaseSensitive))
				{
					Problem(FString::Printf(TEXT("tool %s is listed but the prompt does not name it"), *Tool));
				}
			}
			TSet<FString> Declared;
			for (const FHyperAIStudioPromptPlaceholder& Placeholder : Preset.Placeholders)
			{
				bool bTwice = false;
				Declared.Add(Placeholder.Name, &bTwice);
				if (!IsIdentifier(Placeholder.Name) || IsContextToken(Placeholder.Name) || bTwice)
				{
					Problem(FString::Printf(TEXT("placeholder '%s' must be a new lower-case name"), *Placeholder.Name));
				}
				if (Placeholder.Label.IsEmpty())
				{
					Problem(FString::Printf(TEXT("placeholder %s needs a label"), *Placeholder.Name));
				}
				if (!Preset.Prompt.Contains(TEXT("{{") + Placeholder.Name + TEXT("}}"), ESearchCase::CaseSensitive))
				{
					Problem(FString::Printf(TEXT("placeholder %s is declared but the prompt does not use it"), *Placeholder.Name));
				}
			}
			for (const FString& Used : BracedContents(Preset.Prompt))
			{
				if (!IsContextToken(Used) && !Declared.Contains(Used))
				{
					Problem(FString::Printf(TEXT("{{%s}} is neither a context token nor a declared placeholder"), *Used));
				}
			}
			for (const FString& Unclosed : UnclosedNames(Preset.Prompt))
			{
				Problem(FString::Printf(TEXT("{{%s has no closing }}"), *Unclosed));
			}
			if (!Preset.SelectionKind.IsEmpty() && Preset.SelectionKind != TEXT("actors") && Preset.SelectionKind != TEXT("assets")
				&& Preset.SelectionKind != TEXT("none"))
			{
				Problem(TEXT("context.selection must be actors, assets or none"));
			}
			if (Preset.SelectionMin < 0)
			{
				Problem(TEXT("context.min must not be negative"));
			}
		}
		return Errors;
	}

	TArray<FString> ValidateInstalled(const FLibrary& Library)
	{
		TSet<FString> Packs;
		for (const FHyperAIStudioCapabilityPackDefinition& Pack : FHyperAIStudioCapabilityPackRegistry::GetCatalog().Packs)
		{
			Packs.Add(Pack.Id);
		}
		TSet<FString> Skills;
		TArray<FString> Folders;
		for (const FString& Directory : {HyperAIStudio::Skills::BuiltInDirectory(), FPaths::Combine(Agents::TeamDirectory(), TEXT("skills"))})
		{
			Folders.Reset();
			IFileManager::Get().FindFiles(Folders, *FPaths::Combine(Directory, TEXT("*")), /*Files=*/false, /*Directories=*/true);
			Skills.Append(Folders);
		}
		TSet<FString> AgentNames = {TEXT("team")};
		TArray<FString> AgentFiles;
		IFileManager::Get().FindFiles(AgentFiles, *FPaths::Combine(Agents::TeamDirectory(), TEXT("agents"), TEXT("*.md")), /*Files=*/true, /*Directories=*/false);
		for (const FString& File : AgentFiles)
		{
			AgentNames.Add(FPaths::GetBaseFilename(File));
		}
		UHyperAIStudioSettings* Defaults = NewObject<UHyperAIStudioSettings>();
		Defaults->AgentTaskRoutes.Reset();
		Defaults->EnsureDefaultTaskRoutes();
		TSet<FString> Routes;
		for (const FHyperAIStudioAgentTaskRoute& Route : Defaults->AgentTaskRoutes)
		{
			Routes.Add(Route.Category);
		}
		return Validate(Library, Packs, CatalogPackOfTool(), Skills, AgentNames, Routes);
	}

	FPromptContext CaptureContext()
	{
		FPromptContext Context;
		Context.Project = FApp::GetProjectName();
		if (GEditor)
		{
			for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
			{
				if (const AActor* Actor = Cast<AActor>(*It))
				{
					Context.ActorLabels.Add(Actor->GetActorLabel());
					Context.ActorPaths.Add(Actor->GetPathName());
					Context.ActorClasses.Add(ClassChain(Actor->GetClass(), AActor::StaticClass()));
				}
			}
			if (const UWorld* World = GEditor->GetEditorWorldContext().World())
			{
				Context.Level = World->GetPackage()->GetName();
			}
		}
		if (FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
		{
			IContentBrowserSingleton& Browser = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get();
			TArray<FAssetData> Assets;
			Browser.GetSelectedAssets(Assets);
			for (const FAssetData& Asset : Assets)
			{
				Context.AssetPaths.Add(Asset.GetSoftObjectPath().ToString());
				const UClass* Class = Asset.GetClass();
				Context.AssetClasses.Add(Class ? ClassChain(Class, nullptr) : Asset.AssetClassPath.GetAssetName().ToString());
			}
			Context.ContentFolder = Browser.GetCurrentPath().GetInternalPathString();
			if (Context.ContentFolder.IsEmpty() && !Assets.IsEmpty())
			{
				Context.ContentFolder = FPackageName::GetLongPackagePath(Assets[0].PackageName.ToString());
			}
		}
		if (Context.ContentFolder.IsEmpty())
		{
			Context.ContentFolder = DefaultContentFolder;
		}
		return Context;
	}

	FString ContextValue(const FString& Token, const FPromptContext& Context)
	{
		if (Token == TEXT("selected_actors") || Token == TEXT("selected_actor"))
		{
			TArray<FString> Lines;
			for (int32 Index = 0; Index < Context.ActorPaths.Num(); ++Index)
			{
				const FString Label = Context.ActorLabels.IsValidIndex(Index) ? Context.ActorLabels[Index] : FString();
				Lines.Add(Label.IsEmpty() ? Context.ActorPaths[Index] : FString::Printf(TEXT("%s (%s)"), *Label, *Context.ActorPaths[Index]));
			}
			return Lines.IsEmpty() ? FString() : Token == TEXT("selected_actor") ? Lines[0] : FString::Join(Lines, TEXT("\n"));
		}
		if (Token == TEXT("selected_assets"))
		{
			return FString::Join(Context.AssetPaths, TEXT("\n"));
		}
		if (Token == TEXT("selected_asset"))
		{
			return Context.AssetPaths.IsEmpty() ? FString() : Context.AssetPaths[0];
		}
		if (Token == TEXT("level"))
		{
			return Context.Level;
		}
		if (Token == TEXT("content_folder"))
		{
			return Context.ContentFolder.IsEmpty() ? FString(DefaultContentFolder) : Context.ContentFolder;
		}
		if (Token == TEXT("project"))
		{
			return Context.Project.IsEmpty() ? FString(FApp::GetProjectName()) : Context.Project;
		}
		return FString();
	}

	FText TokenLabel(const FString& Token)
	{
		const TPair<const TCHAR*, FText> Labels[] = {
			{TEXT("selected_actors"), LOCTEXT("TokenSelectedActors", "Selected actors")},
			{TEXT("selected_actor"), LOCTEXT("TokenSelectedActor", "Selected actor")},
			{TEXT("selected_assets"), LOCTEXT("TokenSelectedAssets", "Selected assets")},
			{TEXT("selected_asset"), LOCTEXT("TokenSelectedAsset", "Selected asset")},
			{TEXT("level"), LOCTEXT("TokenLevel", "Level")},
			{TEXT("content_folder"), LOCTEXT("TokenContentFolder", "Content folder")},
			{TEXT("project"), LOCTEXT("TokenProject", "Project")},
		};
		for (const TPair<const TCHAR*, FText>& Label : Labels)
		{
			if (Token == Label.Key)
			{
				return Label.Value;
			}
		}
		return FText::FromString(Token);
	}

	TArray<FString> UsedNames(const FString& Prompt)
	{
		TArray<FString> Names;
		for (const FString& Used : BracedContents(Prompt))
		{
			if (IsIdentifier(Used))
			{
				Names.AddUnique(Used);
			}
		}
		return Names;
	}

	FString Fill(const FHyperAIStudioPromptPreset& Preset, const FPromptContext& Context, const TMap<FString, FString>& Values,
		TArray<FString>& OutBlanks, const bool bUseDefaults)
	{
		OutBlanks.Reset();
		const FString& Prompt = Preset.Prompt;
		FString Out;
		Out.Reserve(Prompt.Len() + 256);
		int32 Index = 0;
		while (Index < Prompt.Len())
		{
			// Only {{name}}, so the braces of a JSON plan, {"Kind": "..."} or {{"X":1}}, stay as written.
			if (Index + 2 < Prompt.Len() && Prompt[Index] == TEXT('{') && Prompt[Index + 1] == TEXT('{') && Prompt[Index + 2] >= TEXT('a') && Prompt[Index + 2] <= TEXT('z'))
			{
				int32 End = Index + 2;
				while (End < Prompt.Len() && (IsLowerOrDigit(Prompt[End]) || Prompt[End] == TEXT('_')))
				{
					++End;
				}
				const FString Name = Prompt.Mid(Index + 2, End - Index - 2);
				const FHyperAIStudioPromptPlaceholder* Placeholder = Preset.Placeholders.FindByPredicate(
					[&Name](const FHyperAIStudioPromptPlaceholder& Each) { return Each.Name == Name; });
				const bool bToken = IsContextToken(Name);
				if (End + 1 < Prompt.Len() && Prompt[End] == TEXT('}') && Prompt[End + 1] == TEXT('}') && (Placeholder || bToken))
				{
					FString Value;
					if (const FString* Typed = Values.Find(Name); Typed && !Typed->TrimStartAndEnd().IsEmpty())
					{
						Value = *Typed;
					}
					else if (bToken)
					{
						Value = ContextValue(Name, Context);
					}
					if (Value.IsEmpty() && Placeholder && bUseDefaults)
					{
						Value = Placeholder->Default;
					}
					if (Value.IsEmpty())
					{
						const FString Label = Placeholder ? Preset.DisplayLabel(*Placeholder).ToString() : TokenLabel(Name).ToString();
						Value = TEXT("[") + Label + TEXT("]");
						OutBlanks.AddUnique(Label);
					}
					Out += Value;
					Index = End + 2;
					continue;
				}
			}
			Out.AppendChar(Prompt[Index]);
			++Index;
		}
		return Out;
	}

	bool FitsSelection(const FHyperAIStudioPromptPreset& Preset, const FPromptContext& Context)
	{
		const bool bActors = Preset.SelectionKind == TEXT("actors");
		if (!bActors && Preset.SelectionKind != TEXT("assets"))
		{
			return false;
		}
		const TArray<FString>& Classes = bActors ? Context.ActorClasses : Context.AssetClasses;
		const int32 Need = FMath::Max(1, Preset.SelectionMin);
		if (Preset.SelectionClasses.IsEmpty())
		{
			return (bActors ? Context.ActorPaths.Num() : Context.AssetPaths.Num()) >= Need;
		}
		int32 Matching = 0;
		for (const FString& Chain : Classes)
		{
			TArray<FString> Names;
			Chain.ParseIntoArray(Names, TEXT(","));
			Matching += Names.ContainsByPredicate([&Preset](const FString& Name)
			{
				return Preset.SelectionClasses.ContainsByPredicate([&Name](const FString& Wanted) { return SameClass(Name, Wanted); });
			}) ? 1 : 0;
		}
		return Matching >= Need;
	}

	TArray<int32> Search(const FLibrary& Library, const FString& Query, const FString& CategoryId, const FPromptContext* Context, const int32 Limit)
	{
		TArray<int32> Candidates;
		for (int32 Index = 0; Index < Library.Presets.Num(); ++Index)
		{
			const FHyperAIStudioPromptPreset& Preset = Library.Presets[Index];
			if (!Preset.bHidden && (CategoryId.IsEmpty() || Preset.Category == CategoryId))
			{
				Candidates.Add(Index);
			}
		}
		const FString Needle = Query.TrimStartAndEnd();
		if (Needle.IsEmpty())
		{
			if (Limit > 0 && Candidates.Num() > Limit)
			{
				Candidates.SetNum(Limit);
			}
			return Candidates;
		}
		// Memory's word search, as skills are found for a message; the shown language's words count when it is not English.
		const TArray<FString> Words = Memory::SearchWords(Needle);
		TArray<TArray<FString>> Documents;
		for (const int32 Index : Candidates)
		{
			const FHyperAIStudioPromptPreset& Preset = Library.Presets[Index];
			TArray<FString>& Document = Documents.Add_GetRef(PresetWords(Preset, /*bShownLanguage=*/false));
			if (Preset.bBuiltIn && !Preset.DisplayTitle().ToString().Equals(Preset.Title))
			{
				Document.Append(PresetWords(Preset, /*bShownLanguage=*/true));
			}
		}
		const TArray<Memory::FRank> Ranks = Memory::Rank(Words, Documents);
		TArray<TPair<int32, double>> Scored;
		for (int32 Each = 0; Each < Candidates.Num(); ++Each)
		{
			const FHyperAIStudioPromptPreset& Preset = Library.Presets[Candidates[Each]];
			double Score = Ranks.IsValidIndex(Each) && Ranks[Each].Matched > 0 ? Ranks[Each].Score : 0.0;
			// Text without spaces, such as a Chinese title, is matched as it is typed.
			if (Preset.DisplayTitle().ToString().Contains(Needle))
			{
				Score += 2.0;
			}
			else if (Preset.DisplaySummary().ToString().Contains(Needle))
			{
				Score += 1.0;
			}
			if (Score > 0.0 && Context && FitsSelection(Preset, *Context))
			{
				Score *= 1.25;
			}
			if (Score > 0.0)
			{
				Scored.Emplace(Candidates[Each], Score);
			}
		}
		Scored.StableSort([](const TPair<int32, double>& A, const TPair<int32, double>& B) { return A.Value > B.Value; });
		TArray<int32> Found;
		for (const TPair<int32, double>& Each : Scored)
		{
			if (Limit > 0 && Found.Num() >= Limit)
			{
				break;
			}
			Found.Add(Each.Key);
		}
		return Found;
	}

	TSet<FString> EnabledPacks(const TArray<FHyperAIStudioRuntimeToolBinding>& Bindings)
	{
		TSet<FString> Packs;
		for (const FHyperAIStudioRuntimeToolBinding& Binding : Bindings)
		{
			if (const FString* Pack = Binding.bToolEnabled ? CatalogPackOfTool().Find(Binding.ToolName) : nullptr)
			{
				Packs.Add(*Pack);
			}
		}
		return Packs;
	}

	FString MissingPack(const FHyperAIStudioPromptPreset& Preset, const TSet<FString>& Enabled)
	{
		for (const FString& Pack : Preset.Packs)
		{
			if (!Enabled.Contains(Pack))
			{
				return Pack;
			}
		}
		return FString();
	}

	bool IsAvailable(const FHyperAIStudioPromptPreset& Preset, const TArray<FHyperAIStudioRuntimeToolBinding>& Bindings)
	{
		return MissingPack(Preset, EnabledPacks(Bindings)).IsEmpty();
	}

	TArray<FString> Recent()
	{
		return UHyperAIStudioChatHistoryPrefs::GetRecentPromptPresets();
	}

	void NoteUsed(const FString& Id)
	{
		UHyperAIStudioChatHistoryPrefs::AddRecentPromptPreset(Id);
	}
}

FText FHyperAIStudioPromptPreset::DisplayTitle() const
{
	return HyperAIStudio::PromptPresets::Localized(Id + TEXT(".title"), Title, bBuiltIn);
}

FText FHyperAIStudioPromptPreset::DisplaySummary() const
{
	return HyperAIStudio::PromptPresets::Localized(Id + TEXT(".summary"), Summary, bBuiltIn);
}

FText FHyperAIStudioPromptPreset::DisplayLabel(const FHyperAIStudioPromptPlaceholder& Placeholder) const
{
	return HyperAIStudio::PromptPresets::Localized(FString::Printf(TEXT("%s.ph.%s.label"), *Id, *Placeholder.Name),
		Placeholder.Label.IsEmpty() ? Placeholder.Name : Placeholder.Label, bBuiltIn && !Placeholder.Label.IsEmpty());
}

FText FHyperAIStudioPromptPreset::DisplayHint(const FHyperAIStudioPromptPlaceholder& Placeholder) const
{
	return Placeholder.Hint.IsEmpty() ? FText::GetEmpty()
		: HyperAIStudio::PromptPresets::Localized(FString::Printf(TEXT("%s.ph.%s.hint"), *Id, *Placeholder.Name), Placeholder.Hint, bBuiltIn);
}

FText FHyperAIStudioPromptCategory::DisplayTitle() const
{
	return HyperAIStudio::PromptPresets::Localized(TEXT("category.") + Id, Title, bBuiltIn);
}

#undef LOCTEXT_NAMESPACE
