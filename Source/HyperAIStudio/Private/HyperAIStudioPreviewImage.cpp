// Games by Hyper 2026.

#include "HyperAIStudioPreviewImage.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::Preview::Private
{
	/** A resource name Slate has not seen before, so a reloaded file replaces its predecessor. */
	FName MakeResourceName(const FString& Path, const FDateTime& Stamp)
	{
		return FName(*FString::Printf(TEXT("HyperAIPreview_%08x_%lld"), GetTypeHash(Path), Stamp.GetTicks()));
	}
}

const FSlateBrush* FHyperAIStudioPreviewImage::Get(const FString& Path)
{
	if (Path.IsEmpty())
	{
		Forget();
		return nullptr;
	}
	const FString Full = FPaths::ConvertRelativePathToFull(Path);
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Full);
	if (Stamp == FDateTime::MinValue())
	{
		Forget();
		Error = TEXT("The file is not there.");
		return nullptr;
	}
	if (Brush.IsValid() && Full == LoadedPath && Stamp == LoadedStamp)
	{
		return Brush.Get();
	}

	FImage Loaded;
	if (!FImageUtils::LoadImage(*Full, Loaded) || Loaded.SizeX <= 0 || Loaded.SizeY <= 0)
	{
		Forget();
		Error = TEXT("That file is not a readable image.");
		return nullptr;
	}
	SourceSize = FIntPoint(Loaded.SizeX, Loaded.SizeY);

	// Slate wants BGRA8, and a 4K capture would otherwise sit in graphics memory at full size for a thumbnail.
	FImage Display;
	const int32 LongestEdge = FMath::Max(Loaded.SizeX, Loaded.SizeY);
	if (LongestEdge > MaxEdgePixels)
	{
		const double Scale = static_cast<double>(MaxEdgePixels) / static_cast<double>(LongestEdge);
		Display.Init(FMath::Max(1, FMath::RoundToInt(Loaded.SizeX * Scale)),
			FMath::Max(1, FMath::RoundToInt(Loaded.SizeY * Scale)), ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FImageCore::ResizeImage(Loaded, Display);
	}
	else
	{
		Loaded.CopyTo(Display, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	}

	const TArrayView64<const FColor> Colors = Display.AsBGRA8();
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Colors.GetData()), Colors.Num() * sizeof(FColor));
	const TSharedPtr<FSlateDynamicImageBrush> Created = FSlateDynamicImageBrush::CreateWithImageData(
		HyperAIStudio::Preview::Private::MakeResourceName(Full, Stamp),
		FVector2D(Display.SizeX, Display.SizeY), Bytes);
	if (!Created.IsValid())
	{
		Forget();
		Error = TEXT("Slate could not take the image.");
		return nullptr;
	}
	Brush = Created;
	LoadedPath = Full;
	LoadedStamp = Stamp;
	Error.Reset();
	return Brush.Get();
}

void FHyperAIStudioPreviewImage::Forget()
{
	Brush.Reset();
	LoadedPath.Reset();
	LoadedStamp = FDateTime::MinValue();
	SourceSize = FIntPoint::ZeroValue;
	Error.Reset();
}
