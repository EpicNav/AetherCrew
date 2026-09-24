// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioWidgetShot.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "Styling/AppStyle.h"
#include "TextureResource.h"
#include "Widgets/Layout/SBorder.h"

namespace HyperAIStudio::Tests
{
	/** Where the UI screenshots go: Saved/HyperAIStudio/Screens. */
	FString ScreensFolder()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("Screens"));
	}

	/**
	 * Draws Widget off screen at Size on the editor's panel background and writes it as a PNG, for the step reports.
	 * It is drawn a few times first, so lists, text and pictures that arrive on first paint are all there. Returns
	 * the file, or empty when this editor cannot render.
	 */
	FString SaveWidgetShot(const TSharedRef<SWidget>& Widget, const FIntPoint Size, const FString& Name, const int32 Frames)
	{
		if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
		{
			return FString();
		}
		const TSharedRef<SWidget> Framed = SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(12.0f)
			[
				Widget
			];
		// Slate draws linear colour into a float target, and the read below encodes it as sRGB once, so the bytes
		// match what the editor shows on screen and dark greys keep their precision.
		FWidgetRenderer Renderer(/*bUseGammaCorrection=*/false, /*bInClearTarget=*/true);
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>();
		Target->ClearColor = FLinearColor::Transparent;
		Target->InitCustomFormat(Size.X, Size.Y, PF_FloatRGBA, /*bInForceLinearGamma=*/true);
		Target->UpdateResourceImmediate(true);
		Target->AddToRoot();
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			Renderer.DrawWidget(Target, Framed, FVector2D(Size), 1.0f / 30.0f);
			// SVG icons are rasterised when the editor's own renderer next draws its windows; until then they are blank
			// squares. Let it draw, so the frames after the first show them.
			FSlateApplication::Get().Tick();
			FlushRenderingCommands();
		}
		FReadSurfaceDataFlags Flags(RCM_UNorm);
		Flags.SetLinearToGamma(true);
		TArray<FColor> Pixels;
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		const bool bRead = Resource && Resource->ReadPixels(Pixels, Flags) && Pixels.Num() == Size.X * Size.Y;
		Target->RemoveFromRoot();
		if (!bRead)
		{
			return FString();
		}
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		IFileManager::Get().MakeDirectory(*ScreensFolder(), /*Tree=*/true);
		const FString File = ScreensFolder() / (Name + TEXT(".png"));
		return FFileHelper::SaveArrayToFile(Png, *File) ? File : FString();
	}

	/** A dusk-scene picture of Size, so screenshots show something that reads as an image, not a flat tile. */
	void WriteScenePng(const FString& Path, const FIntPoint Size, const FLinearColor Top, const FLinearColor Bottom)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size.X * Size.Y);
		const FVector2f Sun(Size.X * 0.68f, Size.Y * 0.42f);
		const float SunRadius = Size.Y * 0.12f;
		for (int32 Y = 0; Y < Size.Y; ++Y)
		{
			for (int32 X = 0; X < Size.X; ++X)
			{
				const float V = static_cast<float>(Y) / FMath::Max(1, Size.Y - 1);
				FLinearColor Color = FMath::Lerp(Top, Bottom, V);
				if (FVector2f::Distance(FVector2f(X, Y), Sun) < SunRadius && V < 0.62f)
				{
					Color = FLinearColor(1.0f, 0.85f, 0.55f);
				}
				if (V > 0.62f)
				{
					Color = FMath::Lerp(FLinearColor(0.08f, 0.10f, 0.22f), FLinearColor(0.02f, 0.03f, 0.08f), (V - 0.62f) / 0.38f);
				}
				// A lighthouse on the shore.
				if (X > Size.X * 0.22f && X < Size.X * 0.27f && V > 0.25f && V < 0.66f)
				{
					Color = ((Y / FMath::Max(1, Size.Y / 16)) % 2 == 0) ? FLinearColor(0.9f, 0.9f, 0.88f) : FLinearColor(0.7f, 0.1f, 0.1f);
				}
				// The colours above are written as sRGB already.
				Pixels[Y * Size.X + X] = Color.ToFColor(/*bSRGB=*/false);
			}
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		FFileHelper::SaveArrayToFile(Png, *Path);
	}
}

#endif
