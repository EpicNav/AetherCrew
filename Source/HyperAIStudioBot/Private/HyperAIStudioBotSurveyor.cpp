// Games by Hyper 2026.

#include "HyperAIStudioBotSurveyor.h"

#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioBotShared.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TextureResource.h"
#include <limits>
#if WITH_EDITOR
#include "ShaderCompiler.h"
#endif

namespace HyperAIStudio::Bot::Survey
{
	const TCHAR* CellName(const ECell Cell)
	{
		switch (Cell)
		{
		case ECell::Reachable: return TEXT("reachable");
		case ECell::Unreachable: return TEXT("unreachable");
		default: return TEXT("no_ground");
		}
	}

	FGrid FGrid::Over(const FBox& Bounds, const FVector& Center, const double Range, const int32 InCells)
	{
		FGrid Grid;
		Grid.Cells = FMath::Clamp(InCells, MinCells, MaxCells);
		FVector2D Middle(Center.X, Center.Y);
		double Side = 2.0 * Range;
		if (Range <= 0.0)
		{
			const FBox Box = Bounds.IsValid ? Bounds : FBox(Center - FVector(2000.0), Center + FVector(2000.0));
			Middle = FVector2D(Box.GetCenter().X, Box.GetCenter().Y);
			Side = FMath::Max(Box.GetSize().X, Box.GetSize().Y);
		}
		Side = FMath::Max(Side, 400.0);
		Grid.CellSize = Side / Grid.Cells;
		Grid.Origin = Middle - FVector2D(Side * 0.5, Side * 0.5);
		return Grid;
	}

	int32 FGrid::IndexAt(const FVector2D& Point) const
	{
		const int32 X = FMath::FloorToInt32((Point.X - Origin.X) / CellSize);
		const int32 Y = FMath::FloorToInt32((Point.Y - Origin.Y) / CellSize);
		return X >= 0 && Y >= 0 && X < Cells && Y < Cells ? IndexOf(X, Y) : INDEX_NONE;
	}

	FVector2D FGrid::Center(const int32 Index) const
	{
		return Origin + FVector2D((Index % Cells + 0.5) * CellSize, (Index / Cells + 0.5) * CellSize);
	}

	TArray<ECell> Classify(const FGrid& Grid, const TArray<double>& GroundZ, const int32 Start, const double MaxRise,
		TFunctionRef<bool(int32 From, int32 To)> Open)
	{
		TArray<ECell> Out;
		Out.Init(ECell::NoGround, Grid.Num());
		for (int32 Index = 0; Index < Grid.Num() && Index < GroundZ.Num(); ++Index)
		{
			if (FMath::IsFinite(GroundZ[Index]))
			{
				Out[Index] = ECell::Unreachable;
			}
		}
		if (!Out.IsValidIndex(Start) || Out[Start] == ECell::NoGround)
		{
			return Out;
		}
		TArray<int32> Queue = {Start};
		Out[Start] = ECell::Reachable;
		for (int32 Head = 0; Head < Queue.Num(); ++Head)
		{
			const int32 At = Queue[Head];
			const int32 X = At % Grid.Cells;
			const int32 Y = At / Grid.Cells;
			const int32 Next[4][2] = {{X + 1, Y}, {X - 1, Y}, {X, Y + 1}, {X, Y - 1}};
			for (const auto& N : Next)
			{
				if (N[0] < 0 || N[1] < 0 || N[0] >= Grid.Cells || N[1] >= Grid.Cells)
				{
					continue;
				}
				const int32 To = Grid.IndexOf(N[0], N[1]);
				if (Out[To] != ECell::Unreachable || FMath::Abs(GroundZ[To] - GroundZ[At]) > MaxRise || !Open(At, To))
				{
					continue;
				}
				Out[To] = ECell::Reachable;
				Queue.Add(To);
			}
		}
		return Out;
	}

	TArray<int32> Spread(const TArray<FVector2D>& Points, const int32 Count)
	{
		TArray<int32> Picks;
		if (Points.IsEmpty() || Count <= 0)
		{
			return Picks;
		}
		TArray<double> Nearest;
		Nearest.Init(TNumericLimits<double>::Max(), Points.Num());
		int32 Pick = 0;
		while (Picks.Num() < FMath::Min(Count, Points.Num()))
		{
			Picks.Add(Pick);
			int32 Farthest = INDEX_NONE;
			for (int32 Index = 0; Index < Points.Num(); ++Index)
			{
				Nearest[Index] = FMath::Min(Nearest[Index], FVector2D::DistSquared(Points[Index], Points[Pick]));
				if (!Picks.Contains(Index) && (Farthest == INDEX_NONE || Nearest[Index] > Nearest[Farthest]))
				{
					Farthest = Index;
				}
			}
			if (Farthest == INDEX_NONE)
			{
				break;
			}
			Pick = Farthest;
		}
		return Picks;
	}

	double ReachablePercent(const TArray<ECell>& Cells)
	{
		int32 Ground = 0;
		int32 Reached = 0;
		for (const ECell Cell : Cells)
		{
			Ground += Cell != ECell::NoGround;
			Reached += Cell == ECell::Reachable;
		}
		return Ground > 0 ? 100.0 * Reached / Ground : 0.0;
	}

	FVector2D PixelToWorld(const FGrid& Grid, const int32 Size, const double U, const double V)
	{
		return FVector2D(Grid.Origin.X + Grid.Size() * (1.0 - V / Size), Grid.Origin.Y + Grid.Size() * (U / Size));
	}

	FVector2D WorldToPixel(const FGrid& Grid, const int32 Size, const FVector2D& World)
	{
		return FVector2D((World.Y - Grid.Origin.Y) / Grid.Size() * Size, (1.0 - (World.X - Grid.Origin.X) / Grid.Size()) * Size);
	}

	namespace
	{
		void Blend(FColor& Pixel, const FColor& Tint, const float Alpha)
		{
			Pixel.R = static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.R), static_cast<float>(Tint.R), Alpha));
			Pixel.G = static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.G), static_cast<float>(Tint.G), Alpha));
			Pixel.B = static_cast<uint8>(FMath::Lerp(static_cast<float>(Pixel.B), static_cast<float>(Tint.B), Alpha));
			Pixel.A = 255;
		}

		void Mark(TArray<FColor>& Pixels, const int32 Size, const FVector2D& At, const int32 Radius, const FColor& Color, const bool bCross)
		{
			for (int32 DY = -Radius; DY <= Radius; ++DY)
			{
				for (int32 DX = -Radius; DX <= Radius; ++DX)
				{
					const bool bOn = bCross ? (FMath::Abs(FMath::Abs(DX) - FMath::Abs(DY)) <= 1) : (DX * DX + DY * DY <= Radius * Radius);
					const int32 U = FMath::RoundToInt32(At.X) + DX;
					const int32 V = FMath::RoundToInt32(At.Y) + DY;
					if (bOn && U >= 0 && V >= 0 && U < Size && V < Size)
					{
						Pixels[V * Size + U] = Color;
					}
				}
			}
		}
	}

	void PaintOverview(TArray<FColor>& Pixels, const int32 Size, const FGrid& Grid, const TArray<ECell>& Cells,
		const TArray<bool>& Walked, const int32 Start, const TArray<FFinding>& Stuck, const TArray<FFinding>& Falls)
	{
		if (Pixels.Num() != Size * Size)
		{
			Pixels.Init(FColor::Black, Size * Size);
		}
		const double CellPixels = static_cast<double>(Size) / Grid.Cells;
		for (int32 V = 0; V < Size; ++V)
		{
			for (int32 U = 0; U < Size; ++U)
			{
				const int32 Index = Grid.IndexAt(PixelToWorld(Grid, Size, U + 0.5, V + 0.5));
				FColor& Pixel = Pixels[V * Size + U];
				Pixel.A = 255;
				if (!Cells.IsValidIndex(Index))
				{
					continue;
				}
				switch (Cells[Index])
				{
				case ECell::Reachable: Blend(Pixel, FColor(40, 200, 70), 0.35f); break;
				case ECell::Unreachable: Blend(Pixel, FColor(220, 40, 40), 0.45f); break;
				default: Blend(Pixel, FColor(10, 10, 20), 0.7f); break;
				}
				if (Walked.IsValidIndex(Index) && Walked[Index])
				{
					Blend(Pixel, FColor(50, 120, 255), 0.35f);
				}
				// Cell edges, faintly.
				const double FU = FMath::Fmod(U + 0.5, CellPixels);
				const double FV = FMath::Fmod(V + 0.5, CellPixels);
				if (CellPixels >= 6.0 && (FU < 1.0 || FV < 1.0))
				{
					Blend(Pixel, FColor::Black, 0.3f);
				}
			}
		}
		if (Cells.IsValidIndex(Start))
		{
			Mark(Pixels, Size, WorldToPixel(Grid, Size, Grid.Center(Start)), 7, FColor::White, false);
		}
		for (const FFinding& Spot : Stuck)
		{
			Mark(Pixels, Size, WorldToPixel(Grid, Size, FVector2D(Spot.Location)), 6, FColor(255, 220, 0), false);
		}
		for (const FFinding& Fall : Falls)
		{
			Mark(Pixels, Size, WorldToPixel(Grid, Size, FVector2D(Fall.Location)), 8, FColor(255, 0, 255), true);
		}
	}
}

using namespace HyperAIStudio::Bot;
using namespace HyperAIStudio::Bot::Private;
using namespace HyperAIStudio::Bot::Survey;

namespace
{
	constexpr int32 FrameWidth = 960;
	constexpr int32 FrameHeight = 540;
	constexpr int32 MaxFrames = 240;
	constexpr int32 MaxWalks = 8;
	constexpr double EyeHeight = 170.0;

	FString Location3(const FVector& Value)
	{
		return FString::Printf(TEXT("(%.0f, %.0f, %.0f)"), Value.X, Value.Y, Value.Z);
	}

	TSharedRef<FJsonObject> PointJson(const FVector& Point, const FString& Detail)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Point.X)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Point.Y)),
			MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Point.Z))});
		if (!Detail.IsEmpty())
		{
			Object->SetStringField(TEXT("detail"), Detail);
		}
		return Object;
	}
}

FHyperAIBotSurveyor::FHyperAIBotSurveyor(UWorld& InWorld, const FHyperAIBotStep& InStep, const FString& InFolder)
	: World(&InWorld)
	, Step(InStep)
	, Folder(InFolder)
{
}

FHyperAIBotSurveyor::~FHyperAIBotSurveyor()
{
	if (Capture.IsValid() && Capture->IsRegistered() && World.IsValid())
	{
		Capture->UnregisterComponent();
	}
}

double FHyperAIBotSurveyor::PawnHalfHeight() const
{
	UWorld* W = World.Get();
	APawn* Pawn = W ? PlayerPawn(*W, Step.PlayerIndex) : nullptr;
	float Radius = 34.f;
	float HalfHeight = 88.f;
	if (Pawn)
	{
		Pawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
	}
	return HalfHeight;
}

FVector FHyperAIBotSurveyor::GroundPoint(const int32 Index) const
{
	const FVector2D At = Grid.Center(Index);
	return FVector(At.X, At.Y, GroundZ.IsValidIndex(Index) && FMath::IsFinite(GroundZ[Index]) ? GroundZ[Index] : StartLocation.Z);
}

FHyperAIBotResult FHyperAIBotSurveyor::Begin()
{
	UWorld* W = World.Get();
	APawn* Pawn = W ? PlayerPawn(*W, Step.PlayerIndex) : nullptr;
	if (!Pawn)
	{
		return FResult::Failed(TEXT("no_player"), TEXT("There is no player pawn to survey from."));
	}
	StartLocation = Pawn->GetActorLocation();
	StartRotation = Pawn->GetController() ? Pawn->GetController()->GetControlRotation() : Pawn->GetActorRotation();
	// The level: what the player can collide with, without pawns and sky-sized things.
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		if (!IsValid(*It) || It->IsA<APawn>() || It->IsActorBeingDestroyed())
		{
			continue;
		}
		const FBox Box = It->GetComponentsBoundingBox(/*bNonColliding=*/false);
		if (Box.IsValid && Box.GetSize().GetMax() < 2.0e6)
		{
			LevelBounds += Box;
		}
	}
	LevelBounds += StartLocation;
	int32 CellCount = DefaultCells;
	if (!Step.Value.IsEmpty())
	{
		LexTryParseString(CellCount, *Step.Value.TrimStartAndEnd());
	}
	Grid = FGrid::Over(LevelBounds, StartLocation, Step.Range, CellCount);
	GroundZ.Init(std::numeric_limits<double>::quiet_NaN(), Grid.Num());
	Cells.Init(ECell::NoGround, Grid.Num());
	Walked.Init(false, Grid.Num());
	Start = Grid.IndexAt(FVector2D(StartLocation));
	KillZ = W->GetWorldSettings() && W->GetWorldSettings()->bEnableWorldBoundsChecks ? W->GetWorldSettings()->KillZ : -HALF_WORLD_MAX;
	const UNavigationSystemV1* Navigation = FNavigationSystem::GetCurrent<UNavigationSystemV1>(W);
	bNavmesh = Navigation && const_cast<UNavigationSystemV1*>(Navigation)->GetDefaultNavDataInstance(FNavigationSystem::DontCreate) != nullptr;
	const ACharacter* Character = Cast<ACharacter>(Pawn);
	const double StepHeight = Character && Character->GetCharacterMovement() ? Character->GetCharacterMovement()->MaxStepHeight : 45.0;
	// Over a whole cell a ramp rises more than a step: about 35 degrees of slope, or a step, whichever is more.
	MaxRise = FMath::Max(StepHeight, Grid.CellSize * 0.7);
	WalkingLeft = Step.Seconds > 0.f ? Step.Seconds : DefaultWalkSeconds;
	IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists=*/false, /*Tree=*/true);
	IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
	Phase = EPhase::Grid;
	NextRow = 0;
	return FResult();
}

void FHyperAIBotSurveyor::SortRow(const int32 Y)
{
	UWorld* W = World.Get();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotSurvey), /*bTraceComplex=*/false);
	for (TActorIterator<APawn> It(W); It; ++It)
	{
		Params.AddIgnoredActor(*It);
	}
	const double Top = LevelBounds.Max.Z + 500.0;
	const double Bottom = FMath::Min(LevelBounds.Min.Z - 500.0, StartLocation.Z - 5000.0);
	for (int32 X = 0; X < Grid.Cells; ++X)
	{
		const int32 Index = Grid.IndexOf(X, Y);
		const FVector2D At = Grid.Center(Index);
		FHitResult Hit;
		if (W->LineTraceSingleByChannel(Hit, FVector(At.X, At.Y, Top), FVector(At.X, At.Y, Bottom), ECC_Pawn, Params) && Hit.bBlockingHit)
		{
			GroundZ[Index] = Hit.ImpactPoint.Z;
		}
	}
	if (Start != INDEX_NONE && Start / Grid.Cells == Y)
	{
		// The player stands here, whatever the trace through the cell's middle found.
		GroundZ[Start] = StartLocation.Z - PawnHalfHeight();
	}
}

void FHyperAIBotSurveyor::ReachRow(const int32 Y)
{
	UWorld* W = World.Get();
	UNavigationSystemV1* Navigation = W ? FNavigationSystem::GetCurrent<UNavigationSystemV1>(W) : nullptr;
	APawn* Pawn = W ? PlayerPawn(*W, Step.PlayerIndex) : nullptr;
	for (int32 X = 0; X < Grid.Cells; ++X)
	{
		const int32 Index = Grid.IndexOf(X, Y);
		if (!FMath::IsFinite(GroundZ[Index]))
		{
			continue;
		}
		Cells[Index] = ECell::Unreachable;
		FNavLocation OnNav;
		const FVector Extent(Grid.CellSize * 0.5, Grid.CellSize * 0.5, 250.0);
		if (!Navigation || !Navigation->ProjectPointToNavigation(GroundPoint(Index), OnNav, Extent))
		{
			continue;
		}
		const UNavigationPath* Route = UNavigationSystemV1::FindPathToLocationSynchronously(W, StartLocation, OnNav.Location, Pawn);
		if (Index == Start || (Route && Route->IsValid() && !Route->IsPartial()))
		{
			Cells[Index] = ECell::Reachable;
		}
	}
}

bool FHyperAIBotSurveyor::IsOpen(const int32 From, const int32 To) const
{
	UWorld* W = World.Get();
	if (!W)
	{
		return false;
	}
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotSurveyEdge), false);
	for (TActorIterator<APawn> It(W); It; ++It)
	{
		Params.AddIgnoredActor(*It);
	}
	const FVector Lift(0.0, 0.0, 60.0);
	FHitResult Hit;
	return !W->LineTraceSingleByChannel(Hit, GroundPoint(From) + Lift, GroundPoint(To) + Lift, ECC_Pawn, Params);
}

TArray<FVector> FHyperAIBotSurveyor::RouteBetween(const int32 From, const int32 To) const
{
	TArray<FVector> Route;
	UWorld* W = World.Get();
	if (bNavmesh && W)
	{
		const UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(W, GroundPoint(From), GroundPoint(To), PlayerPawn(*W, Step.PlayerIndex));
		if (Path && Path->IsValid() && Path->PathPoints.Num() > 1)
		{
			Route = Path->PathPoints;
			Route.RemoveAt(0);
			return Route;
		}
	}
	// Cell to cell over reachable ground, the shortest way in cells.
	TMap<int32, int32> CameFrom;
	TArray<int32> Queue = {From};
	CameFrom.Add(From, INDEX_NONE);
	for (int32 Head = 0; Head < Queue.Num() && !CameFrom.Contains(To); ++Head)
	{
		const int32 At = Queue[Head];
		const int32 X = At % Grid.Cells;
		const int32 Y = At / Grid.Cells;
		const int32 Next[4][2] = {{X + 1, Y}, {X - 1, Y}, {X, Y + 1}, {X, Y - 1}};
		for (const auto& N : Next)
		{
			if (N[0] < 0 || N[1] < 0 || N[0] >= Grid.Cells || N[1] >= Grid.Cells)
			{
				continue;
			}
			const int32 Cell = Grid.IndexOf(N[0], N[1]);
			if (CameFrom.Contains(Cell) || Cells[Cell] != ECell::Reachable || FMath::Abs(GroundZ[Cell] - GroundZ[At]) > MaxRise || !IsOpen(At, Cell))
			{
				continue;
			}
			CameFrom.Add(Cell, At);
			Queue.Add(Cell);
		}
	}
	for (int32 At = CameFrom.Contains(To) ? To : INDEX_NONE; At != INDEX_NONE && At != From; At = CameFrom[At])
	{
		Route.Insert(GroundPoint(At), 0);
	}
	if (Route.IsEmpty())
	{
		Route.Add(GroundPoint(To));
	}
	return Route;
}

void FHyperAIBotSurveyor::PlanWalks()
{
	TArray<int32> Reachable;
	TArray<FVector2D> Points;
	if (Cells.IsValidIndex(Start) && Cells[Start] == ECell::Reachable)
	{
		Reachable.Add(Start);
		Points.Add(Grid.Center(Start));
	}
	for (int32 Index = 0; Index < Cells.Num(); ++Index)
	{
		if (Cells[Index] == ECell::Reachable && Index != Start)
		{
			Reachable.Add(Index);
			Points.Add(Grid.Center(Index));
		}
	}
	const TArray<int32> Picks = Spread(Points, MaxWalks);
	for (int32 PickIndex = 0; PickIndex < Picks.Num(); ++PickIndex)
	{
		const int32 From = Reachable[Picks[PickIndex]];
		FWalk Walk;
		Walk.From = From;
		// Every other walk heads for a reachable cell's edge with no ground beyond, where the player could fall out.
		const int32 X = From % Grid.Cells;
		const int32 Y = From / Grid.Cells;
		const int32 Next[4][2] = {{X + 1, Y}, {X - 1, Y}, {X, Y + 1}, {X, Y - 1}};
		int32 Edge = INDEX_NONE;
		for (const auto& N : Next)
		{
			if (N[0] >= 0 && N[1] >= 0 && N[0] < Grid.Cells && N[1] < Grid.Cells && Cells[Grid.IndexOf(N[0], N[1])] == ECell::NoGround)
			{
				Edge = Grid.IndexOf(N[0], N[1]);
				break;
			}
		}
		if (Edge != INDEX_NONE && PickIndex % 2 == 1)
		{
			Walk.bEdge = true;
			Walk.Goal = FVector(Grid.Center(Edge), GroundZ[From]);
			Walk.Route = {Walk.Goal};
		}
		else
		{
			const int32 To = Reachable[Picks[(PickIndex + 1) % Picks.Num()]];
			Walk.Goal = GroundPoint(To);
			Walk.Route = RouteBetween(From, To);
		}
		Walks.Add(Walk);
	}
	// The views: the reachable ground seen from spread-out spots at eye height, looking across the level.
	Views.Reset();
	for (const int32 Pick : Spread(Points, MaxViews))
	{
		Views.Add(Reachable[Pick]);
	}
}

void FHyperAIBotSurveyor::PutPlayerAt(const FVector& Ground)
{
	UWorld* W = World.Get();
	APawn* Pawn = W ? PlayerPawn(*W, Step.PlayerIndex) : nullptr;
	if (!Pawn)
	{
		return;
	}
	Pawn->SetActorLocation(Ground + FVector(0.0, 0.0, PawnHalfHeight() + 5.0), /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
	if (ACharacter* Character = Cast<ACharacter>(Pawn); Character && Character->GetCharacterMovement())
	{
		Character->GetCharacterMovement()->StopMovementImmediately();
	}
}

bool FHyperAIBotSurveyor::StartWalk()
{
	if (NextWalk >= Walks.Num() || WalkingLeft <= 0.0)
	{
		return false;
	}
	const FWalk& Walk = Walks[NextWalk];
	PutPlayerAt(GroundPoint(Walk.From));
	UWorld* W = World.Get();
	APawn* Pawn = W ? PlayerPawn(*W, Step.PlayerIndex) : nullptr;
	if (!Pawn)
	{
		return false;
	}
	WalkStartZ = Pawn->GetActorLocation().Z;
	LastMoved = Pawn->GetActorLocation();
	LastMovedAt = 0.0;
	WalkElapsed = 0.0;
	NextWaypoint = 0;
	bWalking = true;
	return true;
}

bool FHyperAIBotSurveyor::TickWalk(const double Dt)
{
	UWorld* W = World.Get();
	const FWalk& Walk = Walks[NextWalk];
	APlayerController* Controller = W ? PlayerController(*W, Step.PlayerIndex) : nullptr;
	APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
	WalkElapsed += Dt;
	WalkingLeft -= Dt;
	if (!Pawn || Pawn->IsActorBeingDestroyed())
	{
		// The game destroyed the pawn: it fell out of the world and was killed, as KillZ does.
		Falls.Add({LastMoved, TEXT("the player fell out of the world here and its pawn was destroyed")});
		if (AGameModeBase* Mode = W ? W->GetAuthGameMode() : nullptr; Mode && Controller)
		{
			Mode->RestartPlayer(Controller);
		}
		return true;
	}
	const FVector Here = Pawn->GetActorLocation();
	const int32 Cell = Grid.IndexAt(FVector2D(Here));
	if (Walked.IsValidIndex(Cell))
	{
		Walked[Cell] = true;
	}
	if (Here.Z < KillZ + 100.0)
	{
		Falls.Add({LastMoved, FString::Printf(TEXT("the player fell below KillZ (%.0f) from here"), KillZ)});
		return true;
	}
	if (Here.Z < WalkStartZ - 200.0)
	{
		// Falling with nothing below to land on: out of the world.
		FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIBotSurveyFall), false, Pawn);
		FHitResult Hit;
		const double Bottom = FMath::Min(LevelBounds.Min.Z - 1000.0, Here.Z - 20000.0);
		if (!W->LineTraceSingleByChannel(Hit, Here, FVector(Here.X, Here.Y, Bottom), ECC_Pawn, Params))
		{
			Falls.Add({LastMoved, FString::Printf(TEXT("the player walked off here and fell with no ground below (%s)"), Walk.bEdge ? TEXT("an edge with no barrier") : TEXT("on the way across"))});
			return true;
		}
	}
	// Along the route, a waypoint at a time.
	const double Reached = FMath::Max(60.0, Grid.CellSize * 0.25);
	while (NextWaypoint < Walk.Route.Num() - 1 && FVector::Dist2D(Here, Walk.Route[NextWaypoint]) < Reached)
	{
		++NextWaypoint;
	}
	const FVector Toward = Walk.Route.IsValidIndex(NextWaypoint) ? Walk.Route[NextWaypoint] : Walk.Goal;
	const FVector Direction = (Toward - Here).GetSafeNormal2D();
	if (!Walk.bEdge && FVector::Dist2D(Here, Walk.Goal) < 60.0)
	{
		return true;
	}
	if (FVector::Dist(Here, LastMoved) > 30.0)
	{
		LastMoved = Here;
		LastMovedAt = WalkElapsed;
	}
	else if (WalkElapsed - LastMovedAt > 1.0)
	{
		// A walk to the edge that stops is held back by a wall or railing, which is what an edge should do.
		if (!Walk.bEdge)
		{
			Stuck.Add({Here, FString::Printf(TEXT("no progress for 1 s walking toward %s along a %d-point route"), *Location3(Toward), Walk.Route.Num())});
		}
		return true;
	}
	Pawn->AddMovementInput(Direction, 1.f);
	FRotator View = Controller->GetControlRotation();
	View.Yaw = Direction.Rotation().Yaw;
	Controller->SetControlRotation(View);
	if (++WalkFrames % 3 == 0 && FramePaths.Num() < MaxFrames)
	{
		FVector Eye;
		FRotator Look;
		Controller->GetPlayerViewPoint(Eye, Look);
		TArray<FColor> Pixels;
		if (Render(Eye, Look, FrameWidth, FrameHeight, 0.f, Pixels))
		{
			SaveFrame(Pixels, FrameWidth, FrameHeight);
		}
	}
	return WalkElapsed >= WalkSeconds || WalkingLeft <= 0.0;
}

bool FHyperAIBotSurveyor::Render(const FVector& Location, const FRotator& Rotation, const int32 Width, const int32 Height, const float OrthoWidth,
	TArray<FColor>& OutPixels)
{
	UWorld* W = World.Get();
	if (!W || !W->Scene)
	{
		return false;
	}
	if (!Target.IsValid() || Target->SizeX != Width || Target->SizeY != Height)
	{
		Target.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient));
		Target->RenderTargetFormat = RTF_RGBA8;
		Target->ClearColor = FLinearColor::Black;
		Target->InitAutoFormat(Width, Height);
		Target->UpdateResourceImmediate(/*bClearRenderTarget=*/true);
	}
	if (!Capture.IsValid())
	{
		Capture.Reset(NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient));
		Capture->CaptureSource = SCS_FinalColorLDR;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = true;
		Capture->RegisterComponentWithWorld(W);
	}
	Capture->TextureTarget = Target.Get();
	Capture->ProjectionType = OrthoWidth > 0.f ? ECameraProjectionMode::Orthographic : ECameraProjectionMode::Perspective;
	Capture->OrthoWidth = OrthoWidth > 0.f ? OrthoWidth : Capture->OrthoWidth;
	Capture->FOVAngle = 90.f;
	Capture->SetWorldLocationAndRotation(Location, Rotation);
#if WITH_EDITOR
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
#endif
	W->SendAllEndOfFrameUpdates();
	Capture->CaptureScene();
	FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
	return Resource && Resource->ReadPixels(OutPixels) && OutPixels.Num() == Width * Height;
}

bool FHyperAIBotSurveyor::SaveFrame(const TArray<FColor>& Pixels, const int32 Width, const int32 Height)
{
	TArray<FColor> Opaque = Pixels;
	for (FColor& Pixel : Opaque)
	{
		Pixel.A = 255;
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(Opaque.GetData(), Opaque.Num()), Png);
	const FString Path = FPaths::Combine(Folder, FString::Printf(TEXT("frame_%04d.png"), FramePaths.Num() + 1));
	if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *Path))
	{
		return false;
	}
	FramePaths.Add(Path);
	return true;
}

FHyperAIBotResult FHyperAIBotSurveyor::Tick()
{
	UWorld* W = World.Get();
	if (!W)
	{
		return FResult::Failed(TEXT("world_unavailable"), TEXT("The game world ended during the survey."));
	}
	switch (Phase)
	{
	case EPhase::Grid:
		for (int32 Rows = 0; Rows < 4 && NextRow < Grid.Cells; ++Rows)
		{
			SortRow(NextRow++);
		}
		if (NextRow >= Grid.Cells)
		{
			if (bNavmesh)
			{
				Phase = EPhase::Reach;
				NextRow = 0;
				break;
			}
			// No navmesh: the player's ground, cell to cell, where the rise is small enough and nothing at knee height blocks.
			Cells = Classify(Grid, GroundZ, Start, MaxRise, [this](const int32 From, const int32 To) { return IsOpen(From, To); });
			PlanWalks();
			Phase = EPhase::Views;
		}
		break;
	case EPhase::Reach:
		for (int32 Rows = 0; Rows < 2 && NextRow < Grid.Cells; ++Rows)
		{
			ReachRow(NextRow++);
		}
		if (NextRow >= Grid.Cells)
		{
			PlanWalks();
			Phase = EPhase::Views;
		}
		break;
	case EPhase::Views:
		if (NextView < Views.Num())
		{
			const FVector Eye = GroundPoint(Views[NextView]) + FVector(0.0, 0.0, EyeHeight);
			const FVector2D Middle = Grid.Origin + FVector2D(Grid.Size() * 0.5);
			FRotator Look = (FVector(Middle, Eye.Z) - Eye).Rotation();
			if (FVector2D::Distance(FVector2D(Eye), Middle) < Grid.CellSize)
			{
				Look.Yaw = 90.0 * NextView;
			}
			Look.Pitch = -12.0;
			TArray<FColor> Pixels;
			if (Render(Eye, Look, FrameWidth, FrameHeight, 0.f, Pixels))
			{
				SaveFrame(Pixels, FrameWidth, FrameHeight);
			}
			++NextView;
			break;
		}
		Phase = EPhase::Walks;
		break;
	case EPhase::Walks:
		if (!bWalking && !StartWalk())
		{
			Phase = EPhase::Finish;
			break;
		}
		if (TickWalk(FMath::Max(static_cast<double>(W->GetDeltaSeconds()), 1.0 / 120.0)))
		{
			bWalking = false;
			++NextWalk;
			++WalksDone;
		}
		break;
	case EPhase::Finish:
		return Finish();
	}
	return FResult();
}

FHyperAIBotResult FHyperAIBotSurveyor::Finish()
{
	UWorld* W = World.Get();
	// The overview: the grid seen straight down, the survey drawn over it.
	TArray<FColor> Overview;
	const FVector2D Middle = Grid.Origin + FVector2D(Grid.Size() * 0.5);
	const bool bRendered = Render(FVector(Middle, LevelBounds.Max.Z + 2000.0), FRotator(-90.0, 0.0, 0.0), OverviewSize, OverviewSize,
		static_cast<float>(Grid.Size()), Overview);
	PaintOverview(Overview, OverviewSize, Grid, Cells, Walked, Start, Stuck, Falls);
	for (FColor& Pixel : Overview)
	{
		Pixel.A = 255;
	}
	TArray64<uint8> Png;
	FImageUtils::PNGCompressImageArray(OverviewSize, OverviewSize, TArrayView64<const FColor>(Overview.GetData(), Overview.Num()), Png);
	const FString OverviewPath = FPaths::Combine(Folder, TEXT("overview.png"));
	FFileHelper::SaveArrayToFile(Png, *OverviewPath);

	int32 Counts[3] = {};
	for (const ECell Cell : Cells)
	{
		++Counts[static_cast<int32>(Cell)];
	}
	const double Percent = ReachablePercent(Cells);
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema"), TEXT("hyperai.level-survey.v1"));
	Json->SetStringField(TEXT("method"), bNavmesh ? TEXT("navmesh") : TEXT("ground"));
	Json->SetNumberField(TEXT("cells"), Grid.Cells);
	Json->SetNumberField(TEXT("cellSize"), FMath::RoundToDouble(Grid.CellSize));
	Json->SetArrayField(TEXT("origin"), {MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Grid.Origin.X)), MakeShared<FJsonValueNumber>(FMath::RoundToDouble(Grid.Origin.Y))});
	Json->SetNumberField(TEXT("reachable"), Counts[static_cast<int32>(ECell::Reachable)]);
	Json->SetNumberField(TEXT("unreachable"), Counts[static_cast<int32>(ECell::Unreachable)]);
	Json->SetNumberField(TEXT("noGround"), Counts[static_cast<int32>(ECell::NoGround)]);
	Json->SetNumberField(TEXT("reachablePercent"), FMath::RoundToDouble(Percent * 10.0) / 10.0);
	Json->SetObjectField(TEXT("start"), PointJson(StartLocation, FString()));
	FString Rows;
	for (int32 Y = 0; Y < Grid.Cells; ++Y)
	{
		for (int32 X = 0; X < Grid.Cells; ++X)
		{
			const int32 Index = Grid.IndexOf(X, Y);
			Rows.AppendChar(Index == Start ? TEXT('S') : Cells[Index] == ECell::Reachable ? (Walked[Index] ? TEXT('w') : TEXT('r'))
				: Cells[Index] == ECell::Unreachable ? TEXT('u') : TEXT('.'));
		}
		Rows.AppendChar(TEXT('|'));
	}
	Json->SetStringField(TEXT("map"), Rows);
	Json->SetStringField(TEXT("mapKey"), TEXT("rows by Y from origin, X across: S start, r reachable, w reachable and walked, u unreachable, . no ground"));
	TArray<TSharedPtr<FJsonValue>> StuckJson;
	for (const FFinding& Spot : Stuck)
	{
		StuckJson.Add(MakeShared<FJsonValueObject>(PointJson(Spot.Location, Spot.Detail)));
	}
	TArray<TSharedPtr<FJsonValue>> FallJson;
	for (const FFinding& Fall : Falls)
	{
		FallJson.Add(MakeShared<FJsonValueObject>(PointJson(Fall.Location, Fall.Detail)));
	}
	Json->SetArrayField(TEXT("stuck"), StuckJson);
	Json->SetArrayField(TEXT("falls"), FallJson);
	Json->SetNumberField(TEXT("walks"), WalksDone);
	Json->SetNumberField(TEXT("frames"), FramePaths.Num());
	Json->SetStringField(TEXT("overview"), OverviewPath);
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Json, Writer);
	FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Folder, TEXT("coverage.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	Stop();
	TArray<FString> Found;
	for (int32 Index = 0; Index < Stuck.Num() && Index < 4; ++Index)
	{
		Found.Add(TEXT("stuck at ") + Location3(Stuck[Index].Location));
	}
	for (int32 Index = 0; Index < Falls.Num() && Index < 4; ++Index)
	{
		Found.Add(TEXT("fell out at ") + Location3(Falls[Index].Location));
	}
	const FString Detail = FString::Printf(
		TEXT("%dx%d grid of %.0f cm cells (%s): %d reachable (%.0f%% of the ground), %d unreachable, %d with no ground; %d views and %d walks, %d frames; %d stuck spot(s), %d fall(s) out of the world%s%s; overview %s, coverage.json beside it%s"),
		Grid.Cells, Grid.Cells, Grid.CellSize, bNavmesh ? TEXT("navmesh paths") : TEXT("no navmesh: ground connectivity"),
		Counts[static_cast<int32>(ECell::Reachable)], Percent, Counts[static_cast<int32>(ECell::Unreachable)], Counts[static_cast<int32>(ECell::NoGround)],
		Views.Num(), WalksDone, FramePaths.Num(), Stuck.Num(), Falls.Num(), Found.IsEmpty() ? TEXT("") : TEXT(": "), *FString::Join(Found, TEXT(", ")),
		*OverviewPath, bRendered ? TEXT("") : TEXT(" (no render of the level here: the overlay alone)"));
	return FResult::Passed(Detail, Folder);
}

void FHyperAIBotSurveyor::Stop()
{
	UWorld* W = World.Get();
	APawn* Pawn = W ? PlayerPawn(*W, Step.PlayerIndex) : nullptr;
	if (Pawn)
	{
		Pawn->SetActorLocation(StartLocation, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
		if (ACharacter* Character = Cast<ACharacter>(Pawn); Character && Character->GetCharacterMovement())
		{
			Character->GetCharacterMovement()->StopMovementImmediately();
		}
		if (AController* Controller = Pawn->GetController())
		{
			Controller->SetControlRotation(StartRotation);
		}
	}
	bWalking = false;
}
