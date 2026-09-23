// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AnimGraphNode_BlendSpaceBase.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Components/Border.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/ProgressBar.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "FileHelpers.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "PreviewScene.h"
#include "RHIShaderPlatform.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "WidgetBlueprint.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "WorldPartition/WorldPartitionHelpers.h"

/**
 * Benchmark verification, run by Tools/Benchmark/bench.py in a fresh editor process.
 *
 * A fresh process only has what was saved, so every "saved" check is real rather than read off a dirty
 * flag. Checks use engine APIs, never HyperAIStudio tools, and each test writes a JSON report beside
 * the automation result so the harness can say which check failed and why.
 *
 * Named HyperAIBench.* rather than HyperAIStudio.* so the plugin's own suite never runs them.
 */
namespace HyperAIStudio::Benchmark
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* const SkeletonPath = TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin.SK_Mannequin");

	struct FCheck
	{
		FString Id;
		bool bPass = false;
		bool bRequired = true;
		FString Detail;
	};

	class FReport
	{
	public:
		explicit FReport(const FString& InTask) : Task(InTask) {}

		bool Add(const FString& Id, const bool bPass, const FString& Detail, const bool bRequired = true)
		{
			Checks.Add({Id, bPass, bRequired, Detail});
			return bPass;
		}

		bool Passed() const
		{
			for (const FCheck& Check : Checks)
			{
				if (Check.bRequired && !Check.bPass) return false;
			}
			return !Checks.IsEmpty();
		}

		void Finish(FAutomationTestBase& Test) const
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("task"), Task);
			Root->SetStringField(TEXT("stage"), Stage);
			Root->SetBoolField(TEXT("pass"), Passed());
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FCheck& Check : Checks)
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("id"), Check.Id);
				Item->SetBoolField(TEXT("pass"), Check.bPass);
				Item->SetBoolField(TEXT("required"), Check.bRequired);
				Item->SetStringField(TEXT("detail"), Check.Detail);
				Items.Add(MakeShared<FJsonValueObject>(Item));
				const FString Line = FString::Printf(TEXT("[%s] %s: %s"),
					Check.bPass ? TEXT("PASS") : Check.bRequired ? TEXT("FAIL") : TEXT("WARN"), *Check.Id, *Check.Detail);
				if (Check.bRequired && !Check.bPass) Test.AddError(Line);
				else Test.AddInfo(Line);
			}
			Root->SetArrayField(TEXT("checks"), Items);
			FString Text;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
			FJsonSerializer::Serialize(Root, Writer);
			const FString Path = FPaths::ProjectSavedDir() / TEXT("HyperAIBench") / TEXT("verify") / (Task + TEXT(".json"));
			FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		FString Stage = TEXT("structure+runtime");

	private:
		FString Task;
		TArray<FCheck> Checks;
	};

	template <typename T>
	T* LoadSaved(const FString& PackageName)
	{
		if (!FPackageName::DoesPackageExist(PackageName)) return nullptr;
		return LoadObject<T>(nullptr, *(PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(PackageName)));
	}

	bool NearlyText(const FString& Value, const TCHAR* Expected)
	{
		return Value.TrimStartAndEnd().Equals(Expected, ESearchCase::IgnoreCase);
	}

	/** What a check needs to know about one placed actor, captured while it is loaded. */
	struct FActorFacts
	{
		FString Label;
		FString Class;
		FVector Location = FVector::ZeroVector;
		FBox Bounds = FBox(ForceInit);
		TArray<FString> Meshes;
		TArray<FString> SlotZeroMaterials;
		FString NiagaraAsset;
		bool bCamera = false;
		FVector CameraLocation = FVector::ZeroVector;
		FVector CameraForward = FVector::ForwardVector;
		bool bPointLight = false;
		bool bDirectionalLight = false;
		bool bSkyLight = false;
		bool bSkyAtmosphere = false;
	};

	FActorFacts Describe(AActor& Actor)
	{
		FActorFacts Facts;
		Facts.Label = Actor.GetActorLabel();
		Facts.Class = Actor.GetClass()->GetName();
		Facts.Location = Actor.GetActorLocation();
		Facts.Bounds = Actor.GetComponentsBoundingBox(/*bNonColliding=*/true);
		TInlineComponentArray<UActorComponent*> Components(&Actor);
		for (UActorComponent* Component : Components)
		{
			if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
			{
				if (Mesh->GetStaticMesh()) Facts.Meshes.Add(Mesh->GetStaticMesh()->GetPathName());
				if (const UMaterialInterface* Material = Mesh->GetMaterial(0))
				{
					Facts.SlotZeroMaterials.Add(Material->GetPathName());
					if (const UMaterial* Base = Material->GetMaterial()) Facts.SlotZeroMaterials.AddUnique(Base->GetPathName());
				}
			}
			if (const UNiagaraComponent* Niagara = Cast<UNiagaraComponent>(Component))
			{
				if (Niagara->GetAsset()) Facts.NiagaraAsset = Niagara->GetAsset()->GetPathName();
			}
			if (const UCameraComponent* Camera = Cast<UCameraComponent>(Component))
			{
				Facts.bCamera = true;
				Facts.CameraLocation = Camera->GetComponentLocation();
				Facts.CameraForward = Camera->GetForwardVector();
			}
			Facts.bPointLight |= Component->IsA<UPointLightComponent>();
			Facts.bDirectionalLight |= Component->IsA<UDirectionalLightComponent>();
			Facts.bSkyLight |= Component->IsA<USkyLightComponent>();
			Facts.bSkyAtmosphere |= Component->IsA<USkyAtmosphereComponent>();
		}
		Facts.Meshes.Sort();
		return Facts;
	}

	bool LoadLevel(const FString& PackageName, TArray<FActorFacts>& OutActors, FString& OutError)
	{
		OutActors.Reset();
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			OutError = TEXT("not saved");
			return false;
		}
		if (!FEditorFileUtils::LoadMap(PackageName, /*LoadAsTemplate=*/false, /*bShowProgress=*/false))
		{
			OutError = TEXT("the map did not load");
			return false;
		}
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World || World->GetOutermost()->GetName() != PackageName)
		{
			OutError = TEXT("a different map is open after loading");
			return false;
		}
		if (UWorldPartition* Partition = World->GetWorldPartition())
		{
			// Partitioned maps load nothing until asked; visit every actor, loading it for the callback.
			FWorldPartitionHelpers::ForEachActorWithLoading(Partition, [&OutActors](const FWorldPartitionActorDescInstance* Desc)
			{
				if (AActor* Actor = Desc ? Desc->GetActor() : nullptr) OutActors.Add(Describe(*Actor));
				return true;
			});
		}
		else
		{
			for (TActorIterator<AActor> It(World); It; ++It) OutActors.Add(Describe(**It));
		}
		return true;
	}

	/** Holds every actor of a partitioned level loaded while it lives, so a capture sees the whole scene. */
	struct FLoadedScene
	{
		TArray<FWorldPartitionReference> References;

		explicit FLoadedScene(UWorld* World)
		{
			UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
			if (!Partition) return;
			FWorldPartitionHelpers::ForEachActorDescInstance(Partition, [this, Partition](const FWorldPartitionActorDescInstance* Desc)
			{
				if (Desc) References.Emplace(Partition, Desc->GetGuid());
				return true;
			});
		}
	};

	constexpr int32 CaptureWidth = 320;
	constexpr int32 CaptureHeight = 180;

	/**
	 * An independent final-colour capture through a camera actor's view: engine APIs only, so the verifier never
	 * grades HyperAIStudio with HyperAIStudio's own capture tool. MaterialTime >= 0 forces material time.
	 */
	bool CaptureCamera(UWorld& World, const TCHAR* Label, const float MaterialTime, TArray<FColor>& OutPixels, FString& OutError)
	{
		const UCameraComponent* Camera = nullptr;
		for (TActorIterator<AActor> It(&World); It && !Camera; ++It)
		{
			if (It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)) Camera = It->FindComponentByClass<UCameraComponent>();
		}
		if (!Camera)
		{
			OutError = FString::Printf(TEXT("no camera labelled %s"), Label);
			return false;
		}
		FMinimalViewInfo View;
		const_cast<UCameraComponent*>(Camera)->GetCameraView(0.f, View);
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Target->RenderTargetFormat = RTF_RGBA8;
		Target->ClearColor = FLinearColor::Black;
		Target->InitAutoFormat(CaptureWidth, CaptureHeight);
		Target->UpdateResourceImmediate(/*bClearRenderTarget=*/true);
		USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Capture->CaptureSource = SCS_FinalColorLDR;
		Capture->TextureTarget = Target;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = true;
		Capture->FOVAngle = View.FOV;
		Capture->SetWorldLocationAndRotation(View.Location, View.Rotation);
		Capture->RegisterComponentWithWorld(&World);
		if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
		IConsoleVariable* TimeOverride = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Test.OverrideTimeMaterialExpressions"));
		if (TimeOverride && MaterialTime >= 0.f) TimeOverride->Set(MaterialTime, ECVF_SetByCode);
		bool bRead = false;
		// A few passes first, so exposure and temporal history settle.
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			World.SendAllEndOfFrameUpdates();
			Capture->CaptureScene();
			FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
			bRead = Resource && Resource->ReadPixels(OutPixels);
		}
		if (TimeOverride && MaterialTime >= 0.f) TimeOverride->Set(-1.f, ECVF_SetByCode);
		Capture->UnregisterComponent();
		if (!bRead || OutPixels.Num() != CaptureWidth * CaptureHeight)
		{
			OutError = TEXT("the capture could not be read back");
			return false;
		}
		return true;
	}

	struct FImageStats
	{
		float MeanLuma = 0.f;
		float LumaDeviation = 0.f;
		float BlackShare = 0.f;
		float CyanShare = 0.f;
		float OrangeShare = 0.f;
	};

	FImageStats Measure(const TArray<FColor>& Pixels)
	{
		FImageStats Stats;
		if (Pixels.IsEmpty()) return Stats;
		double Sum = 0.0;
		double SumSquares = 0.0;
		for (const FColor& Pixel : Pixels)
		{
			const double Luma = (0.2126 * Pixel.R + 0.7152 * Pixel.G + 0.0722 * Pixel.B) / 255.0;
			Sum += Luma;
			SumSquares += Luma * Luma;
			// Hue as seen: on the display-encoded values, since linear light pushes oranges toward red.
			const FLinearColor Hsv = FLinearColor(Pixel.R / 255.f, Pixel.G / 255.f, Pixel.B / 255.f).LinearRGBToHSV();
			Stats.BlackShare += Hsv.B < 0.03f ? 1.f : 0.f;
			Stats.CyanShare += Hsv.R >= 160.f && Hsv.R <= 220.f && Hsv.G > 0.25f && Hsv.B > 0.15f ? 1.f : 0.f;
			Stats.OrangeShare += Hsv.R >= 10.f && Hsv.R <= 55.f && Hsv.G > 0.35f && Hsv.B > 0.25f ? 1.f : 0.f;
		}
		const double Count = Pixels.Num();
		Stats.MeanLuma = static_cast<float>(Sum / Count);
		Stats.LumaDeviation = static_cast<float>(FMath::Sqrt(FMath::Max(0.0, SumSquares / Count - FMath::Square(Sum / Count))));
		Stats.BlackShare /= Count;
		Stats.CyanShare /= Count;
		Stats.OrangeShare /= Count;
		return Stats;
	}

	/** Share of pixels whose largest channel moved by more than 8 of 255. */
	float ChangedShare(const TArray<FColor>& A, const TArray<FColor>& B)
	{
		if (A.Num() != B.Num() || A.IsEmpty()) return 0.f;
		int32 Changed = 0;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			const int32 Delta = FMath::Max3(FMath::Abs(A[Index].R - B[Index].R), FMath::Abs(A[Index].G - B[Index].G), FMath::Abs(A[Index].B - B[Index].B));
			Changed += Delta > 8 ? 1 : 0;
		}
		return static_cast<float>(Changed) / A.Num();
	}

	/** Evidence for the scorecard, beside the task's JSON report. */
	FString SaveEvidence(const FString& Task, const TCHAR* View, const TArray<FColor>& Pixels)
	{
		TArray<FColor> Opaque = Pixels;
		for (FColor& Pixel : Opaque) Pixel.A = 255;
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(CaptureWidth, CaptureHeight, Opaque, Png);
		const FString Name = FString::Printf(TEXT("%s_%s.png"), *Task, View);
		FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("HyperAIBench") / TEXT("verify") / Name));
		return Name;
	}

	/** A view that shows lit structure: not black, not flat, not blown out. */
	bool ShowsLitStructure(const FImageStats& Stats)
	{
		return Stats.BlackShare < 0.9f && Stats.LumaDeviation > 0.02f && Stats.MeanLuma > 0.04f && Stats.MeanLuma < 0.97f;
	}

	FString Describe(const FImageStats& Stats)
	{
		return FString::Printf(TEXT("mean luma %.2f, deviation %.3f, %.0f%% black, %.1f%% cyan, %.1f%% orange"), Stats.MeanLuma,
			Stats.LumaDeviation, Stats.BlackShare * 100.f, Stats.CyanShare * 100.f, Stats.OrangeShare * 100.f);
	}

	const FActorFacts* FindLabel(const TArray<FActorFacts>& Actors, const TCHAR* Label)
	{
		return Actors.FindByPredicate([Label](const FActorFacts& Facts) { return Facts.Label.Equals(Label, ESearchCase::IgnoreCase); });
	}

	/** Whether a camera looks at a point: forward within ~30 degrees of the direction to it. */
	float FacingDot(const FActorFacts& Camera, const FVector& Target)
	{
		return static_cast<float>(FVector::DotProduct(Camera.CameraForward.GetSafeNormal(), (Target - Camera.CameraLocation).GetSafeNormal()));
	}

	float PitchDegrees(const FActorFacts& Camera)
	{
		return static_cast<float>(FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Camera.CameraForward.GetSafeNormal().Z, -1.0, 1.0))));
	}

	struct FEmitterCount
	{
		FString Name;
		int32 Particles = 0;
		int32 Spawned = 0;
		bool bGpu = false;
	};

	/** Runs the system in its own preview world for two spans and reads per-emitter counts after each. */
	void Simulate(UNiagaraSystem& System, TArray<FEmitterCount>& OutEarly, TArray<FEmitterCount>& OutLate)
	{
		FPreviewScene Scene{FPreviewScene::ConstructionValues()};
		UNiagaraComponent* Component = NewObject<UNiagaraComponent>(GetTransientPackage(), NAME_None, RF_Transient);
		Component->SetAutoActivate(false);
		Component->SetAsset(&System);
		Component->SetForceSolo(true);
		Scene.AddComponent(Component, FTransform::Identity);
		Component->Activate(/*bReset=*/true);
		auto Read = [Component](TArray<FEmitterCount>& Out)
		{
			const FNiagaraSystemInstanceControllerPtr Controller = Component->GetSystemInstanceController();
			const FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
			if (!Instance) return;
			for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
			{
				Out.Add({Emitter->GetEmitterHandle().GetName().ToString(), Emitter->GetNumParticles(),
					Emitter->GetTotalSpawnedParticles(), Emitter->GetSimTarget() == ENiagaraSimTarget::GPUComputeSim});
			}
		};
		Component->AdvanceSimulation(120, 1.f / 60.f);
		Read(OutEarly);
		// A burst that dies out is not a beacon; it must still be emitting several seconds later.
		Component->AdvanceSimulation(360, 1.f / 60.f);
		Read(OutLate);
		Scene.RemoveComponent(Component);
	}

	FString DescribeCounts(const TArray<FEmitterCount>& Counts)
	{
		TArray<FString> Parts;
		for (const FEmitterCount& Count : Counts)
		{
			Parts.Add(FString::Printf(TEXT("%s=%d%s"), *Count.Name, Count.Particles, Count.bGpu ? TEXT(" (gpu, estimate)") : TEXT("")));
		}
		return Parts.IsEmpty() ? FString(TEXT("no emitter instances")) : FString::Join(Parts, TEXT(", "));
	}

	bool AllLive(const TArray<FEmitterCount>& Counts, const int32 Expected)
	{
		if (Counts.Num() != Expected) return false;
		for (const FEmitterCount& Count : Counts)
		{
			if ((Count.bGpu ? Count.Spawned : Count.Particles) <= 0) return false;
		}
		return true;
	}

	bool IsTimeDriven(const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions, const int32 Depth = 0)
	{
		for (const UMaterialExpression* Expression : Expressions)
		{
			if (Expression && (Expression->IsA<UMaterialExpressionTime>() || Expression->IsA<UMaterialExpressionPanner>())) return true;
			const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
			const UMaterialFunction* Function = Call ? Cast<UMaterialFunction>(Call->MaterialFunction) : nullptr;
			if (Function && Depth < 3 && IsTimeDriven(Function->GetExpressions(), Depth + 1)) return true;
		}
		return false;
	}

	UEdGraph* FindGraph(const UBlueprint& Blueprint, const FName Name)
	{
		for (UEdGraph* Graph : Blueprint.FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == Name) return Graph;
		}
		return nullptr;
	}

	/** True when following input links back from Node reaches a node of the given class. */
	bool FeedsFrom(const UEdGraphNode* Node, const UClass* Wanted, TSet<const UEdGraphNode*>& Seen)
	{
		if (!Node || Seen.Contains(Node) || Seen.Num() > 256) return false;
		Seen.Add(Node);
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input) continue;
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				const UEdGraphNode* Source = Linked ? Linked->GetOwningNode() : nullptr;
				if (Source && (Source->IsA(Wanted) || FeedsFrom(Source, Wanted, Seen))) return true;
			}
		}
		return false;
	}

	bool MentionsSpeed(const UEdGraph* Graph)
	{
		if (!Graph) return false;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
			if (Get && Get->GetVarName() == TEXT("Speed")) return true;
			if (Node && Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(TEXT("Speed"))) return true;
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIBenchVerifyNiagara, "HyperAIBench.Verify.01Niagara", HyperAIStudio::Benchmark::Flags)

bool FHyperAIBenchVerifyNiagara::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Benchmark;
	FReport Report(TEXT("01-niagara"));
	UNiagaraSystem* System = LoadSaved<UNiagaraSystem>(TEXT("/Game/Bench/Niagara/NS_SignalBeacon"));
	if (Report.Add(TEXT("system.saved"), System != nullptr, System ? TEXT("NS_SignalBeacon loads from disk") : TEXT("NS_SignalBeacon is not saved")))
	{
		TArray<FString> Names;
		bool bAllEnabled = true;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			Names.Add(Handle.GetName().ToString());
			bAllEnabled &= Handle.GetIsEnabled();
		}
		const bool bNamed = Names.ContainsByPredicate([](const FString& N) { return N.Equals(TEXT("Core"), ESearchCase::IgnoreCase); })
			&& Names.ContainsByPredicate([](const FString& N) { return N.Equals(TEXT("Halo"), ESearchCase::IgnoreCase); });
		Report.Add(TEXT("system.emitters"), Names.Num() == 2 && bNamed && bAllEnabled,
			FString::Printf(TEXT("emitters: %s%s"), *FString::Join(Names, TEXT(", ")), bAllEnabled ? TEXT("") : TEXT(" (some disabled)")));
		System->WaitForCompilationComplete(/*bIncludingGPUShaders=*/false, /*bShowProgress=*/false);
		Report.Add(TEXT("system.compiled"), System->IsValid(), System->IsValid() ? TEXT("valid after compilation") : TEXT("the system is not valid after compiling"));

		TArray<FEmitterCount> Early;
		TArray<FEmitterCount> Late;
		Simulate(*System, Early, Late);
		Report.Add(TEXT("runtime.live_2s"), AllLive(Early, 2), DescribeCounts(Early));
		Report.Add(TEXT("runtime.live_8s"), AllLive(Late, 2), DescribeCounts(Late));
	}

	TArray<FActorFacts> Actors;
	FString Error;
	const bool bLoaded = LoadLevel(TEXT("/Game/Bench/Niagara/L_Bench_Niagara"), Actors, Error);
	if (Report.Add(TEXT("level.saved"), bLoaded, bLoaded ? FString(TEXT("L_Bench_Niagara loads")) : Error))
	{
		const FActorFacts* Beacon = FindLabel(Actors, TEXT("Beacon"));
		const bool bUsesSystem = Beacon && Beacon->NiagaraAsset.StartsWith(TEXT("/Game/Bench/Niagara/NS_SignalBeacon"));
		Report.Add(TEXT("level.beacon"), bUsesSystem && Beacon->Location.Size() < 50.0,
			Beacon ? FString::Printf(TEXT("Beacon at %s playing %s"), *Beacon->Location.ToCompactString(), *Beacon->NiagaraAsset) : TEXT("no actor labelled Beacon"));
		const FActorFacts* Camera = FindLabel(Actors, TEXT("CAM_Beacon"));
		const FVector Target = Beacon ? Beacon->Location : FVector::ZeroVector;
		const double Distance = Camera ? FVector::Dist(Camera->CameraLocation, Target) : 0.0;
		Report.Add(TEXT("level.camera"), Camera && Camera->bCamera && Distance >= 300.0 && Distance <= 1200.0 && FacingDot(*Camera, Target) > 0.85f,
			Camera ? FString::Printf(TEXT("CAM_Beacon %.0f units away, facing %.2f"), Distance, FacingDot(*Camera, Target)) : TEXT("no camera labelled CAM_Beacon"));
	}
	Report.Finish(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIBenchVerifyMaterial, "HyperAIBench.Verify.02Material", HyperAIStudio::Benchmark::Flags)

bool FHyperAIBenchVerifyMaterial::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Benchmark;
	FReport Report(TEXT("02-material"));
	const FString MaterialPath = TEXT("/Game/Bench/Material/M_PulseGrid.M_PulseGrid");
	UMaterial* Material = LoadSaved<UMaterial>(TEXT("/Game/Bench/Material/M_PulseGrid"));
	if (Report.Add(TEXT("material.saved"), Material != nullptr, Material ? TEXT("M_PulseGrid loads from disk") : TEXT("M_PulseGrid is not saved as a Material")))
	{
		FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
		if (Resource) Resource->FinishCompilation();
		const TArray<FString> Errors = Resource ? Resource->GetCompileErrors() : TArray<FString>();
		Report.Add(TEXT("material.compiles"), Resource && Errors.IsEmpty(),
			!Resource ? TEXT("no material resource for this platform") : Errors.IsEmpty() ? TEXT("no compile errors") : Errors[0]);
		// A structural hint only: visual.animated is the proof, and a graph can animate through nodes this misses.
		Report.Add(TEXT("material.animated"), IsTimeDriven(Material->GetExpressions()), TEXT("a Time or Panner node drives the graph"), /*bRequired=*/false);
		const UMaterialEditorOnlyData* Inputs = Material->GetEditorOnlyData();
		Report.Add(TEXT("material.outputs"), Inputs && (Inputs->EmissiveColor.IsConnected() || Inputs->BaseColor.IsConnected()),
			TEXT("emissive or base colour is connected"));
	}

	TArray<FActorFacts> Actors;
	FString Error;
	const bool bLoaded = LoadLevel(TEXT("/Game/Bench/Material/L_Bench_Material"), Actors, Error);
	if (Report.Add(TEXT("level.saved"), bLoaded, bLoaded ? FString(TEXT("L_Bench_Material loads")) : Error))
	{
		const FActorFacts* Plane = FindLabel(Actors, TEXT("SM_Plane"));
		const FVector Size = Plane ? Plane->Bounds.GetSize() : FVector::ZeroVector;
		const bool bFlat = Plane && !Plane->Meshes.IsEmpty() && Size.Z <= FMath::Max(10.0, 0.05 * FMath::Max(Size.X, Size.Y));
		Report.Add(TEXT("level.plane"), bFlat, Plane ? FString::Printf(TEXT("SM_Plane bounds %s"), *Size.ToCompactString()) : TEXT("no actor labelled SM_Plane"));
		Report.Add(TEXT("level.assigned"), Plane && Plane->SlotZeroMaterials.Contains(MaterialPath),
			Plane ? FString::Printf(TEXT("slot 0 uses %s"), Plane->SlotZeroMaterials.IsEmpty() ? TEXT("nothing") : *FString::Join(Plane->SlotZeroMaterials, TEXT(" / "))) : TEXT("no SM_Plane"));
		const FActorFacts* Camera = FindLabel(Actors, TEXT("CAM_Material"));
		const FVector Target = Plane ? Plane->Bounds.GetCenter() : FVector::ZeroVector;
		Report.Add(TEXT("level.camera"), Camera && Camera->bCamera && FacingDot(*Camera, Target) > 0.7f,
			Camera ? FString::Printf(TEXT("CAM_Material facing %.2f"), FacingDot(*Camera, Target)) : TEXT("no camera labelled CAM_Material"));

		// Visual: what CAM_Material sees at six forced material times, spread over 2.5 s so a sweep with a
		// period of a few seconds shows up at least once.
		Report.Stage = TEXT("structure+runtime+visual");
		UWorld* World = GEditor->GetEditorWorldContext().World();
		const FLoadedScene Scene(World);
		constexpr int32 FrameCount = 6;
		TArray<FColor> Frames[FrameCount];
		const float Times[FrameCount] = {0.f, 0.33f, 0.66f, 1.f, 1.7f, 2.5f};
		FString CaptureError;
		bool bCaptured = true;
		for (int32 Index = 0; Index < FrameCount && bCaptured; ++Index)
		{
			bCaptured = CaptureCamera(*World, TEXT("CAM_Material"), Times[Index], Frames[Index], CaptureError);
		}
		// An emissive pulse can leave one instant black, so the brightest frame is judged and kept.
		int32 Brightest = 0;
		for (int32 Index = 1; Index < FrameCount && bCaptured; ++Index)
		{
			if (Measure(Frames[Index]).MeanLuma > Measure(Frames[Brightest]).MeanLuma) Brightest = Index;
		}
		if (Report.Add(TEXT("visual.captured"), bCaptured, bCaptured ? SaveEvidence(TEXT("02-material"), TEXT("CAM_Material"), Frames[Brightest]) : CaptureError))
		{
			const FImageStats Stats = Measure(Frames[Brightest]);
			Report.Add(TEXT("visual.nonblank"), Stats.BlackShare < 0.98f && (Stats.LumaDeviation > 0.01f || Stats.MeanLuma > 0.02f),
				FString::Printf(TEXT("brightest frame, material time %.2f: %s"), Times[Brightest], *Describe(Stats)));
			float Change = 0.f;
			for (int32 Index = 1; Index < FrameCount; ++Index) Change = FMath::Max(Change, ChangedShare(Frames[0], Frames[Index]));
			Report.Add(TEXT("visual.animated"), Change > 0.01f, FString::Printf(TEXT("up to %.1f%% of pixels change from material time 0 over 2.5 s"), Change * 100.f));
			float Cyan = 0.f;
			float Orange = 0.f;
			for (const TArray<FColor>& Frame : Frames)
			{
				const FImageStats Each = Measure(Frame);
				Cyan = FMath::Max(Cyan, Each.CyanShare);
				Orange = FMath::Max(Orange, Each.OrangeShare);
			}
			// The task asks for a cyan grid and an orange pulse; a frame without either is not that material.
			Report.Add(TEXT("visual.palette"), Cyan > 0.002f && Orange > 0.002f,
				FString::Printf(TEXT("cyan up to %.1f%%, orange up to %.1f%% of pixels"), Cyan * 100.f, Orange * 100.f));
		}
	}
	Report.Finish(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIBenchVerifyLevel, "HyperAIBench.Verify.03Level", HyperAIStudio::Benchmark::Flags)

bool FHyperAIBenchVerifyLevel::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Benchmark;
	FReport Report(TEXT("03-level"));
	TArray<FActorFacts> Actors;
	FString Error;
	const bool bLoaded = LoadLevel(TEXT("/Game/Bench/Level/L_Bench_Courtyard"), Actors, Error);
	if (Report.Add(TEXT("level.saved"), bLoaded, bLoaded ? FString(TEXT("L_Bench_Courtyard loads")) : Error))
	{
		auto Any = [&Actors](bool FActorFacts::* Flag) { return Actors.ContainsByPredicate([Flag](const FActorFacts& F) { return F.*Flag; }); };
		Report.Add(TEXT("lighting.sun"), Any(&FActorFacts::bDirectionalLight), TEXT("a directional light"));
		Report.Add(TEXT("lighting.sky_light"), Any(&FActorFacts::bSkyLight), TEXT("a sky light"));
		Report.Add(TEXT("lighting.atmosphere"), Any(&FActorFacts::bSkyAtmosphere), TEXT("a sky atmosphere"));

		TArray<const FActorFacts*> Points;
		for (const FActorFacts& Facts : Actors) if (Facts.bPointLight) Points.Add(&Facts);
		const bool bMirroredPair = Points.ContainsByPredicate([&Points](const FActorFacts* A)
		{
			return A->Location.X > 10.0 && Points.ContainsByPredicate([A](const FActorFacts* B)
			{
				return FMath::Abs(A->Location.X + B->Location.X) < 25.0 && FMath::Abs(A->Location.Y - B->Location.Y) < 25.0
					&& FMath::Abs(A->Location.Z - B->Location.Z) < 25.0;
			});
		});
		Report.Add(TEXT("lighting.pylon_lights"), Points.Num() >= 2 && bMirroredPair,
			FString::Printf(TEXT("%d point lights, mirrored pair %s"), Points.Num(), bMirroredPair ? TEXT("found") : TEXT("missing")));

		TArray<const FActorFacts*> Props;
		for (const FActorFacts& Facts : Actors) if (!Facts.Meshes.IsEmpty() && Facts.Bounds.IsValid) Props.Add(&Facts);
		// A floor is a wide piece whose walking surface, its top, is near Z=0; a thick slab counts as much as a plane.
		auto IsSlab = [](const FActorFacts* P)
		{
			const FVector Size = P->Bounds.GetSize();
			return Size.Z < 0.5 * FMath::Min(Size.X, Size.Y);
		};
		const bool bFloor = Props.ContainsByPredicate([&IsSlab](const FActorFacts* P)
		{
			const FVector Size = P->Bounds.GetSize();
			return Size.X >= 2000.0 && Size.Y >= 2000.0 && IsSlab(P) && FMath::Abs(P->Bounds.Max.Z) < 200.0;
		});
		const FActorFacts* Widest = nullptr;
		for (const FActorFacts* P : Props)
		{
			const FVector Size = P->Bounds.GetSize();
			if (IsSlab(P) && (!Widest || Size.X * Size.Y > Widest->Bounds.GetSize().X * Widest->Bounds.GetSize().Y)) Widest = P;
		}
		Report.Add(TEXT("layout.floor"), bFloor, Widest
			? FString::Printf(TEXT("needs one piece at least 2000 x 2000 with its top near Z=0; widest flat piece is %s, %.0f x %.0f with its top at Z=%.0f"),
				*Widest->Label, Widest->Bounds.GetSize().X, Widest->Bounds.GetSize().Y, Widest->Bounds.Max.Z)
			: FString(TEXT("needs one piece at least 2000 x 2000 with its top near Z=0; there is no flat piece")));
		int32 Walls = 0;
		for (const FActorFacts* P : Props)
		{
			const FVector Center = P->Bounds.GetCenter();
			Walls += P->Bounds.GetSize().Z >= 150.0 && FMath::Max(FMath::Abs(Center.X), FMath::Abs(Center.Y)) >= 1000.0 ? 1 : 0;
		}
		Report.Add(TEXT("layout.walls"), Walls >= 4, FString::Printf(TEXT("%d tall pieces near the perimeter"), Walls));
		const bool bPylon = Props.ContainsByPredicate([](const FActorFacts* P)
		{
			const FVector Center = P->Bounds.GetCenter();
			return FMath::Abs(Center.X) < 300.0 && FMath::Abs(Center.Y) < 300.0 && P->Bounds.GetSize().Z >= 200.0;
		});
		Report.Add(TEXT("layout.pylon"), bPylon, TEXT("a tall piece at the centre"));

		// Mirror each prop's bounds across X=0 and look for a prop with the same meshes there; a prop that
		// straddles the plane can be its own mirror.
		TArray<FString> Unmatched;
		for (const FActorFacts* P : Props)
		{
			const FBox Mirrored(FVector(-P->Bounds.Max.X, P->Bounds.Min.Y, P->Bounds.Min.Z), FVector(-P->Bounds.Min.X, P->Bounds.Max.Y, P->Bounds.Max.Z));
			const bool bMatched = Props.ContainsByPredicate([P, &Mirrored](const FActorFacts* Q)
			{
				return Q->Meshes == P->Meshes && Q->Bounds.Min.Equals(Mirrored.Min, 30.0) && Q->Bounds.Max.Equals(Mirrored.Max, 30.0);
			});
			if (!bMatched) Unmatched.Add(P->Label);
		}
		Report.Add(TEXT("layout.symmetric"), !Props.IsEmpty() && Unmatched.IsEmpty(),
			FString::Printf(TEXT("%d props, %d without a mirror%s%s"), Props.Num(), Unmatched.Num(), Unmatched.IsEmpty() ? TEXT("") : TEXT(": "),
				*FString::Join(TArrayView<const FString>(Unmatched.GetData(), FMath::Min(Unmatched.Num(), 8)), TEXT(", "))));

		const FActorFacts* Front = FindLabel(Actors, TEXT("CAM_Front"));
		const FActorFacts* Top = FindLabel(Actors, TEXT("CAM_Top"));
		Report.Add(TEXT("views.front"), Front && Front->bCamera && PitchDegrees(*Front) > -35.f && PitchDegrees(*Front) < 20.f,
			Front ? FString::Printf(TEXT("CAM_Front pitch %.0f"), PitchDegrees(*Front)) : TEXT("no camera labelled CAM_Front"));
		Report.Add(TEXT("views.top"), Top && Top->bCamera && PitchDegrees(*Top) <= -60.f,
			Top ? FString::Printf(TEXT("CAM_Top pitch %.0f"), PitchDegrees(*Top)) : TEXT("no camera labelled CAM_Top"));

		// Visual: both required views show lit structure.
		Report.Stage = TEXT("structure+runtime+visual");
		UWorld* World = GEditor->GetEditorWorldContext().World();
		const FLoadedScene Scene(World);
		for (const TCHAR* View : {TEXT("CAM_Front"), TEXT("CAM_Top")})
		{
			TArray<FColor> Pixels;
			FString CaptureError;
			const bool bCaptured = CaptureCamera(*World, View, -1.f, Pixels, CaptureError);
			const FImageStats Stats = Measure(Pixels);
			Report.Add(FString::Printf(TEXT("visual.%s"), View == FString(TEXT("CAM_Front")) ? TEXT("front") : TEXT("top")),
				bCaptured && ShowsLitStructure(Stats),
				bCaptured ? SaveEvidence(TEXT("03-level"), View, Pixels) + TEXT(": ") + Describe(Stats) : CaptureError);
		}
	}
	Report.Finish(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIBenchVerifyUMG, "HyperAIBench.Verify.04UMG", HyperAIStudio::Benchmark::Flags)

bool FHyperAIBenchVerifyUMG::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Benchmark;
	FReport Report(TEXT("04-umg"));
	UWidgetBlueprint* Widget = LoadSaved<UWidgetBlueprint>(TEXT("/Game/Bench/UI/WBP_SignalRelay"));
	if (Report.Add(TEXT("wbp.saved"), Widget != nullptr, Widget ? TEXT("WBP_SignalRelay loads from disk") : TEXT("WBP_SignalRelay is not saved")))
	{
		FKismetEditorUtilities::CompileBlueprint(Widget);
		Report.Add(TEXT("wbp.compiles"), Widget->Status != BS_Error, Widget->Status == BS_Error ? TEXT("compile errors") : TEXT("compiles"));
		UWidgetTree* Tree = Widget->WidgetTree;
		UWidget* Panel = Tree ? Tree->FindWidget(TEXT("Panel")) : nullptr;
		Report.Add(TEXT("wbp.panel"), Panel != nullptr, Panel ? FString::Printf(TEXT("Panel is a %s"), *Panel->GetClass()->GetName()) : TEXT("no widget named Panel"));
		if (const UBorder* Border = Cast<UBorder>(Panel))
		{
			const float Luminance = Border->GetBrushColor().GetLuminance();
			Report.Add(TEXT("wbp.panel_dark"), Luminance < 0.25f, FString::Printf(TEXT("brush luminance %.2f"), Luminance), /*bRequired=*/false);
		}
		bool bTitle = false;
		bool bStatus = false;
		float Percent = -1.f;
		if (Tree)
		{
			Tree->ForEachWidget([&](UWidget* Child)
			{
				if (const UTextBlock* Text = Cast<UTextBlock>(Child))
				{
					bTitle |= NearlyText(Text->GetText().ToString(), TEXT("SIGNAL RELAY"));
					bStatus |= NearlyText(Text->GetText().ToString(), TEXT("LINK STABLE"));
				}
				if (const UProgressBar* Bar = Cast<UProgressBar>(Child)) Percent = Bar->GetPercent();
			});
		}
		Report.Add(TEXT("wbp.title"), bTitle, TEXT("a text reading SIGNAL RELAY"));
		Report.Add(TEXT("wbp.status"), bStatus, TEXT("a text reading LINK STABLE"));
		Report.Add(TEXT("wbp.progress"), FMath::IsNearlyEqual(Percent, 0.75f, 0.011f), FString::Printf(TEXT("progress bar at %.2f"), Percent));

		const TObjectPtr<UWidgetAnimation>* Found = Widget->Animations.FindByPredicate([](const UWidgetAnimation* A)
		{
			return A && (A->GetName().Equals(TEXT("Pulse"), ESearchCase::IgnoreCase) || A->GetDisplayLabel().Equals(TEXT("Pulse"), ESearchCase::IgnoreCase));
		});
		UWidgetAnimation* Pulse = Found ? Found->Get() : nullptr;
		UMovieScene* Scene = Pulse ? Pulse->GetMovieScene() : nullptr;
		if (Report.Add(TEXT("anim.exists"), Scene != nullptr, Scene ? TEXT("animation Pulse") : TEXT("no animation named Pulse")))
		{
			const TRange<FFrameNumber> Range = Scene->GetPlaybackRange();
			const FFrameRate Rate = Scene->GetTickResolution();
			const double Seconds = Rate.AsSeconds(FFrameTime(Range.GetUpperBoundValue() - Range.GetLowerBoundValue()));
			Report.Add(TEXT("anim.duration"), FMath::IsNearlyEqual(Seconds, 1.0, 0.1), FString::Printf(TEXT("%.2f seconds"), Seconds));

			UMovieSceneFloatTrack* Opacity = nullptr;
			for (const FWidgetAnimationBinding& Binding : Pulse->GetBindings())
			{
				if (!Binding.WidgetName.ToString().Equals(TEXT("Panel"), ESearchCase::IgnoreCase)) continue;
				for (UMovieSceneTrack* Track : Scene->FindTracks(UMovieSceneFloatTrack::StaticClass(), Binding.AnimationGuid))
				{
					UMovieSceneFloatTrack* Float = Cast<UMovieSceneFloatTrack>(Track);
					if (Float && Float->GetPropertyName() == TEXT("RenderOpacity")) Opacity = Float;
				}
			}
			const UMovieSceneFloatSection* Section = Opacity && !Opacity->GetAllSections().IsEmpty()
				? Cast<UMovieSceneFloatSection>(Opacity->GetAllSections()[0]) : nullptr;
			if (Report.Add(TEXT("anim.opacity_track"), Section != nullptr, Section ? TEXT("Panel.RenderOpacity is animated") : TEXT("no RenderOpacity track bound to Panel")))
			{
				float Start = -1.f;
				float End = -1.f;
				Section->GetChannel().Evaluate(FFrameTime(Range.GetLowerBoundValue()), Start);
				Section->GetChannel().Evaluate(FFrameTime(Range.GetUpperBoundValue()), End);
				Report.Add(TEXT("anim.opacity_values"), FMath::IsNearlyEqual(Start, 0.3f, 0.05f) && FMath::IsNearlyEqual(End, 1.0f, 0.05f),
					FString::Printf(TEXT("opacity %.2f at the start, %.2f at the end"), Start, End));
			}
		}
	}
	Report.Finish(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIBenchVerifyAnimBP, "HyperAIBench.Verify.05AnimBP", HyperAIStudio::Benchmark::Flags)

bool FHyperAIBenchVerifyAnimBP::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::Benchmark;
	FReport Report(TEXT("05-animbp"));
	const FString BlendSpacePath = TEXT("/Game/Bench/Anim/BS_Locomotion.BS_Locomotion");
	UBlendSpace* BlendSpace = LoadSaved<UBlendSpace>(TEXT("/Game/Bench/Anim/BS_Locomotion"));
	if (Report.Add(TEXT("bs.saved"), BlendSpace != nullptr, BlendSpace ? TEXT("BS_Locomotion loads from disk") : TEXT("BS_Locomotion is not saved as a Blend Space")))
	{
		const USkeleton* Skeleton = BlendSpace->GetSkeleton();
		Report.Add(TEXT("bs.skeleton"), Skeleton && Skeleton->GetPathName() == SkeletonPath, Skeleton ? Skeleton->GetPathName() : TEXT("no skeleton"));
		const FBlendParameter& Axis = BlendSpace->GetBlendParameter(0);
		Report.Add(TEXT("bs.axis"), Axis.DisplayName.Equals(TEXT("Speed"), ESearchCase::IgnoreCase) && FMath::IsNearlyEqual(Axis.Min, 0.f, 1.f) && FMath::IsNearlyEqual(Axis.Max, 500.f, 1.f),
			FString::Printf(TEXT("axis %s from %.0f to %.0f"), *Axis.DisplayName, Axis.Min, Axis.Max));
		bool bAllValid = true;
		bool bIdle = false;
		bool bWalk = false;
		bool bJog = false;
		TArray<FString> Samples;
		for (const FBlendSample& Sample : BlendSpace->GetBlendSamples())
		{
			const FString Name = Sample.Animation ? Sample.Animation->GetName() : FString(TEXT("none"));
			const double X = Sample.SampleValue.X;
			bAllValid &= Sample.bIsValid && Sample.Animation != nullptr;
			bIdle |= FMath::Abs(X) <= 25.0 && Name.Contains(TEXT("Idle"));
			bWalk |= X >= 150.0 && X <= 250.0 && Name.Contains(TEXT("Walk"));
			bJog |= FMath::Abs(X - 500.0) <= 25.0 && (Name.Contains(TEXT("Jog")) || Name.Contains(TEXT("Run")));
			Samples.Add(FString::Printf(TEXT("%s@%.0f"), *Name, X));
		}
		Report.Add(TEXT("bs.samples"), bAllValid && bIdle && bWalk && bJog, FString::Join(Samples, TEXT(", ")));
	}

	UAnimBlueprint* Anim = LoadSaved<UAnimBlueprint>(TEXT("/Game/Bench/Anim/ABP_Bench"));
	if (Report.Add(TEXT("abp.saved"), Anim != nullptr, Anim ? TEXT("ABP_Bench loads from disk") : TEXT("ABP_Bench is not saved")))
	{
		Report.Add(TEXT("abp.skeleton"), Anim->TargetSkeleton && Anim->TargetSkeleton->GetPathName() == SkeletonPath,
			Anim->TargetSkeleton ? Anim->TargetSkeleton->GetPathName() : TEXT("no target skeleton"));
		FKismetEditorUtilities::CompileBlueprint(Anim);
		Report.Add(TEXT("abp.compiles"), Anim->Status != BS_Error, Anim->Status == BS_Error ? TEXT("compile errors") : TEXT("compiles"));
		const FBPVariableDescription* Speed = Anim->NewVariables.FindByPredicate([](const FBPVariableDescription& V) { return V.VarName == TEXT("Speed"); });
		Report.Add(TEXT("abp.speed_variable"), Speed && (Speed->VarType.PinCategory == UEdGraphSchema_K2::PC_Real || Speed->VarType.PinCategory == UEdGraphSchema_K2::PC_Float),
			Speed ? FString::Printf(TEXT("Speed is %s"), *Speed->VarType.PinCategory.ToString()) : TEXT("no variable named Speed"));

		TArray<UAnimGraphNode_StateMachineBase*> Machines;
		FBlueprintEditorUtils::GetAllNodesOfClass(Anim, Machines);
		const UAnimationStateMachineGraph* Graph = nullptr;
		TMap<FString, const UAnimStateNode*> States;
		for (const UAnimGraphNode_StateMachineBase* Machine : Machines)
		{
			TMap<FString, const UAnimStateNode*> Found;
			if (!Machine || !Machine->EditorStateMachineGraph) continue;
			for (const UEdGraphNode* Node : Machine->EditorStateMachineGraph->Nodes)
			{
				if (const UAnimStateNode* State = Cast<UAnimStateNode>(Node)) Found.Add(State->GetStateName(), State);
			}
			if (Found.Contains(TEXT("Idle")) || Found.Contains(TEXT("Locomotion")) || !Graph)
			{
				Graph = Machine->EditorStateMachineGraph;
				States = MoveTemp(Found);
			}
		}
		TArray<FString> StateNames;
		States.GetKeys(StateNames);
		Report.Add(TEXT("sm.states"), States.Num() == 2 && States.Contains(TEXT("Idle")) && States.Contains(TEXT("Locomotion")),
			Graph ? FString::Printf(TEXT("states: %s"), *FString::Join(StateNames, TEXT(", "))) : TEXT("no state machine"));

		const UAnimStateTransitionNode* ToLocomotion = nullptr;
		const UAnimStateTransitionNode* ToIdle = nullptr;
		if (Graph)
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node);
				if (!Transition || !Transition->GetPreviousState() || !Transition->GetNextState()) continue;
				const FString From = Transition->GetPreviousState()->GetStateName();
				const FString To = Transition->GetNextState()->GetStateName();
				if (From == TEXT("Idle") && To == TEXT("Locomotion")) ToLocomotion = Transition;
				if (From == TEXT("Locomotion") && To == TEXT("Idle")) ToIdle = Transition;
			}
		}
		Report.Add(TEXT("sm.transitions"), ToLocomotion && ToIdle,
			FString::Printf(TEXT("Idle->Locomotion %s, Locomotion->Idle %s"), ToLocomotion ? TEXT("yes") : TEXT("no"), ToIdle ? TEXT("yes") : TEXT("no")));
		Report.Add(TEXT("sm.rules_use_speed"), ToLocomotion && ToIdle && MentionsSpeed(ToLocomotion->GetBoundGraph()) && MentionsSpeed(ToIdle->GetBoundGraph()),
			TEXT("both transition rules read Speed"));

		const UAnimStateNode* const* Locomotion = States.Find(TEXT("Locomotion"));
		bool bBlendSpace = false;
		bool bDriven = false;
		if (Locomotion && (*Locomotion)->GetBoundGraph())
		{
			for (const UEdGraphNode* Node : (*Locomotion)->GetBoundGraph()->Nodes)
			{
				const UAnimGraphNode_BlendSpaceBase* Player = Cast<UAnimGraphNode_BlendSpaceBase>(Node);
				const UBlendSpace* Played = Player ? Cast<UBlendSpace>(Player->GetAnimationAsset()) : nullptr;
				if (!Played || Played->GetPathName() != BlendSpacePath) continue;
				bBlendSpace = true;
				const UEdGraphPin* X = Player->FindPin(TEXT("X"));
				bDriven |= (X && !X->LinkedTo.IsEmpty()) || Player->HasBinding(TEXT("X"));
			}
		}
		Report.Add(TEXT("sm.locomotion_blend_space"), bBlendSpace && bDriven,
			FString::Printf(TEXT("BS_Locomotion %s, X %s"), bBlendSpace ? TEXT("plays") : TEXT("missing"), bDriven ? TEXT("driven") : TEXT("not driven")));

		const UAnimStateNode* const* Idle = States.Find(TEXT("Idle"));
		bool bIdleClip = false;
		if (Idle && (*Idle)->GetBoundGraph())
		{
			for (const UEdGraphNode* Node : (*Idle)->GetBoundGraph()->Nodes)
			{
				const UAnimGraphNode_SequencePlayer* Player = Cast<UAnimGraphNode_SequencePlayer>(Node);
				bIdleClip |= Player && Player->GetAnimationAsset() && Player->GetAnimationAsset()->GetName().Contains(TEXT("Idle"));
			}
		}
		Report.Add(TEXT("sm.idle_clip"), bIdleClip, TEXT("Idle plays an idle sequence"));

		bool bOutput = false;
		if (const UEdGraph* AnimGraph = FindGraph(*Anim, TEXT("AnimGraph")))
		{
			for (const UEdGraphNode* Node : AnimGraph->Nodes)
			{
				TSet<const UEdGraphNode*> Seen;
				bOutput |= Node && Node->IsA<UAnimGraphNode_Root>() && FeedsFrom(Node, UAnimGraphNode_StateMachineBase::StaticClass(), Seen);
			}
		}
		Report.Add(TEXT("abp.output"), bOutput, TEXT("the output pose is fed by the state machine"));
	}
	Report.Finish(*this);
	return true;
}

#endif
