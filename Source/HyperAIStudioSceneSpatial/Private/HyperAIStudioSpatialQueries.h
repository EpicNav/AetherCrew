// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "Engine/EngineTypes.h"
#include "Engine/HitResult.h"

class AActor;
class UPrimitiveComponent;
class UWorld;
struct FHyperAISpatialQuery;
struct FHyperAISpatialQueryReport;
struct FHyperAISpatialQueryRequest;
struct FHyperAISpatialResult;

/** Spatial questions and placement math over a world: every function takes the world, so tests run on worlds of their own. Game thread only. */
namespace HyperAIStudio::SceneSpatial::Spatial
{
	/** Hits returned per trace or sweep with bMulti. */
	constexpr int32 MaxHits = 16;
	/** Actors and components a query is told to pass through, at most. */
	constexpr int32 MaxIgnored = 4096;

	FString LabelOf(const AActor& Actor);
	FString FormatVector(const FVector& Value);
	FString FormatRotator(const FRotator& Value);

	/** A collision channel by its name (Visibility, WorldDynamic, or one the project adds); Fallback when Name is empty. */
	bool ResolveChannel(const FString& Name, ECollisionChannel Fallback, ECollisionChannel& OutChannel, FString& OutError);
	/** The channel names ResolveChannel takes. */
	FString DescribeChannels();

	/**
	 * A loaded actor by its exact label (any case) or object path. A label several actors share is refused with every
	 * path; one that belongs to a World Partition actor that is not loaded says so.
	 */
	AActor* FindActor(UWorld& World, const FString& LabelOrPath, FString& OutError);

	/** Sprites, arrows, debug drawing and other editor helpers: not what an actor looks like, and never drawn in a game. */
	bool IsHelperPrimitive(const UPrimitiveComponent& Component);
	/**
	 * What an actor looks like, in the world and in its own space (unscaled): its registered components' bounds, helpers
	 * left out, and shapes that block nothing (a pickup's overlap sphere) too unless the actor has nothing else.
	 */
	FBox WorldBounds(const AActor& Actor);
	FBox LocalBounds(const AActor& Actor);

	/** The tag PCG puts on every component it generates (PCGHelpers::DefaultPCGTag), as text so this module needs no PCG. */
	FName PCGGeneratedTag();
	/** PCG's partition actor class, which holds a partitioned volume's instances; null while the PCG plugin is not loaded. */
	UClass* PCGPartitionActorClass();
	/** Instances PCG generated: tagged, or on a partition actor of PartitionClass (PCGPartitionActorClass()). */
	bool IsPCG(const UPrimitiveComponent& Component, const UClass* PartitionClass);
	/** Foliage instances, and instances PCG generated: what bIgnoreFoliage passes through. */
	bool IsFoliageOrPCG(const UPrimitiveComponent& Component);
	/** Adds the world's foliage and PCG-generated instances to Params' ignores; false when there were more than MaxIgnored. */
	bool IgnoreFoliage(UWorld& World, FCollisionQueryParams& Params);

	/** A landscape whose edit layers are still merging, so its collision, and any ground found on it, lags behind. */
	bool IsLandscapePending(UWorld& World);

	/**
	 * The surface under a point: below it, or above it within 50 m when the point is buried. A point inside a mesh finds
	 * the surface under that mesh, and says it started inside.
	 */
	struct FGround
	{
		bool bFound = false;
		bool bStartedInside = false;
		FVector Point = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
		float Slope = 0.f;
		const AActor* Actor = nullptr;
		FHitResult Hit;
	};
	FGround FindGround(const UWorld& World, const FVector& Point, ECollisionChannel Channel, const FCollisionQueryParams& Params);

	/** A spot near Point where a box of Extent (turned by Yaw) fits, searched ring by ring outwards in a fixed order. */
	struct FClearSpotSettings
	{
		FVector Point = FVector::ZeroVector;
		FVector Extent = FVector(50.0);
		double Yaw = 0.0;
		float Clearance = 10.f;
		float Step = 0.f;
		float MaxRadius = 2000.f;
		float MaxSlope = 35.f;
		bool bSnapToGround = true;
		/** What is in the way: what blocks Channel, and or any loaded actor's bounds. */
		bool bCollision = true;
		bool bBounds = false;
		ECollisionChannel Channel = ECC_WorldDynamic;
		ECollisionChannel GroundChannel = ECC_Visibility;
		/** What the in-the-way test passes through, and what the ground traces pass through. */
		FCollisionQueryParams Params;
		FCollisionQueryParams GroundParams;
		/** Actors whose bounds never count as in the way. */
		TSet<const AActor*> IgnoreBounds;
		/** Spots earlier moves of the same plan already took. */
		TArray<FBox> Claimed;
		/** FPlatformTime::Seconds() past which the search stops; 0 for none. */
		double Deadline = 0.0;
	};
	struct FClearSpot
	{
		bool bFound = false;
		/** The middle of the spot's floor. */
		FVector Location = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
		float Slope = 0.f;
		/** The space the spot keeps free, Clearance included. */
		FBox Box = FBox(ForceInit);
		int32 Candidates = 0;
		int32 Blocked = 0;
		int32 NoGround = 0;
		int32 TooSteep = 0;
		bool bDeadline = false;
		FString Rejected() const;
	};
	FClearSpot FindClearSpot(UWorld& World, const FClearSpotSettings& Settings);

	/** What a drop moves: an actor's bounds in its own space (unscaled) and its transform, or those of a mesh about to be placed. */
	struct FDropSubject
	{
		FBox LocalBox = FBox(ForceInit);
		FTransform Transform = FTransform::Identity;
	};
	struct FDropSettings
	{
		ECollisionChannel Channel = ECC_Visibility;
		float TraceDistance = 50000.f;
		bool bCorners = false;
		bool bAlign = false;
		float MaxAlignDegrees = 30.f;
		float Sink = 0.f;
		FCollisionQueryParams Params;
	};
	/**
	 * Where the subject comes to rest on the ground below it: its bounds' bottom on the highest surface found under it,
	 * or, aligned, its bounds against the surface's plane, turned to the surface's normal. False when nothing is below.
	 */
	bool PlanDrop(const UWorld& World, const FDropSubject& Subject, const FDropSettings& Settings, FTransform& OutTransform);

	/** Answers one query. Deadline is FPlatformTime::Seconds() past which a clear_spot search stops. */
	void Run(UWorld& World, const FHyperAISpatialQuery& Query, double Deadline, FHyperAISpatialResult& Out);
	/** Answers the request's queries in order, within its deadline and output size; the request's limits are taken as given. */
	void RunAll(UWorld& World, const FHyperAISpatialQueryRequest& Request, FHyperAISpatialQueryReport& Out);
	int64 EstimateBytes(const FHyperAISpatialQueryReport& Report);
}
