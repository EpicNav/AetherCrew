// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioToolGuide.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioToolGuideTest,
	"HyperAIStudio.NativeTools.ToolGuide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioToolGuideTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Guide = HyperAIStudio::ToolGuide::Build();
	const FString Epic = HyperAIStudio::ToolGuide::BuildEpic();
	// Kept beside the test log, for a look at what agents read.
	FFileHelper::SaveStringToFile(Guide, *(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/ToolGuide.md")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(Epic, *(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/Epic-ToolGuide.md")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// A guessed enum name fails the whole call before any tool runs, so the guide spells the names out.
	TestTrue(TEXT("an enum lists its names"), Guide.Contains(TEXT("teardown: enum(StopOwnedSession|LeaveSessionRunning)")));
	TestTrue(TEXT("nested shapes are listed once"), Guide.Contains(TEXT("\n## Shapes\n")) && Guide.Contains(TEXT("- `HyperAINiagaraEditOp` `{")));
	TestTrue(TEXT("and not their engine types"), !Guide.Contains(TEXT("- `Vector` `{")));
	TestTrue(TEXT("the wait tool is found by its toolset"), Guide.Contains(TEXT("\n## `HyperAIStudio.HyperAIStudioNativeReadToolset`\n")));

	TestTrue(TEXT("Epic's toolsets have their own guide"), Epic.StartsWith(TEXT("# Epic Toolsets")));
	TestFalse(TEXT("without HyperAI's"), Epic.Contains(TEXT("## `HyperAIStudio")));
	if (Epic.Contains(TEXT("## `editor_toolset.toolsets.scene.SceneTools`")))
	{
		TestTrue(TEXT("with each tool's fields"), Epic.Contains(TEXT("- `load_level` `{")));
	}
	else
	{
		AddInfo(TEXT("The editor toolsets are not enabled here, so their lines were not checked."));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
