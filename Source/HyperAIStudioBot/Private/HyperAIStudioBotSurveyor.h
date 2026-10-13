// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioBot.h"
#include "HyperAIStudioBotSurvey.h"
#include "UObject/StrongObjectPtr.h"

class APawn;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UWorld;

/**
 * A SurveyLevel step, a frame at a time: lays the grid and sorts its cells (a row a frame), captures the viewpoints (one
 * a frame), walks the player in short walks from spread-out reachable spots, some toward the edges, to find where it gets
 * stuck or falls, then renders the overview from above and writes it with coverage.json.
 */
class FHyperAIBotSurveyor
{
public:
	FHyperAIBotSurveyor(UWorld& InWorld, const FHyperAIBotStep& InStep, const FString& InFolder);
	~FHyperAIBotSurveyor();

	FHyperAIBotResult Begin();
	FHyperAIBotResult Tick();
	/** Puts the player back where it started. */
	void Stop();

private:
	enum class EPhase : uint8
	{
		Grid,
		/** Navmesh paths to every cell with ground, a row a frame. */
		Reach,
		Views,
		Walks,
		Finish
	};

	void SortRow(int32 Y);
	void ReachRow(int32 Y);
	void PlanWalks();
	/** Whether the player gets from one cell to its neighbour: nothing at knee height between their ground points. */
	bool IsOpen(int32 From, int32 To) const;
	/** The way a player walks from one reachable cell to another: the navmesh's path, or cell to cell over the ground. */
	TArray<FVector> RouteBetween(int32 From, int32 To) const;
	bool StartWalk();
	/** One frame of the current walk; true when it has ended. */
	bool TickWalk(double Dt);
	void PutPlayerAt(const FVector& Ground);
	/** Renders Size, from Location toward Rotation (orthographic over OrthoWidth cm when above 0) into Pixels. */
	bool Render(const FVector& Location, const FRotator& Rotation, int32 Width, int32 Height, float OrthoWidth, TArray<FColor>& OutPixels);
	bool SaveFrame(const TArray<FColor>& Pixels, int32 Width, int32 Height);
	FHyperAIBotResult Finish();
	FVector GroundPoint(int32 Index) const;
	double PawnHalfHeight() const;

	TWeakObjectPtr<UWorld> World;
	FHyperAIBotStep Step;
	FString Folder;

	EPhase Phase = EPhase::Grid;
	HyperAIStudio::Bot::Survey::FGrid Grid;
	TArray<double> GroundZ;
	TArray<HyperAIStudio::Bot::Survey::ECell> Cells;
	TArray<bool> Walked;
	int32 Start = INDEX_NONE;
	int32 NextRow = 0;
	bool bNavmesh = false;
	double MaxRise = 0.0;
	double KillZ = -HALF_WORLD_MAX;
	FBox LevelBounds = FBox(ForceInit);

	TArray<int32> Views;
	int32 NextView = 0;

	struct FWalk
	{
		int32 From = INDEX_NONE;
		FVector Goal = FVector::ZeroVector;
		/** Toward the edge of the reachable ground, to see whether the player can fall out there. */
		bool bEdge = false;
		/** The points it walks through, Goal last. */
		TArray<FVector> Route;
	};
	TArray<FWalk> Walks;
	int32 NextWalk = 0;
	int32 NextWaypoint = 0;
	bool bWalking = false;
	double WalkElapsed = 0.0;
	double WalkingLeft = 0.0;
	double WalkStartZ = 0.0;
	FVector LastMoved = FVector::ZeroVector;
	double LastMovedAt = 0.0;
	int32 WalkFrames = 0;
	int32 WalksDone = 0;

	TArray<HyperAIStudio::Bot::Survey::FFinding> Stuck;
	TArray<HyperAIStudio::Bot::Survey::FFinding> Falls;
	TArray<FString> FramePaths;

	FVector StartLocation = FVector::ZeroVector;
	FRotator StartRotation = FRotator::ZeroRotator;

	TStrongObjectPtr<USceneCaptureComponent2D> Capture;
	TStrongObjectPtr<UTextureRenderTarget2D> Target;
};
