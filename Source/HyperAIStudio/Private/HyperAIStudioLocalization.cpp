// Games by Hyper 2026.

#include "HyperAIStudioLocalization.h"

#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/ILocalizedTextSource.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Internationalization/TextLocalizationResource.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLocalization, Log, All);

namespace HyperAIStudio::Localization
{
	namespace
	{
		FCriticalSection& Lock()
		{
			static FCriticalSection Section;
			return Section;
		}

		/** The language setting, read on the game thread; localization can load on other threads. */
		FString& CachedSetting()
		{
			static FString Setting;
			return Setting;
		}

		FString& DirectoryForTests()
		{
			static FString Directory;
			return Directory;
		}

		FString TranslationDirectory()
		{
			{
				FScopeLock Guard(&Lock());
				if (!DirectoryForTests().IsEmpty())
				{
					return DirectoryForTests();
				}
			}
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
			return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Localization")) : FString();
		}

		FString ReadSetting()
		{
			const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
			return Settings ? Settings->UILanguage : FString();
		}

		TArray<FString> EditorCultures()
		{
			const FCultureRef Language = FInternationalization::Get().GetCurrentLanguage();
			return Language->GetPrioritizedParentCultureNames();
		}

		class FTextSource final : public ILocalizedTextSource
		{
		public:
			virtual int32 GetPriority() const override
			{
				return ELocalizedTextSourcePriority::Highest;
			}

			virtual bool GetNativeCultureName(const ELocalizedTextSourceCategory, FString&) override
			{
				return false;
			}

			virtual void GetLocalizedCultureNames(const ELocalizationLoadFlags, TSet<FString>& OutCultures) override
			{
				OutCultures.Append(GetTranslatedCultures());
			}

			virtual void LoadLocalizedResources(const ELocalizationLoadFlags, TArrayView<const FString> PrioritizedCultures,
				FTextLocalizationResource&, FTextLocalizationResource& LocalizedResource) override
			{
				FString Setting;
				{
					FScopeLock Guard(&Lock());
					Setting = CachedSetting();
				}
				const FString Culture = ResolveCulture(Setting, PrioritizedCultures);
				TArray<FEntry> Entries;
				FString Error;
				if (!Culture.IsEmpty() && !LoadEntries(FPaths::Combine(TranslationDirectory(), Culture + TEXT(".json")), Entries, Error))
				{
					UE_LOG(LogHyperAIStudioLocalization, Warning, TEXT("HyperAI's %s text did not load: %s"), *Culture, *Error);
				}
				// The manager applies an entry only while its source matches the text's, so an English string that changed
				// since it was translated shows in English until it is translated again.
				TSet<FString> Keys;
				for (const FEntry& Entry : Entries)
				{
					LocalizedResource.AddEntry(FTextKey(Entry.Namespace), FTextKey(Entry.Key), Entry.Source, Entry.Text, GetPriority());
					Keys.Add(Entry.Namespace + TEXT("/") + Entry.Key);
				}
				// The live table keeps an entry until something replaces it, so text translated before goes back to English.
				FScopeLock Guard(&Lock());
				for (const FEntry& Previous : Applied)
				{
					if (!Keys.Contains(Previous.Namespace + TEXT("/") + Previous.Key))
					{
						LocalizedResource.AddEntry(FTextKey(Previous.Namespace), FTextKey(Previous.Key), Previous.Source, Previous.Source, GetPriority());
					}
				}
				Applied = MoveTemp(Entries);
			}

		private:
			/** What the last load put in the live table. */
			TArray<FEntry> Applied;
		};
	}

	void Startup()
	{
		static bool bRegistered = false;
		if (bRegistered)
		{
			return;
		}
		bRegistered = true;
		const FString Setting = ReadSetting();
		{
			FScopeLock Guard(&Lock());
			CachedSetting() = Setting;
		}
		// Refreshing reloads every text in the editor, so only when HyperAI has something to show.
		const bool bTranslated = !ResolveCulture(Setting, EditorCultures()).IsEmpty();
		FTextLocalizationManager::Get().RegisterTextSource(MakeShared<FTextSource>(), bTranslated);
	}

	void Refresh()
	{
		const FString Setting = ReadSetting();
		{
			FScopeLock Guard(&Lock());
			CachedSetting() = Setting;
		}
		FTextLocalizationManager::Get().RefreshResources();
	}

	TArray<FString> GetTranslatedCultures()
	{
		TArray<FString> Files;
		const FString Directory = TranslationDirectory();
		if (!Directory.IsEmpty())
		{
			IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), /*Files=*/true, /*Directories=*/false);
		}
		TArray<FString> Cultures;
		for (const FString& File : Files)
		{
			Cultures.Add(FPaths::GetBaseFilename(File));
		}
		Cultures.Sort();
		return Cultures;
	}

	FString ResolveCulture(const FString& Setting, TArrayView<const FString> EditorCultures)
	{
		const TArray<FString> Translated = GetTranslatedCultures();
		auto Find = [&Translated](const FString& Culture)
		{
			const FString* Match = Translated.FindByPredicate([&Culture](const FString& Item) { return Item.Equals(Culture, ESearchCase::IgnoreCase); });
			return Match ? *Match : FString();
		};
		if (Setting.Equals(TEXT("en"), ESearchCase::IgnoreCase))
		{
			return FString();
		}
		if (Setting.IsEmpty() || Setting.Equals(TEXT("Editor"), ESearchCase::IgnoreCase))
		{
			// The editor's culture and its parents, most specific first: zh-Hans-CN, then zh-Hans, then zh.
			for (const FString& Culture : EditorCultures)
			{
				if (Culture.StartsWith(TEXT("en"), ESearchCase::IgnoreCase))
				{
					return FString();
				}
				const FString Match = Find(Culture);
				if (!Match.IsEmpty())
				{
					return Match;
				}
			}
			return FString();
		}
		return Find(Setting);
	}

	bool LoadEntries(const FString& File, TArray<FEntry>& OutEntries, FString& OutError)
	{
		OutEntries.Reset();
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *File))
		{
			OutError = FString::Printf(TEXT("%s could not be read."), *File);
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			OutError = FString::Printf(TEXT("%s is not valid JSON."), *File);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Root->TryGetArrayField(TEXT("entries"), Items) || !Items)
		{
			OutError = FString::Printf(TEXT("%s has no entries."), *File);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const TSharedPtr<FJsonObject> Object = Item.IsValid() ? Item->AsObject() : nullptr;
			FEntry Entry;
			if (Object.IsValid() && Object->TryGetStringField(TEXT("namespace"), Entry.Namespace) && Object->TryGetStringField(TEXT("key"), Entry.Key)
				&& Object->TryGetStringField(TEXT("source"), Entry.Source) && Object->TryGetStringField(TEXT("text"), Entry.Text)
				&& !Entry.Key.IsEmpty() && !Entry.Text.IsEmpty())
			{
				OutEntries.Add(MoveTemp(Entry));
			}
		}
		return true;
	}

#if WITH_DEV_AUTOMATION_TESTS
	void SetDirectoryForTests(const FString& Directory)
	{
		FScopeLock Guard(&Lock());
		DirectoryForTests() = Directory;
	}
#endif
}

TArray<FString> UHyperAIStudioSettings::GetLanguageOptions()
{
	TArray<FString> Options = {TEXT("Editor"), TEXT("en")};
	Options.Append(HyperAIStudio::Localization::GetTranslatedCultures());
	return Options;
}

#if WITH_EDITOR
void UHyperAIStudioSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UHyperAIStudioSettings, UILanguage))
	{
		HyperAIStudio::Localization::Refresh();
	}
}
#endif
