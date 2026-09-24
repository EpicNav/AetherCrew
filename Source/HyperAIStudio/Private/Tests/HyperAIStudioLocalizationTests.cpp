// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HyperAIStudioLocalization.h"
#include "HyperAIStudioSettings.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLocalizationTest,
	"HyperAIStudio.Localization.OwnLanguage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLocalizationTest::RunTest(const FString& Parameters)
{
	namespace L10n = HyperAIStudio::Localization;
	const FString Directory = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
		TEXT("Localization-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Json = TEXT(R"JSON({"culture": "zz", "entries": [
		{"namespace": "HyperAIStudioLocalizationTests", "key": "Greeting", "source": "Hello there", "text": "Bonjour"},
		{"namespace": "HyperAIStudioLocalizationTests", "key": "Changed", "source": "The old English", "text": "Ancien"},
		{"namespace": "HyperAIStudioLocalizationTests", "key": "Untranslated", "source": "Not yet", "text": ""}]})JSON");
	TestTrue(TEXT("The translation file is written"),
		FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Directory, TEXT("zz.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	L10n::SetDirectoryForTests(Directory);

	TArray<L10n::FEntry> Entries;
	FString Error;
	TestTrue(TEXT("Translated entries load; empty ones are skipped"),
		L10n::LoadEntries(FPaths::Combine(Directory, TEXT("zz.json")), Entries, Error) && Entries.Num() == 2);
	TestTrue(TEXT("The file's culture is offered"), L10n::GetTranslatedCultures() == TArray<FString>{TEXT("zz")});
	const TArray<FString> English = {TEXT("en-US"), TEXT("en")};
	TestEqual(TEXT("en is English"), L10n::ResolveCulture(TEXT("en"), TArray<FString>{TEXT("zz")}), FString());
	TestEqual(TEXT("Editor follows a translated editor language"), L10n::ResolveCulture(TEXT("Editor"), TArray<FString>{TEXT("zz-YY"), TEXT("zz")}), FString(TEXT("zz")));
	TestEqual(TEXT("Editor in English shows English"), L10n::ResolveCulture(TEXT("Editor"), English), FString());
	TestEqual(TEXT("A chosen language does not depend on the editor's"), L10n::ResolveCulture(TEXT("zz"), English), FString(TEXT("zz")));
	TestEqual(TEXT("A language without a file shows English"), L10n::ResolveCulture(TEXT("fr"), English), FString());

	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const FString Saved = Settings->UILanguage;
	Settings->UILanguage = TEXT("zz");
	L10n::Refresh();
	// The engine reloads text on a task.
	FTextLocalizationManager::Get().WaitForAsyncTasks();
	TestEqual(TEXT("HyperAI text shows in its own language"),
		NSLOCTEXT("HyperAIStudioLocalizationTests", "Greeting", "Hello there").ToString(), FString(TEXT("Bonjour")));
	TestEqual(TEXT("A string whose English changed stays English until it is translated again"),
		NSLOCTEXT("HyperAIStudioLocalizationTests", "Changed", "The new English").ToString(), FString(TEXT("The new English")));
	TestEqual(TEXT("An untranslated string stays English"),
		NSLOCTEXT("HyperAIStudioLocalizationTests", "Untranslated", "Not yet").ToString(), FString(TEXT("Not yet")));

	Settings->UILanguage = Saved;
	L10n::SetDirectoryForTests(FString());
	L10n::Refresh();
	FTextLocalizationManager::Get().WaitForAsyncTasks();
	TestEqual(TEXT("Switching back shows English again"),
		NSLOCTEXT("HyperAIStudioLocalizationTests", "Greeting", "Hello there").ToString(), FString(TEXT("Hello there")));

	// The Simplified Chinese translation that ships with the plugin loads from the plugin's own folder.
	Settings->UILanguage = TEXT("zh-Hans");
	L10n::Refresh();
	FTextLocalizationManager::Get().WaitForAsyncTasks();
	TestEqual(TEXT("The shipped Chinese translation applies"),
		NSLOCTEXT("SHyperAIStudioQuickActionWindow", "StartAgent", "Start Agent").ToString(), FString(TEXT("\u542f\u52a8\u667a\u80fd\u4f53")));
	Settings->UILanguage = Saved;
	L10n::Refresh();
	FTextLocalizationManager::Get().WaitForAsyncTasks();
	TestEqual(TEXT("and English comes back"),
		NSLOCTEXT("SHyperAIStudioQuickActionWindow", "StartAgent", "Start Agent").ToString(), FString(TEXT("Start Agent")));
	IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists=*/false, /*Tree=*/true);
	return true;
}

#endif
