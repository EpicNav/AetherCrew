// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioBot.h"
#include "HyperAIStudioBotAim.h"
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Slate/SceneViewport.h"
#include "Tests/HyperAIStudioBotFixtures.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SViewport.h"

namespace HyperAIStudio::BotAimTests
{
	using namespace HyperAIStudio::Bot::Aim;

	FHyperAIBotStep Make(const EHyperAIBotAction Action)
	{
		FHyperAIBotStep Step;
		Step.Action = Action;
		return Step;
	}

	bool Passed(const FHyperAIBotResult& Result)
	{
		return Result.State == FHyperAIBotResult::EState::Passed;
	}

	/** Degrees between two headings, level. */
	double Between(const FVector& A, const FVector& B)
	{
		const FVector X = A.GetSafeNormal2D();
		const FVector Y = B.GetSafeNormal2D();
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(X, Y), -1.0, 1.0)));
	}

	/** An isometric game's evidence, as Aetherfall shows it: the cursor hidden, an aim component, the camera high and steep, and Mouse XY on a look action. */
	FEvidence Isometric()
	{
		FEvidence Evidence;
		Evidence.bViewport = true;
		Evidence.bCursorShown = false;
		Evidence.Reader = TEXT("AimComponent.GetAimWorldLocation");
		Evidence.CameraPitch = -50.0;
		Evidence.CameraDistance = 1800.0;
		Evidence.MouseAction = TEXT("IA_Look_Mouse");
		Evidence.bMouseLooks = true;
		Evidence.StickAction = TEXT("IA_Look_Stick");
		Evidence.InputType = TEXT("MouseAndKeyboard");
		return Evidence;
	}

	FEvidence ThirdPerson()
	{
		FEvidence Evidence;
		Evidence.bViewport = true;
		Evidence.CameraPitch = -12.0;
		Evidence.CameraDistance = 420.0;
		Evidence.MouseAction = TEXT("IA_Look");
		Evidence.bMouseLooks = true;
		Evidence.StickAction = TEXT("IA_Look");
		return Evidence;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBotAimRulesTest,
	"HyperAIStudio.Bot.Aim.Rules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBotAimRulesTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BotAimTests;
	// Auto's choice.
	const FChoice Iso = Choose(Isometric());
	TestEqual(TEXT("An isometric game with an aim of its own aims by the cursor, hidden or not: ") + Iso.Why, ModeName(Iso.Mode), FString(TEXT("cursor")));
	TestTrue(TEXT("and says why"), Iso.Why.Contains(TEXT("camera looks down")) && Iso.Why.Contains(TEXT("AimComponent.GetAimWorldLocation")));
	const FChoice Chase = Choose(ThirdPerson());
	TestEqual(TEXT("A third-person game aims with the view: ") + Chase.Why, ModeName(Chase.Mode), FString(TEXT("look")));
	FEvidence TopDown = ThirdPerson();
	TopDown.bCursorShown = true;
	TopDown.CameraPitch = -60.0;
	TopDown.CameraDistance = 1400.0;
	TopDown.bMouseLooks = false;
	TestEqual(TEXT("A top-down game with its cursor showing aims by the cursor"), ModeName(Choose(TopDown).Mode), FString(TEXT("cursor")));
	FEvidence Pad = Isometric();
	Pad.InputType = TEXT("Gamepad");
	const FChoice Stick = Choose(Pad);
	TestEqual(TEXT("A player CommonInput says is on a gamepad aims by the stick: ") + Stick.Why, ModeName(Stick.Mode), FString(TEXT("stick")));
	Pad.StickAction.Reset();
	TestEqual(TEXT("unless nothing reads the right stick"), ModeName(Choose(Pad).Mode), FString(TEXT("cursor")));
	FEvidence Copy = Isometric();
	Copy.bViewport = false;
	TestEqual(TEXT("Without a window there is no cursor"), ModeName(Choose(Copy).Mode), FString(TEXT("look")));
	FEvidence Second = Isometric();
	Second.bSharedWindow = true;
	TestEqual(TEXT("A second local player shares the first one's cursor, so it aims by the stick"), ModeName(Choose(Second).Mode), FString(TEXT("stick")));

	// The stick, relative to the camera: up is where the camera faces.
	auto Near = [](const FVector2D& A, const FVector2D& B) { return A.Equals(B, 0.01); };
	TestTrue(TEXT("Straight ahead of the camera is stick up"), Near(StickToward(0.0, FVector(500.0, 0.0, 0.0)), FVector2D(0.0, 1.0)));
	TestTrue(TEXT("Right of it is stick right"), Near(StickToward(0.0, FVector(0.0, 300.0, 0.0)), FVector2D(1.0, 0.0)));
	TestTrue(TEXT("Behind it is stick down"), Near(StickToward(0.0, FVector(-10.0, 0.0, 50.0)), FVector2D(0.0, -1.0)));
	TestTrue(TEXT("A camera turned 90 degrees turns the stick with it"), Near(StickToward(90.0, FVector(0.0, 300.0, 0.0)), FVector2D(0.0, 1.0)));
	TestTrue(TEXT("An isometric camera at 45 degrees"), Near(StickToward(45.0, FVector(1.0, 1.0, 0.0)), FVector2D(0.0, 1.0)) && Near(StickToward(45.0, FVector(1.0, -1.0, 0.0)), FVector2D(-1.0, 0.0)));
	TestTrue(TEXT("Always at full tilt"), FMath::IsNearlyEqual(StickToward(30.0, FVector(400.0, 450.0, -90.0)).Size(), 1.0, 1e-6));
	TestTrue(TEXT("and level"), Near(StickToward(0.0, FVector(400.0, 450.0, -900.0)), FVector2D(450.0, 400.0).GetSafeNormal()));
	TestTrue(TEXT("Nothing to point at, no tilt"), StickToward(0.0, FVector(0.0, 0.0, 100.0)).IsZero());

	// The nudge keeps a moving-cursor gate open: every frame it moves more than a few pixels, round the target.
	FVector2D Sum = FVector2D::ZeroVector;
	for (int32 Frame = 0; Frame < 8; ++Frame)
	{
		Sum += Nudge(Frame);
		TestTrue(TEXT("Each frame moves the cursor more than 5 pixels"), FVector2D::Distance(Nudge(Frame), Nudge(Frame + 1)) > 5.0);
		TestTrue(TEXT("and keeps it within a few of the target"), Nudge(Frame).Size() <= NudgePixels + 1e-6);
	}
	TestTrue(TEXT("centred on the target"), Sum.IsNearlyZero());

	// Reading the game's aim back.
	FAimReading Direction;
	Direction.Value = FVector(1.0, 1.0, -0.4);
	TestTrue(TEXT("A direction is compared by its level bearing"), FMath::IsNearlyEqual(OffBy(Direction, FVector::ZeroVector, FVector(500.0, 500.0, 0.0)), 0.0, 1e-3));
	TestTrue(TEXT("90 degrees off reads 90"), FMath::IsNearlyEqual(OffBy(Direction, FVector::ZeroVector, FVector(-500.0, 500.0, 0.0)), 90.0, 1e-3));
	FAimReading Point;
	Point.bPoint = true;
	Point.Value = FVector(300.0, 400.0, 0.0);
	TestTrue(TEXT("A point is compared level, in cm"), FMath::IsNearlyEqual(OffBy(Point, FVector::ZeroVector, FVector(300.0, 430.0, 90.0)), 30.0, 1e-3));

	// What a step may ask.
	FString Error;
	FHyperAIBotStep Aim = Make(EHyperAIBotAction::AimAt);
	TestFalse(TEXT("AimAt needs something to aim at"), FHyperAIBotExecutor::Validate(Aim, Error));
	Aim.Target = TEXT("Enemy");
	TestTrue(TEXT("a target"), FHyperAIBotExecutor::Validate(Aim, Error));
	Aim.Target.Reset();
	Aim.Location = FVector(100.0, 0.0, 0.0);
	TestTrue(TEXT("or a location"), FHyperAIBotExecutor::Validate(Aim, Error));
	Aim.Lead = 3.f;
	TestFalse(TEXT("Lead is 0 to 2 s"), FHyperAIBotExecutor::Validate(Aim, Error));
	Aim.Lead = 0.2f;
	Aim.Tolerance = 120.f;
	TestFalse(TEXT("Tolerance is 0 to 90 degrees"), FHyperAIBotExecutor::Validate(Aim, Error));
	FHyperAIBotStep Press = Make(EHyperAIBotAction::Press);
	Press.InputAction = TEXT("IA_Attack");
	Press.Aim = EHyperAIBotAim::Cursor;
	TestFalse(TEXT("An aimed press needs something to aim at"), FHyperAIBotExecutor::Validate(Press, Error));
	Press.Target = TEXT("Enemy");
	TestTrue(TEXT("and with it is valid"), FHyperAIBotExecutor::Validate(Press, Error));
	FHyperAIBotStep Wait = Make(EHyperAIBotAction::Wait);
	Wait.Seconds = 1.f;
	Wait.Aim = EHyperAIBotAim::Look;
	TestFalse(TEXT("Only aiming steps take aim"), FHyperAIBotExecutor::Validate(Wait, Error));
	Wait.Aim = static_cast<EHyperAIBotAim>(42);
	TestFalse(TEXT("Aims are a closed set"), FHyperAIBotExecutor::Validate(Wait, Error));

	// Playtest plans carry the aim, and hash it.
	FHyperAIPIEPlaytestRequest Request;
	Request.OperationId = TEXT("aim-rules-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
	Request.Mode = EHyperAIPIEPlaytestMode::AttachOnly;
	Request.Teardown = EHyperAIPIEPlaytestTeardown::LeaveSessionRunning;
	FHyperAIPIEPlaytestStep& Shot = Request.Steps.AddDefaulted_GetRef();
	Shot.StepId = TEXT("slash");
	Shot.Kind = EHyperAIPIEPlaytestStepKind::Bot;
	Shot.Bot = Press;
	FHyperAIStudioPIENormalizedPlan Plan;
	FString Code;
	TestTrue(TEXT("An aimed press is a valid playtest step: ") + Error, FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Request, Plan, Code, Error));
	const FString CursorHash = Plan.PlanHash;
	Request.Steps[0].Bot.Aim = EHyperAIBotAim::Look;
	TestTrue(TEXT("so is one aimed by the view"), FHyperAIStudioPIEPlaytestContracts::NormalizeRequest(Request, Plan, Code, Error));
	TestNotEqual(TEXT("and it plans differently"), Plan.PlanHash, CursorHash);

	// In a world with no window, as verification copies are: no cursor, and the view aims.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	World->GetWorldSettings()->NotifyBeginPlay();
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ADefaultPawn* Pawn = World->SpawnActor<ADefaultPawn>(FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, Spawn);
	APlayerController* Controller = World->SpawnActor<APlayerController>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	Controller->Possess(Pawn);
	UHyperAIStudioTestCursorAim* Reader = NewObject<UHyperAIStudioTestCursorAim>(Pawn, TEXT("Aim"));
	Pawn->AddInstanceComponent(Reader);
	Reader->RegisterComponent();
	const FString Output = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()), TEXT("AimTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	{
		const TArray<FAimReading> Readings = ReadGameAim(*Pawn);
		TestEqual(TEXT("The game's aim is read from any component, by name"), Readings.Num(), 3);
		TestTrue(TEXT("naming where each came from"), Readings.Num() == 3 && Readings[0].Source == TEXT("Aim.GetAimWorldLocation") && Readings[0].bPoint && !Readings[1].bPoint);
		const FEvidence Bare = Gather(*World, 0);
		TestFalse(TEXT("This world has no window"), Bare.bViewport);
		TestEqual(TEXT("and finds the aim of its own"), Bare.Reader, FString(TEXT("Aim.GetAimWorldLocation")));

		FHyperAIBotExecutor Bot(*World, Output);
		FHyperAIBotStep Cursor = Make(EHyperAIBotAction::AimAt);
		Cursor.Location = FVector(600.0, 600.0, 100.0);
		Cursor.Aim = EHyperAIBotAim::Cursor;
		const FHyperAIBotResult NoWindow = Bot.Begin(Cursor);
		TestEqual(TEXT("A cursor aim without a window says so"), NoWindow.Code, FString(TEXT("viewport_unavailable")));
		Cursor.Aim = EHyperAIBotAim::Auto;
		Cursor.Seconds = 0.1f;
		FHyperAIBotResult Looked = Bot.Begin(Cursor);
		for (int32 Frame = 0; Frame < 30 && !Looked.IsDone(); ++Frame)
		{
			World->Tick(LEVELTICK_All, 1.f / 30.f);
			Looked = Bot.Tick();
		}
		// The component's own aim reads the cursor, which this world does not have, so it stays where it started.
		TestEqual(TEXT("Auto aims with the view here, and the game's aim, still ahead, is 45 degrees off: ") + Looked.Detail, Looked.Code, FString(TEXT("aim_off_target")));
		TestTrue(TEXT("saying how it aimed and why"), Looked.Detail.Contains(TEXT("aimed by look (auto, this world has no window")));
		TestTrue(TEXT("and the view turned to the point"), Between(Controller->GetControlRotation().Vector(), FVector(1.0, 1.0, 0.0)) < 1.0);
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	IFileManager::Get().DeleteDirectory(*Output, /*RequireExists=*/false, /*Tree=*/true);
	return true;
}

namespace HyperAIStudio::BotAimTests
{
	/**
	 * Plays in a real Play In Editor window: a third-person pawn and an isometric one whose aim component reads the
	 * cursor as Aetherfall's does, a target and a decoy, and the bot aiming at them every way it can.
	 */
	struct FInPlay
	{
		FAutomationTestBase& Test;
		TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
		double Deadline = 0.0;
		int32 Phase = 0;
		int32 Frames = 0;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<APlayerController> Controller;
		TWeakObjectPtr<APawn> Chase;
		TWeakObjectPtr<APawn> Iso;
		TWeakObjectPtr<UHyperAIStudioTestCursorAim> Aim;
		TWeakObjectPtr<AActor> Target;
		TWeakObjectPtr<AActor> Decoy;
		TUniquePtr<FHyperAIBotExecutor> Bot;
		FString Output;
		bool bStepBegun = false;
		FHyperAIBotResult Result;
		int32 Slashes = 0;
		FVector2D RealBefore = FVector2D::ZeroVector;
		FIntPoint WindowBefore = FIntPoint(-1, -1);
		bool bSawView = false;
		bool bRealOnBotCursor = false;
		int32 HoldFrames = 0;
		int32 HoldActive = 0;
		FVector2D StickSeen = FVector2D::ZeroVector;
		FVector2D HeldCursor = FVector2D::ZeroVector;

		explicit FInPlay(FAutomationTestBase& InTest) : Test(InTest) {}

		FSceneViewport* Viewport() const
		{
			const ULocalPlayer* Local = Controller.IsValid() ? Controller->GetLocalPlayer() : nullptr;
			return Local && Local->ViewportClient ? Local->ViewportClient->GetGameViewport() : nullptr;
		}

		/** Where a window pixel is on the desktop, as the bot computes it. */
		FVector2D Desktop(const FVector2D& Pixel) const
		{
			const FSceneViewport* View = Viewport();
			if (!View)
			{
				return FVector2D(-1.0e6);
			}
			const FGeometry& Geometry = View->GetCachedGeometry();
			const FVector2D Scale = UE::Slate::Viewport::GetViewportLocalToPixelScale(Geometry, FVector2D(View->GetSizeXY()));
			return FVector2D(Geometry.LocalToAbsolute(Pixel / Scale));
		}

		/** Runs Step a frame at a time; true with Result once it is done. */
		bool Run(const FHyperAIBotStep& Step, const TFunction<void()>& EachFrame = nullptr)
		{
			if (!bStepBegun)
			{
				bStepBegun = true;
				Result = Bot->Begin(Step);
			}
			else
			{
				Result = Bot->Tick();
			}
			if (EachFrame)
			{
				EachFrame();
			}
			if (Result.IsDone())
			{
				bStepBegun = false;
				return true;
			}
			return false;
		}

		void Watch()
		{
			// The real cursor never lands where the bot puts the window's.
			const FVector2D Real(FSlateApplication::Get().GetCursorPos());
			if (Aim.IsValid() && Aim->Cursor.X >= 0.0 && FVector2D::Distance(Real, Desktop(Aim->Cursor)) < 0.5 && FVector2D::Distance(Real, RealBefore) > 0.5)
			{
				bRealOnBotCursor = true;
			}
			if (World.IsValid())
			{
				for (const FView& View : Active(*World))
				{
					bSawView |= View.Mode == EHyperAIBotAim::Cursor && View.bPointOnScreen && View.bFromOnScreen;
				}
			}
		}

		ADefaultPawn* SpawnPawn(UWorld& InWorld, const FVector& At) const
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ADefaultPawn* Pawn = InWorld.SpawnActor<ADefaultPawn>(At, FRotator::ZeroRotator, Params);
			// No default WASD and mouse-look bindings: the test's own input is all it reads.
			Pawn->bAddDefaultMovementBindings = false;
			return Pawn;
		}

		AActor* Marker(UWorld& InWorld, const FVector& Center, const TCHAR* Label) const
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AStaticMeshActor* Actor = InWorld.SpawnActor<AStaticMeshActor>(Center, FRotator::ZeroRotator, Params);
			UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
			Mesh->SetMobility(EComponentMobility::Movable);
			Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Actor->SetActorScale3D(FVector(0.2));
			Actor->SetActorLabel(Label);
			return Actor;
		}

		UCameraComponent* Camera(APawn& Pawn, const FVector& Location, const FRotator& Rotation, const bool bAbsolute) const
		{
			UCameraComponent* Component = NewObject<UCameraComponent>(&Pawn, TEXT("TestCamera"));
			Pawn.AddInstanceComponent(Component);
			Component->SetupAttachment(Pawn.GetRootComponent());
			Component->bUsePawnControlRotation = false;
			Component->SetUsingAbsoluteLocation(bAbsolute);
			Component->SetUsingAbsoluteRotation(bAbsolute);
			Component->RegisterComponent();
			if (bAbsolute)
			{
				Component->SetWorldLocationAndRotation(Location, Rotation);
			}
			else
			{
				Component->SetRelativeLocationAndRotation(Location, Rotation);
			}
			return Component;
		}

		double BearingTo(const AActor* Actor) const
		{
			return Iso.IsValid() && Actor && Aim.IsValid() ? Between(Aim->GetAimDirectionFlat(), Actor->GetActorLocation() - Iso->GetActorLocation()) : 180.0;
		}

		/** One frame; true once the test is over. */
		bool Update()
		{
			++Frames;
			const bool bLate = FPlatformTime::Seconds() > Deadline;
			if (bLate && Phase < 100)
			{
				Test.AddError(FString::Printf(TEXT("The play test ran out of time in phase %d%s"), Phase, Result.Detail.IsEmpty() ? TEXT("") : *(TEXT(": ") + Result.Detail)));
				Phase = 100;
			}
			if (Phase < 100 && Phase > 1 && (!World.IsValid() || !Controller.IsValid()))
			{
				Test.AddError(FString::Printf(TEXT("The play session ended early, in phase %d"), Phase));
				Phase = 100;
			}
			if (Phase >= 3 && Phase < 100)
			{
				Watch();
			}
			switch (Phase)
			{
			case 0:
				// Play In Editor starts on the next editor frame; the window is ready once its viewport has a size.
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					UWorld* Candidate = Context.World();
					APlayerController* First = Candidate && Context.WorldType == EWorldType::PIE && Candidate->HasBegunPlay() ? Candidate->GetFirstPlayerController() : nullptr;
					if (First && First->GetLocalPlayer())
					{
						World = Candidate;
						Controller = First;
					}
				}
				if (Viewport() && Viewport()->GetSizeXY().X > 16)
				{
					Phase = 1;
					Frames = 0;
				}
				return false;
			case 1:
			{
				if (Frames < 10)
				{
					return false;
				}
				UWorld& W = *World.Get();
				APlayerController& PC = *Controller.Get();
				const FVector Start = PC.GetPawn() ? PC.GetPawn()->GetActorLocation() : PC.GetFocalLocation();
				const FVector Base = Start + FVector(0.0, 0.0, 400.0);
				// A third-person pawn: the camera close behind it, the cursor hidden, and no aim of its own.
				APawn* ChasePawn = SpawnPawn(W, Base + FVector(0.0, -3000.0, 0.0));
				Camera(*ChasePawn, FVector(-400.0, 0.0, 120.0), FRotator(-10.0, 0.0, 0.0), /*bAbsolute=*/false);
				// An isometric pawn: the camera high and steep, and an aim component that reads the cursor.
				APawn* IsoPawn = SpawnPawn(W, Base);
				Camera(*IsoPawn, Base + FVector(-700.0, 0.0, 1000.0), FRotator(-55.0, 0.0, 0.0), /*bAbsolute=*/true);
				UHyperAIStudioTestCursorAim* Component = NewObject<UHyperAIStudioTestCursorAim>(IsoPawn, TEXT("Aim"));
				IsoPawn->AddInstanceComponent(Component);
				Component->RegisterComponent();
				float Radius = 0.f;
				float HalfHeight = 0.f;
				IsoPawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
				const double FeetZ = Base.Z - HalfHeight;
				// The target ahead and right, the decoy ahead and left, both on the floor, 97 degrees apart.
				Target = Marker(W, FVector(Base.X + 400.0, Base.Y + 450.0, FeetZ + 10.0), TEXT("AimTarget"));
				Decoy = Marker(W, FVector(Base.X + 400.0, Base.Y - 450.0, FeetZ + 10.0), TEXT("AimDecoy"));
				Chase = ChasePawn;
				Iso = IsoPawn;
				Aim = Component;
				// As Aetherfall does: the OS cursor hidden, the aim read from the cursor all the same.
				PC.bShowMouseCursor = false;
				PC.Possess(ChasePawn);
				Bot = MakeUnique<FHyperAIBotExecutor>(W, Output);
				RealBefore = FVector2D(FSlateApplication::Get().GetCursorPos());
				Phase = 2;
				Frames = 0;
				return false;
			}
			case 2:
			{
				if (Frames < 6)
				{
					return false;
				}
				const FChoice Choice = Choose(Gather(*World.Get(), 0));
				Test.TestEqual(TEXT("Auto picks the view for a third-person pawn: ") + Choice.Why, ModeName(Choice.Mode), FString(TEXT("look")));
				Phase = 3;
				return false;
			}
			case 3:
			{
				FHyperAIBotStep Look = Make(EHyperAIBotAction::AimAt);
				Look.Target = TEXT("AimDecoy");
				Look.Aim = EHyperAIBotAim::Auto;
				Look.Seconds = 0.2f;
				if (!Run(Look))
				{
					return false;
				}
				Test.TestTrue(TEXT("and AimAt turns the view to it: ") + Result.Detail, Passed(Result) && Result.Detail.Contains(TEXT("aimed by look (auto")));
				Test.TestTrue(TEXT("the control rotation faces the decoy"), Between(Controller->GetControlRotation().Vector(), Decoy->GetActorLocation() - Chase->GetActorLocation()) < 2.0);
				Controller->Possess(Iso.Get());
				Phase = 4;
				Frames = 0;
				return false;
			}
			case 4:
			{
				if (Frames < 8)
				{
					return false;
				}
				const FEvidence Evidence = Gather(*World.Get(), 0);
				const FChoice Choice = Choose(Evidence);
				Test.TestEqual(TEXT("Auto picks the cursor for the isometric pawn, though its cursor is hidden: ") + Choice.Why, ModeName(Choice.Mode), FString(TEXT("cursor")));
				Test.TestEqual(TEXT("finding its aim component"), Evidence.Reader, FString(TEXT("Aim.GetAimWorldLocation")));
				WindowBefore = FIntPoint(Viewport()->GetMouseX(), Viewport()->GetMouseY());
				Phase = 5;
				return false;
			}
			case 5:
			{
				// Aim at the decoy, and leave the cursor there.
				FHyperAIBotStep Step = Make(EHyperAIBotAction::AimAt);
				Step.Target = TEXT("AimDecoy");
				Step.Aim = EHyperAIBotAim::Auto;
				Step.Seconds = 0.4f;
				if (!Run(Step))
				{
					return false;
				}
				Test.TestTrue(TEXT("AimAt aims by the cursor: ") + Result.Detail, Passed(Result) && Result.Detail.Contains(TEXT("aimed by cursor (auto")));
				Test.TestTrue(TEXT("reporting the cursor's point in the window"), Result.Detail.Contains(TEXT("the window's cursor at (")));
				Test.TestTrue(FString::Printf(TEXT("The game's own aim follows it to the decoy, %.1f deg off"), BearingTo(Decoy.Get())), BearingTo(Decoy.Get()) < 5.0);
				float X = 0.f;
				float Y = 0.f;
				Test.TestTrue(TEXT("GetMousePosition reads the bot's cursor, wherever the real one is"),
					Controller->GetMousePosition(X, Y) && FVector2D(X, Y).Equals(Aim->Cursor, 0.5));
				Test.TestTrue(FString::Printf(TEXT("and the mouse's motion reached the game as Mouse XY (%d samples)"), Aim->LookSamples), Aim->LookSamples > 0);
				Test.TestTrue(TEXT("A recording's overlay draws the cursor's aim"), bSawView);
				HeldCursor = Aim->Cursor;
				Phase = 6;
				return false;
			}
			case 6:
			{
				FHyperAIBotStep Rest = Make(EHyperAIBotAction::Wait);
				Rest.Seconds = 0.8f;
				if (!Run(Rest))
				{
					return false;
				}
				Test.TestFalse(TEXT("A cursor at rest stops counting as aiming, as a still mouse does"), Aim->IsMouseAimActive());
				Test.TestTrue(FString::Printf(TEXT("and stays where the bot left it, (%.0f, %.0f) then (%.0f, %.0f)"), HeldCursor.X, HeldCursor.Y, Aim->Cursor.X, Aim->Cursor.Y),
					Aim->Cursor.Equals(HeldCursor, 0.5));
				Slashes = Aim->Slashes;
				Phase = 7;
				return false;
			}
			case 7:
			{
				// The bug: an attack that is not aimed goes where the cursor last was.
				FHyperAIBotStep Unaimed = Make(EHyperAIBotAction::Press);
				Unaimed.InputAction = TEXT("IA_TestSlash");
				if (!Run(Unaimed))
				{
					return false;
				}
				Test.TestTrue(TEXT("An unaimed press presses"), Passed(Result) && Aim->Slashes == Slashes + 1);
				Test.TestTrue(FString::Printf(TEXT("and the slash goes toward the stale cursor's decoy (%.1f deg off it)"), Between(Aim->LastSlash, Decoy->GetActorLocation() - Iso->GetActorLocation())),
					Between(Aim->LastSlash, Decoy->GetActorLocation() - Iso->GetActorLocation()) < 10.0);
				Slashes = Aim->Slashes;
				Phase = 8;
				return false;
			}
			case 8:
			{
				// Aiming with the view, as the bot did before, does nothing for a game that reads the cursor.
				FHyperAIBotStep Look = Make(EHyperAIBotAction::Press);
				Look.InputAction = TEXT("IA_TestSlash");
				Look.Target = TEXT("AimTarget");
				Look.Aim = EHyperAIBotAim::Look;
				if (!Run(Look))
				{
					return false;
				}
				Test.TestEqual(TEXT("A press aimed with the view says the game aims elsewhere: ") + Result.Detail, Result.Code, FString(TEXT("aim_off_target")));
				Test.TestTrue(TEXT("and its slash still went to the decoy"), Aim->Slashes == Slashes + 1 && Between(Aim->LastSlash, Decoy->GetActorLocation() - Iso->GetActorLocation()) < 10.0);
				Slashes = Aim->Slashes;
				Phase = 9;
				return false;
			}
			case 9:
			{
				FHyperAIBotStep Cursor = Make(EHyperAIBotAction::Press);
				Cursor.InputAction = TEXT("IA_TestSlash");
				Cursor.Target = TEXT("AimTarget");
				Cursor.Aim = EHyperAIBotAim::Cursor;
				if (!Run(Cursor))
				{
					return false;
				}
				Test.TestTrue(TEXT("A press aimed by the cursor passes, the game's aim checked: ") + Result.Detail, Passed(Result) && Result.Detail.Contains(TEXT("the game's own aim: Aim.GetAimWorldLocation")));
				Test.TestTrue(FString::Printf(TEXT("and the timed slash lands toward the target, not the stale cursor (%.1f deg off the target)"), Between(Aim->LastSlash, Target->GetActorLocation() - Iso->GetActorLocation())),
					Aim->Slashes == Slashes + 1 && Between(Aim->LastSlash, Target->GetActorLocation() - Iso->GetActorLocation()) < 10.0);
				Test.TestTrue(TEXT("with the cursor counting as aiming as it swung"), Aim->bLastSlashActive);
				Slashes = Aim->Slashes;
				Phase = 10;
				return false;
			}
			case 10:
			{
				FHyperAIBotStep Hold = Make(EHyperAIBotAction::Hold);
				Hold.InputAction = TEXT("IA_TestSlash");
				Hold.Target = TEXT("AimTarget");
				Hold.Aim = EHyperAIBotAim::Cursor;
				Hold.Seconds = 1.2f;
				if (!Run(Hold, [this]()
				{
					// Every frame from the swing on, the moving-cursor gate stays open.
					if (Aim->Slashes > Slashes)
					{
						++HoldFrames;
						HoldActive += Aim->IsMouseAimActive() ? 1 : 0;
					}
				}))
				{
					return false;
				}
				Test.TestTrue(TEXT("An aimed hold passes: ") + Result.Detail, Passed(Result));
				Test.TestTrue(FString::Printf(TEXT("and the recency gate stays open all through the hold (%d of %d frames)"), HoldActive, HoldFrames), HoldFrames > 10 && HoldActive == HoldFrames);
				Test.TestTrue(TEXT("aimed at the target"), BearingTo(Target.Get()) < 10.0);
				Phase = 11;
				return false;
			}
			case 11:
			{
				FHyperAIBotStep Stick = Make(EHyperAIBotAction::AimAt);
				Stick.Target = TEXT("AimTarget");
				Stick.Aim = EHyperAIBotAim::Stick;
				Stick.Seconds = 0.4f;
				if (!Run(Stick, [this]() { if (!Aim->Stick.IsNearlyZero()) { StickSeen = Aim->Stick; } }))
				{
					return false;
				}
				FVector CameraAt;
				FRotator View;
				Controller->GetPlayerViewPoint(CameraAt, View);
				const FVector2D Expected = StickToward(View.Yaw, Target->GetActorLocation() - Iso->GetActorLocation());
				Test.TestTrue(TEXT("A stick aim passes, the game aiming along the stick: ") + Result.Detail, Passed(Result) && Result.Detail.Contains(TEXT("aimed by stick")));
				Test.TestTrue(FString::Printf(TEXT("The game read the stick at (%.3f, %.3f), toward the target from the camera's yaw (%.3f, %.3f)"), StickSeen.X, StickSeen.Y, Expected.X, Expected.Y),
					StickSeen.Equals(Expected, 0.02) && FMath::IsNearlyEqual(Expected.X, 0.747, 0.01) && FMath::IsNearlyEqual(Expected.Y, 0.664, 0.01));
				Phase = 12;
				Frames = 0;
				return false;
			}
			case 12:
			{
				if (Frames < 4)
				{
					return false;
				}
				Test.TestTrue(TEXT("Letting go springs the stick back"), Aim->Stick.IsNearlyZero());
				Bot->Stop();
				Bot.Reset();
				const FVector2D Real(FSlateApplication::Get().GetCursorPos());
				const FSceneViewport* View = Viewport();
				const FVector2D Local(View->GetCachedGeometry().AbsoluteToLocal(Real));
				const FVector2D Size(View->GetCachedGeometry().GetLocalSize());
				if (Local.X < 0.0 || Local.Y < 0.0 || Local.X >= Size.X || Local.Y >= Size.Y)
				{
					Test.TestEqual(TEXT("When the bot stops, the window's cursor goes back to where it was"), FIntPoint(View->GetMouseX(), View->GetMouseY()), WindowBefore);
				}
				Test.TestFalse(TEXT("The real cursor never went to the bot's cursor"), bRealOnBotCursor);
				if (Real.Equals(RealBefore, 0.5))
				{
					Test.TestTrue(TEXT("The real cursor did not move"), true);
				}
				else
				{
					Test.AddInfo(FString::Printf(TEXT("The real cursor moved from (%.0f, %.0f) to (%.0f, %.0f) during the test, by the person at the machine: never onto the bot's cursor."),
						RealBefore.X, RealBefore.Y, Real.X, Real.Y));
				}
				Phase = 100;
				return false;
			}
			case 100:
				Bot.Reset();
				if (GEditor && GEditor->IsPlaySessionInProgress())
				{
					GEditor->RequestEndPlayMap();
				}
				Phase = 101;
				return false;
			default:
				if (GEditor && (GEditor->IsPlaySessionInProgress() || GEditor->IsPlaySessionRequestQueued()) && FPlatformTime::Seconds() < Deadline + 30.0)
				{
					return false;
				}
				IFileManager::Get().DeleteDirectory(*Output, /*RequireExists=*/false, /*Tree=*/true);
				return true;
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBotAimInPlayTest,
	"HyperAIStudio.Bot.Aim.InPlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBotAimInPlayTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::BotAimTests;
	if (!GEditor || GEditor->IsPlaySessionInProgress() || !FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Skipped: a play session is already running, or there is no Slate."));
		return true;
	}
	if (!FSlateApplication::Get().IsMouseAttached())
	{
		AddInfo(TEXT("Skipped: the engine reports no mouse on this machine, so no game reads a cursor here."));
		return true;
	}
	TSharedRef<FInPlay> Play = MakeShared<FInPlay>(*this);
	ULevelEditorPlaySettings* Settings = DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
	Settings->SetPlayNetMode(PIE_Standalone);
	Settings->SetRunUnderOneProcess(true);
	Settings->SetPlayNumberOfClients(1);
	Settings->bLaunchSeparateServer = false;
	Settings->GameGetsMouseControl = false;
	Play->Settings.Reset(Settings);
	Play->Output = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()), TEXT("AimInPlay-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	Play->Deadline = FPlatformTime::Seconds() + 90.0;
	FRequestPlaySessionParams Params;
	Params.SessionDestination = EPlaySessionDestinationType::InProcess;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.EditorPlaySettings = Settings;
	Params.bAllowOnlineSubsystem = false;
	GEditor->RequestPlaySession(Params);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Play]() { return Play->Update(); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBotAimPerClientTest,
	"HyperAIStudio.Bot.Aim.PerClient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBotAimPerClientTest::RunTest(const FString& Parameters)
{
	if (!GEditor || GEditor->IsPlaySessionInProgress() || !FSlateApplication::IsInitialized() || !FSlateApplication::Get().IsMouseAttached())
	{
		AddInfo(TEXT("Skipped: a play session is already running, or there is no Slate or mouse."));
		return true;
	}
	// A listen server and its client, each in its own window: each player's bot puts that window's cursor on its own
	// pawn, and the recording draws the host's aim.
	auto Aimed = [](const TCHAR* StepId, const EHyperAIBotAim Aim, const int32 PieInstance)
	{
		FHyperAIPIEPlaytestStep Step;
		Step.StepId = StepId;
		Step.Kind = EHyperAIPIEPlaytestStepKind::Bot;
		Step.Bot.Action = EHyperAIBotAction::AimAt;
		Step.Bot.Target = TEXT("Player");
		Step.Bot.Aim = Aim;
		Step.Bot.Seconds = 0.3f;
		Step.PieInstance = PieInstance;
		Step.TimeoutMs = 10000;
		return Step;
	};
	FHyperAIPIEPlaytestRequest Request;
	Request.OperationId = TEXT("aim-clients-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower();
	Request.Mode = EHyperAIPIEPlaytestMode::StartInProcess;
	Request.Teardown = EHyperAIPIEPlaytestTeardown::StopOwnedSession;
	Request.NetMode = EHyperAIPIEPlaytestNetMode::ListenServer;
	Request.NumPlayers = 2;
	Request.bRecordVideo = true;
	Request.WholeTimeoutMs = 90000;
	Request.Steps = {Aimed(TEXT("host-cursor"), EHyperAIBotAim::Cursor, 0), Aimed(TEXT("client-cursor"), EHyperAIBotAim::Cursor, 1),
		Aimed(TEXT("client-auto"), EHyperAIBotAim::Auto, 1)};
	FHyperAIStudioPIEPlaytestService::Startup();
	const FHyperAIPIEPlaytestRunResult Submitted = FHyperAIStudioPIEPlaytestService::Submit(Request);
	if (!TestTrue(TEXT("the run is accepted: ") + Submitted.Diagnostic, Submitted.bAccepted))
	{
		return true;
	}
	const FString OperationId = Request.OperationId;
	const double Deadline = FPlatformTime::Seconds() + 100.0;
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, OperationId, Deadline]()
	{
		const FHyperAIPIEPlaytestStatusResult Status = FHyperAIStudioPIEPlaytestService::GetStatus(OperationId, 64, FString());
		if (!Status.bTerminal && FPlatformTime::Seconds() < Deadline)
		{
			return false;
		}
		TestEqual(*FString::Printf(TEXT("the run completes (%s)"), *Status.Diagnostic), Status.Status, FString(TEXT("completed")));
		auto Step = [&Status](const TCHAR* StepId) -> const FHyperAIPIEPlaytestEvidence*
		{
			return Status.Evidence.FindByPredicate([StepId](const FHyperAIPIEPlaytestEvidence& Item) { return Item.StepId == StepId && Item.Status == TEXT("completed"); });
		};
		for (const TCHAR* StepId : {TEXT("host-cursor"), TEXT("client-cursor")})
		{
			const FHyperAIPIEPlaytestEvidence* Done = Step(StepId);
			TestTrue(FString::Printf(TEXT("%s aims by its own window's cursor: %s"), StepId, Done ? *Done->ValuePreview : TEXT("no evidence")),
				Done && Done->ValuePreview.Contains(TEXT("aimed by cursor")) && Done->ValuePreview.Contains(TEXT("the window's cursor at (")) && !Done->ValuePreview.Contains(TEXT("read the cursor back")));
		}
		const FHyperAIPIEPlaytestEvidence* Auto = Step(TEXT("client-auto"));
		TestTrue(TEXT("and auto picks for the client from its own window: ") + (Auto ? Auto->ValuePreview : FString()), Auto && Auto->ValuePreview.Contains(TEXT("(auto, ")));
		TestNotNull(TEXT("the run is recorded, aims drawn"), Status.Evidence.FindByPredicate([](const FHyperAIPIEPlaytestEvidence& Item)
		{
			return Item.Kind == TEXT("video") && Item.Status == TEXT("completed");
		}));
		return true;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
