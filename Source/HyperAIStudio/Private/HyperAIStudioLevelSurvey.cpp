// Games by Hyper 2026.

#include "HyperAIStudioLevelSurvey.h"

#include "HAL/FileManager.h"
#include "HyperAIStudioVideoWriter.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::LevelSurvey
{
	FString WriteVideo(const FString& Folder, FString& OutError)
	{
		TArray<FString> Frames;
		IFileManager::Get().FindFiles(Frames, *FPaths::Combine(Folder, TEXT("frame_*.png")), /*Files=*/true, /*Directories=*/false);
		Frames.Sort();
		if (Frames.IsEmpty())
		{
			OutError = TEXT("the survey saved no frames");
			return FString();
		}
		const FString Path = FPaths::Combine(Folder, TEXT("survey.mp4"));
		FHyperAIStudioVideoWriter Writer;
		for (const FString& Name : Frames)
		{
			FImage Image;
			if (!FImageUtils::LoadImage(*FPaths::Combine(Folder, Name), Image))
			{
				continue;
			}
			FImage Bgra;
			Image.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			if (!Writer.IsOpen() && !Writer.Open(Path, Bgra.SizeX, Bgra.SizeY, VideoFps, OutError))
			{
				return FString();
			}
			if (Bgra.SizeX < Writer.GetWidth() || Bgra.SizeY < Writer.GetHeight())
			{
				continue;
			}
			// The writer takes an even size; an odd frame loses its last column or row.
			TArray<FColor> Pixels;
			Pixels.SetNumUninitialized(Writer.GetWidth() * Writer.GetHeight());
			const TArrayView64<FColor> Source = Bgra.AsBGRA8();
			for (int32 Y = 0; Y < Writer.GetHeight(); ++Y)
			{
				FMemory::Memcpy(&Pixels[Y * Writer.GetWidth()], &Source[static_cast<int64>(Y) * Bgra.SizeX], Writer.GetWidth() * sizeof(FColor));
			}
			if (!Writer.WriteFrame(Pixels, OutError))
			{
				return FString();
			}
		}
		if (!Writer.IsOpen() || !Writer.Close(OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("no frame could be read");
			}
			return FString();
		}
		return Path;
	}

	FHyperAIBotResult WithVideo(const EHyperAIBotAction Action, const FHyperAIBotResult& Result)
	{
		if (Action != EHyperAIBotAction::SurveyLevel || Result.State != FHyperAIBotResult::EState::Passed || Result.File.IsEmpty())
		{
			return Result;
		}
		FHyperAIBotResult Out = Result;
		FString Error;
		const FString Video = WriteVideo(Result.File, Error);
		Out.Detail += Video.IsEmpty() ? FString::Printf(TEXT("; no video: %s"), *Error) : FString::Printf(TEXT("; video %s"), *Video);
		return Out;
	}
}
