// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HyperAIStudioLightingToolset.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioLightingCapture.h"
#include "HyperAIStudioLightingGate.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/SpotLight.h"
#include "Engine/Scene.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Components/BoxReflectionCaptureComponent.h"
#include "Components/SphereReflectionCaptureComponent.h"
#include "Engine/BoxReflectionCapture.h"
#include "Engine/SphereReflectionCapture.h"
#include "HyperAIStudioLightingLookDev.h"
#include "UObject/UObjectIterator.h"
#include "MaterialShared.h"
#include "SceneInterface.h"
#include "ShaderCompiler.h"
#include "HyperAIStudioPreviewPanel.h"
#include "HyperAIStudioSettings.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::Lighting::Tests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	constexpr int32 Side = 64;

	TArray<FColor> Solid(const FColor Colour)
	{
		TArray<FColor> Pixels;
		Pixels.Init(Colour, Side * Side);
		return Pixels;
	}

	FHyperAILightingOp Op(const TCHAR* Kind, const TCHAR* Property = TEXT(""), const TCHAR* Value = TEXT(""), const TCHAR* Label = TEXT(""))
	{
		FHyperAILightingOp Result;
		Result.Kind = Kind;
		Result.Property = Property;
		Result.Value = Value;
		Result.ActorLabel = Label;
		return Result;
	}

	FString ValueOf(const TArray<FHyperAILightingActorRecord>& Actors, const TCHAR* Kind, const TCHAR* Key)
	{
		for (const FHyperAILightingActorRecord& Actor : Actors)
		{
			if (Actor.Kind != Kind) continue;
			for (const FHyperAILightingProperty& Property : Actor.Properties)
			{
				if (Property.Key == Key) return Property.Value;
			}
		}
		return TEXT("<missing>");
	}

	bool PumpUntil(TFunctionRef<bool()> Done, const double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Deadline) return false;
			FTSTicker::GetCoreTicker().Tick(0.016f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FlushRenderingCommands();
		}
		return true;
	}

	/** A lit material with the parameters a look-dev change takes, in an unsaved package of its own, as a project's material would be. */
	UMaterial* MakeLookDevMaterial(const FString& Name)
	{
		UPackage* Package = CreatePackage(*(TEXT("/Temp/HyperAILookDevTests/") + Name));
		UMaterial* Material = NewObject<UMaterial>(Package, *Name, RF_Public | RF_Standalone);
		auto Scalar = [Material](const TCHAR* ParameterName, const float Value)
		{
			UMaterialExpressionScalarParameter* Expression = NewObject<UMaterialExpressionScalarParameter>(Material);
			Expression->ParameterName = ParameterName;
			Expression->DefaultValue = Value;
			return Expression;
		};
		UMaterialExpressionScalarParameter* Roughness = Scalar(TEXT("Roughness"), 0.8f);
		UMaterialExpressionScalarParameter* Specular = Scalar(TEXT("Specular"), 0.1f);
		UMaterialExpressionVectorParameter* Colour = NewObject<UMaterialExpressionVectorParameter>(Material);
		Colour->ParameterName = TEXT("BaseColor");
		Colour->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f, 0.25f);
		UMaterialExpressionTextureSampleParameter2D* Detail = NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
		Detail->ParameterName = TEXT("Detail");
		Detail->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		UMaterialExpressionMultiply* Tinted = NewObject<UMaterialExpressionMultiply>(Material);
		Tinted->A.Expression = Colour;
		Tinted->B.Expression = Detail;
		UMaterialExpressionConstant* One = NewObject<UMaterialExpressionConstant>(Material);
		One->R = 1.f;
		UMaterialExpressionConstant* Zero = NewObject<UMaterialExpressionConstant>(Material);
		UMaterialExpressionStaticSwitchParameter* Wet = NewObject<UMaterialExpressionStaticSwitchParameter>(Material);
		Wet->ParameterName = TEXT("Wet");
		Wet->A.Expression = One;
		Wet->B.Expression = Zero;
		for (UMaterialExpression* Expression : TArray<UMaterialExpression*>{Roughness, Specular, Colour, Detail, Tinted, One, Zero, Wet})
		{
			Material->GetExpressionCollection().AddExpression(Expression);
		}
		UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
		Inputs->BaseColor.Expression = Tinted;
		Inputs->Roughness.Expression = Roughness;
		Inputs->Specular.Expression = Specular;
		Inputs->Metallic.Expression = Wet;
		Material->PostEditChange();
		return Material;
	}

	/** What a test made is never saved, so it is only forgotten. */
	void Forget(UObject* Object)
	{
		FAssetRegistryModule::AssetDeleted(Object);
		Object->GetPackage()->SetDirtyFlag(false);
		Object->ClearFlags(RF_Public | RF_Standalone);
		Object->MarkAsGarbage();
	}

	AStaticMeshActor* PlaceMesh(UWorld& World, UStaticMesh* Mesh, const FVector& Location, const TCHAR* Label, UMaterialInterface* Material = nullptr)
	{
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transactional;
		AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator, Spawn);
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		Component->UnregisterComponent();
		Component->SetStaticMesh(Mesh);
		if (Material)
		{
			Component->SetMaterial(0, Material);
		}
		Component->RegisterComponent();
		Actor->SetActorLabel(Label);
		return Actor;
	}

	FString ParameterOf(const FHyperAILightingMaterialRecord& Record, const TCHAR* Name)
	{
		const FHyperAILightingProperty* Found = Record.Parameters.FindByPredicate([Name](const FHyperAILightingProperty& Property) { return Property.Key == Name; });
		return Found ? Found->Value : FString(TEXT("<missing>"));
	}

	FString RenderingFact(UWorld& World, const TCHAR* Key)
	{
		for (const FHyperAILightingProperty& Fact : HyperAIStudio::Lighting::Gate::ReadRendering(World))
		{
			if (Fact.Key == Key) return Fact.Value;
		}
		return TEXT("<missing>");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingCohortTest,
	"HyperAIStudio.Lighting.Contracts.ExactFiveToolCohort",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingCohortTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Names = FHyperAIStudioLightingContracts::GetToolNames();
	TestEqual(TEXT("five tools"), Names.Num(), 5);
	const FHyperAIStudioDomainAdapterDescriptor& Descriptor = FHyperAIStudioLightingContracts::GetAdapterDescriptor();
	TestEqual(TEXT("one variant per tool"), Descriptor.Variants.Num(), Names.Num());
	TestFalse(TEXT("adapter fingerprint sealed"), Descriptor.AdapterFingerprint.IsEmpty());
	int32 Callables = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioLightingToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->HasMetaData(TEXT("AICallable"))) continue;
		++Callables;
		TestTrue(*FString::Printf(TEXT("%s is a manifest tool"), *It->GetName()), Names.Contains(It->GetName()));
		const FString ToolTip = It->GetMetaData(TEXT("ToolTip"));
		TestTrue(*FString::Printf(TEXT("%s has a description within 400 characters"), *It->GetName()), !ToolTip.IsEmpty() && ToolTip.Len() <= 400);
	}
	TestEqual(TEXT("AICallable functions match the manifest"), Callables, Names.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingPreviewSourceTest,
	"HyperAIStudio.Lighting.Preview.SourceIsRegistered",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingPreviewSourceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FHyperAIStudioPreviewPanel::FSource* Source = FHyperAIStudioPreviewPanel::FindSource(FName(TEXT("lighting")));
	if (!TestNotNull(TEXT("the lighting module offers the preview panel a source"), Source))
	{
		return false;
	}
	TestTrue(TEXT("which needs a reference image"), Source->bNeedsReference);
	TestTrue(TEXT("and is bound"), Source->Refresh.IsBound());

	// Without a readable reference the source reports why rather than half-filling the panel.
	FHyperAIStudioPreviewComparison Comparison;
	FString Error;
	TestFalse(TEXT("a missing reference fails"), Source->Refresh.Execute(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/not_here.png"), Comparison, Error));
	TestFalse(TEXT("with a reason"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingImageMetricsTest,
	"HyperAIStudio.Lighting.Metrics.MeasureAndCompare",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingImageMetricsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;

	// sRGB 118 is linear 0.18: photographic middle grey.
	const FHyperAILightingImageMetrics Grey = Gate::MeasurePixels(Solid(FColor(118, 118, 118)), Side, Side);
	TestTrue(TEXT("grey measures"), Grey.bValid);
	TestTrue(*FString::Printf(TEXT("median luminance is middle grey (%f)"), Grey.LuminanceP50), FMath::IsNearlyEqual(Grey.LuminanceP50, 0.18f, 0.005f));
	TestTrue(*FString::Printf(TEXT("neutral grey is about 6500 K (%f)"), Grey.ColorTemperatureK), FMath::IsNearlyEqual(Grey.ColorTemperatureK, 6500.f, 200.f));
	TestEqual(TEXT("grey has no saturation"), Grey.MeanSaturation, 0.f);
	TestTrue(TEXT("flat image has contrast 1"), FMath::IsNearlyEqual(Grey.ContrastRatio, 1.f, 0.001f));
	float Sum = 0.f;
	for (const float Bin : Grey.Histogram) Sum += Bin;
	TestEqual(TEXT("16 histogram bins"), Grey.Histogram.Num(), 16);
	TestTrue(TEXT("histogram sums to 1"), FMath::IsNearlyEqual(Sum, 1.f, 0.0001f));

	const FHyperAILightingImageMetrics Warm = Gate::MeasurePixels(Solid(FColor(240, 180, 120)), Side, Side);
	const FHyperAILightingImageMetrics Cool = Gate::MeasurePixels(Solid(FColor(150, 190, 240)), Side, Side);
	TestTrue(*FString::Printf(TEXT("orange reads warm (%f K)"), Warm.ColorTemperatureK), Warm.ColorTemperatureK < 4000.f);
	TestTrue(*FString::Printf(TEXT("blue reads cool (%f K)"), Cool.ColorTemperatureK), Cool.ColorTemperatureK > 9000.f);
	TestTrue(TEXT("colour is saturated"), Warm.MeanSaturation > 0.4f);

	TArray<FColor> Split = Solid(FColor::Black);
	for (int32 Index = Split.Num() / 2; Index < Split.Num(); ++Index) Split[Index] = FColor::White;
	const FHyperAILightingImageMetrics Hard = Gate::MeasurePixels(Split, Side, Side);
	TestTrue(*FString::Printf(TEXT("black and white is high contrast (%f)"), Hard.ContrastRatio), Hard.ContrastRatio > 100.f);
	TestTrue(TEXT("half the pixels are shadow"), FMath::IsNearlyEqual(Hard.ShadowFraction, 0.5f, 0.01f));
	TestTrue(TEXT("half the pixels clip"), FMath::IsNearlyEqual(Hard.HighlightFraction, 0.5f, 0.01f));

	const FHyperAILightingImageMetrics Black = Gate::MeasurePixels(Solid(FColor::Black), Side, Side);
	const FHyperAILightingImageMetrics White = Gate::MeasurePixels(Solid(FColor::White), Side, Side);
	TestEqual(TEXT("an image matches itself"), Gate::HistogramDistance(Grey.Histogram, Grey.Histogram), 0.f);
	TestTrue(TEXT("black and white are as far apart as possible"), FMath::IsNearlyEqual(Gate::HistogramDistance(Black.Histogram, White.Histogram), 1.f, 0.0001f));
	TestFalse(TEXT("wrong pixel count does not measure"), Gate::MeasurePixels(Solid(FColor::Black), Side, Side + 1).bValid);

	// sRGB 60 is linear 0.045: two stops under middle grey.
	const FHyperAILightingImageMetrics Dark = Gate::MeasurePixels(Solid(FColor(60, 60, 60)), Side, Side);
	FHyperAILightingCompareReport Locked;
	Gate::CompareMetrics(Grey, Dark, /*bExposureLocked=*/true, 0.05f, Locked);
	TestTrue(*FString::Printf(TEXT("two stops brighter wanted (%f)"), Locked.ExposureDeltaEV), FMath::IsNearlyEqual(Locked.ExposureDeltaEV, 2.f, 0.1f));
	TestFalse(TEXT("not matched"), Locked.bMatched);
	TestTrue(TEXT("first suggestion brightens through the locked exposure: ") + (Locked.Suggestions.IsEmpty() ? FString() : Locked.Suggestions[0]),
		!Locked.Suggestions.IsEmpty() && Locked.Suggestions[0].StartsWith(TEXT("Brighten")) && Locked.Suggestions[0].Contains(TEXT("lock_exposure with EV100 lowered")));
	FHyperAILightingCompareReport Unlocked;
	Gate::CompareMetrics(Grey, Dark, /*bExposureLocked=*/false, 0.05f, Unlocked);
	TestTrue(TEXT("auto exposure is flagged first"), !Unlocked.Suggestions.IsEmpty() && Unlocked.Suggestions[0].Contains(TEXT("lock_exposure first")));
	FHyperAILightingCompareReport Warmer;
	Gate::CompareMetrics(Warm, Cool, true, 0.05f, Warmer);
	TestTrue(TEXT("a warmer reference asks for warmth"), Warmer.ColorTemperatureDeltaK < 0.f
		&& Warmer.Suggestions.ContainsByPredicate([](const FString& Text) { return Text.StartsWith(TEXT("Warm the image")); }));
	FHyperAILightingCompareReport Same;
	Gate::CompareMetrics(Grey, Grey, true, 0.05f, Same);
	TestTrue(TEXT("identical images match"), Same.bMatched && Same.HistogramDistance == 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingCompareFilesTest,
	"HyperAIStudio.Lighting.Metrics.CompareImageFiles",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingCompareFilesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	const FString Folder = FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests");
	TestTrue(TEXT("reference written"), Gate::WritePng(Folder / TEXT("lighting_reference.png"), Solid(FColor(118, 118, 118)), Side, Side));
	TestTrue(TEXT("current written"), Gate::WritePng(Folder / TEXT("lighting_current.png"), Solid(FColor(60, 60, 60)), Side, Side));

	FHyperAILightingCompareRequest Request;
	Request.ReferenceImagePath = TEXT("Saved/HyperAIStudio/Tests/lighting_reference.png");
	Request.CurrentImagePath = FPaths::ConvertRelativePathToFull(Folder / TEXT("lighting_current.png"));
	const FHyperAILightingCompareReport Report = FHyperAIStudioLightingContracts::Compare(Request);
	TestTrue(*FString::Printf(TEXT("compared (%s: %s)"), *Report.Status, *Report.Diagnostic), Report.bOk);
	TestEqual(TEXT("status says it differs"), Report.Status, FString(TEXT("differs")));
	TestTrue(TEXT("PNG round trip keeps the two-stop gap"), FMath::IsNearlyEqual(Report.ExposureDeltaEV, 2.f, 0.1f));
	TestTrue(TEXT("a supplied image writes no capture"), Report.CurrentImageWrittenPath.IsEmpty());

	Request.CurrentImagePath = Request.ReferenceImagePath;
	TestEqual(TEXT("an image matches itself"), FHyperAIStudioLightingContracts::Compare(Request).Status, FString(TEXT("matched")));

	Request.ReferenceImagePath = TEXT("../../outside_the_project.png");
	const FHyperAILightingCompareReport Outside = FHyperAIStudioLightingContracts::Compare(Request);
	TestEqual(TEXT("paths outside the project are refused"), Outside.Status, FString(TEXT("reference_unreadable")));
	TestTrue(TEXT("and the reason says so"), Outside.Diagnostic.Contains(TEXT("inside the project")));
	Request.ReferenceImagePath = TEXT("Saved/HyperAIStudio/Tests/lighting_reference.exr");
	TestTrue(TEXT("HDR and other formats are refused"), FHyperAIStudioLightingContracts::Compare(Request).Diagnostic.Contains(TEXT("PNG, JPG")));
	Request.ReferenceImagePath = TEXT("Saved/HyperAIStudio/Tests/lighting_reference.png");
	Request.MatchThreshold = 0.f;
	TestEqual(TEXT("threshold must be positive"), FHyperAIStudioLightingContracts::Compare(Request).Status, FString(TEXT("invalid_request")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingOpsTest,
	"HyperAIStudio.Lighting.Ops.ValidateApplyAndUndo",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingOpsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILightingTestWorld"));
	if (!World)
	{
		AddError(TEXT("Could not create a test world."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};
	TestEqual(TEXT("a new world has no lighting"), Gate::ReadActors(*World).Num(), 0);

	auto Rejects = [&](const TArray<FHyperAILightingOp>& Ops, const TCHAR* Expected)
	{
		FString Error;
		const bool bValid = Gate::ValidateOps(*World, Ops, Error);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Error), !bValid && Error.Contains(Expected));
	};
	Rejects({Op(TEXT("set_moon"), TEXT("intensity"), TEXT("1"))}, TEXT("Unknown kind"));
	Rejects({Op(TEXT("set_sun"), TEXT("intensity"), TEXT("999999"))}, TEXT("from 0 to 200000"));
	Rejects({Op(TEXT("set_sun"), TEXT("intensity"), TEXT("nan"))}, TEXT("needs a number"));
	Rejects({Op(TEXT("set_sun"), TEXT("color"), TEXT("1,1"))}, TEXT("r,g,b"));
	Rejects({Op(TEXT("set_post_process"), TEXT("shadows_offset"), TEXT("0.9,0,0"))}, TEXT("from -0.5 to 0.5"));
	Rejects({Op(TEXT("set_sun"), TEXT("luminance"), TEXT("1"))}, TEXT("no setting 'luminance'"));
	Rejects({Op(TEXT("lock_exposure"))}, TEXT("EV100"));
	Rejects({Op(TEXT("lock_exposure"), TEXT(""), TEXT("12")), Op(TEXT("set_post_process"), TEXT("exposure_bias"), TEXT("1"))}, TEXT("exposure is locked"));
	Rejects({Op(TEXT("set_sun"), TEXT("intensity"), TEXT("5"), TEXT("Nope"))}, TEXT("no sun actor is labelled"));
	FString Error;
	TestTrue(TEXT("a later op may name the actor an earlier op adds"), Gate::ValidateOps(*World,
		{Op(TEXT("set_sun"), TEXT("intensity"), TEXT("5")), Op(TEXT("set_sun"), TEXT("temperature"), TEXT("4000"), TEXT("HyperAI_Sun"))}, Error));
	TestEqual(TEXT("validation adds nothing"), Gate::ReadActors(*World).Num(), 0);

	const FString Before = Gate::ComputeRevision(*World);
	const TArray<FHyperAILightingOp> Ops = {
		Op(TEXT("set_sun"), TEXT("intensity"), TEXT("7.5")),
		Op(TEXT("set_sun"), TEXT("temperature"), TEXT("4500")),
		Op(TEXT("set_sun"), TEXT("pitch"), TEXT("-20")),
		Op(TEXT("set_sun"), TEXT("color"), TEXT("1,0.5,0.25")),
		Op(TEXT("set_fog"), TEXT("density"), TEXT("0.05")),
		Op(TEXT("set_sky_light"), TEXT("intensity"), TEXT("2")),
		Op(TEXT("set_sky_atmosphere"), TEXT("mie_scale"), TEXT("1.5")),
		Op(TEXT("set_post_process"), TEXT("saturation"), TEXT("0.8")),
		Op(TEXT("set_post_process"), TEXT("shadows_gain"), TEXT("1,0.8,1.6")),
		Op(TEXT("set_post_process"), TEXT("shadows_offset"), TEXT("0.04,0,-0.02")),
		Op(TEXT("set_post_process"), TEXT("highlights_gain"), TEXT("1.1,1,0.9")),
		Op(TEXT("lock_exposure"), TEXT(""), TEXT("12"))};
	TArray<AActor*> Touched;
	TestTrue(TEXT("plan applies: ") + Error, Gate::ApplyOps(*World, Ops, Touched, Error));
	TestEqual(TEXT("one actor per kind was added"), Touched.Num(), 5);
	const TArray<FHyperAILightingActorRecord> After = Gate::ReadActors(*World);
	TestEqual(TEXT("sun intensity"), ValueOf(After, TEXT("sun"), TEXT("intensity")), FString(TEXT("7.5")));
	TestEqual(TEXT("sun temperature"), ValueOf(After, TEXT("sun"), TEXT("temperature")), FString(TEXT("4500")));
	TestEqual(TEXT("temperature switched on"), ValueOf(After, TEXT("sun"), TEXT("use_temperature")), FString(TEXT("true")));
	TestEqual(TEXT("sun pitch"), ValueOf(After, TEXT("sun"), TEXT("pitch")), FString(TEXT("-20")));
	TestEqual(TEXT("fog density"), ValueOf(After, TEXT("fog"), TEXT("density")), FString(TEXT("0.05")));
	TestEqual(TEXT("sky light intensity"), ValueOf(After, TEXT("sky_light"), TEXT("intensity")), FString(TEXT("2")));
	TestEqual(TEXT("mie scale"), ValueOf(After, TEXT("sky_atmosphere"), TEXT("mie_scale")), FString(TEXT("1.5")));
	TestEqual(TEXT("saturation"), ValueOf(After, TEXT("post_process"), TEXT("saturation")), FString(TEXT("0.8")));
	TestEqual(TEXT("shadows graded apart"), ValueOf(After, TEXT("post_process"), TEXT("shadows_gain")), FString(TEXT("1,0.8,1.6")));
	TestEqual(TEXT("with a lift that may go below zero"), ValueOf(After, TEXT("post_process"), TEXT("shadows_offset")), FString(TEXT("0.04,0,-0.02")));
	TestEqual(TEXT("and highlights graded apart"), ValueOf(After, TEXT("post_process"), TEXT("highlights_gain")), FString(TEXT("1.1,1,0.9")));
	TestEqual(TEXT("exposure is manual"), ValueOf(After, TEXT("post_process"), TEXT("exposure_method")), FString(TEXT("manual")));
	TestEqual(TEXT("and locked at EV100 12"), ValueOf(After, TEXT("post_process"), TEXT("exposure_ev100")), FString(TEXT("12")));
	TestTrue(TEXT("the level reports exposure locked"), Gate::IsExposureLocked(*World));
	TestEqual(TEXT("added actors are labelled"), After[0].Label, FString(TEXT("HyperAI_Sun")));
	TestNotEqual(TEXT("the revision moved"), Gate::ComputeRevision(*World), Before);
	TestEqual(TEXT("every op resolves to its actor"), Gate::ResolveTargets(*World, Ops).Num(), 5);

	Touched.Reset();
	TestTrue(TEXT("unlock then bias applies: ") + Error, Gate::ApplyOps(*World,
		{Op(TEXT("unlock_exposure")), Op(TEXT("set_post_process"), TEXT("exposure_bias"), TEXT("1.5"))}, Touched, Error));
	const TArray<FHyperAILightingActorRecord> Unlocked = Gate::ReadActors(*World);
	TestEqual(TEXT("exposure back to the default method"), ValueOf(Unlocked, TEXT("post_process"), TEXT("exposure_method")), FString(TEXT("default")));
	TestEqual(TEXT("bias set"), ValueOf(Unlocked, TEXT("post_process"), TEXT("exposure_bias")), FString(TEXT("1.5")));
	TestEqual(TEXT("the existing volume was reused"), Unlocked.Num(), After.Num());

	// Every editor undo checks that no graph pin is left unresolved, and a package still loading in the background
	// holds pins that are; finish loading first so another asset's load cannot fail this test.
	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestEqual(TEXT("one undo restores the lock"), ValueOf(Gate::ReadActors(*World), TEXT("post_process"), TEXT("exposure_method")), FString(TEXT("manual")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingScatteringNotesTest,
	"HyperAIStudio.Lighting.Ops.SkyAtmosphereScaleNotes",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingScatteringNotesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	// The engine's own defaults, which the notes name, and the Details panel's range, which the ops take.
	const USkyAtmosphereComponent* Defaults = GetDefault<USkyAtmosphereComponent>();
	TestTrue(*FString::Printf(TEXT("the engine's default scales are 0.0331 and 0.003996 (%g, %g)"), Defaults->RayleighScatteringScale, Defaults->MieScatteringScale),
		FMath::IsNearlyEqual(Defaults->RayleighScatteringScale, 0.0331f, 1e-6f) && FMath::IsNearlyEqual(Defaults->MieScatteringScale, 0.003996f, 1e-7f));
	auto UIMax = [](const FName Name)
	{
		const FProperty* Property = FindFProperty<FProperty>(USkyAtmosphereComponent::StaticClass(), Name);
		return Property ? FCString::Atof(*Property->GetMetaData(TEXT("UIMax"))) : -1.f;
	};
	TestEqual(TEXT("the panel slides rayleigh_scale to 2"), UIMax(GET_MEMBER_NAME_CHECKED(USkyAtmosphereComponent, RayleighScatteringScale)), 2.f);
	TestEqual(TEXT("and mie_scale to 5"), UIMax(GET_MEMBER_NAME_CHECKED(USkyAtmosphereComponent, MieScatteringScale)), 5.f);

	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILightingScatteringWorld"));
	if (!TestNotNull(TEXT("a test world"), World)) return false;
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};
	FString Error;
	TArray<FString> Notes;
	TestTrue(TEXT("the panel's whole range is taken: ") + Error, Gate::ValidateOps(*World,
		{Op(TEXT("set_sky_atmosphere"), TEXT("rayleigh_scale"), TEXT("2")), Op(TEXT("set_sky_atmosphere"), TEXT("mie_scale"), TEXT("5"))}, Error));
	TestTrue(TEXT("and no more"), !Gate::ValidateOps(*World, {Op(TEXT("set_sky_atmosphere"), TEXT("rayleigh_scale"), TEXT("2.5"))}, Error) && Error.Contains(TEXT("from 0 to 2")));

	// The plan that filled a sky with orange haze: valid, and said why.
	TestTrue(TEXT("the hazy plan checks"), Gate::ValidateOps(*World,
		{Op(TEXT("set_sun"), TEXT("intensity"), TEXT("8")), Op(TEXT("set_sky_atmosphere"), TEXT("rayleigh_scale"), TEXT("1.1")),
			Op(TEXT("set_sky_atmosphere"), TEXT("mie_scale"), TEXT("1.4"))}, Error, &Notes));
	TestEqual(*FString::Printf(TEXT("with a note for each scale: %s"), *FString::Join(Notes, TEXT(" | "))), Notes.Num(), 2);
	if (Notes.Num() == 2)
	{
		TestTrue(TEXT("rayleigh_scale: how far past its default, and what it does"), Notes[0].StartsWith(TEXT("Op 1: set_sky_atmosphere rayleigh_scale 1.1 is 33x the default 0.0331: the sky turns to haze"))
			&& Notes[0].Contains(TEXT("values near 0.03 are normal")));
		TestTrue(TEXT("mie_scale too"), Notes[1].StartsWith(TEXT("Op 2: set_sky_atmosphere mie_scale 1.4 is 350x the default 0.003996: "))
			&& Notes[1].Contains(TEXT("values near 0.004 are normal")));
	}
	Notes.Reset();
	TestTrue(TEXT("values within ten times the default"), Gate::ValidateOps(*World,
		{Op(TEXT("set_sky_atmosphere"), TEXT("rayleigh_scale"), TEXT("0.3")), Op(TEXT("set_sky_atmosphere"), TEXT("mie_scale"), TEXT("0.03"))}, Error, &Notes));
	TestEqual(TEXT("pass without a note: ") + FString::Join(Notes, TEXT(" | ")), Notes.Num(), 0);

	// A sky as that plan left it. The same setting by its Details-panel label is noted too.
	ASkyAtmosphere* Atmosphere = World->SpawnActor<ASkyAtmosphere>();
	if (!TestTrue(TEXT("a sky atmosphere"), Atmosphere && Atmosphere->GetComponent())) return false;
	Atmosphere->GetComponent()->RayleighScatteringScale = 1.1f;
	Notes.Reset();
	TestTrue(TEXT("Mie Scattering Scale by name checks"), Gate::ValidateOps(*World, {Op(TEXT("set_sky_atmosphere"), TEXT("Mie Scattering Scale"), TEXT("0.2"))}, Error, &Notes));
	TestTrue(TEXT("and is noted as mie_scale: ") + FString::Join(Notes, TEXT(" | ")), Notes.Num() == 1 && Notes[0].Contains(TEXT("mie_scale 0.2 is 50x the default")));

	// Inspection shows the defaults beside the values, and the note on a scale far past its own.
	const TArray<FHyperAILightingActorRecord> Actors = Gate::ReadActors(*World);
	TestEqual(TEXT("the value set"), ValueOf(Actors, TEXT("sky_atmosphere"), TEXT("rayleigh_scale")), FString(TEXT("1.1")));
	TestEqual(TEXT("its default beside it"), ValueOf(Actors, TEXT("sky_atmosphere"), TEXT("rayleigh_scale_default")), FString(TEXT("0.0331")));
	TestEqual(TEXT("and mie_scale's"), ValueOf(Actors, TEXT("sky_atmosphere"), TEXT("mie_scale_default")), FString(TEXT("0.003996")));
	TestTrue(TEXT("with a note on the far one alone: ") + ValueOf(Actors, TEXT("sky_atmosphere"), TEXT("note")),
		ValueOf(Actors, TEXT("sky_atmosphere"), TEXT("note")).StartsWith(TEXT("rayleigh_scale 1.1 is 33x the default 0.0331"))
		&& !ValueOf(Actors, TEXT("sky_atmosphere"), TEXT("note")).Contains(TEXT("mie_scale")));

	// The dry run carries the notes, in the open level.
	const FHyperAILightingInspectReport Inspected = FHyperAIStudioLightingContracts::Inspect({});
	if (!Inspected.bOk || !FHyperAIStudioLightingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("No editor level, or lighting_lookdev is not admitted here; the dry run is skipped. ") + Inspected.Diagnostic);
		return true;
	}
	UWorld* EditorWorld = Gate::GetEditorWorld();
	if (EditorWorld && !Gate::LightOwningSky(*EditorWorld).IsEmpty())
	{
		AddInfo(TEXT("The open level's sky owns its atmosphere; the dry run is skipped."));
		return true;
	}
	FHyperAILightingApplyPlanRequest Plan;
	Plan.ExpectedRevision = Inspected.Revision;
	Plan.Ops = {Op(TEXT("set_sky_atmosphere"), TEXT("rayleigh_scale"), TEXT("1.1"))};
	const FHyperAILightingApplyPlanReport DryRun = FHyperAIStudioLightingContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("the dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk);
	TestTrue(TEXT("and notes the scale: ") + FString::Join(DryRun.Notes, TEXT(" | ")),
		DryRun.Notes.Num() == 1 && DryRun.Notes[0].Contains(TEXT("rayleigh_scale 1.1 is 33x the default 0.0331")));
	TestTrue(TEXT("pointing at the notes"), DryRun.Diagnostic.Contains(TEXT("Read the notes first")));
	TestEqual(TEXT("and changes nothing"), FHyperAIStudioLightingContracts::Inspect({}).Revision, Inspected.Revision);
	Plan.Ops = {Op(TEXT("set_sky_atmosphere"), TEXT("rayleigh_scale"), TEXT("0.04"))};
	TestEqual(TEXT("a usual value has no note"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Notes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingFrontDoorTest,
	"HyperAIStudio.Lighting.Contracts.PlanFrontDoor",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingFrontDoorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	const FHyperAILightingInspectReport Inspected = FHyperAIStudioLightingContracts::Inspect({});
	if (!Inspected.bOk)
	{
		AddInfo(TEXT("No editor level is open; front-door checks need one. ") + Inspected.Diagnostic);
		return true;
	}
	TestTrue(TEXT("inspect returns a canonical revision"), FHyperAIStudioLightingContracts::IsCanonicalSha256(Inspected.Revision));
	TestFalse(TEXT("inspect names the level"), Inspected.LevelPath.IsEmpty());

	FHyperAILightingApplyPlanRequest Plan;
	Plan.Ops = {Op(TEXT("set_post_process"), TEXT("contrast"), TEXT("1.05"))};
	TestEqual(TEXT("revision required"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_revision")));
	Plan.ExpectedRevision = TEXT("sha256:") + FString::ChrN(64, TEXT('0'));
	TestEqual(TEXT("stale revision refused"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("stale_revision")));
	Plan.ExpectedRevision = Inspected.Revision;
	Plan.OperationId = TEXT("lighting-front-door");
	TestEqual(TEXT("a dry run takes no operation id"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("unexpected_submission_fields")));
	Plan.OperationId.Reset();
	Plan.Ops = {Op(TEXT("set_post_process"), TEXT("contrast"), TEXT("9"))};
	TestEqual(TEXT("out-of-range ops refused before anything is prepared"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_op")));
	Plan.Ops.Reset();
	TestEqual(TEXT("empty plans refused"), FHyperAIStudioLightingContracts::BuildPlan(Plan).Status, FString(TEXT("invalid_op_count")));

	Plan.Ops = {Op(TEXT("set_post_process"), TEXT("contrast"), TEXT("1.05"))};
	if (!FHyperAIStudioLightingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("lighting_lookdev is not admitted here; the prepared dry run is skipped."));
		return true;
	}
	const FHyperAILightingApplyPlanReport DryRun = FHyperAIStudioLightingContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk && DryRun.bTrustedPrepared);
	TestEqual(TEXT("dry run changes nothing"), FHyperAIStudioLightingContracts::Inspect({}).Revision, Inspected.Revision);
	TestEqual(TEXT("the same plan seals the same hash"), FHyperAIStudioLightingContracts::BuildPlan(Plan).PlanHash, DryRun.PlanHash);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingEndToEndTest,
	"HyperAIStudio.Lighting.Execution.JournaledApplyAndUndo",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingEndToEndTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	// This edits the open level, so it runs only in unattended test runs, never in an editor someone is using.
	if (!FApp::IsUnattended())
	{
		AddInfo(TEXT("Runs only in unattended test runs, so it never edits the level you have open."));
		return true;
	}
	const FHyperAILightingInspectReport Before = FHyperAIStudioLightingContracts::Inspect({});
	if (!Before.bOk || !FHyperAIStudioLightingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		AddInfo(TEXT("No editor level, or lighting_lookdev is not admitted here; skipped."));
		return true;
	}
	// The approval queue has its own test; this one exercises the executor, so it runs the edit unattended.
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bRequiredApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bRequiredApproval;
	};
	FHyperAILightingApplyPlanRequest Plan;
	Plan.ExpectedRevision = Before.Revision;
	Plan.Ops = {Op(TEXT("set_post_process"), TEXT("contrast"), TEXT("1.05")), Op(TEXT("lock_exposure"), TEXT(""), TEXT("11"))};
	const FHyperAILightingApplyPlanReport DryRun = FHyperAIStudioLightingContracts::BuildPlan(Plan);
	if (!TestTrue(*FString::Printf(TEXT("dry run prepares (%s: %s)"), *DryRun.Status, *DryRun.Diagnostic), DryRun.bOk))
	{
		return false;
	}
	Plan.bDryRun = false;
	Plan.OperationId = TEXT("lighting-e2e-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
	Plan.ExpectedPlanHash = DryRun.PlanHash;
	const FHyperAILightingApplyPlanReport Submitted = FHyperAIStudioLightingContracts::BuildPlan(Plan);
	TestTrue(*FString::Printf(TEXT("submitted (%s: %s)"), *Submitted.Status, *Submitted.Diagnostic), Submitted.bExecutionSubmitted);
	FHyperAIStudioTypedArtifactOperationStatus Status;
	FString StatusError;
	PumpUntil([&]()
	{
		return !Submitted.bExecutionSubmitted
			|| (FHyperAIStudioTrustedExecutionFacade::QueryStatus(Plan.OperationId, Status, StatusError) && Status.bTerminal);
	}, 30.0);
	TestEqual(*FString::Printf(TEXT("operation completed (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));

	const FHyperAILightingInspectReport After = FHyperAIStudioLightingContracts::Inspect({});
	TestNotEqual(TEXT("the lighting changed"), After.Revision, Before.Revision);
	TestTrue(TEXT("exposure is locked"), After.bExposureLocked);
	TestEqual(TEXT("contrast applied"), ValueOf(After.Actors, TEXT("post_process"), TEXT("contrast")), FString(TEXT("1.05")));
	FlushAsyncLoading();
	TestTrue(TEXT("undo is available"), GEditor && GEditor->UndoTransaction());
	TestEqual(TEXT("one undo restores the level's lighting"), FHyperAIStudioLightingContracts::Inspect({}).Revision, Before.Revision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingCaptureContractsTest,
	"HyperAIStudio.Lighting.CaptureView.RefusesBadRequests",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingCaptureContractsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Capture = HyperAIStudio::Lighting::Capture;
	// Near misses are fitted, not refused: agents send 0 or "" for fields they do not care about, and guess at ranges.
	FHyperAICaptureViewRequest Near;
	Near.Source = TEXT("");
	Near.CameraActorLabel = TEXT("CAM_Front");
	Near.Width = 5000;
	Near.Height = 720;
	Near.WarmupFrames = 30;
	Near.IntervalSeconds = 0.f;
	Near.DiffThreshold = 0.f;
	Near.TimeMode = TEXT("");
	Near.Label = TEXT("front view!");
	FHyperAIStudioRequestFit Fit;
	Capture::Fit(Near, Fit);
	TestEqual(TEXT("an empty source with a camera label is the camera"), Near.Source, FString(TEXT("camera")));
	TestTrue(TEXT("an oversized picture is scaled down, keeping its shape"), Near.Width == 1920 && Near.Height == 276);
	TestEqual(TEXT("warm-up past the most is the most"), Near.WarmupFrames, 8);
	TestTrue(TEXT("zero means the default"), Near.IntervalSeconds == 0.5f && Near.DiffThreshold == 0.03f && Near.TimeMode == TEXT("world"));
	TestEqual(TEXT("a label is cleaned"), Near.Label, FString(TEXT("front_view_")));
	const FString Said = Fit.AppendTo(TEXT("Captured."));
	TestTrue(TEXT("the reply says what changed: ") + Said, Said.Contains(TEXT("size 5000x720 -> 1920x276")) && Said.Contains(TEXT("warmup_frames 30 -> 8"))
		&& !Said.Contains(TEXT("interval_seconds")));
	FHyperAICaptureViewRequest Animated;
	Animated.TimeMode = TEXT("material");
	Animated.IntervalSeconds = 0.25f;
	FHyperAIStudioRequestFit AnimatedFit;
	Capture::Fit(Animated, AnimatedFit);
	TestTrue(TEXT("material time without times takes one per interval"), Animated.TimeMode == TEXT("material_time")
		&& Animated.MaterialTimes.Num() == 2 && Animated.MaterialTimes[1] == 0.25f && Animated.FrameCount == 2);

	FHyperAICaptureViewRequest Request;
	Request.Source = TEXT("sideways");
	TestEqual(TEXT("what cannot be guessed is still refused"), Capture::RunNow(Request).Status, FString(TEXT("invalid_request")));
	Request.Width = 256;
	Request.Source = TEXT("camera");
	Request.CameraActorLabel = TEXT("NoSuchCamera_HyperAITest");
	TestEqual(TEXT("a missing camera is named, not guessed"), Capture::RunNow(Request).Status, FString(TEXT("view_not_found")));
	Request.Source = TEXT("transform");
	Request.TimeMode = TEXT("world");
	Request.FrameCount = 2;
	Request.WarmupFrames = 0;
	TestEqual(TEXT("world-time frames need the async tool"), Capture::RunNow(Request).Status, FString(TEXT("async_required")));

	// Settling: a fresh edit waits however still the picture looks; an old one waits for two draws that match.
	bool bTimedOut = false;
	TestFalse(TEXT("a fresh edit is not settled"), Capture::IsSettled(0.2, 0.5, 60, 0.f, bTimedOut));
	TestFalse(TEXT("an edit a few frames old is not settled"), Capture::IsSettled(0.2, 3.0, 10, 0.f, bTimedOut));
	TestFalse(TEXT("an old edit still showing is not settled"), Capture::IsSettled(2.0, 2.0, 90, 0.02f, bTimedOut));
	TestTrue(TEXT("an old edit that stopped changing is settled"), Capture::IsSettled(2.0, 2.0, 90, 0.001f, bTimedOut) && !bTimedOut);
	TestTrue(TEXT("with no edit known, still draws are settled"), Capture::IsSettled(0.1, -1.0, 0, 0.001f, bTimedOut));
	TestTrue(TEXT("settling gives up after its time"), Capture::IsSettled(7.0, 0.1, 1, 0.5f, bTimedOut) && bTimedOut);

	float Mean = 1.f;
	float Changed = 1.f;
	const TArray<FColor> Black = {FColor::Black, FColor::Black, FColor::Black, FColor::Black};
	const TArray<FColor> Half = {FColor::Black, FColor::Black, FColor::White, FColor::White};
	Capture::Diff(Black, Black, 0.03f, Mean, Changed);
	TestTrue(TEXT("identical frames do not differ"), Mean == 0.f && Changed == 0.f);
	Capture::Diff(Black, Half, 0.03f, Mean, Changed);
	TestEqual(TEXT("half the pixels changed"), Changed, 0.5f);
	TestEqual(TEXT("mean change counts every channel"), Mean, 0.5f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingCaptureAnimationTest,
	"HyperAIStudio.Lighting.CaptureView.MaterialTimeProvesAnimation",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingCaptureAnimationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	namespace Capture = HyperAIStudio::Lighting::Capture;
	UWorld* World = HyperAIStudio::Lighting::Gate::GetEditorWorld();
	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (!FApp::CanEverRender() || !World || !PlaneMesh)
	{
		AddWarning(TEXT("Needs a rendering editor with a level open."));
		return true;
	}

	// An unlit plane whose emissive is sin(2 pi t): black at t = 0, white a quarter period later.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
	UMaterialExpressionTime* Time = NewObject<UMaterialExpressionTime>(Material);
	UMaterialExpressionSine* Sine = NewObject<UMaterialExpressionSine>(Material);
	Sine->Period = 1.f;
	Sine->Input.Expression = Time;
	Material->GetExpressionCollection().AddExpression(Time);
	Material->GetExpressionCollection().AddExpression(Sine);
	Material->GetEditorOnlyData()->EmissiveColor.Expression = Sine;
	Material->SetShadingModel(MSM_Unlit);
	Material->PostEditChange();
	// A transient material queues no compile on its own; without one the plane renders the default grid.
	Material->ForceRecompileForRendering();

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6);
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	// A kilometre up: below the level, exponential height fog thickens until it hides a surface two metres away.
	AStaticMeshActor* Plane = World->SpawnActor<AStaticMeshActor>(FVector(0.0, 0.0, 100000.0), FRotator::ZeroRotator, Spawn);
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(FVector(0.0, 0.0, 100200.0), FRotator(-90.0, 0.0, 0.0), Spawn);
	ON_SCOPE_EXIT
	{
		if (Plane) Plane->Destroy();
		if (Camera) Camera->Destroy();
	};
	if (!TestNotNull(TEXT("plane spawned"), Plane) || !TestNotNull(TEXT("camera spawned"), Camera)) return false;
	// A registered static component refuses a new mesh, so it is set the way the mesh actor factory does.
	Plane->GetStaticMeshComponent()->UnregisterComponent();
	Plane->GetStaticMeshComponent()->SetStaticMesh(PlaneMesh);
	Plane->GetStaticMeshComponent()->SetMaterial(0, Material);
	Plane->GetStaticMeshComponent()->RegisterComponent();
	Plane->SetActorScale3D(FVector(20.0));
	Camera->SetActorLabel(TEXT("CAM_HyperAICaptureTest_") + Suffix);
	Camera->GetCameraComponent()->SetFieldOfView(60.f);
	if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
	const FMaterialResource* Resource = World->Scene ? Material->GetMaterialResource(World->Scene->GetShaderPlatform()) : nullptr;
	if (!TestTrue(TEXT("the test material compiled: ") + (Resource ? FString::Join(Resource->GetCompileErrors(), TEXT(" ")) : FString()),
		Resource && Resource->GetGameThreadShaderMap() && Resource->GetCompileErrors().IsEmpty()))
	{
		return false;
	}

	FHyperAICaptureViewRequest Request;
	Request.Source = TEXT("camera");
	Request.CameraActorLabel = Camera->GetActorLabel();
	Request.Width = 256;
	Request.Height = 256;
	Request.FrameCount = 3;
	Request.TimeMode = TEXT("material_time");
	Request.MaterialTimes = {0.f, 0.f, 0.25f};
	Request.WarmupFrames = 2;
	Request.Label = TEXT("hyperai-test");
	const FHyperAICaptureViewReport Report = Capture::RunNow(Request);
	if (!TestTrue(FString::Printf(TEXT("captured (%s: %s)"), *Report.Status, *Report.Diagnostic), Report.bOk)) return false;
	TestEqual(TEXT("three frames"), Report.Frames.Num(), 3);
	TestEqual(TEXT("two comparisons"), Report.Diffs.Num(), 2);
	if (Report.Diffs.Num() == 2)
	{
		TestTrue(FString::Printf(TEXT("the same material time renders the same (%.4f changed)"), Report.Diffs[0].ChangedPixelFraction),
			Report.Diffs[0].ChangedPixelFraction < 0.01f);
		TestTrue(FString::Printf(TEXT("a quarter period later the surface has changed (%.4f changed)"), Report.Diffs[1].ChangedPixelFraction),
			Report.Diffs[1].ChangedPixelFraction > 0.5f);
	}
	for (const FHyperAICaptureFrame& Frame : Report.Frames)
	{
		TestTrue(TEXT("each frame is a PNG in the project: ") + Frame.ImagePath,
			IFileManager::Get().FileExists(*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Frame.ImagePath)));
	}
	const IConsoleVariable* TimeOverride = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Test.OverrideTimeMaterialExpressions"));
	TestTrue(TEXT("forced material time is put back"), !TimeOverride || TimeOverride->GetFloat() < 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingActorMaterialTest,
	"HyperAIStudio.Lighting.Ops.SetActorMaterial",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingActorMaterialTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILightingMaterialWorld"));
	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (!World || !PlaneMesh)
	{
		AddError(TEXT("Needs a test world and the engine plane."));
		return false;
	}
	ON_SCOPE_EXIT { World->DestroyWorld(/*bInformEngineOfWorld=*/false); };
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transactional;
	AStaticMeshActor* Plane = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Plane->GetStaticMeshComponent()->UnregisterComponent();
	Plane->GetStaticMeshComponent()->SetStaticMesh(PlaneMesh);
	Plane->GetStaticMeshComponent()->RegisterComponent();
	Plane->SetActorLabel(TEXT("SM_Plane"));
	UMaterialInterface* Original = Plane->GetStaticMeshComponent()->GetMaterial(0);
	// Assign something the plane does not already show, or undo could not be told apart from no edit.
	const TCHAR* GridPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial");
	const TCHAR* ShapePath = TEXT("/Engine/BasicShapes/BasicShapeMaterial");
	UMaterialInterface* Grid = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	const bool bUseShape = Grid == Original;
	const TCHAR* AssignedPath = bUseShape ? ShapePath : GridPath;
	UMaterialInterface* Assigned = bUseShape
		? LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")) : Grid;
	if (!TestNotNull(TEXT("a material to assign"), Assigned) || !TestNotEqual(TEXT("and it differs from the plane's"), Assigned, Original)) return false;

	auto Rejects = [&](const TArray<FHyperAILightingOp>& Ops, const TCHAR* Expected)
	{
		FString Error;
		const bool bValid = Gate::ValidateOps(*World, Ops, Error);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Error), !bValid && Error.Contains(Expected));
	};
	Rejects({Op(TEXT("set_actor_material"), TEXT(""), GridPath)}, TEXT("needs actor_label"));
	Rejects({Op(TEXT("set_actor_material"), TEXT(""), GridPath, TEXT("Nobody"))}, TEXT("no actor is labelled"));
	Rejects({Op(TEXT("set_actor_material"), TEXT(""), TEXT("/Game/NoSuchMaterial"), TEXT("SM_Plane"))}, TEXT("is not a material"));
	Rejects({Op(TEXT("set_actor_material"), TEXT("slot:7"), GridPath, TEXT("SM_Plane"))}, TEXT("its slots are 0"));
	Rejects({Op(TEXT("set_actor_material"), TEXT("component:Nope/slot:0"), GridPath, TEXT("SM_Plane"))}, TEXT("has no mesh component 'Nope'"));
	Rejects({Op(TEXT("set_actor_material"), TEXT("material:0"), GridPath, TEXT("SM_Plane"))}, TEXT("slot:<index or name>"));

	// A path without the object name resolves too, and the plan never spawns anything.
	const TArray<FHyperAILightingOp> Ops = {Op(TEXT("set_actor_material"), TEXT("slot:0"), AssignedPath, TEXT("SM_Plane"))};
	TArray<AActor*> Touched;
	FString Error;
	TestTrue(TEXT("the material applies: ") + Error, Gate::ApplyOps(*World, Ops, Touched, Error));
	TestEqual(TEXT("slot 0 holds it"), Plane->GetStaticMeshComponent()->GetMaterial(0), Assigned);
	TestTrue(TEXT("only the plane was touched"), Touched.Num() == 1 && Touched[0] == Plane);
	TestEqual(TEXT("no lighting actor was added"), Gate::ReadActors(*World).Num(), 0);
	TestTrue(TEXT("the Validate phase sees it: ") + Error, Gate::CheckMaterialOps(*World, Ops, Error));
	TestTrue(TEXT("the plane is the save target"), Gate::ResolveTargets(*World, Ops).Num() == 1 && Gate::ResolveTargets(*World, Ops)[0] == Plane);

	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestEqual(TEXT("one undo restores the original material"), Plane->GetStaticMeshComponent()->GetMaterial(0), Original);
		TestFalse(TEXT("and the postcondition notices"), Gate::CheckMaterialOps(*World, Ops, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingSkySystemTest,
	"HyperAIStudio.Lighting.Ops.SkySystem",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingSkySystemTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	// A Blueprint named and shaped like Ultra Dynamic Sky: the variables it reads by the names its Details panel shows.
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/HyperAITests/Sky_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, TEXT("Ultra_Dynamic_Sky"), BPTYPE_Normal);
	if (!TestNotNull(TEXT("a stand-in sky Blueprint"), Blueprint)) return false;
	FEdGraphPinType Real;
	Real.PinCategory = TEXT("real");
	Real.PinSubCategory = TEXT("double");
	FEdGraphPinType Bool;
	Bool.PinCategory = TEXT("bool");
	FEdGraphPinType Colour;
	Colour.PinCategory = TEXT("struct");
	Colour.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	auto Add = [Blueprint](const TCHAR* Name, const FEdGraphPinType& Type, const TCHAR* Default, const bool bOnInstance)
	{
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, Type, Default);
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, Name, !bOnInstance);
	};
	Add(TEXT("Time of Day"), Real, TEXT("1300.0"), true);
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Time of Day"), nullptr, TEXT("ClampMin"), TEXT("0"));
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, TEXT("Time of Day"), nullptr, TEXT("ClampMax"), TEXT("2400"));
	Add(TEXT("Cloud Coverage"), Real, TEXT("1.0"), true);
	Add(TEXT("Apply Exposure Settings"), Bool, TEXT("true"), true);
	Add(TEXT("Sun Light Color"), Colour, TEXT("(R=1.0,G=1.0,B=1.0,A=1.0)"), true);
	Add(TEXT("Internal Cache"), Real, TEXT("0.0"), false);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	if (!TestNotNull(TEXT("it compiled to a class"), Blueprint->GeneratedClass.Get())) return false;

	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILightingSkyWorld"));
	if (!TestNotNull(TEXT("a test world"), World)) return false;
	ON_SCOPE_EXIT { World->DestroyWorld(/*bInformEngineOfWorld=*/false); };
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transactional;
	AActor* Sky = World->SpawnActor(Blueprint->GeneratedClass, nullptr, nullptr, Spawn);
	if (!TestNotNull(TEXT("the sky spawned"), Sky)) return false;
	Sky->SetActorLabel(TEXT("Ultra_Dynamic_Sky"));
	auto Read = [&](const TCHAR* Key) { return ValueOf(Gate::ReadActors(*World), TEXT("sky_system"), Key); };

	TestEqual(TEXT("inspect names the system"), Read(TEXT("system")), FString(TEXT("Ultra Dynamic Sky")));
	TestEqual(TEXT("which owns its lights"), Read(TEXT("owns_lights")), FString(TEXT("true")));
	TestEqual(TEXT("and shows Time of Day as a plain number"), Read(TEXT("Time of Day")), FString(TEXT("1300")));
	TestEqual(TEXT("and whether it applies its own exposure"), Read(TEXT("Apply Exposure Settings")), FString(TEXT("true")));
	TestEqual(TEXT("a variable the placed actor cannot edit stays out"), Read(TEXT("Internal Cache")), FString(TEXT("<missing>")));
	TestEqual(TEXT("the sky is found as the light owner"), Gate::LightOwningSky(*World), FString(TEXT("Ultra Dynamic Sky ('Ultra_Dynamic_Sky')")));
	// A viewport set to a fixed exposure counts as locked whatever the sky does, so only check without one.
	if (!(GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->ExposureSettings.bFixed))
	{
		TestFalse(TEXT("a sky applying its own exposure means exposure is not locked"), Gate::IsExposureLocked(*World));
	}

	auto Rejects = [&](const TArray<FHyperAILightingOp>& Ops, const TCHAR* Expected)
	{
		FString Error;
		const bool bValid = Gate::ValidateOps(*World, Ops, Error);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Error), !bValid && Error.Contains(Expected));
	};
	Rejects({Op(TEXT("set_sun"), TEXT("intensity"), TEXT("5"))}, TEXT("owns its sun, sky light, fog, atmosphere and clouds"));
	Rejects({Op(TEXT("set_clouds"), TEXT("material:Cloud_GlobalCoverage"), TEXT("0.5"))}, TEXT("so set_clouds would fight it"));
	Rejects({Op(TEXT("set_fog"), TEXT("density"), TEXT("0.1"))}, TEXT("Use set_sky_system instead: Time of Day"));
	Rejects({Op(TEXT("set_sky_system"), TEXT("Rain"), TEXT("1"))}, TEXT("no sky in the level has a variable 'Rain'"));
	Rejects({Op(TEXT("set_sky_system"), TEXT("Time of Day"), TEXT("dusk"))}, TEXT("does not take 'dusk'"));
	Rejects({Op(TEXT("set_sky_system"), TEXT("Time of Day"), TEXT("3000"))}, TEXT("from 0 to 2400"));
	Rejects({Op(TEXT("set_sky_system"), TEXT("Apply Exposure Settings"), TEXT("maybe"))}, TEXT("takes true or false"));
	Rejects({Op(TEXT("set_sky_system"), TEXT("Internal Cache"), TEXT("1"))}, TEXT("is not editable on the placed actor"));
	Rejects({Op(TEXT("set_sky_system"), TEXT("Time of Day"), TEXT("1200"), TEXT("Other_Sky"))}, TEXT("no sky is labelled 'Other_Sky'"));
	Rejects({Op(TEXT("set_sky_system"), TEXT(""), TEXT("1"))}, TEXT("needs property"));
	FString Error;
	TestTrue(TEXT("a post-process edit is still fine beside the sky"), Gate::ValidateOps(*World, {Op(TEXT("set_post_process"), TEXT("contrast"), TEXT("1.1"))}, Error));
	FActorSpawnParameters LampSpawn;
	LampSpawn.ObjectFlags |= RF_Transactional;
	World->SpawnActor<APointLight>(FVector(0.0, 0.0, 300.0), FRotator::ZeroRotator, LampSpawn)->SetActorLabel(TEXT("Lamp"));
	TestTrue(TEXT("a local light is the level's, not the sky's, so it may change beside the sky: ") + Error,
		Gate::ValidateOps(*World, {Op(TEXT("set_light"), TEXT("intensity"), TEXT("20"), TEXT("Lamp"))}, Error));

	const FString Before = Gate::ComputeRevision(*World);
	const TArray<FHyperAILightingOp> Ops = {
		Op(TEXT("set_sky_system"), TEXT("Time of Day"), TEXT("1730")),
		Op(TEXT("set_sky_system"), TEXT("cloud coverage"), TEXT("3.5")),
		Op(TEXT("set_sky_system"), TEXT("Sun Light Color"), TEXT("(R=1,G=0.5,B=0.25,A=1)"))};
	TArray<AActor*> Touched;
	TestTrue(TEXT("sky edits apply: ") + Error, Gate::ApplyOps(*World, Ops, Touched, Error));
	TestTrue(TEXT("only the sky was touched"), Touched.Num() == 1 && Touched[0] == Sky);
	TestEqual(TEXT("Time of Day moved"), Read(TEXT("Time of Day")), FString(TEXT("1730")));
	TestEqual(TEXT("a name in any case finds its variable"), Read(TEXT("Cloud Coverage")), FString(TEXT("3.5")));
	TestTrue(TEXT("the colour is set: ") + Read(TEXT("Sun Light Color")), Read(TEXT("Sun Light Color")).Contains(TEXT("G=0.5")));
	TestNotEqual(TEXT("the revision follows the sky"), Gate::ComputeRevision(*World), Before);
	TestTrue(TEXT("the sky is the save target"), Gate::ResolveTargets(*World, Ops).Num() == 1 && Gate::ResolveTargets(*World, Ops)[0] == Sky);
	TestEqual(TEXT("no stock light was added"), Gate::ReadActors(*World).Num(), 1);

	Touched.Reset();
	TestTrue(TEXT("locking exposure applies: ") + Error, Gate::ApplyOps(*World, {Op(TEXT("lock_exposure"), TEXT(""), TEXT("13"))}, Touched, Error));
	TestEqual(TEXT("locking also turns the sky's own exposure off"), Read(TEXT("Apply Exposure Settings")), FString(TEXT("false")));
	TestTrue(TEXT("so the level now counts as locked"), Gate::IsExposureLocked(*World));
	TestTrue(TEXT("and the sky is among the touched actors"), Touched.Contains(Sky));
	TestTrue(TEXT("lock ops resolve to the sky too, for saving"), Gate::ResolveTargets(*World, {Op(TEXT("lock_exposure"), TEXT(""), TEXT("13"))}).Contains(Sky));

	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction() && GEditor->UndoTransaction())
	{
		TestEqual(TEXT("two undos give back the sky's exposure"), Read(TEXT("Apply Exposure Settings")), FString(TEXT("true")));
		TestEqual(TEXT("and its time of day"), Read(TEXT("Time of Day")), FString(TEXT("1300")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingLocalLightsTest,
	"HyperAIStudio.Lighting.Ops.LocalLights",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingLocalLightsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILocalLightsWorld"));
	if (!World)
	{
		AddError(TEXT("Could not create a test world."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transactional;
	auto Place = [&](UClass* Class, const FVector& Location, const FRotator& Rotation, const TCHAR* Label)
	{
		AActor* Actor = World->SpawnActor(Class, &Location, &Rotation, Spawn);
		Actor->SetActorLabel(Label);
		return Actor;
	};
	APointLight* Lamp = CastChecked<APointLight>(Place(APointLight::StaticClass(), FVector(0.0, 0.0, 200.0), FRotator::ZeroRotator, TEXT("Lamp")));
	Place(ASpotLight::StaticClass(), FVector(300.0, 0.0, 300.0), FRotator(-90.0, 0.0, 0.0), TEXT("Accent"));
	Place(ARectLight::StaticClass(), FVector(-400.0, 0.0, 250.0), FRotator::ZeroRotator, TEXT("Window"));
	AActor* Behind = Place(APointLight::StaticClass(), FVector(-3000.0, 0.0, 250.0), FRotator::ZeroRotator, TEXT("Behind"));
	CastChecked<APointLight>(Behind)->PointLightComponent->AttenuationRadius = 100.f;
	// A torch Blueprint's shape: an actor holding two lights among its components.
	AActor* Torch = Place(AStaticMeshActor::StaticClass(), FVector(600.0, 0.0, 0.0), FRotator::ZeroRotator, TEXT("Torch"));
	for (const TCHAR* Name : {TEXT("Flame"), TEXT("Glow")})
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(Torch, Name);
		Light->SetupAttachment(Torch->GetRootComponent());
		Light->RegisterComponent();
		Torch->AddInstanceComponent(Light);
	}
	Place(ACameraActor::StaticClass(), FVector(-1000.0, 0.0, 250.0), FRotator::ZeroRotator, TEXT("Cam"));

	struct FRead
	{
		TArray<FHyperAILightingActorRecord> Records;
		int32 Total = 0;
		FString View;
	};
	auto Read = [&](const TCHAR* Camera, const TCHAR* Filter, const int32 Limit)
	{
		FRead Out;
		Out.Records = Gate::ReadLocalLights(*World, Camera, Filter, Limit, Out.Total, Out.View);
		return Out;
	};
	auto Value = [](const TArray<FHyperAILightingActorRecord>& Records, const TCHAR* Label, const TCHAR* Key, const TCHAR* Component = TEXT(""))
	{
		for (const FHyperAILightingActorRecord& Record : Records)
		{
			const FHyperAILightingProperty* Held = Record.Properties.FindByPredicate([](const FHyperAILightingProperty& Property) { return Property.Key == TEXT("component"); });
			if (Record.Label != Label || (Held ? Held->Value : FString()) != Component) continue;
			const FHyperAILightingProperty* Found = Record.Properties.FindByPredicate([Key](const FHyperAILightingProperty& Property) { return Property.Key == Key; });
			return Found ? Found->Value : FString(TEXT("<missing>"));
		}
		return FString(TEXT("<missing>"));
	};
	auto KindOf = [](const TArray<FHyperAILightingActorRecord>& Records, const TCHAR* Label)
	{
		const FHyperAILightingActorRecord* Found = Records.FindByPredicate([Label](const FHyperAILightingActorRecord& Record) { return Record.Label == Label; });
		return Found ? Found->Kind : FString(TEXT("<missing>"));
	};

	const FRead All = Read(TEXT(""), TEXT(""), 32);
	TestEqual(TEXT("every local light is counted, the torch's two included"), All.Total, 6);
	TestEqual(TEXT("and every one is listed"), All.Records.Num(), 6);
	TestEqual(TEXT("a test world has no view to order by"), All.View, FString());
	TestEqual(TEXT("the lamp reads as a point light"), KindOf(All.Records, TEXT("Lamp")), FString(TEXT("point_light")));
	TestEqual(TEXT("the accent as a spot light"), KindOf(All.Records, TEXT("Accent")), FString(TEXT("spot_light")));
	TestEqual(TEXT("the window as a rect light"), KindOf(All.Records, TEXT("Window")), FString(TEXT("rect_light")));
	TestNotEqual(TEXT("a spot light reports its cone"), Value(All.Records, TEXT("Accent"), TEXT("outer_cone_angle")), FString(TEXT("<missing>")));
	TestNotEqual(TEXT("a rect light its size"), Value(All.Records, TEXT("Window"), TEXT("source_width")), FString(TEXT("<missing>")));
	TestEqual(TEXT("a point light has no cone"), Value(All.Records, TEXT("Lamp"), TEXT("outer_cone_angle")), FString(TEXT("<missing>")));
	TestNotEqual(TEXT("each says what units its intensity is in"), Value(All.Records, TEXT("Lamp"), TEXT("intensity_units")), FString(TEXT("<missing>")));
	TestEqual(TEXT("a light inside an actor is read by its component"), Value(All.Records, TEXT("Torch"), TEXT("enabled"), TEXT("Glow")), FString(TEXT("true")));

	const FRead Seen = Read(TEXT("Cam"), TEXT(""), 32);
	TestEqual(TEXT("ordered from the named camera"), Seen.View, FString(TEXT("Cam")));
	TestTrue(TEXT("the nearest light in view comes first"), Seen.Records.Num() == 6 && Seen.Records[0].Label == TEXT("Window"));
	TestEqual(TEXT("and says it is in view"), Value(Seen.Records, TEXT("Window"), TEXT("in_view")), FString(TEXT("true")));
	TestEqual(TEXT("a light behind the camera that reaches nowhere near the view is not"), Value(Seen.Records, TEXT("Behind"), TEXT("in_view")), FString(TEXT("false")));
	TestTrue(TEXT("and comes last"), Seen.Records.Num() == 6 && Seen.Records.Last().Label == TEXT("Behind"));
	TestEqual(TEXT("a filter keeps matching labels, in any case"), Read(TEXT(""), TEXT("tor"), 32).Records.Num(), 2);
	TestEqual(TEXT("while the total still counts every light"), Read(TEXT(""), TEXT("tor"), 32).Total, 6);
	TestEqual(TEXT("the limit bounds the list"), Read(TEXT("Cam"), TEXT(""), 2).Records.Num(), 2);
	TestEqual(TEXT("a camera that is not there gives no view"), Read(TEXT("NoCam"), TEXT(""), 32).View, FString());

	auto Rejects = [&](const TArray<FHyperAILightingOp>& Ops, const TCHAR* Expected)
	{
		FString Error;
		const bool bValid = Gate::ValidateOps(*World, Ops, Error);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Error), !bValid && Error.Contains(Expected));
	};
	Rejects({Op(TEXT("set_light"), TEXT("outer_cone_angle"), TEXT("30"), TEXT("Lamp"))}, TEXT("applies only to spot lights"));
	Rejects({Op(TEXT("set_light"), TEXT("source_width"), TEXT("100"), TEXT("Accent"))}, TEXT("applies only to rect lights"));
	Rejects({Op(TEXT("set_light"), TEXT("aim_at"), TEXT("0,0,0"), TEXT("Lamp"))}, TEXT("applies only to spot and rect lights"));
	Rejects({Op(TEXT("set_light"), TEXT("intensity"), TEXT("5"), TEXT("Torch"))}, TEXT("holds 2 lights"));
	Rejects({Op(TEXT("set_light"), TEXT("component:Wick/intensity"), TEXT("5"), TEXT("Torch"))}, TEXT("no light component 'Wick'"));
	Rejects({Op(TEXT("set_light"), TEXT("component:Glow/location"), TEXT("0,0,0"), TEXT("Torch"))}, TEXT("moves and aims only light actors"));
	Rejects({Op(TEXT("set_light"), TEXT("intensity"), TEXT("5"), TEXT("Nobody"))}, TEXT("no actor is labelled 'Nobody'"));
	Rejects({Op(TEXT("set_light"), TEXT("intensity"), TEXT("-1"), TEXT("Lamp"))}, TEXT("from 0 to"));
	Rejects({Op(TEXT("set_light"), TEXT("color"), TEXT("2,0,0"), TEXT("Lamp"))}, TEXT("r,g,b"));
	Rejects({Op(TEXT("set_light"), TEXT("glow"), TEXT("1"), TEXT("Lamp"))}, TEXT("no setting 'glow'"));
	Rejects({Op(TEXT("set_light"), TEXT("ies_profile"), TEXT("/Engine/BasicShapes/Cube"), TEXT("Lamp"))}, TEXT("not an IES light profile"));
	Rejects({Op(TEXT("set_light"), TEXT("intensity"), TEXT("5"))}, TEXT("needs actor_label"));
	Rejects({Op(TEXT("add_light"), TEXT("point"), TEXT("0,0,100"), TEXT("Lamp"))}, TEXT("already labelled 'Lamp'"));
	Rejects({Op(TEXT("add_light"), TEXT("cone"), TEXT("0,0,100"), TEXT("Fill"))}, TEXT("point, spot or rect"));
	Rejects({Op(TEXT("add_light"), TEXT("spot"), TEXT("up high"), TEXT("Fill"))}, TEXT("x,y,z"));
	Rejects({Op(TEXT("add_light"), TEXT("spot"), TEXT("0,0,100"), TEXT("Fill")), Op(TEXT("set_light"), TEXT("source_width"), TEXT("50"), TEXT("Fill"))},
		TEXT("applies only to rect lights"));
	Rejects({Op(TEXT("set_fog"), TEXT("volumetric_fog"), TEXT("maybe"))}, TEXT("takes true or false"));
	Rejects({Op(TEXT("set_light"), TEXT("intensity_units"), TEXT("furlongs"), TEXT("Lamp"))}, TEXT("takes unitless, candelas, lumens"));
	FString Error;
	TestTrue(TEXT("a later op may set a light an earlier op adds: ") + Error, Gate::ValidateOps(*World,
		{Op(TEXT("add_light"), TEXT("rect"), TEXT("100,100,300"), TEXT("Fill")), Op(TEXT("set_light"), TEXT("source_width"), TEXT("80"), TEXT("Fill"))}, Error));
	TestEqual(TEXT("validation adds nothing"), Read(TEXT(""), TEXT(""), 32).Total, 6);

	const FString Before = Gate::ComputeRevision(*World);
	const TArray<FHyperAILightingOp> Ops = {
		Op(TEXT("set_light"), TEXT("intensity"), TEXT("12"), TEXT("Lamp")),
		Op(TEXT("set_light"), TEXT("temperature"), TEXT("2700"), TEXT("Lamp")),
		Op(TEXT("set_light"), TEXT("attenuation_radius"), TEXT("900"), TEXT("Lamp")),
		Op(TEXT("set_light"), TEXT("source_radius"), TEXT("5"), TEXT("Lamp")),
		Op(TEXT("set_light"), TEXT("outer_cone_angle"), TEXT("30"), TEXT("Accent")),
		Op(TEXT("set_light"), TEXT("aim_at"), TEXT("0,0,0"), TEXT("Accent")),
		Op(TEXT("set_light"), TEXT("volumetric_scattering"), TEXT("3"), TEXT("Accent")),
		Op(TEXT("set_light"), TEXT("source_width"), TEXT("200"), TEXT("Window")),
		Op(TEXT("set_light"), TEXT("barn_door_angle"), TEXT("45"), TEXT("Window")),
		Op(TEXT("set_light"), TEXT("cast_shadows"), TEXT("false"), TEXT("Window")),
		Op(TEXT("set_light"), TEXT("component:Glow/enabled"), TEXT("false"), TEXT("Torch")),
		Op(TEXT("add_light"), TEXT("rect"), TEXT("100,100,300"), TEXT("Fill")),
		Op(TEXT("set_light"), TEXT("intensity"), TEXT("4"), TEXT("Fill")),
		Op(TEXT("set_light"), TEXT("aim_at"), TEXT("100,100,0"), TEXT("Fill")),
		Op(TEXT("set_fog"), TEXT("volumetric_fog"), TEXT("true")),
		Op(TEXT("set_fog"), TEXT("volumetric_albedo"), TEXT("0.9,0.8,0.7"))};
	TArray<AActor*> Touched;
	TestTrue(TEXT("the plan applies: ") + Error, Gate::ApplyOps(*World, Ops, Touched, Error));
	const FRead After = Read(TEXT(""), TEXT(""), 32);
	TestEqual(TEXT("one light was added"), After.Total, 7);
	TestEqual(TEXT("lamp intensity"), Value(After.Records, TEXT("Lamp"), TEXT("intensity")), FString(TEXT("12")));
	TestEqual(TEXT("lamp temperature"), Value(After.Records, TEXT("Lamp"), TEXT("temperature")), FString(TEXT("2700")));
	TestEqual(TEXT("which switches temperature on"), Value(After.Records, TEXT("Lamp"), TEXT("use_temperature")), FString(TEXT("true")));
	TestEqual(TEXT("lamp reach"), Value(After.Records, TEXT("Lamp"), TEXT("attenuation_radius")), FString(TEXT("900")));
	TestEqual(TEXT("lamp size"), Value(After.Records, TEXT("Lamp"), TEXT("source_radius")), FString(TEXT("5")));
	TestEqual(TEXT("accent cone"), Value(After.Records, TEXT("Accent"), TEXT("outer_cone_angle")), FString(TEXT("30")));
	TestEqual(TEXT("accent shafts"), Value(After.Records, TEXT("Accent"), TEXT("volumetric_scattering")), FString(TEXT("3")));
	TestTrue(TEXT("the accent now points down at the origin: ") + Value(After.Records, TEXT("Accent"), TEXT("rotation")),
		Value(After.Records, TEXT("Accent"), TEXT("rotation")).StartsWith(TEXT("-45")));
	TestEqual(TEXT("window width"), Value(After.Records, TEXT("Window"), TEXT("source_width")), FString(TEXT("200")));
	TestEqual(TEXT("window barn doors"), Value(After.Records, TEXT("Window"), TEXT("barn_door_angle")), FString(TEXT("45")));
	TestEqual(TEXT("window shadows off"), Value(After.Records, TEXT("Window"), TEXT("cast_shadows")), FString(TEXT("false")));
	TestEqual(TEXT("the torch's glow is off"), Value(After.Records, TEXT("Torch"), TEXT("enabled"), TEXT("Glow")), FString(TEXT("false")));
	TestEqual(TEXT("its flame is not"), Value(After.Records, TEXT("Torch"), TEXT("enabled"), TEXT("Flame")), FString(TEXT("true")));
	TestEqual(TEXT("the added light is a rect light"), KindOf(After.Records, TEXT("Fill")), FString(TEXT("rect_light")));
	TestEqual(TEXT("marked as added"), Value(After.Records, TEXT("Fill"), TEXT("added_by_hyperai")), FString(TEXT("true")));
	TestEqual(TEXT("and movable"), Value(After.Records, TEXT("Fill"), TEXT("mobility")), FString(TEXT("Movable")));
	TestEqual(TEXT("set in the same plan"), Value(After.Records, TEXT("Fill"), TEXT("intensity")), FString(TEXT("4")));
	TestTrue(TEXT("and pointing straight down: ") + Value(After.Records, TEXT("Fill"), TEXT("rotation")),
		Value(After.Records, TEXT("Fill"), TEXT("rotation")).StartsWith(TEXT("-90")));
	TestEqual(TEXT("volumetric fog is on"), ValueOf(Gate::ReadActors(*World), TEXT("fog"), TEXT("volumetric_fog")), FString(TEXT("true")));
	TestNotEqual(TEXT("the revision moved"), Gate::ComputeRevision(*World), Before);
	TestEqual(TEXT("every op resolves to its actor, for saving"), Gate::ResolveTargets(*World, Ops).Num(), 6);

	static const TConsoleVariableData<int32>* DefaultUnits = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.DefaultFeature.LightUnits"));
	TestEqual(TEXT("an added light is in the project's default light units, as a placed one would be"), Value(After.Records, TEXT("Fill"), TEXT("intensity_units")),
		StaticEnum<ELightUnits>()->GetNameStringByValue(DefaultUnits ? DefaultUnits->GetValueOnGameThread() : 0).ToLower());
	const float Brightness = Lamp->PointLightComponent->ComputeLightBrightness();
	const FString Units = Value(After.Records, TEXT("Lamp"), TEXT("intensity_units")) == TEXT("lumens") ? TEXT("candelas") : TEXT("lumens");
	Touched.Reset();
	TestTrue(TEXT("units change: ") + Error, Gate::ApplyOps(*World, {Op(TEXT("set_light"), TEXT("intensity_units"), *Units, TEXT("Lamp"))}, Touched, Error));
	TestEqual(TEXT("to the units asked for"), Value(Read(TEXT(""), TEXT(""), 32).Records, TEXT("Lamp"), TEXT("intensity_units")), Units);
	TestTrue(TEXT("and the lamp is as bright as before"), FMath::IsNearlyEqual(Lamp->PointLightComponent->ComputeLightBrightness(), Brightness, Brightness * 0.001f + 0.001f));

	const FString Listed = Gate::ComputeRevision(*World);
	Touched.Reset();
	TestTrue(TEXT("a far light changes: ") + Error, Gate::ApplyOps(*World, {Op(TEXT("set_light"), TEXT("intensity"), TEXT("3"), TEXT("Behind"))}, Touched, Error));
	TestNotEqual(TEXT("the revision covers lights no short listing shows"), Gate::ComputeRevision(*World), Listed);

	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestNotEqual(TEXT("one undo takes the far light's change back"), Value(Read(TEXT(""), TEXT(""), 32).Records, TEXT("Behind"), TEXT("intensity")), FString(TEXT("3")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingDetailsTest,
	"HyperAIStudio.Lighting.Ops.DetailsByName",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingDetailsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILightingDetailsWorld"));
	if (!World)
	{
		AddError(TEXT("Could not create a test world."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};
	auto Rejects = [&](const TArray<FHyperAILightingOp>& Ops, const TCHAR* Expected)
	{
		FString Error;
		const bool bValid = Gate::ValidateOps(*World, Ops, Error);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Error), !bValid && Error.Contains(Expected));
	};
	Rejects({Op(TEXT("set_post_process"), TEXT("Intensity"), TEXT("1"))}, TEXT("several settings called 'Intensity'"));
	Rejects({Op(TEXT("set_sky_atmosphere"), TEXT("MieAnisotropy"), TEXT("1.5"))}, TEXT("0.999"));
	Rejects({Op(TEXT("set_sun"), TEXT("BloomScale"), TEXT("bright"))}, TEXT("does not take 'bright'"));
	Rejects({Op(TEXT("set_sun"), TEXT("BloomScale"), TEXT(""))}, TEXT("or any setting its Details panel shows"));
	// The exposure lock's settings stay with lock_exposure, and whether a light moves stays with the level.
	Rejects({Op(TEXT("set_post_process"), TEXT("AutoExposureBias"), TEXT("1"))}, TEXT("no setting 'AutoExposureBias'"));
	Rejects({Op(TEXT("set_sun"), TEXT("Mobility"), TEXT("Static"))}, TEXT("no setting 'Mobility'"));
	FString Error;
	TestTrue(TEXT("a setting by name validates before the plan adds its actor: ") + Error, Gate::ValidateOps(*World, {Op(TEXT("set_sun"), TEXT("BloomScale"), TEXT("0.3"))}, Error));
	TestEqual(TEXT("and validation adds nothing"), Gate::ReadActors(*World).Num(), 0);

	const TArray<FHyperAILightingOp> Ops = {
		Op(TEXT("set_sun"), TEXT("light_shaft_bloom"), TEXT("true")),
		Op(TEXT("set_sun"), TEXT("BloomScale"), TEXT("0.3")),
		Op(TEXT("set_sun"), TEXT("shadow_amount"), TEXT("0.7")),
		Op(TEXT("set_sky_light"), TEXT("MinOcclusion"), TEXT("0.2")),
		Op(TEXT("set_sky_light"), TEXT("OcclusionTint"), TEXT("0.1,0.1,0.15")),
		Op(TEXT("set_fog"), TEXT("Directional Inscattering Exponent"), TEXT("8")),
		Op(TEXT("set_fog"), TEXT("SecondFogData"), TEXT("(FogDensity=0.02,FogHeightFalloff=0.1,FogHeightOffset=500)")),
		Op(TEXT("set_sky_atmosphere"), TEXT("MieAnisotropy"), TEXT("0.9")),
		Op(TEXT("set_post_process"), TEXT("LocalExposureShadowContrastScale"), TEXT("0.6")),
		Op(TEXT("set_post_process"), TEXT("bloom_intensity"), TEXT("0.5"))};
	TArray<AActor*> Touched;
	const bool bApplied = Gate::ApplyOps(*World, Ops, Touched, Error);
	if (!TestTrue(TEXT("the plan applies: ") + Error, bApplied))
	{
		return false;
	}
	ADirectionalLight* Sun = nullptr;
	ASkyLight* Sky = nullptr;
	AExponentialHeightFog* Fog = nullptr;
	ASkyAtmosphere* Atmosphere = nullptr;
	APostProcessVolume* Volume = nullptr;
	for (AActor* Actor : Touched)
	{
		Sun = Sun ? Sun : Cast<ADirectionalLight>(Actor);
		Sky = Sky ? Sky : Cast<ASkyLight>(Actor);
		Fog = Fog ? Fog : Cast<AExponentialHeightFog>(Actor);
		Atmosphere = Atmosphere ? Atmosphere : Cast<ASkyAtmosphere>(Actor);
		Volume = Volume ? Volume : Cast<APostProcessVolume>(Actor);
	}
	if (!TestTrue(TEXT("one actor of each kind was added"), Touched.Num() == 5 && Sun && Sky && Fog && Atmosphere && Volume))
	{
		return false;
	}
	const UDirectionalLightComponent& Light = *Sun->GetComponent();
	TestTrue(TEXT("light shaft bloom is on, by its label in the Details panel"), Light.bEnableLightShaftBloom);
	TestEqual(TEXT("bloom scale, by its C++ name"), Light.BloomScale, 0.3f);
	TestEqual(TEXT("shadow amount, by its name in words"), Light.ShadowAmount, 0.7f);
	TestEqual(TEXT("min occlusion"), Sky->GetLightComponent()->MinOcclusion, 0.2f);
	TestTrue(TEXT("a colour kept as sRGB bytes is given in linear r,g,b like every other colour"),
		Sky->GetLightComponent()->OcclusionTint == FLinearColor(0.1f, 0.1f, 0.15f).ToFColor(true));
	TestEqual(TEXT("directional inscattering exponent, spaced as a person types it"), Fog->GetComponent()->DirectionalInscatteringExponent, 8.f);
	TestEqual(TEXT("a struct in Unreal's text form"), Fog->GetComponent()->SecondFogData.FogDensity, 0.02f);
	TestEqual(TEXT("mie anisotropy"), Atmosphere->GetComponent()->MieAnisotropy, 0.9f);
	const FPostProcessSettings& Post = Volume->Settings;
	TestTrue(TEXT("a post-process setting takes effect with its override on"), Post.bOverride_LocalExposureShadowContrastScale && Post.LocalExposureShadowContrastScale == 0.6f);
	TestTrue(TEXT("bloom intensity by its C++ name in words, though its label is only Intensity"), Post.bOverride_BloomIntensity && Post.BloomIntensity == 0.5f);

	const TArray<FHyperAILightingActorRecord> Records = Gate::ReadActors(*World);
	TestTrue(TEXT("inspect lists what was set through the Details panel: ") + ValueOf(Records, TEXT("sun"), TEXT("BloomScale")),
		ValueOf(Records, TEXT("sun"), TEXT("BloomScale")).StartsWith(TEXT("0.3")));
	TestNotEqual(TEXT("post process too"), ValueOf(Records, TEXT("post_process"), TEXT("LocalExposureShadowContrastScale")), FString(TEXT("<missing>")));
	TestEqual(TEXT("but not what it left alone"), ValueOf(Records, TEXT("sun"), TEXT("BloomThreshold")), FString(TEXT("<missing>")));
	const FHyperAILightingActorRecord* SunRecord = Records.FindByPredicate([](const FHyperAILightingActorRecord& Record) { return Record.Kind == TEXT("sun"); });
	TestFalse(TEXT("nor what a fixed key already shows"), SunRecord && SunRecord->Properties.ContainsByPredicate([](const FHyperAILightingProperty& Property)
	{
		return Property.Key.Equals(TEXT("Intensity"), ESearchCase::CaseSensitive);
	}));
	const FString Revised = Gate::ComputeRevision(*World);
	Touched.Reset();
	TestTrue(TEXT("a threshold applies: ") + Error, Gate::ApplyOps(*World, {Op(TEXT("set_sun"), TEXT("BloomThreshold"), TEXT("2"))}, Touched, Error));
	TestNotEqual(TEXT("the revision covers settings changed by name"), Gate::ComputeRevision(*World), Revised);

	Touched.Reset();
	TestTrue(TEXT("default applies: ") + Error, Gate::ApplyOps(*World, {Op(TEXT("set_post_process"), TEXT("LocalExposureShadowContrastScale"), TEXT("default"))}, Touched, Error));
	TestFalse(TEXT("default lifts the override"), Volume->Settings.bOverride_LocalExposureShadowContrastScale);
	TestEqual(TEXT("and inspect no longer lists it"), ValueOf(Gate::ReadActors(*World), TEXT("post_process"), TEXT("LocalExposureShadowContrastScale")), FString(TEXT("<missing>")));
	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestTrue(TEXT("one undo puts the override back"), Volume->Settings.bOverride_LocalExposureShadowContrastScale);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingMadeAssetsTest,
	"HyperAIStudio.Lighting.Ops.CloudsShadowsAndLut",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingMadeAssetsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	namespace Lut = HyperAIStudio::Lighting::Lut;
	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	const FString Stem = TEXT("hyperai_test_lut_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString LutImage = TEXT("Saved/HyperAIStudio/Tests/") + Stem + TEXT(".png");
	const FString WrongSize = TEXT("Saved/HyperAIStudio/Tests/") + Stem + TEXT("_wrong.png");
	// What the plan makes is never saved, so it is only forgotten, once the test world that uses it is gone.
	TArray<UObject*> Made;
	ON_SCOPE_EXIT
	{
		for (UObject* Object : Made)
		{
			FAssetRegistryModule::AssetDeleted(Object);
			Object->GetPackage()->SetDirtyFlag(false);
			Object->ClearFlags(RF_Public | RF_Standalone);
			Object->MarkAsGarbage();
		}
		IFileManager::Get().Delete(*(FPaths::ProjectDir() / LutImage));
		IFileManager::Get().Delete(*(FPaths::ProjectDir() / WrongSize));
	};
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILightingMadeAssetsWorld"));
	if (!World || !SphereMesh)
	{
		AddError(TEXT("Needs a test world and the engine sphere."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};
	// A blockout tree's canopy: a solid sphere where the finished tree's leaves would let light through.
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transactional;
	AStaticMeshActor* Canopy = World->SpawnActor<AStaticMeshActor>(FVector(0.0, 0.0, 600.0), FRotator::ZeroRotator, Spawn);
	Canopy->GetStaticMeshComponent()->UnregisterComponent();
	Canopy->GetStaticMeshComponent()->SetStaticMesh(SphereMesh);
	Canopy->GetStaticMeshComponent()->RegisterComponent();
	Canopy->SetActorLabel(TEXT("Tree_Canopy"));
	// A warm grade as hyper_image_compare's fit_lut would write one.
	Lut::FTable Warm = Lut::Identity();
	for (FVector3f& Node : Warm)
	{
		Node = FVector3f(FMath::Min(Node.X * 1.1f + 0.02f, 1.f), Node.Y, Node.Z * 0.85f);
	}
	TestTrue(TEXT("the LUT image is written"), Gate::WritePng(FPaths::ProjectDir() / LutImage, Lut::ToStrip(Warm), Lut::StripWidth, Lut::StripHeight));
	TestTrue(TEXT("and one of the wrong size"), Gate::WritePng(FPaths::ProjectDir() / WrongSize, Solid(FColor(128, 128, 128)), Side, Side));

	auto Rejects = [&](const TArray<FHyperAILightingOp>& Ops, const TCHAR* Expected)
	{
		FString Error;
		const bool bValid = Gate::ValidateOps(*World, Ops, Error);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Error), !bValid && Error.Contains(Expected));
	};
	Rejects({Op(TEXT("set_actor_shadow"), TEXT(""), TEXT("false"))}, TEXT("needs actor_label"));
	Rejects({Op(TEXT("set_actor_shadow"), TEXT(""), TEXT("maybe"), TEXT("Tree_Canopy"))}, TEXT("takes true"));
	Rejects({Op(TEXT("set_actor_shadow"), TEXT("component:Leaves"), TEXT("false"), TEXT("Tree_Canopy"))}, TEXT("has no mesh component 'Leaves'"));
	Rejects({Op(TEXT("set_actor_shadow"), TEXT(""), TEXT("false"), TEXT("Nobody"))}, TEXT("no actor is labelled 'Nobody'"));
	Rejects({Op(TEXT("set_clouds"), TEXT("material:Cloud_Fluffiness"), TEXT("1"))}, TEXT("has no parameter 'Cloud_Fluffiness'"));
	Rejects({Op(TEXT("set_clouds"), TEXT("material:Cloud_GlobalCoverage"), TEXT("lots"))}, TEXT("takes a number"));
	Rejects({Op(TEXT("set_post_process"), TEXT("color_grading_lut"), TEXT("/Game/NoSuchLut"))}, TEXT("is not a texture"));
	Rejects({Op(TEXT("set_post_process"), TEXT("color_grading_lut"), *WrongSize)}, TEXT("a LUT is 256 x 16"));

	const TArray<FHyperAILightingOp> Ops = {
		Op(TEXT("set_actor_shadow"), TEXT(""), TEXT("false"), TEXT("Tree_Canopy")),
		Op(TEXT("set_clouds"), TEXT("layer_bottom_altitude"), TEXT("3")),
		Op(TEXT("set_clouds"), TEXT("material:Cloud_GlobalCoverage"), TEXT("0.35")),
		Op(TEXT("set_clouds"), TEXT("material:Cloud_AlbedoColor"), TEXT("0.9,0.85,0.8")),
		Op(TEXT("set_post_process"), TEXT("color_grading_lut"), *LutImage)};
	TArray<AActor*> Touched;
	FString Error;
	const bool bApplied = Gate::ApplyOps(*World, Ops, Touched, Error);
	AVolumetricCloud* Clouds = nullptr;
	APostProcessVolume* Volume = nullptr;
	for (AActor* Actor : Touched)
	{
		Clouds = Clouds ? Clouds : Cast<AVolumetricCloud>(Actor);
		Volume = Volume ? Volume : Cast<APostProcessVolume>(Actor);
	}
	UVolumetricCloudComponent* Cloud = Clouds ? Clouds->FindComponentByClass<UVolumetricCloudComponent>() : nullptr;
	UMaterialInstanceConstant* Instance = Cloud ? Cast<UMaterialInstanceConstant>(Cloud->Material.Get()) : nullptr;
	UTexture2D* Texture = Volume ? Cast<UTexture2D>(Volume->Settings.ColorGradingLUT.Get()) : nullptr;
	if (Instance && Instance->GetPathName().StartsWith(Gate::MadeAssetsRoot)) Made.Add(Instance);
	if (Texture && Texture->GetPathName().StartsWith(Gate::MadeAssetsRoot)) Made.Add(Texture);
	if (!TestTrue(TEXT("the plan applies: ") + Error, bApplied) || !TestTrue(TEXT("with clouds, a post-process volume and what they hold"), Cloud && Instance && Texture))
	{
		return false;
	}

	TestFalse(TEXT("the canopy lets the light through"), Canopy->GetStaticMeshComponent()->CastShadow);
	TestTrue(TEXT("and is saved with the plan"), Gate::ResolveTargets(*World, Ops).Contains(Canopy));
	TestEqual(TEXT("the clouds' layer by its name in words"), Cloud->LayerBottomAltitude, 3.f);
	TestTrue(TEXT("the clouds have their own material instance, under HyperAI: ") + Instance->GetPathName(),
		Instance->GetPathName().StartsWith(FString(Gate::MadeAssetsRoot) + TEXT("Clouds/MI_Clouds_")));
	TestTrue(TEXT("made from the material they had"), Instance->Parent && Instance->Parent->GetName() == TEXT("m_SimpleVolumetricCloud_Inst"));
	float Coverage = 0.f;
	TestTrue(TEXT("coverage set on it"), Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Cloud_GlobalCoverage")), Coverage) && Coverage == 0.35f);
	FLinearColor Albedo;
	TestTrue(TEXT("and a colour"), Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("Cloud_AlbedoColor")), Albedo)
		&& FVector3f(Albedo.R, Albedo.G, Albedo.B).Equals(FVector3f(0.9f, 0.85f, 0.8f)));
	TestEqual(TEXT("inspect shows the coverage"), ValueOf(Gate::ReadActors(*World), TEXT("clouds"), TEXT("material:Cloud_GlobalCoverage")), FString(TEXT("0.35")));

	TestEqual(TEXT("the LUT is imported under HyperAI"), Texture->GetPathName(), FString(Gate::MadeAssetsRoot) + TEXT("LUTs/LUT_") + Stem + TEXT(".LUT_") + Stem);
	TestTrue(TEXT("as a LUT: its texture group, no mips, no sRGB decode"),
		Texture->LODGroup == TEXTUREGROUP_ColorLookupTable && Texture->MipGenSettings == TMGS_NoMipmaps && !Texture->SRGB);
	TestTrue(TEXT("and the grade uses it at full strength"), Volume->Settings.bOverride_ColorGradingLUT
		&& Volume->Settings.bOverride_ColorGradingIntensity && Volume->Settings.ColorGradingIntensity == 1.f);
	Lut::FTable Read;
	FString LutPath;
	float Worst = 1.f;
	if (TestTrue(TEXT("the level's LUT reads back"), Gate::ReadLevelLut(*World, Read, LutPath) && Read.Num() == Warm.Num()))
	{
		Worst = 0.f;
		for (int32 Index = 0; Index < Read.Num(); ++Index)
		{
			Worst = FMath::Max(Worst, (Read[Index] - Warm[Index]).GetAbsMax());
		}
	}
	TestTrue(FString::Printf(TEXT("as the grade it was made from (worst %.4f)"), Worst), Worst <= 1.f / 255.f + 0.0001f);
	TestEqual(TEXT("with its path"), LutPath, Texture->GetPathName());
	const TArray<UPackage*> Packages = Gate::PackagesOf(Touched);
	TestTrue(TEXT("both made assets are saved with the plan"), Packages.Contains(Texture->GetPackage()) && Packages.Contains(Instance->GetPackage()));

	Touched.Reset();
	TestTrue(TEXT("the clouds change again: ") + Error, Gate::ApplyOps(*World, {Op(TEXT("set_clouds"), TEXT("material:Cloud_GlobalCoverage"), TEXT("0.5"))}, Touched, Error));
	TestTrue(TEXT("on the same instance"), Cloud->Material.Get() == Instance
		&& Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Cloud_GlobalCoverage")), Coverage) && Coverage == 0.5f);
	Touched.Reset();
	TestTrue(TEXT("none applies: ") + Error, Gate::ApplyOps(*World, {Op(TEXT("set_post_process"), TEXT("color_grading_lut"), TEXT("none"))}, Touched, Error));
	TestTrue(TEXT("none takes the LUT off"), !Volume->Settings.bOverride_ColorGradingLUT && !Volume->Settings.ColorGradingLUT);
	TestFalse(TEXT("and the level has no LUT to read"), Gate::ReadLevelLut(*World, Read, LutPath));
	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestTrue(TEXT("one undo puts the LUT back"), Volume->Settings.ColorGradingLUT.Get() == Texture);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingMaterialParameterTest,
	"HyperAIStudio.Lighting.Ops.MaterialParameters",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingMaterialParameterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	namespace LookDev = HyperAIStudio::Lighting::LookDev;
	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* Shape = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILightingLookDevWorld"));
	if (!World || !PlaneMesh || !Shape)
	{
		AddError(TEXT("Needs a test world, the engine plane and its material."));
		return false;
	}
	UMaterial* Floor = MakeLookDevMaterial(TEXT("M_LookDevFloor_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString Folder = LookDev::FolderFor(*World);
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		for (TObjectIterator<UMaterialInstanceConstant> It; It; ++It)
		{
			if (IsValid(*It) && It->GetPathName().StartsWith(Folder)) Forget(*It);
		}
		Forget(Floor);
	};
	AStaticMeshActor* FloorA = PlaceMesh(*World, PlaneMesh, FVector(0.0, 0.0, 0.0), TEXT("Floor_A"), Floor);
	AStaticMeshActor* FloorB = PlaceMesh(*World, PlaneMesh, FVector(0.0, 150.0, 0.0), TEXT("Floor_B"), Floor);
	AStaticMeshActor* Wall = PlaceMesh(*World, PlaneMesh, FVector(0.0, -150.0, 0.0), TEXT("Wall"), Shape);
	UStaticMeshComponent* SlotA = FloorA->GetStaticMeshComponent();
	UStaticMeshComponent* SlotB = FloorB->GetStaticMeshComponent();

	// Without a view there is nothing to list; HyperAI_MatchCamera gives one.
	int32 Total = 0;
	TestTrue(TEXT("no view, no materials"), Gate::ReadMaterials(*World, FString(), 12, Total).IsEmpty() && Total == 0);
	FActorSpawnParameters Spawn;
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(FVector(-400.0, 0.0, 300.0), (FVector::ZeroVector - FVector(-400.0, 0.0, 300.0)).Rotation(), Spawn);
	Camera->SetActorLabel(TEXT("HyperAI_MatchCamera"));
	const TArray<FHyperAILightingMaterialRecord> Seen = Gate::ReadMaterials(*World, FString(), 12, Total);
	const FHyperAILightingMaterialRecord* Record = Seen.FindByPredicate([Floor](const FHyperAILightingMaterialRecord& Each) { return Each.Path == Floor->GetPathName(); });
	if (!TestTrue(FString::Printf(TEXT("the floor's material is listed among the %d in view"), Total), Record != nullptr && Total >= 2))
	{
		return false;
	}
	TestEqual(TEXT("as a material"), Record->Kind, FString(TEXT("material")));
	TestEqual(TEXT("shown by both floors"), Record->ActorCount, 2);
	TestTrue(TEXT("the nearest named, with the slot the ops take"), Record->Actors.Contains(TEXT("Floor_A")) && Record->Slot == TEXT("slot:0"));
	// Substrate may name a converted material's model its own way, so this asks only that it is lit.
	TestTrue(TEXT("lit and opaque: ") + Record->ShadingModel + TEXT(" ") + Record->BlendMode,
		!Record->ShadingModel.IsEmpty() && Record->ShadingModel != TEXT("unlit") && Record->BlendMode == TEXT("opaque"));
	TestTrue(TEXT("covering part of the view"), Record->ViewShare > 0.f && Record->ViewShare <= 1.f);
	TestEqual(TEXT("its roughness"), ParameterOf(*Record, TEXT("Roughness")), FString(TEXT("0.8")));
	TestEqual(TEXT("its colour, linear with alpha"), ParameterOf(*Record, TEXT("BaseColor")), FString(TEXT("0.5,0.5,0.5,0.25")));
	TestTrue(TEXT("and its texture"), ParameterOf(*Record, TEXT("Detail")).Contains(TEXT("DefaultTexture")));
	TestTrue(TEXT("a dull specular is noted"), Record->Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("Specular 0.10 dulls")); }));
	if (RenderingFact(*World, TEXT("reflection_method")) == TEXT("lumen"))
	{
		TestTrue(TEXT("under Lumen reflections, so is a roughness above the trace limit"),
			Record->Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("above Lumen's reflection trace limit")); }));
	}

	auto Rejects = [&](const TArray<FHyperAILightingOp>& Ops, const TCHAR* Expected)
	{
		FString Error;
		const bool bValid = Gate::ValidateOps(*World, Ops, Error);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Error), !bValid && Error.Contains(Expected));
	};
	Rejects({Op(TEXT("set_material_parameter"), TEXT("Roughness"), TEXT("0.2"))}, TEXT("needs actor_label"));
	Rejects({Op(TEXT("set_material_parameter"), TEXT(""), TEXT("0.2"), TEXT("Floor_A"))}, TEXT("[component:<name>/][slot:<index or name>/]<parameter>"));
	Rejects({Op(TEXT("set_material_parameter"), TEXT("Glossiness"), TEXT("0.2"), TEXT("Floor_A"))}, TEXT("has no parameter 'Glossiness'; its parameters are BaseColor, Detail, Roughness, Specular"));
	Rejects({Op(TEXT("set_material_parameter"), TEXT("Wet"), TEXT("true"), TEXT("Floor_A"))}, TEXT("static switch"));
	Rejects({Op(TEXT("set_material_parameter"), TEXT("Roughness"), TEXT("shiny"), TEXT("Floor_A"))}, TEXT("takes a number"));
	Rejects({Op(TEXT("set_material_parameter"), TEXT("BaseColor"), TEXT("1,2"), TEXT("Floor_A"))}, TEXT("takes a linear colour"));
	Rejects({Op(TEXT("set_material_parameter"), TEXT("Detail"), TEXT("/Game/NoSuchTexture"), TEXT("Floor_A"))}, TEXT("is not a texture"));
	Rejects({Op(TEXT("set_material_parameter"), TEXT("slot:3/Roughness"), TEXT("0.2"), TEXT("Floor_A"))}, TEXT("its slots are 0"));
	// A plan that first gives the slot another material is checked against that one.
	Rejects({Op(TEXT("set_actor_material"), TEXT(""), TEXT("/Engine/BasicShapes/BasicShapeMaterial"), TEXT("Floor_A")),
		Op(TEXT("set_material_parameter"), TEXT("Detail"), TEXT("/Engine/EngineResources/WhiteSquareTexture"), TEXT("Floor_A"))}, TEXT("has no parameter 'Detail'"));
	TestEqual(TEXT("checking changes nothing"), SlotA->GetMaterial(0), static_cast<UMaterialInterface*>(Floor));

	// Level-wide: the level's own instance, shown wherever the material was.
	const TArray<FHyperAILightingOp> Glossy = {Op(TEXT("set_material_parameter"), TEXT("roughness"), TEXT("0.2"), TEXT("Floor_A"))};
	TArray<AActor*> Touched;
	FString Error;
	if (!TestTrue(TEXT("a level-wide change applies: ") + Error, Gate::ApplyOps(*World, Glossy, Touched, Error)))
	{
		return false;
	}
	UMaterialInstanceConstant* Level = Cast<UMaterialInstanceConstant>(SlotA->GetMaterial(0));
	if (!TestNotNull(TEXT("the floor shows a material instance"), Level))
	{
		return false;
	}
	TestTrue(TEXT("the level's own, in its LookDev folder: ") + Level->GetPathName(), Level->GetPathName().StartsWith(Folder + TEXT("MI_") + Floor->GetName()));
	TestEqual(TEXT("made from the floor's material"), Level->Parent.Get(), static_cast<UMaterialInterface*>(Floor));
	TestEqual(TEXT("every other slot that showed it shows it too"), SlotB->GetMaterial(0), static_cast<UMaterialInterface*>(Level));
	TestEqual(TEXT("a slot showing another material keeps it"), Wall->GetStaticMeshComponent()->GetMaterial(0), Shape);
	float Value = 0.f;
	TestTrue(TEXT("the instance holds the roughness, by the parameter's own name"),
		Level->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Roughness")), Value) && FMath::IsNearlyEqual(Value, 0.2f));
	TestTrue(TEXT("the project's material is unchanged"), Floor->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Roughness")), Value) && FMath::IsNearlyEqual(Value, 0.8f));
	TestTrue(TEXT("both floors were touched, the wall not"), Touched.Contains(FloorA) && Touched.Contains(FloorB) && !Touched.Contains(Wall));
	TestTrue(TEXT("the Validate phase sees it: ") + Error, Gate::CheckMaterialOps(*World, Glossy, Error));
	TestTrue(TEXT("and the instance saves with the plan"), Gate::PackagesOf(Gate::ResolveTargets(*World, Glossy)).Contains(Level->GetPackage()));
	const TArray<FHyperAILightingMaterialRecord> Tuned = Gate::ReadMaterials(*World, FString(), 12, Total);
	const FHyperAILightingMaterialRecord* Shown = Tuned.FindByPredicate([Level](const FHyperAILightingMaterialRecord& Each) { return Each.Path == Level->GetPathName(); });
	if (TestNotNull(TEXT("inspect lists the level's instance"), Shown))
	{
		TestEqual(TEXT("as its look-dev instance"), Shown->Kind, FString(TEXT("look_dev")));
		TestEqual(TEXT("with the roughness it holds"), ParameterOf(*Shown, TEXT("Roughness")), FString(TEXT("0.2")));
	}

	Touched.Reset();
	TestTrue(TEXT("a colour by the other floor applies: ") + Error,
		Gate::ApplyOps(*World, {Op(TEXT("set_material_parameter"), TEXT("base_color"), TEXT("0.3,0.25,0.2"), TEXT("Floor_B"))}, Touched, Error));
	FLinearColor Colour;
	TestTrue(TEXT("on the same instance, keeping its alpha"), SlotB->GetMaterial(0) == Level
		&& Level->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("BaseColor")), Colour) && Colour.Equals(FLinearColor(0.3f, 0.25f, 0.2f, 0.25f)));
	Touched.Reset();
	TestTrue(TEXT("a texture applies: ") + Error,
		Gate::ApplyOps(*World, {Op(TEXT("set_material_parameter"), TEXT("Detail"), TEXT("/Engine/EngineResources/WhiteSquareTexture"), TEXT("Floor_A"))}, Touched, Error));
	UTexture* Detail = nullptr;
	TestTrue(TEXT("by its path"), Level->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Detail")), Detail) && Detail && Detail->GetName() == TEXT("WhiteSquareTexture"));

	// One actor alone: its own instance, made from the level's.
	const TArray<FHyperAILightingOp> Polished = {Op(TEXT("set_actor_material_parameter"), TEXT("slot:0/Roughness"), TEXT("0.05"), TEXT("Floor_B"))};
	Touched.Reset();
	TestTrue(TEXT("an actor's own change applies: ") + Error, Gate::ApplyOps(*World, Polished, Touched, Error));
	UMaterialInstanceConstant* Own = Cast<UMaterialInstanceConstant>(SlotB->GetMaterial(0));
	if (!TestTrue(TEXT("the floor has its own instance, in Actors"), Own && Own != Level && Own->GetPathName().StartsWith(Folder + TEXT("Actors/MI_Floor_B_0"))))
	{
		return false;
	}
	TestEqual(TEXT("made from the level's, so it takes what the level changes"), Own->Parent.Get(), static_cast<UMaterialInterface*>(Level));
	TestEqual(TEXT("the other floor keeps the level's"), SlotA->GetMaterial(0), static_cast<UMaterialInterface*>(Level));
	TestTrue(TEXT("its roughness is its own"), Own->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Roughness")), Value) && FMath::IsNearlyEqual(Value, 0.05f));
	TestTrue(TEXT("the level's is unchanged"), Level->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Roughness")), Value) && FMath::IsNearlyEqual(Value, 0.2f));
	TestTrue(TEXT("the Validate phase sees it: ") + Error, Gate::CheckMaterialOps(*World, Polished, Error));
	Touched.Reset();
	TestTrue(TEXT("a level-wide change through that floor applies: ") + Error,
		Gate::ApplyOps(*World, {Op(TEXT("set_material_parameter"), TEXT("Specular"), TEXT("0.5"), TEXT("Floor_B"))}, Touched, Error));
	TestTrue(TEXT("on the level's instance"), Level->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Specular")), Value) && FMath::IsNearlyEqual(Value, 0.5f));
	TestTrue(TEXT("which the floor's own takes"), Own->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Specular")), Value) && FMath::IsNearlyEqual(Value, 0.5f));
	const TArray<FHyperAILightingOp> Dropped = {Op(TEXT("set_actor_material_parameter"), TEXT("Roughness"), TEXT("default"), TEXT("Floor_B"))};
	Touched.Reset();
	TestTrue(TEXT("default applies: ") + Error, Gate::ApplyOps(*World, Dropped, Touched, Error));
	TestTrue(TEXT("default drops the floor's own roughness for the level's"),
		Own->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Roughness")), Value) && FMath::IsNearlyEqual(Value, 0.2f));
	TestTrue(TEXT("the Validate phase sees that too: ") + Error, Gate::CheckMaterialOps(*World, Dropped, Error));

	const FString Revision = Gate::ComputeRevision(*World);
	Touched.Reset();
	TestTrue(TEXT("one more change applies: ") + Error, Gate::ApplyOps(*World, {Op(TEXT("set_material_parameter"), TEXT("Roughness"), TEXT("0.3"), TEXT("Floor_A"))}, Touched, Error));
	TestNotEqual(TEXT("the revision covers the surfaces"), Gate::ComputeRevision(*World), Revision);
	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestTrue(TEXT("one undo puts the roughness back"), Level->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Roughness")), Value) && FMath::IsNearlyEqual(Value, 0.2f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLightingCapturesTest,
	"HyperAIStudio.Lighting.Ops.ReflectionCapturesAndMeshFlags",
	HyperAIStudio::Lighting::Tests::Flags)

bool FHyperAIStudioLightingCapturesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Lighting::Tests;
	namespace Gate = HyperAIStudio::Lighting::Gate;
	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, TEXT("HyperAILightingCapturesWorld"));
	if (!World || !SphereMesh)
	{
		AddError(TEXT("Needs a test world and the engine sphere."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};
	AStaticMeshActor* Lamp = PlaceMesh(*World, SphereMesh, FVector(0.0, 0.0, 300.0), TEXT("Neon_Sign"));
	auto Rejects = [&](const TArray<FHyperAILightingOp>& Ops, const TCHAR* Expected)
	{
		FString Error;
		const bool bValid = Gate::ValidateOps(*World, Ops, Error);
		TestTrue(*FString::Printf(TEXT("rejected with '%s' (got '%s')"), Expected, *Error), !bValid && Error.Contains(Expected));
	};
	Rejects({Op(TEXT("set_actor_lighting"), TEXT("emissive_light_source"), TEXT("true"))}, TEXT("needs actor_label"));
	Rejects({Op(TEXT("set_actor_lighting"), TEXT("glow"), TEXT("true"), TEXT("Neon_Sign"))}, TEXT("has no setting 'glow'"));
	Rejects({Op(TEXT("set_actor_lighting"), TEXT("emissive_light_source"), TEXT("maybe"), TEXT("Neon_Sign"))}, TEXT("takes true or false"));
	Rejects({Op(TEXT("add_reflection_capture"), TEXT("plane"), TEXT("0,0,0"), TEXT("Capture_Hall"))}, TEXT("sphere or box"));
	Rejects({Op(TEXT("add_reflection_capture"), TEXT("sphere"), TEXT("0,0,0"), TEXT("Neon_Sign"))}, TEXT("already labelled 'Neon_Sign'"));
	Rejects({Op(TEXT("set_reflection_capture"), TEXT("Brightness"), TEXT("2"), TEXT("Nobody"))}, TEXT("no reflection capture is labelled 'Nobody'"));
	Rejects({Op(TEXT("add_reflection_capture"), TEXT("sphere"), TEXT("0,0,200"), TEXT("Capture_Hall")),
		Op(TEXT("set_reflection_capture"), TEXT("box_extent"), TEXT("500,400,300"), TEXT("Capture_Hall"))}, TEXT("box_extent sizes only a box"));
	Rejects({Op(TEXT("add_reflection_capture"), TEXT("sphere"), TEXT("0,0,200"), TEXT("Capture_Hall")),
		Op(TEXT("set_reflection_capture"), TEXT("Radiance"), TEXT("2"), TEXT("Capture_Hall"))}, TEXT("no setting 'Radiance'"));

	const TArray<FHyperAILightingOp> Ops = {
		Op(TEXT("set_actor_lighting"), TEXT("emissive_light_source"), TEXT("true"), TEXT("Neon_Sign")),
		Op(TEXT("set_actor_lighting"), TEXT("affect_indirect_lighting"), TEXT("false"), TEXT("Neon_Sign")),
		Op(TEXT("add_reflection_capture"), TEXT("sphere"), TEXT("0,0,200"), TEXT("Capture_Hall")),
		Op(TEXT("set_reflection_capture"), TEXT("InfluenceRadius"), TEXT("1500"), TEXT("Capture_Hall")),
		Op(TEXT("set_reflection_capture"), TEXT("Brightness"), TEXT("2"), TEXT("Capture_Hall")),
		Op(TEXT("add_reflection_capture"), TEXT("box"), TEXT("800,0,200"), TEXT("Capture_Room")),
		Op(TEXT("set_reflection_capture"), TEXT("box_extent"), TEXT("500,400,300"), TEXT("Capture_Room"))};
	TArray<AActor*> Touched;
	FString Error;
	if (!TestTrue(TEXT("the plan applies: ") + Error, Gate::ApplyOps(*World, Ops, Touched, Error)))
	{
		return false;
	}
	const UStaticMeshComponent* Mesh = Lamp->GetStaticMeshComponent();
	TestTrue(TEXT("the sign is an emissive light source"), Mesh->bEmissiveLightSource);
	TestFalse(TEXT("and takes no part in bounce light"), Mesh->bAffectDynamicIndirectLighting);
	ASphereReflectionCapture* Hall = nullptr;
	ABoxReflectionCapture* Room = nullptr;
	for (AActor* Actor : Touched)
	{
		Hall = Hall ? Hall : Cast<ASphereReflectionCapture>(Actor);
		Room = Room ? Room : Cast<ABoxReflectionCapture>(Actor);
	}
	if (!TestTrue(TEXT("a sphere and a box capture were added"), Hall && Room))
	{
		return false;
	}
	TestEqual(TEXT("labelled as asked"), Hall->GetActorLabel(), FString(TEXT("Capture_Hall")));
	TestTrue(TEXT("and marked as added"), Hall->Tags.Contains(FName(TEXT("HyperAI.AddedReflectionCapture"))));
	TestEqual(TEXT("where asked"), Hall->GetActorLocation(), FVector(0.0, 0.0, 200.0));
	const USphereReflectionCaptureComponent* Sphere = Cast<USphereReflectionCaptureComponent>(Hall->GetCaptureComponent());
	TestTrue(TEXT("its reach by the Details panel's name"), Sphere && Sphere->InfluenceRadius == 1500.f);
	TestEqual(TEXT("and its brightness"), Hall->GetCaptureComponent()->Brightness, 2.f);
	TestEqual(TEXT("the box's half sizes are its scale"), Room->GetActorScale3D(), FVector(500.0, 400.0, 300.0));
	const TArray<FHyperAILightingActorRecord> Records = Gate::ReadActors(*World);
	TestEqual(TEXT("inspect lists both captures"), Records.FilterByPredicate([](const FHyperAILightingActorRecord& Each) { return Each.Kind == TEXT("reflection_capture"); }).Num(), 2);
	TestEqual(TEXT("and the rendering counts them"), RenderingFact(*World, TEXT("reflection_captures")), FString(TEXT("2")));
	TestNotEqual(TEXT("the rendering says how the project lights"), RenderingFact(*World, TEXT("gi_method")), FString(TEXT("<missing>")));
	TestTrue(TEXT("and what that means"), RenderingFact(*World, TEXT("note")) != TEXT("<missing>"));
	TestTrue(TEXT("each capture is a target the plan saves"), Gate::ResolveTargets(*World, Ops).Contains(Hall) && Gate::ResolveTargets(*World, Ops).Contains(Room));
	FlushAsyncLoading();
	if (GEditor && GEditor->UndoTransaction())
	{
		TestFalse(TEXT("one undo takes the flag back"), Mesh->bEmissiveLightSource);
		TestEqual(TEXT("and the captures away"), Gate::ReadActors(*World).FilterByPredicate([](const FHyperAILightingActorRecord& Each) { return Each.Kind == TEXT("reflection_capture"); }).Num(), 0);
	}
	return true;
}

#endif
