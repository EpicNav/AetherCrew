// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioVideoWriter.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioVideoWriterTest,
	"HyperAIStudio.Playtest.VideoWriter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioVideoWriterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/VideoWriter.mp4"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	FHyperAIStudioVideoWriter Writer;
	FString Error;
	// An odd size rounds down to even, as H.264 needs.
	if (!TestTrue(TEXT("the writer opens: ") + Error, Writer.Open(Path, 321, 181, 15, Error)))
	{
		return true;
	}
	TestEqual(TEXT("width rounds to even"), Writer.GetWidth(), 320);
	TestEqual(TEXT("height rounds to even"), Writer.GetHeight(), 180);
	TArray<FColor> Frame;
	Frame.SetNumUninitialized(Writer.GetWidth() * Writer.GetHeight());
	for (int32 Index = 0; Index < 30; ++Index)
	{
		// A bar sweeping across a gradient, so the encoder has motion to code.
		for (int32 Y = 0; Y < Writer.GetHeight(); ++Y)
		{
			for (int32 X = 0; X < Writer.GetWidth(); ++X)
			{
				const bool bBar = FMath::Abs(X - Index * 10) < 8;
				Frame[Y * Writer.GetWidth() + X] = bBar ? FColor::White : FColor(X * 255 / Writer.GetWidth(), Y * 255 / Writer.GetHeight(), 96);
			}
		}
		if (!TestTrue(TEXT("a frame writes: ") + Error, Writer.WriteFrame(Frame, Error)))
		{
			return true;
		}
	}
	TestFalse(TEXT("a frame of the wrong size is refused"), Writer.WriteFrame(TArray<FColor>({FColor::Black}), Error));
	TestTrue(TEXT("the file finishes: ") + Error, Writer.Close(Error));
	TArray<uint8> Bytes;
	TestTrue(TEXT("the video is on disk"), FFileHelper::LoadFileToArray(Bytes, *Path));
	TestTrue(TEXT("with some substance"), Bytes.Num() > 1024);
	TestTrue(TEXT("as an MP4 (ftyp box first)"), Bytes.Num() > 8 && FMemory::Memcmp(Bytes.GetData() + 4, "ftyp", 4) == 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
