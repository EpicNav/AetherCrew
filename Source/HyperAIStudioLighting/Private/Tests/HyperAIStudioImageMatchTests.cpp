// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioImageMatch.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioLightingCapture.h"
#include "HyperAIStudioLightingGate.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "Math/RandomStream.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SceneInterface.h"
#include "ShaderCompiler.h"

namespace HyperAIStudio::Lighting::MatchTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	namespace Match = HyperAIStudio::Lighting::Match;

	struct FPicture
	{
		int32 Width = 0;
		int32 Height = 0;
		TArray<FColor> Pixels;
	};

	/** A dusk shore: an orange-to-violet sky, a low sun, a dark sea and a striped lighthouse. */
	FPicture Scene(const int32 Width = 320, const int32 Height = 200)
	{
		FPicture Picture{Width, Height};
		Picture.Pixels.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const float V = static_cast<float>(Y) / (Height - 1);
				FLinearColor Color = FMath::Lerp(FLinearColor(0.95f, 0.47f, 0.2f), FLinearColor(0.45f, 0.25f, 0.5f), V / 0.62f);
				if (FVector2f::Distance(FVector2f(X, Y), FVector2f(Width * 0.68f, Height * 0.42f)) < Height * 0.12f && V < 0.62f)
				{
					Color = FLinearColor(1.f, 0.86f, 0.56f);
				}
				if (V >= 0.62f)
				{
					Color = FMath::Lerp(FLinearColor(0.08f, 0.1f, 0.22f), FLinearColor(0.02f, 0.03f, 0.08f), (V - 0.62f) / 0.38f);
				}
				if (X > Width * 0.22f && X < Width * 0.27f && V > 0.25f && V < 0.66f)
				{
					Color = ((Y * 16 / Height) % 2 == 0) ? FLinearColor(0.92f, 0.92f, 0.9f) : FLinearColor(0.72f, 0.12f, 0.12f);
				}
				Picture.Pixels[Y * Width + X] = Color.ToFColor(/*bSRGB=*/false);
			}
		}
		return Picture;
	}

	FPicture Edited(FPicture Picture, TFunctionRef<void(int32 X, int32 Y, FColor& Pixel)> Edit)
	{
		for (int32 Y = 0; Y < Picture.Height; ++Y)
		{
			for (int32 X = 0; X < Picture.Width; ++X)
			{
				Edit(X, Y, Picture.Pixels[Y * Picture.Width + X]);
			}
		}
		return Picture;
	}

	/** Halves the light: sRGB bytes scaled in linear space. */
	uint8 Darker(const uint8 Value, const float Factor)
	{
		const float C = Value / 255.f;
		const float Linear = C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
		const float Out = Linear * Factor;
		const float Srgb = Out <= 0.0031308f ? Out * 12.92f : 1.055f * FMath::Pow(Out, 1.f / 2.4f) - 0.055f;
		return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Srgb * 255.f), 0, 255));
	}

	FHyperAIImageCompareReport Measure(const FPicture& Reference, const FPicture& Candidate, const TCHAR* Mode = TEXT("look"))
	{
		return Match::ComparePixels(Reference.Pixels, Reference.Width, Reference.Height, Candidate.Pixels, Candidate.Width, Candidate.Height,
			Mode, 80.f, 4);
	}

	FString Folder()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("ImageMatch"));
	}

	FString Save(const FPicture& Picture, const FString& Name)
	{
		const FString File = Folder() / Name;
		HyperAIStudio::Lighting::Gate::WritePng(File, Picture.Pixels, Picture.Width, Picture.Height);
		return File;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageMatchMeasuresTest,
	"HyperAIStudio.Lighting.ImageMatch.MeasuresRespond",
	HyperAIStudio::Lighting::MatchTests::Flags)

bool FHyperAIStudioImageMatchMeasuresTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::MatchTests;
	const FPicture Reference = Scene();

	const FHyperAIImageCompareReport Same = Measure(Reference, Reference, TEXT("aligned"));
	TestTrue(FString::Printf(TEXT("An image matches itself (score %.1f)"), Same.Score), Same.bOk && Same.bMatched && Same.Score > 99.f);
	TestTrue(TEXT("with every measure at the top"), Same.StructureScore > 99.f && Same.PaletteScore > 99.f && Same.LayoutScore > 99.f);

	const FHyperAIImageCompareReport Bigger = Measure(Reference, Scene(640, 400));
	TestTrue(FString::Printf(TEXT("The same picture at twice the size still matches (score %.1f)"), Bigger.Score), Bigger.Score > 95.f);

	const FHyperAIImageCompareReport Dark = Measure(Reference, Edited(Reference, [](int32, int32, FColor& Pixel)
	{
		Pixel = FColor(Darker(Pixel.R, 0.5f), Darker(Pixel.G, 0.5f), Darker(Pixel.B, 0.5f));
	}));
	TestTrue(FString::Printf(TEXT("Half the light reads as about one stop to add (%.2f EV)"), Dark.ExposureDeltaEV),
		Dark.ExposureDeltaEV > 0.7f && Dark.ExposureDeltaEV < 1.3f);
	TestTrue(TEXT("and the first suggestion is to brighten"), !Dark.Suggestions.IsEmpty() && Dark.Suggestions[0].StartsWith(TEXT("Brighten")));
	TestTrue(FString::Printf(TEXT("An overall shift costs tone (%.0f), not layout (%.0f)"), Dark.ToneScore, Dark.LayoutScore),
		Dark.ToneScore < 80.f && Dark.LayoutScore > 85.f);

	const FHyperAIImageCompareReport Cold = Measure(Reference, Edited(Reference, [](int32, int32, FColor& Pixel) { Swap(Pixel.R, Pixel.B); }));
	TestTrue(FString::Printf(TEXT("Swapping red and blue costs the palette (%.0f)"), Cold.PaletteScore), Cold.PaletteScore < 60.f);
	TestTrue(TEXT("and says to warm it"), Cold.Suggestions.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("Warm it")); }));

	const FHyperAIImageCompareReport Flipped = Measure(Reference, Edited(Reference, [&Reference](const int32 X, const int32 Y, FColor& Pixel)
	{
		Pixel = Reference.Pixels[(Reference.Height - 1 - Y) * Reference.Width + X];
	}), TEXT("aligned"));
	TestTrue(FString::Printf(TEXT("Upside down keeps the palette (%.0f) but loses layout (%.0f) and structure (%.0f)"),
		Flipped.PaletteScore, Flipped.LayoutScore, Flipped.StructureScore), Flipped.PaletteScore > 90.f && Flipped.LayoutScore < 60.f && Flipped.StructureScore < 50.f);

	FRandomStream Random(7);
	const FHyperAIImageCompareReport Noisy = Measure(Reference, Edited(Reference, [&Random](int32, const int32 Y, FColor& Pixel)
	{
		const int32 Noise = Random.RandRange(-45, 45);
		Pixel = FColor(FMath::Clamp(Pixel.R + Noise, 0, 255), FMath::Clamp(Pixel.G + Noise, 0, 255), FMath::Clamp(Pixel.B + Noise, 0, 255));
	}));
	TestTrue(FString::Printf(TEXT("Grain costs detail (%.0f)"), Noisy.DetailScore), Noisy.DetailScore < 70.f);
	TestTrue(TEXT("and the cells say busier"), Noisy.Regions.ContainsByPredicate([](const FHyperAIImageRegion& Region) { return Region.Difference.Contains(TEXT("busier")); }));

	const FHyperAIImageCompareReport Local = Measure(Reference, Edited(Reference, [&Reference](const int32 X, const int32 Y, FColor& Pixel)
	{
		if (X < Reference.Width / 4 && Y < Reference.Height / 4)
		{
			Pixel = FColor(Darker(Pixel.R, 0.25f), Darker(Pixel.G, 0.25f), Darker(Pixel.B, 0.25f));
		}
	}));
	const FHyperAIImageRegion* Worst = Local.Regions.IsEmpty() ? nullptr : &Local.Regions[0];
	TestTrue(TEXT("A darkened corner is the worst cell"), Worst && Worst->Row == 0 && Worst->Column == 0);
	TestTrue(TEXT("named top left and darker, with both colours given"), Worst && Worst->Name.StartsWith(TEXT("top left"))
		&& Worst->Difference.Contains(TEXT("darker")) && Worst->ReferenceColor.R > Worst->CandidateColor.R);
	TestEqual(TEXT("Every cell of the grid is reported"), Local.Regions.Num(), 16);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageMatchSessionsTest,
	"HyperAIStudio.Lighting.ImageMatch.SessionsRememberAndStop",
	HyperAIStudio::Lighting::MatchTests::Flags)

bool FHyperAIStudioImageMatchSessionsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::MatchTests;
	const FString SessionId = TEXT("test_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(10);
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*(Match::SessionsFolder() / SessionId), false, true);
		IFileManager::Get().DeleteDirectory(*Folder(), false, true);
	};
	const FPicture Reference = Scene();
	const FString ReferenceFile = Save(Reference, TEXT("reference.png"));
	auto Attempt = [&](const float Light, const FString& Name)
	{
		return Save(Edited(Reference, [Light](int32, int32, FColor& Pixel)
		{
			Pixel = FColor(Darker(Pixel.R, Light), Darker(Pixel.G, Light), Darker(Pixel.B, Light));
		}), Name);
	};

	FHyperAIImageCompareRequest Request;
	Request.ReferenceImagePath = ReferenceFile;
	Request.SessionId = SessionId;
	Request.TargetScore = 99.5f;
	const float Lights[] = {0.25f, 0.5f, 0.8f, 0.8f, 0.8f, 0.8f};
	FHyperAIImageCompareReport Report;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Lights); ++Index)
	{
		Request.CandidateImagePath = Attempt(Lights[Index], FString::Printf(TEXT("attempt_%d.png"), Index));
		Request.Note = FString::Printf(TEXT("light x%.2f"), Lights[Index]);
		Report = HyperAIStudio::Lighting::Match::Compare(Request);
		if (!TestTrue(FString::Printf(TEXT("iteration %d compares (%s)"), Index, *Report.Diagnostic), Report.bOk))
		{
			return false;
		}
		if (Index == 1)
		{
			TestTrue(FString::Printf(TEXT("More light scores better (%+.1f)"), Report.Improvement), Report.Improvement > 1.f);
			TestFalse(TEXT("and is no plateau"), Report.bPlateau);
		}
	}
	TestEqual(TEXT("Every iteration is in the history"), Report.History.Num(), 6);
	TestEqual(TEXT("with its note"), Report.History[0].Note, FString(TEXT("light x0.25")));
	TestTrue(TEXT("The best is the first of the brightest attempts"), Report.BestIteration == 2);
	TestTrue(TEXT("Three attempts without a point gained is a plateau"), Report.bPlateau);
	TestTrue(TEXT("which leads the suggestions"), !Report.Suggestions.IsEmpty() && Report.Suggestions[0].Contains(TEXT("different lever")));
	for (const FHyperAIImageIteration& Iteration : Report.History)
	{
		TestTrue(TEXT("each keeps its own copy of the candidate"), Iteration.CandidateImagePath.Contains(SessionId)
			&& IFileManager::Get().FileExists(*(FPaths::ProjectDir() / Iteration.CandidateImagePath)));
	}
	TestTrue(TEXT("The side-by-side and the heat map are written"), IFileManager::Get().FileExists(*(FPaths::ProjectDir() / Report.SideBySidePath))
		&& IFileManager::Get().FileExists(*(FPaths::ProjectDir() / Report.DiffPath)));
	TestEqual(TEXT("The session is found by its reference"), Match::FindSessionFor(ReferenceFile), SessionId);
	FString StoredReference;
	FString Mode;
	TArray<FHyperAIImageIteration> Stored;
	float Target = 0.f;
	TestTrue(TEXT("and read back"), Match::ReadSession(SessionId, StoredReference, Mode, Stored, Target) && Stored.Num() == 6 && Target == 99.5f);

	Request.ReferenceImagePath = Save(Edited(Reference, [](int32, int32, FColor& Pixel) { Swap(Pixel.R, Pixel.G); }), TEXT("other.png"));
	TestEqual(TEXT("A session refuses a different reference"), HyperAIStudio::Lighting::Match::Compare(Request).Status,
		FString(TEXT("session_reference_changed")));
	Request.SessionId = TEXT("../escape");
	TestEqual(TEXT("and a session id that is not a folder name"), HyperAIStudio::Lighting::Match::Compare(Request).Status, FString(TEXT("invalid_request")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioImageMatchLoopTest,
	"HyperAIStudio.Lighting.ImageMatch.MaterialLoopReachesTarget",
	HyperAIStudio::Lighting::MatchTests::Flags)

bool FHyperAIStudioImageMatchLoopTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::MatchTests;
	namespace Capture = HyperAIStudio::Lighting::Capture;
	UWorld* World = HyperAIStudio::Lighting::Gate::GetEditorWorld();
	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (!FApp::CanEverRender() || !World || !PlaneMesh)
	{
		AddWarning(TEXT("Needs a rendering editor with a level open."));
		return true;
	}

	// An unlit surface: Tint times Brightness, shaded across by its texture coordinate, so it has a layout to match.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	UMaterialExpressionVectorParameter* Tint = NewObject<UMaterialExpressionVectorParameter>(Material);
	Tint->ParameterName = TEXT("Tint");
	Tint->DefaultValue = FLinearColor::White;
	UMaterialExpressionScalarParameter* Brightness = NewObject<UMaterialExpressionScalarParameter>(Material);
	Brightness->ParameterName = TEXT("Brightness");
	Brightness->DefaultValue = 1.f;
	UMaterialExpressionTextureCoordinate* Coordinate = NewObject<UMaterialExpressionTextureCoordinate>(Material);
	UMaterialExpressionComponentMask* Across = NewObject<UMaterialExpressionComponentMask>(Material);
	Across->R = true;
	Across->Input.Expression = Coordinate;
	UMaterialExpressionMultiply* Shaded = NewObject<UMaterialExpressionMultiply>(Material);
	Shaded->A.Expression = Tint;
	Shaded->B.Expression = Across;
	UMaterialExpressionMultiply* Lit = NewObject<UMaterialExpressionMultiply>(Material);
	Lit->A.Expression = Shaded;
	Lit->B.Expression = Brightness;
	for (UMaterialExpression* Expression : TArray<UMaterialExpression*>{Tint, Brightness, Coordinate, Across, Shaded, Lit})
	{
		Material->GetExpressionCollection().AddExpression(Expression);
	}
	Material->GetEditorOnlyData()->EmissiveColor.Expression = Lit;
	Material->SetShadingModel(MSM_Unlit);
	Material->PostEditChange();
	Material->ForceRecompileForRendering();
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Material, GetTransientPackage());

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6);
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	// Two kilometres up, clear of the level and of height fog.
	AStaticMeshActor* Plane = World->SpawnActor<AStaticMeshActor>(FVector(0.0, 0.0, 200000.0), FRotator::ZeroRotator, Spawn);
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(FVector(0.0, 0.0, 200250.0), FRotator(-90.0, 0.0, 0.0), Spawn);
	ON_SCOPE_EXIT
	{
		if (Plane) Plane->Destroy();
		if (Camera) Camera->Destroy();
	};
	if (!TestNotNull(TEXT("plane spawned"), Plane) || !TestNotNull(TEXT("camera spawned"), Camera) || !TestNotNull(TEXT("instance made"), Instance))
	{
		return false;
	}
	Plane->GetStaticMeshComponent()->UnregisterComponent();
	Plane->GetStaticMeshComponent()->SetStaticMesh(PlaneMesh);
	Plane->GetStaticMeshComponent()->SetMaterial(0, Instance);
	Plane->GetStaticMeshComponent()->RegisterComponent();
	Plane->SetActorScale3D(FVector(20.0));
	Camera->SetActorLabel(TEXT("CAM_HyperAIMatchTest_") + Suffix);
	Camera->GetCameraComponent()->SetFieldOfView(60.f);
	// Fixed exposure, as the match skill asks for: auto exposure would re-brighten every change the loop makes.
	FPostProcessSettings& Post = Camera->GetCameraComponent()->PostProcessSettings;
	Post.bOverride_AutoExposureMethod = true;
	Post.AutoExposureMethod = AEM_Manual;
	Post.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Post.AutoExposureApplyPhysicalCameraExposure = false;
	Post.bOverride_AutoExposureBias = true;
	Post.AutoExposureBias = 0.f;
	Camera->GetCameraComponent()->PostProcessBlendWeight = 1.f;
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	const FMaterialResource* Resource = World->Scene ? Material->GetMaterialResource(World->Scene->GetShaderPlatform()) : nullptr;
	if (!TestTrue(TEXT("the test material compiled"), Resource && Resource->GetGameThreadShaderMap() && Resource->GetCompileErrors().IsEmpty()))
	{
		return false;
	}

	auto Render = [&](const FLinearColor& Color, const float Light, const FString& Label)
	{
		Instance->SetVectorParameterValue(TEXT("Tint"), Color);
		Instance->SetScalarParameterValue(TEXT("Brightness"), Light);
		FHyperAICaptureViewRequest View;
		View.Source = TEXT("camera");
		View.CameraActorLabel = Camera->GetActorLabel();
		View.Width = 320;
		View.Height = 200;
		View.WarmupFrames = 2;
		View.Label = Label;
		const FHyperAICaptureViewReport Frames = Capture::RunNow(View);
		return Frames.bOk && !Frames.Frames.IsEmpty() ? Frames.Frames[0].ImagePath : FString();
	};

	// The target: warm and bright. The attempt starts cold and dim, and moves only by what the compare reports.
	const FString Reference = Render(FLinearColor(1.f, 0.42f, 0.16f), 3.f, TEXT("match-reference-") + Suffix);
	if (!TestFalse(TEXT("the reference rendered"), Reference.IsEmpty()))
	{
		return false;
	}
	const FString SessionId = TEXT("hyperai_test_material_loop");
	IFileManager::Get().DeleteDirectory(*(Match::SessionsFolder() / SessionId), false, true);
	FLinearColor Color(0.3f, 0.55f, 1.f);
	float Light = 0.5f;
	FHyperAIImageCompareReport Report;
	float FirstScore = -1.f;
	for (int32 Iteration = 0; Iteration < 10; ++Iteration)
	{
		FHyperAIImageCompareRequest Request;
		Request.ReferenceImagePath = Reference;
		Request.CandidateImagePath = Render(Color, Light, FString::Printf(TEXT("match-attempt-%s-%d"), *Suffix, Iteration));
		Request.Mode = TEXT("aligned");
		Request.TargetScore = 90.f;
		Request.SessionId = SessionId;
		Request.Note = FString::Printf(TEXT("tint (%.2f, %.2f, %.2f), brightness %.2f"), Color.R, Color.G, Color.B, Light);
		Report = HyperAIStudio::Lighting::Match::Compare(Request);
		if (!TestTrue(FString::Printf(TEXT("iteration %d compares: %s"), Iteration, *Report.Diagnostic), Report.bOk))
		{
			return false;
		}
		AddInfo(FString::Printf(TEXT("Iteration %d: score %.1f (%s)"), Iteration, Report.Score, *Request.Note));
		if (FirstScore < 0.f)
		{
			FirstScore = Report.Score;
		}
		if (Report.bMatched)
		{
			break;
		}
		// The adjuster an agent would be: brightness from the reported stops, colour from the average colours.
		Light *= FMath::Pow(2.f, FMath::Clamp(Report.ExposureDeltaEV, -2.f, 2.f));
		auto Linear = [](const float Srgb) { return Srgb <= 0.04045f ? Srgb / 12.92f : FMath::Pow((Srgb + 0.055f) / 1.055f, 2.4f); };
		float* Channels[] = {&Color.R, &Color.G, &Color.B};
		const float Reference3[] = {Report.ReferenceMeanColor.R, Report.ReferenceMeanColor.G, Report.ReferenceMeanColor.B};
		const float Candidate3[] = {Report.CandidateMeanColor.R, Report.CandidateMeanColor.G, Report.CandidateMeanColor.B};
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			const float Ratio = (Linear(Reference3[Channel]) + 0.002f) / (Linear(Candidate3[Channel]) + 0.002f);
			*Channels[Channel] *= FMath::Pow(FMath::Clamp(Ratio, 0.25f, 4.f), 0.8f);
		}
		const float Peak = FMath::Max3(Color.R, Color.G, Color.B);
		Color /= FMath::Max(Peak, 0.001f);
		Color.A = 1.f;
	}
	TestTrue(FString::Printf(TEXT("The loop reaches the target of 90 (score %.1f after %d iterations)"), Report.Score, Report.History.Num()),
		Report.bMatched);
	TestTrue(FString::Printf(TEXT("from a start of %.1f"), FirstScore), Report.Score - FirstScore > 20.f);
	TestEqual(TEXT("The session holds every iteration"), Match::FindSessionFor(Reference), SessionId);
	return true;
}

#endif
