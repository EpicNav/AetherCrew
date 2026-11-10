// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "CineCameraActor.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioMovieRenderToolset.h"
#include "HyperAIStudioSettings.h"
#include "LevelSequence.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "ObjectTools.h"
#include "Tests/AutomationCommon.h"
#include "Tracks/MovieSceneCameraCutTrack.h"

namespace HyperAIStudio::MovieRenderTests
{
	using C = FHyperAIStudioMovieRenderContracts;

	FHyperAIMovieRenderOp Op(const TCHAR* Kind, const FString& Asset = FString())
	{
		FHyperAIMovieRenderOp Out;
		Out.Kind = Kind;
		Out.Asset = Asset;
		return Out;
	}

	FHyperAIMovieRenderPlanReport Run(TArray<FHyperAIMovieRenderOp> Ops, const bool bDryRun = false)
	{
		FHyperAIMovieRenderPlanRequest Plan;
		Plan.Operations = MoveTemp(Ops);
		Plan.bDryRun = bDryRun;
		return C::ApplyPlan(Plan);
	}

	/** A one-second sequence at 24 fps, cut to a camera looking over the bench level. */
	ULevelSequence* MakeSequence(UWorld& World, const FString& Path, ACineCameraActor*& OutCamera)
	{
		UPackage* Package = CreatePackage(*Path);
		ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
		Sequence->Initialize();
		UMovieScene* Scene = Sequence->GetMovieScene();
		Scene->SetDisplayRate(FFrameRate(24, 1));
		Scene->SetPlaybackRange(FFrameNumber(0), (1.0 * Scene->GetTickResolution()).RoundToFrame().Value);
		OutCamera = World.SpawnActor<ACineCameraActor>(FVector(-600.0, 0.0, 400.0), FRotator(-20.0, 0.0, 0.0));
		const FGuid Guid = Scene->AddPossessable(OutCamera->GetActorLabel(), OutCamera->GetClass());
		Sequence->BindPossessableObject(Guid, *OutCamera, &World);
		UMovieSceneCameraCutTrack* Cuts = Cast<UMovieSceneCameraCutTrack>(Scene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
		Cuts->AddNewCameraCut(UE::MovieScene::FRelativeObjectBindingID(Guid), FFrameNumber(0));
		FAssetRegistryModule::AssetCreated(Sequence);
		return Sequence;
	}

	void DeleteFolder(const FString& Folder)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Folder), Assets, /*bRecursive=*/true);
		TArray<UObject*> Objects;
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* Object = Asset.GetAsset()) Objects.Add(Object);
		}
		if (!Objects.IsEmpty()) ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioMovieRenderEndToEndTest,
	"HyperAIStudio.NativeTools.Cinematics.MovieRenderEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioMovieRenderEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MovieRenderTests;
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		AddInfo(TEXT("Skipped: no editor world."));
		return true;
	}
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/MovieRender_") + Suffix;
	const FString Config = Folder + TEXT("/MRQ_Draft");
	const FString Graph = Folder + TEXT("/MRG_Test");
	const FString Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MovieRenders") / (TEXT("HyperAITest_") + Suffix));
	ACineCameraActor* Camera = nullptr;
	ULevelSequence* Sequence = MakeSequence(*World, Folder + TEXT("/LS_Render"), Camera);
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	auto Cleanup = [World, Camera, Folder, Output, Settings, bApproval]()
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		if (IsValid(Camera)) World->EditorDestroyActor(Camera, /*bShouldModifyLevel=*/true);
		DeleteFolder(Folder);
		IFileManager::Get().DeleteDirectory(*Output, /*RequireExists=*/false, /*Tree=*/true);
	};

	FHyperAIMovieRenderOp MakeConfig = Op(TEXT("config.create"), Config);
	MakeConfig.Preset = TEXT("draft");
	MakeConfig.Format = TEXT("png");
	MakeConfig.Resolution = FIntPoint(320, 180);
	FHyperAIMovieRenderOp MakeGraph = Op(TEXT("graph.create"), Graph);
	MakeGraph.Resolution = FIntPoint(640, 360);
	FHyperAIMovieRenderOp Render = Op(TEXT("render"), Config);
	Render.Sequence = Sequence->GetPathName();
	Render.Directory = Output;

	// Refused before anything changes.
	FHyperAIMovieRenderOp Gif = MakeConfig;
	Gif.Format = TEXT("gif");
	TestTrue(TEXT("an unknown format is refused"), Run({Gif}, /*bDryRun=*/true).Diagnostic.Contains(TEXT("png, exr")));
	FHyperAIMovieRenderOp Nothing = Render;
	Nothing.Sequence = Folder + TEXT("/LS_Missing");
	TestFalse(TEXT("rendering a sequence that is not there is refused"), Run({Nothing}, /*bDryRun=*/true).bOk);
	TestFalse(TEXT("a render that is not the plan's last op is refused"), Run({Render, MakeConfig}, /*bDryRun=*/true).bOk);
	FHyperAIMovieRenderOp Typo = MakeConfig;
	FHyperAIMovieRenderSetting Setting;
	Setting.Name = TEXT("anti_aliasing.TemporalSamplCount");
	Setting.Value = TEXT("4");
	Typo.Settings = {Setting};
	const FHyperAIMovieRenderPlanReport Mistyped = Run({Typo}, /*bDryRun=*/true);
	TestTrue(TEXT("a mistyped setting is refused with the setting's properties: ") + Mistyped.Diagnostic, !Mistyped.bOk && Mistyped.Diagnostic.Contains(TEXT("TemporalSampleCount")));

	// A draft config and a graph, saved.
	const FHyperAIMovieRenderPlanReport Made = Run({MakeConfig, MakeGraph});
	if (!TestTrue(TEXT("the config and graph are made: ") + Made.Diagnostic + TEXT(" | ") + FString::Join(Made.Notes, TEXT(" | ")), Made.bOk && Made.Saved.Num() == 2))
	{
		Cleanup();
		return false;
	}
	FHyperAIMovieRenderInspectRequest Look;
	Look.Asset = Config;
	const FString ConfigSettings = FString::Join(C::Inspect(Look).Settings, TEXT(" | "));
	TestTrue(TEXT("the config renders png at 320 x 180: ") + ConfigSettings, ConfigSettings.Contains(TEXT("png")) && ConfigSettings.Contains(TEXT("X=320,Y=180")));
	Look.Asset = Graph;
	const FString GraphNodes = FString::Join(C::Inspect(Look).Settings, TEXT(" | "));
	TestTrue(TEXT("the graph's global output settings hold 640 x 360: ") + GraphNodes, GraphNodes.Contains(TEXT("OutputResolution")) && GraphNodes.Contains(TEXT("X=640,Y=360")));

	// The render: a second of frames, here, off screen.
	const FHyperAIMovieRenderPlanReport Started = Run({Render});
	Settings->bRequireApprovalForAgentEdits = bApproval;
	if (!TestTrue(TEXT("the render starts: ") + Started.Diagnostic, Started.bOk && Started.Status == TEXT("rendering") && Started.Output == Output))
	{
		Cleanup();
		return false;
	}
	const FString OperationId = Started.OperationId;
	const double Began = FPlatformTime::Seconds();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, OperationId, Output, Began, Cleanup]()
	{
		const bool bRunning = FHyperAIStudioAsyncJobHost::IsRunning(OperationId);
		if (bRunning && FPlatformTime::Seconds() - Began < 300.0) return false;
		TArray<FString> Frames;
		IFileManager::Get().FindFiles(Frames, *(Output / TEXT("*.png")), /*Files=*/true, /*Directories=*/false);
		TestFalse(TEXT("the render finishes within five minutes"), bRunning);
		TestTrue(FString::Printf(TEXT("24 frames are written as png (%d) in %.0f s"), Frames.Num(), FPlatformTime::Seconds() - Began), Frames.Num() >= 24);
		Cleanup();
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
