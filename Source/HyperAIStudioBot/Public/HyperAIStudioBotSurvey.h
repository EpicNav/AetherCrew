// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

/**
 * The level survey's arithmetic, apart from any world so it can be tested alone: the grid laid over the level, which
 * cells the player can reach from where it stands, where to look from and walk to, and the overview image's overlay.
 */
namespace HyperAIStudio::Bot::Survey
{
	constexpr int32 MinCells = 4;
	constexpr int32 MaxCells = 48;
	constexpr int32 DefaultCells = 24;
	/** Viewpoint frames at most, spread over the reachable cells. */
	constexpr int32 MaxViews = 16;
	constexpr float DefaultWalkSeconds = 12.f;
	/** One walk lasts this long at most, so the walking seconds reach several parts of the level. */
	constexpr float WalkSeconds = 2.f;
	constexpr int32 OverviewSize = 1024;

	enum class ECell : uint8
	{
		/** Nothing under it to stand on, down to the bottom of the level. */
		NoGround,
		/** Ground the player can get to from where it starts. */
		Reachable,
		/** Ground, but no way there from where the player starts. */
		Unreachable
	};

	HYPERAISTUDIOBOT_API const TCHAR* CellName(ECell Cell);

	/** A square grid of Cells x Cells over a region of the level, row by row from its low X, low Y corner. */
	struct HYPERAISTUDIOBOT_API FGrid
	{
		FVector2D Origin = FVector2D::ZeroVector;
		double CellSize = 100.0;
		int32 Cells = DefaultCells;

		/** A square of Cells a side covering Bounds' XY, or only the part within Range of Center when Range is above 0. */
		static FGrid Over(const FBox& Bounds, const FVector& Center, double Range, int32 Cells);

		int32 Num() const { return Cells * Cells; }
		int32 IndexOf(int32 X, int32 Y) const { return Y * Cells + X; }
		/** The cell holding Point, or INDEX_NONE outside the grid. */
		int32 IndexAt(const FVector2D& Point) const;
		FVector2D Center(int32 Index) const;
		double Size() const { return CellSize * Cells; }
	};

	/**
	 * Cells the player reaches from Start over ground: neighbours (four ways) join when both have ground, the rise between
	 * them is at most MaxRise, and Open says nothing blocks the way. GroundZ is NaN where there is no ground. Open(A, B)
	 * may be null to treat every way as open.
	 */
	HYPERAISTUDIOBOT_API TArray<ECell> Classify(const FGrid& Grid, const TArray<double>& GroundZ, int32 Start, double MaxRise,
		TFunctionRef<bool(int32 From, int32 To)> Open);

	/** Count of picks spread out over Points: the first is Points[0], each next the one farthest from those picked. Indices into Points. */
	HYPERAISTUDIOBOT_API TArray<int32> Spread(const TArray<FVector2D>& Points, int32 Count);

	/** The reachable cells' share of the cells with ground, 0 to 100. */
	HYPERAISTUDIOBOT_API double ReachablePercent(const TArray<ECell>& Cells);

	/** Something a walk found: a stuck spot or a fall. */
	struct HYPERAISTUDIOBOT_API FFinding
	{
		FVector Location = FVector::ZeroVector;
		FString Detail;
	};

	/** Where the pixel (U, V) of a Size x Size top-down view of Grid looks, and back: image up is +X, image right is +Y. */
	HYPERAISTUDIOBOT_API FVector2D PixelToWorld(const FGrid& Grid, int32 Size, double U, double V);
	HYPERAISTUDIOBOT_API FVector2D WorldToPixel(const FGrid& Grid, int32 Size, const FVector2D& World);

	/**
	 * Draws the survey over a Size x Size top-down picture of Grid (Pixels; black where there is none): reachable cells
	 * tinted green, unreachable red, no ground dark, walked cells blue, the start white, stuck spots yellow and falls magenta.
	 */
	HYPERAISTUDIOBOT_API void PaintOverview(TArray<FColor>& Pixels, int32 Size, const FGrid& Grid, const TArray<ECell>& Cells,
		const TArray<bool>& Walked, int32 Start, const TArray<FFinding>& Stuck, const TArray<FFinding>& Falls);
}
