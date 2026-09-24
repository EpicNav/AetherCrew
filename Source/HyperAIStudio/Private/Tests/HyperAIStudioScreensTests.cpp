// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioPreviewPanel.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioWidgetShot.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioPreviewPanel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/StrongObjectPtr.h"

namespace HyperAIStudio::ScreensTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** Calls a tool the way an agent does, through Epic's toolset registry, and waits for its JSON answer. */
	TSharedPtr<FJsonObject> CallTool(const FString& Toolset, const FString& Tool, const FString& Json, FString& OutError)
	{
		const TStrongObjectPtr<UToolCallAsyncResultString> Result(UToolsetRegistry::ExecuteTool(Toolset, Tool, Json));
		const double End = FPlatformTime::Seconds() + 30.0;
		while (Result.IsValid() && !Result->bIsComplete && FPlatformTime::Seconds() < End)
		{
			FTSTicker::GetCoreTicker().Tick(0.02f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		}
		if (!Result.IsValid() || !Result->bIsComplete || !Result->Error.IsEmpty())
		{
			OutError = Result.IsValid() ? (Result->Error.IsEmpty() ? FString(TEXT("the call did not finish")) : Result->Error) : FString(TEXT("no result"));
			return nullptr;
		}
		// The registry answers {"returnValue": <the tool's report>}.
		TSharedPtr<FJsonObject> Object;
		const TSharedPtr<FJsonObject>* Report = nullptr;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result->Value), Object) && Object.IsValid()
			&& Object->TryGetObjectField(TEXT("returnValue"), Report) && Report ? *Report : Object;
	}

	FString Quoted(const FString& Path)
	{
		return FString::Printf(TEXT("\"%s\""), *Path.Replace(TEXT("\\"), TEXT("/")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioScreensImageMatchTest,
	"HyperAIStudio.Screens.ImageMatchPreview",
	HyperAIStudio::ScreensTests::Flags)

bool FHyperAIStudioScreensImageMatchTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ScreensTests;
	if (!FHyperAIStudioPreviewPanel::FindSource(FName(TEXT("image_match"))))
	{
		AddWarning(TEXT("The lighting pack is not loaded, so there is no Image match source."));
		return true;
	}
	// Six attempts at a dusk reference, each nearer: cold and flat to begin with, warm by the end.
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("ScreensMatch"));
	const FString Reference = Folder / TEXT("reference.png");
	HyperAIStudio::Tests::WriteScenePng(Reference, FIntPoint(480, 300));
	const FString SessionId = TEXT("screens_demo");
	IFileManager::Get().DeleteDirectory(*(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Match") / SessionId), false, true);
	const TCHAR* Notes[] = {TEXT("first pass: noon light"), TEXT("sun lowered to 8 degrees"), TEXT("sun temperature 3800 K"),
		TEXT("sky light warmed, fog added"), TEXT("exposure locked at EV 11"), TEXT("sea darkened")};
	double LastScore = 0.0;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Notes); ++Index)
	{
		const float T = Index / 5.f;
		const FString Attempt = Folder / FString::Printf(TEXT("attempt_%d.png"), Index);
		HyperAIStudio::Tests::WriteScenePng(Attempt, FIntPoint(480, 300),
			FMath::Lerp(FLinearColor(0.55f, 0.7f, 0.95f), FLinearColor(0.95f, 0.45f, 0.18f), T),
			FMath::Lerp(FLinearColor(0.85f, 0.88f, 0.92f), FLinearColor(0.25f, 0.12f, 0.35f), T));
		const FString Json = FString::Printf(TEXT("{\"request\": {\"referenceImagePath\": %s, \"candidateImagePath\": %s, \"mode\": \"look\", ")
			TEXT("\"targetScore\": 85, \"sessionId\": \"%s\", \"note\": \"%s\", \"grid\": 4}}"),
			*Quoted(Reference), *Quoted(Attempt), *SessionId, Notes[Index]);
		FString Error;
		const TSharedPtr<FJsonObject> Report = CallTool(TEXT("HyperAIStudioLighting.HyperAIStudioLightingToolset"), TEXT("hyper_image_compare"), Json, Error);
		if (!TestTrue(TEXT("hyper_image_compare answers through the registry: ") + Error, Report.IsValid()))
		{
			return false;
		}
		double Score = 0.0;
		Report->TryGetNumberField(TEXT("score"), Score);
		FString Status;
		Report->TryGetStringField(TEXT("status"), Status);
		AddInfo(FString::Printf(TEXT("Iteration %d (%s): score %.0f, %s"), Index, Notes[Index], Score, *Status));
		TestTrue(FString::Printf(TEXT("each nearer attempt scores higher (%.0f after %.0f)"), Score, LastScore), Index == 0 || Score > LastScore);
		LastScore = Score;
	}

	// Opening a reference remembers it among the recents; this test's does not belong there.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const TArray<FString> Recents = Settings->RecentPreviewReferences;
	ON_SCOPE_EXIT
	{
		Settings->RecentPreviewReferences = Recents;
		Settings->SaveConfig();
	};
	const TSharedRef<SHyperAIStudioPreviewPanel> Panel = SNew(SHyperAIStudioPreviewPanel);
	FHyperAIStudioPreviewPanel::OnOpenRequested().Broadcast(FName(TEXT("image_match")), Reference);
	const FString Shot = HyperAIStudio::Tests::SaveWidgetShot(Panel, FIntPoint(1100, 900), TEXT("step11-preview-image-match"));
	if (Shot.IsEmpty())
	{
		AddWarning(TEXT("This editor cannot render, so no screenshot was taken."));
	}
	else
	{
		AddInfo(TEXT("Screenshot: ") + Shot);
	}
	return true;
}

#endif
