// Games by Hyper 2026.

#include "HyperAIStudioLevelSurvey.h"

#include "HAL/FileManager.h"
#include "HyperAIStudioVideoWriter.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

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
			// Not FImageUtils::LoadImage: windows.h renames it to LoadImageW in unity bundles, which does not link.
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Folder, Name)))
			{
				continue;
			}
			IImageWrapperModule& WrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			const EImageFormat Format = WrapperModule.DetectImageFormat(Bytes.GetData(), Bytes.Num());
			const TSharedPtr<IImageWrapper> Wrapper = Format == EImageFormat::Invalid ? nullptr : WrapperModule.CreateImageWrapper(Format);
			TArray64<uint8> Raw;
			if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num())
				|| !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
			{
				continue;
			}
			FImage Image;
			Image.Init(Wrapper->GetWidth(), Wrapper->GetHeight(), ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			FMemory::Memcpy(Image.RawData.GetData(), Raw.GetData(), FMath::Min<int64>(Raw.Num(), Image.RawData.Num()));
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
