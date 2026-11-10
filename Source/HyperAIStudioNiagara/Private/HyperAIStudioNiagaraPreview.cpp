// Games by Hyper 2026.

#include "HyperAIStudioNiagaraPreview.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "PreviewScene.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace HyperAIStudio::Niagara::Preview
{
	namespace
	{
		constexpr int32 MaxAges = 8;
		constexpr float MaxAgeSeconds = 30.f;
		constexpr float FieldOfView = 50.f;
		/** A pixel counts as the effect when any channel moved this far from the empty plate. */
		constexpr int32 CoverageThreshold = 16;
		const TCHAR* const BackdropMesh = TEXT("/Engine/BasicShapes/Plane.Plane");
		const TCHAR* const BackdropMaterial = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

		FHyperAINiagaraPreviewReport Fail(FHyperAINiagaraPreviewReport& Report, const TCHAR* Status, const FString& Diagnostic)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		}

		FVector ViewDirection(const FString& View)
		{
			if (View == TEXT("side")) return FVector(0, -1, 0);
			if (View == TEXT("top")) return FVector(0.001, 0, -1).GetSafeNormal();
			if (View == TEXT("three_quarter")) return FVector(1, 0.8, -0.45).GetSafeNormal();
			return FVector(1, 0, 0);
		}

		/** Bounds a frame can trust: the component's own, which follow the particles for dynamic-bounds emitters. */
		FBox ComponentBox(UNiagaraComponent& Component)
		{
			Component.UpdateBounds();
			const FBoxSphereBounds& Bounds = Component.Bounds;
			return Bounds.SphereRadius > 0.0 ? Bounds.GetBox() : FBox(ForceInit);
		}

		int32 CountParticles(UNiagaraComponent& Component, TArray<FHyperAINiagaraEmitterRuntime>* OutEmitters = nullptr)
		{
			const FNiagaraSystemInstanceControllerPtr Controller = Component.GetSystemInstanceController();
			const FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
			if (!Instance)
			{
				return 0;
			}
			const UEnum* States = StaticEnum<ENiagaraExecutionState>();
			int32 Total = 0;
			for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
			{
				const bool bGpu = Emitter->GetSimTarget() == ENiagaraSimTarget::GPUComputeSim;
				const int32 Count = bGpu ? Emitter->GetTotalSpawnedParticles() : Emitter->GetNumParticles();
				Total += Count;
				if (OutEmitters)
				{
					FHyperAINiagaraEmitterRuntime& Record = OutEmitters->AddDefaulted_GetRef();
					Record.Name = Emitter->GetEmitterHandle().GetName().ToString();
					Record.bEnabled = Emitter->GetEmitterHandle().GetIsEnabled();
					Record.SimTarget = bGpu ? TEXT("gpu") : TEXT("cpu");
					Record.ExecutionState = States ? States->GetNameStringByValue(static_cast<int64>(Emitter->GetExecutionState())) : FString();
					Record.NumParticles = Emitter->GetNumParticles();
					Record.TotalSpawned = Emitter->GetTotalSpawnedParticles();
					Record.CountSource = bGpu ? TEXT("gpu_estimate") : TEXT("cpu");
					Record.bMeetsMinimum = Count > 0;
				}
			}
			return Total;
		}

		/** Steps the simulation from Now to Age at DeltaSeconds a tick. */
		void AdvanceTo(UNiagaraComponent& Component, float& Now, const float Age, const float DeltaSeconds)
		{
			const int32 Ticks = FMath::RoundToInt((Age - Now) / DeltaSeconds);
			if (Ticks > 0)
			{
				Component.AdvanceSimulation(Ticks, DeltaSeconds);
				Now += Ticks * DeltaSeconds;
			}
		}

		struct FCamera
		{
			TStrongObjectPtr<USceneCaptureComponent2D> Capture;
			TStrongObjectPtr<UTextureRenderTarget2D> Target;
			int32 Size = 0;

			bool Read(UWorld& World, TArray<FColor>& OutPixels)
			{
				// Components changed this frame send their render state at its end; the capture must see them now.
				World.SendAllEndOfFrameUpdates();
				Capture->CaptureScene();
				FlushRenderingCommands();
				FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
				return Resource && Resource->ReadPixels(OutPixels) && OutPixels.Num() == Size * Size;
			}
		};
	}

	void Measure(const TArray<FColor>& Pixels, const TArray<FColor>& Plate, const int32 Width, const int32 Height, FHyperAINiagaraPreviewFrame& OutFrame)
	{
		int64 Covered = 0;
		double Sum[3] = {0, 0, 0};
		float Peak = 0.f;
		int32 MinX = Width, MinY = Height, MaxX = -1, MaxY = -1;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const FColor& Pixel = Pixels[Y * Width + X];
				const FColor& Base = Plate.IsValidIndex(Y * Width + X) ? Plate[Y * Width + X] : FColor::Black;
				if (FMath::Abs(Pixel.R - Base.R) < CoverageThreshold && FMath::Abs(Pixel.G - Base.G) < CoverageThreshold
					&& FMath::Abs(Pixel.B - Base.B) < CoverageThreshold)
				{
					continue;
				}
				++Covered;
				Sum[0] += Pixel.R;
				Sum[1] += Pixel.G;
				Sum[2] += Pixel.B;
				Peak = FMath::Max(Peak, (0.2126f * Pixel.R + 0.7152f * Pixel.G + 0.0722f * Pixel.B) / 255.f);
				MinX = FMath::Min(MinX, X);
				MinY = FMath::Min(MinY, Y);
				MaxX = FMath::Max(MaxX, X);
				MaxY = FMath::Max(MaxY, Y);
			}
		}
		OutFrame.Coverage = static_cast<float>(Covered) / FMath::Max<int64>(1, static_cast<int64>(Width) * Height);
		OutFrame.PeakLuminance = Peak;
		OutFrame.MeanColor = Covered > 0
			? FLinearColor(Sum[0] / Covered / 255.0, Sum[1] / Covered / 255.0, Sum[2] / Covered / 255.0, 1.0)
			: FLinearColor::Black;
		OutFrame.ScreenBox = Covered > 0
			? FVector4(static_cast<double>(MinX) / Width, static_cast<double>(MinY) / Height, static_cast<double>(MaxX + 1) / Width, static_cast<double>(MaxY + 1) / Height)
			: FVector4(0, 0, 0, 0);
	}

	FHyperAINiagaraPreviewReport Render(const FHyperAINiagaraPreviewRequest& Request)
	{
		FHyperAINiagaraPreviewReport Report;
		if (!IsInGameThread())
		{
			return Fail(Report, TEXT("game_thread_required"), TEXT("Previews render on the game thread."));
		}
		TArray<float> Ages = Request.Ages.IsEmpty() ? TArray<float>({0.15f, 0.5f, 1.f, 2.f}) : Request.Ages;
		bool bAscending = Ages.Num() <= MaxAges;
		for (int32 Index = 0; Index < Ages.Num(); ++Index)
		{
			bAscending &= FMath::IsFinite(Ages[Index]) && Ages[Index] >= 0.f && Ages[Index] <= MaxAgeSeconds && (Index == 0 || Ages[Index] > Ages[Index - 1]);
		}
		const FString View = Request.View.ToLower();
		const FString Background = Request.Background.ToLower();
		if (!bAscending || Request.TileSize < 128 || Request.TileSize > 768 || Request.DeltaSeconds < 1.f / 120.f || Request.DeltaSeconds > 1.f / 15.f
			|| (View != TEXT("front") && View != TEXT("side") && View != TEXT("top") && View != TEXT("three_quarter"))
			|| (Background != TEXT("black") && Background != TEXT("grey") && Background != TEXT("gray"))
			|| !FMath::IsFinite(Request.Distance) || Request.Distance < 0.f || Request.Distance > 100000.f)
		{
			return Fail(Report, TEXT("invalid_request"), FString::Printf(TEXT("ages: 1-%d ascending seconds from 0 to %.0f; tile_size 128-768; view front, side, top or three_quarter; background black or grey; distance 0-100000 cm."),
				MaxAges, MaxAgeSeconds));
		}
		UNiagaraSystem* System = Cast<UNiagaraSystem>(FSoftObjectPath(Request.TargetPath).TryLoad());
		if (!System)
		{
			return Fail(Report, TEXT("target_not_found"), FString::Printf(TEXT("No Niagara System at '%s'."), *Request.TargetPath.Left(256)));
		}
		// A System mid-compile simulates nothing, and a material mid-compile draws as the default material.
		System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/true, /*bShowProgress=*/false);
		if (!System->IsReadyToRun())
		{
			return Fail(Report, TEXT("system_not_ready"), TEXT("The System did not compile into a runnable state; hyper_niagara_validate lists the errors."));
		}

		FPreviewScene Scene{FPreviewScene::ConstructionValues().SetLightBrightness(3.f).SetSkyBrightness(1.f)};
		UWorld* World = Scene.GetWorld();
		UNiagaraComponent* Component = NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->SetAutoActivate(false);
		Component->SetAsset(System);
		Component->SetForceSolo(true);
		Scene.AddComponent(Component, FTransform::Identity);

		// First pass: where the effect goes over every age, so one framing holds them all.
		Component->Activate(/*bReset=*/true);
		float Now = 0.f;
		FBox Reach(ForceInit);
		double ExtentSum = 0.0;
		int32 Measured = 0;
		for (const float Age : Ages)
		{
			AdvanceTo(*Component, Now, Age, Request.DeltaSeconds);
			const FBox Box = ComponentBox(*Component);
			if (Box.IsValid && Box.GetExtent().GetMax() >= 1.0)
			{
				Reach += Box;
				ExtentSum += Box.GetExtent().Size();
				++Measured;
			}
		}
		if (!Reach.IsValid || Reach.GetExtent().GetMax() < 1.0)
		{
			Reach = FBox(FVector(-50.0), FVector(50.0));
		}
		const FVector Focus = Reach.GetCenter();
		// Framed on the typical age rather than the widest: one stray spark flung far should not shrink the effect to a dot.
		const double Radius = FMath::Clamp(Measured > 0 ? FMath::Lerp(ExtentSum / Measured, Reach.GetExtent().Size(), 0.25) : Reach.GetExtent().Size(), 20.0, 20000.0);
		const double Distance = Request.Distance > 0.f ? Request.Distance : Radius / FMath::Tan(FMath::DegreesToRadians(FieldOfView * 0.5)) * 1.05;
		const FVector Direction = ViewDirection(View);
		Report.FocusPoint = Focus;
		Report.CameraDistance = static_cast<float>(Distance);

		const bool bGrey = Background != TEXT("black");
		if (bGrey)
		{
			UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, BackdropMesh);
			UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, BackdropMaterial);
			if (Plane && Material)
			{
				UStaticMeshComponent* Backdrop = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
				Backdrop->SetStaticMesh(Plane);
				UMaterialInstanceDynamic* Grey = UMaterialInstanceDynamic::Create(Material, Backdrop);
				Grey->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.18f, 0.18f, 0.18f));
				Backdrop->SetMaterial(0, Grey);
				Backdrop->SetCastShadow(false);
				const double Size = (Distance + Radius * 2.0) * 4.0 / 100.0;
				const FTransform Placement(FRotationMatrix::MakeFromZ(-Direction).Rotator(), Focus + Direction * (Radius * 1.5 + 50.0), FVector(Size, Size, 1.0));
				Scene.AddComponent(Backdrop, Placement);
			}
			else
			{
				Report.Notes.Add(TEXT("The grey backdrop's engine mesh or material did not load; the background is black."));
			}
		}

		FCamera Camera;
		Camera.Size = Request.TileSize;
		Camera.Target.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient));
		Camera.Target->RenderTargetFormat = RTF_RGBA8;
		// Shown as a display shows it: an 8-bit target otherwise counts as linear and darkens every mid-tone.
		Camera.Target->TargetGamma = UTextureRenderTarget::GetDefaultDisplayGamma();
		Camera.Target->ClearColor = FLinearColor::Black;
		Camera.Target->InitAutoFormat(Camera.Size, Camera.Size);
		Camera.Target->UpdateResourceImmediate(/*bClearRenderTarget=*/true);
		Camera.Capture.Reset(NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient));
		USceneCaptureComponent2D& Capture = *Camera.Capture;
		Capture.CaptureSource = SCS_FinalColorLDR;
		Capture.TextureTarget = Camera.Target.Get();
		Capture.bCaptureEveryFrame = false;
		Capture.bCaptureOnMovement = false;
		Capture.FOVAngle = FieldOfView;
		Capture.ShowFlags.SetMotionBlur(false);
		Capture.ShowFlags.SetTemporalAA(false);
		Capture.ShowFlags.SetAntiAliasing(true);
		// Fixed exposure, so a dim effect looks dim and frames compare: eye adaptation would brighten an empty frame.
		Capture.PostProcessSettings.bOverride_AutoExposureMethod = true;
		Capture.PostProcessSettings.AutoExposureMethod = AEM_Manual;
		Capture.PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		Capture.PostProcessSettings.AutoExposureApplyPhysicalCameraExposure = false;
		Capture.PostProcessSettings.bOverride_AutoExposureBias = true;
		Capture.PostProcessSettings.AutoExposureBias = 0.f;
		Capture.PostProcessBlendWeight = 1.f;
		Scene.AddComponent(&Capture, FTransform(Direction.Rotation(), Focus - Direction * Distance));

		// The empty plate first: what every tile is measured against.
		Component->DeactivateImmediate();
		TArray<FColor> Plate;
		if (!Camera.Read(*World, Plate))
		{
			Scene.RemoveComponent(Component);
			return Fail(Report, TEXT("capture_failed"), TEXT("The preview capture could not be read back."));
		}

		// Second pass: the same simulation again, drawn at each age. Shaders the first draw asks for finish before the kept draw.
		Component->Activate(/*bReset=*/true);
		Now = 0.f;
		const int32 Tile = Camera.Size;
		const int32 SheetWidth = Tile * Ages.Num();
		TArray<FColor> Sheet;
		Sheet.Init(FColor::Black, SheetWidth * Tile);
		bool bWarmed = false;
		for (int32 Index = 0; Index < Ages.Num(); ++Index)
		{
			AdvanceTo(*Component, Now, Ages[Index], Request.DeltaSeconds);
			TArray<FColor> Pixels;
			if (!bWarmed)
			{
				Camera.Read(*World, Pixels);
				if (GShaderCompilingManager)
				{
					GShaderCompilingManager->FinishAllCompilation();
				}
				bWarmed = true;
			}
			if (!Camera.Read(*World, Pixels))
			{
				Scene.RemoveComponent(Component);
				return Fail(Report, TEXT("capture_failed"), FString::Printf(TEXT("The capture at %.2f s could not be read back."), Ages[Index]));
			}
			FHyperAINiagaraPreviewFrame& Frame = Report.Frames.AddDefaulted_GetRef();
			Frame.Age = Now;
			Frame.Particles = CountParticles(*Component);
			Frame.BoundsSize = ComponentBox(*Component).IsValid ? ComponentBox(*Component).GetSize() : FVector::ZeroVector;
			for (int32 Y = 0; Y < Tile; ++Y)
			{
				for (int32 X = 0; X < Tile; ++X)
				{
					Sheet[Y * SheetWidth + Index * Tile + X] = FColor(Pixels[Y * Tile + X].R, Pixels[Y * Tile + X].G, Pixels[Y * Tile + X].B, 255);
				}
			}
			// Measured against the empty plate pixel by pixel, so a lit backdrop's gradient never counts as the effect.
			Measure(Pixels, Plate, Tile, Tile, Frame);
			if (Frame.Coverage <= 0.f)
			{
				Report.Notes.Add(FString::Printf(TEXT("Nothing visible at %.2f s (%d particles): check spawning, sizes, colour alpha and the materials."), Frame.Age, Frame.Particles));
			}
			else if (Frame.ScreenBox.X <= 0.0 || Frame.ScreenBox.Y <= 0.0 || Frame.ScreenBox.Z >= 1.0 || Frame.ScreenBox.W >= 1.0)
			{
				Report.Notes.Add(FString::Printf(TEXT("At %.2f s the effect runs off the tile edge; raise distance to see all of it."), Frame.Age));
			}
			if (Frame.PeakLuminance >= 0.99f && Frame.Coverage > 0.02f)
			{
				Report.Notes.Add(FString::Printf(TEXT("At %.2f s part of it clips to white; lower the colour's brightness if detail is lost."), Frame.Age));
			}
		}
		CountParticles(*Component, &Report.Emitters);
		Component->DeactivateImmediate();
		Scene.RemoveComponent(Component);

		const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Captures/Niagara"));
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		const FString File = Folder / FString::Printf(TEXT("%s_%s.png"), *System->GetName(), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s")));
		if (!FImageUtils::SaveImageByExtension(*File, FImageView(Sheet.GetData(), SheetWidth, Tile)))
		{
			return Fail(Report, TEXT("write_failed"), FString::Printf(TEXT("Could not write %s."), *File));
		}
		Report.ImagePath = File;
		FString Relative = File;
		FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		Report.ProjectImagePath = Relative;
		Report.Width = SheetWidth;
		Report.Height = Tile;
		Report.bOk = true;
		Report.Status = TEXT("rendered");
		TArray<FString> Summary;
		for (const FHyperAINiagaraPreviewFrame& Frame : Report.Frames)
		{
			Summary.Add(FString::Printf(TEXT("%.2fs %d particles %.0f%% covered"), Frame.Age, Frame.Particles, Frame.Coverage * 100.f));
		}
		Report.Diagnostic = FString::Printf(TEXT("%d tiles of %d px, %s view from %.0f cm: %s. Open image_path to look at it."), Ages.Num(), Tile, *View,
			Distance, *FString::Join(Summary, TEXT("; ")));
		return Report;
	}
}
