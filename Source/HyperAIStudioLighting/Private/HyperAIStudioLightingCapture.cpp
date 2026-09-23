// Games by Hyper 2026.

#include "HyperAIStudioLightingCapture.h"

#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HyperAIStudioLightingGate.h"
#include "LevelEditorViewport.h"
#include "Misc/Paths.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace HyperAIStudio::Lighting::Capture
{
	namespace
	{
		namespace Gate = HyperAIStudio::Lighting::Gate;

		constexpr int32 MaxWidth = 1920;
		constexpr int32 MaxHeight = 1080;
		constexpr int32 MaxFrames = 4;
		constexpr int32 MaxWarmupFrames = 8;
		constexpr int32 MaxSessions = 4;
		constexpr int32 KeptBatches = 20;
		constexpr double SessionSeconds = 60.0;
		constexpr double ShaderWaitSeconds = 20.0;
		// Forces what every material's Time node reads; non-shipping, and always put back to -1 afterwards.
		const TCHAR* const MaterialTimeVariable = TEXT("r.Test.OverrideTimeMaterialExpressions");

		TArray<FString> Capabilities()
		{
			return {
				TEXT("final_colour_capture_from_a_camera_actor_a_transform_or_the_level_viewport"),
				TEXT("one_to_four_frames_by_world_time_or_forced_material_time"),
				TEXT("frame_metrics_and_frame_to_frame_pixel_differences"),
				TEXT("pngs_under_saved_hyperaistudio_captures_last_20_batches_kept"),
				TEXT("unsupported:pie_worlds_orthographic_views_and_regions_of_interest")};
		}

		bool IsSafeLabel(const FString& Label)
		{
			if (Label.Len() > 48) return false;
			for (const TCHAR Character : Label)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-')) return false;
			}
			return true;
		}

		FString Validate(const FHyperAICaptureViewRequest& Request)
		{
			if (Request.Source != TEXT("camera") && Request.Source != TEXT("transform") && Request.Source != TEXT("viewport"))
				return TEXT("Source is camera, transform, or viewport.");
			if (Request.Source == TEXT("camera") && Request.CameraActorLabel.IsEmpty())
				return TEXT("A camera capture needs camera_actor_label.");
			if (Request.Source == TEXT("transform") && (Request.FovDegrees < 5.f || Request.FovDegrees > 170.f))
				return TEXT("fov_degrees is 5-170.");
			if (Request.Width < 64 || Request.Width > MaxWidth || Request.Height < 64 || Request.Height > MaxHeight)
				return FString::Printf(TEXT("Width and height are 64-%d and 64-%d."), MaxWidth, MaxHeight);
			if (Request.FrameCount < 1 || Request.FrameCount > MaxFrames)
				return FString::Printf(TEXT("frame_count is 1-%d."), MaxFrames);
			if (Request.WarmupFrames < 0 || Request.WarmupFrames > MaxWarmupFrames)
				return FString::Printf(TEXT("warmup_frames is 0-%d."), MaxWarmupFrames);
			if (Request.DiffThreshold < 0.001f || Request.DiffThreshold > 0.5f)
				return TEXT("diff_threshold is 0.001-0.5.");
			if (!IsSafeLabel(Request.Label))
				return TEXT("label is up to 48 letters, digits, - or _.");
			if (Request.TimeMode == TEXT("world"))
			{
				if (Request.IntervalSeconds < 0.05f || Request.IntervalSeconds > 5.f) return TEXT("interval_seconds is 0.05-5.");
				return FString();
			}
			if (Request.TimeMode != TEXT("material_time"))
				return TEXT("time_mode is world or material_time.");
			if (Request.MaterialTimes.Num() != Request.FrameCount)
				return TEXT("material_time needs one material_times entry per frame.");
			for (const float Time : Request.MaterialTimes)
			{
				if (Time < 0.f || Time > 3600.f) return TEXT("material_times are 0-3600 seconds.");
			}
			return FString();
		}

		struct FView
		{
			FVector Location = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			float Fov = 90.f;
			FPostProcessSettings PostProcess;
			float PostProcessBlendWeight = 0.f;
		};

		bool ResolveView(UWorld& World, const FHyperAICaptureViewRequest& Request, FView& Out, FString& OutError)
		{
			if (Request.Source == TEXT("transform"))
			{
				Out.Location = Request.Location;
				Out.Rotation = Request.Rotation;
				Out.Fov = Request.FovDegrees;
				return true;
			}
			if (Request.Source == TEXT("viewport"))
			{
				const FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
				if (!Client || !Client->IsPerspective())
				{
					OutError = TEXT("No perspective level viewport is active; capture from a camera or a transform instead.");
					return false;
				}
				Out.Location = Client->GetViewLocation();
				Out.Rotation = Client->GetViewRotation();
				Out.Fov = Client->ViewFOV;
				return true;
			}
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (!It->GetActorLabel().Equals(Request.CameraActorLabel, ESearchCase::IgnoreCase)) continue;
				UCameraComponent* Camera = It->FindComponentByClass<UCameraComponent>();
				if (!Camera)
				{
					OutError = FString::Printf(TEXT("%s has no camera component."), *Request.CameraActorLabel);
					return false;
				}
				FMinimalViewInfo Info;
				Camera->GetCameraView(0.f, Info);
				Out.Location = Info.Location;
				Out.Rotation = Info.Rotation;
				Out.Fov = Info.FOV;
				Out.PostProcess = Info.PostProcessSettings;
				Out.PostProcessBlendWeight = Info.PostProcessBlendWeight;
				return true;
			}
			OutError = FString::Printf(TEXT("No actor labelled %s in the open level."), *Request.CameraActorLabel);
			return false;
		}

		FString CapturesRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Captures"));
		}

		FString ProjectRelative(const FString& Path)
		{
			FString Relative = Path;
			FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
			return Relative;
		}

		/** Keeps the newest batches; captures are evidence for the current task, not an archive. */
		void PruneBatches()
		{
			TArray<TPair<FDateTime, FString>> Batches;
			IFileManager::Get().IterateDirectory(*CapturesRoot(), [&Batches](const TCHAR* Path, const bool bDirectory)
			{
				if (bDirectory) Batches.Emplace(IFileManager::Get().GetTimeStamp(Path), Path);
				return true;
			});
			if (Batches.Num() <= KeptBatches) return;
			Batches.Sort([](const TPair<FDateTime, FString>& A, const TPair<FDateTime, FString>& B) { return A.Key < B.Key; });
			for (int32 Index = 0; Index < Batches.Num() - KeptBatches; ++Index)
			{
				IFileManager::Get().DeleteDirectory(*Batches[Index].Value, /*RequireExists=*/false, /*Tree=*/true);
			}
		}

		class FSession
		{
		public:
			FHyperAICaptureViewRequest Request;
			FHyperAICaptureViewReport Report;
			TStrongObjectPtr<UHyperAICaptureViewResult> Result;

			bool Begin(UWorld& InWorld, FString& OutError)
			{
				FView View;
				if (!ResolveView(InWorld, Request, View, OutError)) return false;
				World = &InWorld;
				Report.Source = Request.Source;
				Report.ViewLocation = View.Location;
				Report.ViewRotation = View.Rotation;
				Report.FovDegrees = View.Fov;
				Report.BatchId = FString::Printf(TEXT("%s-%s-%s"), Request.Label.IsEmpty() ? TEXT("capture") : *Request.Label,
					*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(4).ToLower());
				FolderOnDisk = CapturesRoot() / Report.BatchId;
				Report.Folder = ProjectRelative(FolderOnDisk);
				if (!IFileManager::Get().MakeDirectory(*FolderOnDisk, /*Tree=*/true))
				{
					OutError = TEXT("Could not create the capture folder.");
					return false;
				}

				Target.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient));
				Target->RenderTargetFormat = RTF_RGBA8;
				Target->ClearColor = FLinearColor::Black;
				Target->InitAutoFormat(Request.Width, Request.Height);
				Target->UpdateResourceImmediate(/*bClearRenderTarget=*/true);
				Component.Reset(NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient));
				Component->CaptureSource = SCS_FinalColorLDR;
				Component->TextureTarget = Target.Get();
				Component->bCaptureEveryFrame = false;
				Component->bCaptureOnMovement = false;
				// Temporal effects (anti-aliasing, Lumen, eye adaptation) need history across captures to settle.
				Component->bAlwaysPersistRenderingState = true;
				Component->FOVAngle = View.Fov;
				if (View.PostProcessBlendWeight > 0.f)
				{
					Component->PostProcessSettings = View.PostProcess;
					Component->PostProcessBlendWeight = View.PostProcessBlendWeight;
				}
				Component->SetWorldLocationAndRotation(View.Location, View.Rotation);
				Component->RegisterComponentWithWorld(&InWorld);
				WarmupLeft = Request.WarmupFrames;
				StartedSeconds = FPlatformTime::Seconds();
				return true;
			}

			/** One unit of work: a warm-up, a frame, or a wait. True once the session has finished either way. */
			bool Step(const bool bAllowWaiting)
			{
				UWorld* CurrentWorld = World.Get();
				if (!CurrentWorld || !Component.IsValid())
				{
					Finish(false, TEXT("world_gone"), TEXT("The level closed while capturing."));
					return true;
				}
				const double Now = FPlatformTime::Seconds();
				if (Now - StartedSeconds > SessionSeconds)
				{
					Finish(false, TEXT("timed_out"), TEXT("The capture did not finish within 60 seconds."));
					return true;
				}
				if (!bShadersSettled)
				{
					if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())
					{
						if (!bAllowWaiting)
						{
							GShaderCompilingManager->FinishAllCompilation();
						}
						else if (Now - StartedSeconds < ShaderWaitSeconds)
						{
							return false;
						}
						else
						{
							Report.bShadersStillCompiling = true;
						}
					}
					bShadersSettled = true;
				}
				if (WarmupLeft > 0)
				{
					TArray<FColor> Discard;
					Read(Discard);
					--WarmupLeft;
					return false;
				}

				const int32 Index = Report.Frames.Num();
				const bool bMaterialTime = Request.TimeMode == TEXT("material_time");
				if (!bMaterialTime && Index > 0 && CurrentWorld->GetTimeSeconds() < NextWorldTime)
				{
					if (!bAllowWaiting)
					{
						Finish(false, TEXT("async_required"),
							TEXT("Frames spaced by world time have to wait for the editor to tick; call hyper_capture_view."));
						return true;
					}
					return false;
				}
				IConsoleVariable* TimeOverride = bMaterialTime ? IConsoleManager::Get().FindConsoleVariable(MaterialTimeVariable) : nullptr;
				if (bMaterialTime && !TimeOverride)
				{
					Finish(false, TEXT("material_time_unavailable"), TEXT("This build cannot force material time; use world time."));
					return true;
				}
				if (TimeOverride)
				{
					TimeOverride->Set(Request.MaterialTimes[Index], ECVF_SetByCode);
				}
				TArray<FColor> Pixels;
				const bool bRead = Read(Pixels);
				if (TimeOverride)
				{
					TimeOverride->Set(-1.f, ECVF_SetByCode);
				}
				if (!bRead)
				{
					Finish(false, TEXT("capture_failed"), TEXT("The capture could not be read back."));
					return true;
				}
				const FString File = FolderOnDisk / FString::Printf(TEXT("frame_%d.png"), Index);
				if (!Gate::WritePng(File, Pixels, Request.Width, Request.Height))
				{
					Finish(false, TEXT("write_failed"), FString::Printf(TEXT("Could not write %s."), *File));
					return true;
				}
				FHyperAICaptureFrame& Frame = Report.Frames.AddDefaulted_GetRef();
				Frame.ImagePath = ProjectRelative(File);
				Frame.WorldTimeSeconds = static_cast<float>(CurrentWorld->GetTimeSeconds());
				Frame.MaterialTime = bMaterialTime ? Request.MaterialTimes[Index] : -1.f;
				Frame.Metrics = Gate::MeasurePixels(Pixels, Request.Width, Request.Height);
				Frames.Add(MoveTemp(Pixels));
				NextWorldTime = CurrentWorld->GetTimeSeconds() + Request.IntervalSeconds;
				if (Report.Frames.Num() < Request.FrameCount)
				{
					return false;
				}
				for (int32 B = 1; B < Frames.Num(); ++B)
				{
					FHyperAICaptureDiff& Change = Report.Diffs.AddDefaulted_GetRef();
					Change.FrameA = B - 1;
					Change.FrameB = B;
					Diff(Frames[B - 1], Frames[B], Request.DiffThreshold, Change.MeanAbsDiff, Change.ChangedPixelFraction);
				}
				Report.bWorldTimeAdvanced = !bMaterialTime && Report.Frames.Num() > 1
					&& Report.Frames.Last().WorldTimeSeconds > Report.Frames[0].WorldTimeSeconds;
				TArray<FString> Changes;
				for (const FHyperAICaptureDiff& Change : Report.Diffs)
				{
					Changes.Add(FString::Printf(TEXT("frame %d to %d changed %.1f%% of pixels"), Change.FrameA, Change.FrameB, Change.ChangedPixelFraction * 100.f));
				}
				Finish(true, TEXT("captured"), FString::Printf(TEXT("%d frame%s in %s%s%s%s"), Report.Frames.Num(),
					Report.Frames.Num() == 1 ? TEXT("") : TEXT("s"), *Report.Folder, Changes.IsEmpty() ? TEXT("") : TEXT("; "),
					*FString::Join(Changes, TEXT(", ")),
					!bMaterialTime && Report.Frames.Num() > 1 && !Report.bWorldTimeAdvanced ? TEXT("; the world did not tick between frames") : TEXT("")));
				return true;
			}

			void Finish(const bool bOk, const TCHAR* Status, const FString& Diagnostic)
			{
				if (bFinished) return;
				bFinished = true;
				Report.bOk = bOk;
				Report.Status = Status;
				Report.Diagnostic = Diagnostic;
				Report.Capabilities = Capabilities();
				if (Component.IsValid())
				{
					Component->UnregisterComponent();
					Component.Reset();
				}
				Target.Reset();
				Frames.Empty();
				PruneBatches();
				if (Result.IsValid())
				{
					Result->SetValue(Report);
					Result.Reset();
				}
			}

		private:
			bool Read(TArray<FColor>& OutPixels)
			{
				UWorld* CurrentWorld = World.Get();
				if (!CurrentWorld || !Component.IsValid() || !Target.IsValid()) return false;
				// Actors changed this frame get their render state at its end; the capture must see them now.
				CurrentWorld->SendAllEndOfFrameUpdates();
				Component->CaptureScene();
				FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
				return Resource && Resource->ReadPixels(OutPixels) && OutPixels.Num() == Request.Width * Request.Height;
			}

			TWeakObjectPtr<UWorld> World;
			TStrongObjectPtr<USceneCaptureComponent2D> Component;
			TStrongObjectPtr<UTextureRenderTarget2D> Target;
			TArray<TArray<FColor>> Frames;
			FString FolderOnDisk;
			int32 WarmupLeft = 0;
			double NextWorldTime = 0.0;
			double StartedSeconds = 0.0;
			bool bShadersSettled = false;
			bool bFinished = false;
		};

		TArray<TSharedPtr<FSession>>& Sessions()
		{
			static TArray<TSharedPtr<FSession>> Items;
			return Items;
		}

		void EnsureTicking()
		{
			static FTSTicker::FDelegateHandle Handle;
			if (Handle.IsValid()) return;
			Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				TArray<TSharedPtr<FSession>> Running = Sessions();
				for (const TSharedPtr<FSession>& Session : Running)
				{
					if (Session->Step(/*bAllowWaiting=*/true))
					{
						Sessions().Remove(Session);
					}
				}
				const bool bKeep = !Sessions().IsEmpty();
				if (!bKeep) Handle.Reset();
				return bKeep;
			}));
		}

		FHyperAICaptureViewReport Rejected(const TCHAR* Status, const FString& Diagnostic)
		{
			FHyperAICaptureViewReport Report;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			Report.Capabilities = Capabilities();
			return Report;
		}
	}

	void Diff(TConstArrayView<FColor> A, TConstArrayView<FColor> B, const float Threshold, float& OutMeanAbs, float& OutChangedFraction)
	{
		OutMeanAbs = 0.f;
		OutChangedFraction = 0.f;
		if (A.Num() != B.Num() || A.IsEmpty()) return;
		const int32 Limit = FMath::RoundToInt(Threshold * 255.f);
		double Sum = 0.0;
		int64 Changed = 0;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			const int32 R = FMath::Abs(int32(A[Index].R) - int32(B[Index].R));
			const int32 G = FMath::Abs(int32(A[Index].G) - int32(B[Index].G));
			const int32 Bl = FMath::Abs(int32(A[Index].B) - int32(B[Index].B));
			Sum += R + G + Bl;
			Changed += FMath::Max3(R, G, Bl) > Limit ? 1 : 0;
		}
		OutMeanAbs = static_cast<float>(Sum / (3.0 * 255.0 * A.Num()));
		OutChangedFraction = static_cast<float>(double(Changed) / A.Num());
	}

	UHyperAICaptureViewResult* Start(const FHyperAICaptureViewRequest& Request)
	{
		UHyperAICaptureViewResult* Result = NewObject<UHyperAICaptureViewResult>();
		if (!IsInGameThread())
		{
			Result->SetValue(Rejected(TEXT("game_thread_required"), TEXT("Captures run on the game thread.")));
			return Result;
		}
		const FString Invalid = Validate(Request);
		if (!Invalid.IsEmpty())
		{
			Result->SetValue(Rejected(TEXT("invalid_request"), Invalid));
			return Result;
		}
		UWorld* World = Gate::GetEditorWorld();
		if (!World)
		{
			Result->SetValue(Rejected(TEXT("no_level"), TEXT("No level is open in the editor.")));
			return Result;
		}
		if (Sessions().Num() >= MaxSessions)
		{
			Result->SetValue(Rejected(TEXT("busy"), FString::Printf(TEXT("%d captures are already running; wait for one to finish."), MaxSessions)));
			return Result;
		}
		const TSharedPtr<FSession> Session = MakeShared<FSession>();
		Session->Request = Request;
		Session->Result.Reset(Result);
		FString Error;
		if (!Session->Begin(*World, Error))
		{
			Session->Finish(false, TEXT("view_not_found"), Error);
			return Result;
		}
		// The first unit of work happens now; a single frame with nothing to wait for completes in this call.
		if (!Session->Step(/*bAllowWaiting=*/true))
		{
			Sessions().Add(Session);
			EnsureTicking();
		}
		return Result;
	}

	FHyperAICaptureViewReport RunNow(const FHyperAICaptureViewRequest& Request)
	{
		if (!IsInGameThread()) return Rejected(TEXT("game_thread_required"), TEXT("Captures run on the game thread."));
		const FString Invalid = Validate(Request);
		if (!Invalid.IsEmpty()) return Rejected(TEXT("invalid_request"), Invalid);
		UWorld* World = Gate::GetEditorWorld();
		if (!World) return Rejected(TEXT("no_level"), TEXT("No level is open in the editor."));
		FSession Session;
		Session.Request = Request;
		FString Error;
		if (!Session.Begin(*World, Error)) return Rejected(TEXT("view_not_found"), Error);
		// Every step without waiting makes progress: a warm-up, a frame, or a terminal failure.
		while (!Session.Step(/*bAllowWaiting=*/false)) {}
		return Session.Report;
	}

	void CancelAll()
	{
		TArray<TSharedPtr<FSession>> Running = MoveTemp(Sessions());
		Sessions().Reset();
		for (const TSharedPtr<FSession>& Session : Running)
		{
			Session->Finish(false, TEXT("cancelled"), TEXT("The lighting pack unloaded during the capture."));
		}
	}
}
