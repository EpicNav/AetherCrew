// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class UStaticMesh;
class UWorld;
struct FHyperAISpatialPlanReport;
struct FHyperAISpatialPlanRequest;

/** hyper_spatial_apply_plan's check and apply over a world, so tests run them on worlds of their own. Game thread only. */
namespace HyperAIStudio::SceneSpatial::Plan
{
	/** One actor's move, or one actor to place, as a check computed it. */
	struct FMove
	{
		int32 Operation = 0;
		FString Kind;
		/** The actor moved; unset for a placement. */
		TWeakObjectPtr<AActor> Actor;
		/** The actor's label, or the new actor's. */
		FString Name;
		FTransform From = FTransform::Identity;
		FTransform To = FTransform::Identity;
		/** place: the mesh and the new actor's Outliner folder. */
		TWeakObjectPtr<UStaticMesh> Mesh;
		FString Folder;

		/** Label: (x, y, z) -> (x, y, z), rot pitch/yaw/roll -> pitch/yaw/roll; a placement says where it goes. */
		FString Describe() const;
	};

	/**
	 * Checks the plan against World and computes every move, each against the level as it is now (a clear spot also
	 * keeps off where the plan places and drops things), into OutMoves and Report's Moves and Notes, in the plan's
	 * order. False when it is refused, with Report's Status and Diagnostic saying why.
	 */
	bool Check(UWorld& World, const FHyperAISpatialPlanRequest& Request, TArray<FMove>& OutMoves, FHyperAISpatialPlanReport& Report);

	/**
	 * Applies Checked in one undo step. With bRecheck the plan is computed again first, as after an approval: if the
	 * level changed since the check so that any move would differ (by 1 cm or half a degree), nothing is applied.
	 * Without it Checked is applied as it is, for a caller that checked it in the same call; its Notes are the caller's
	 * to add. Saves the level when the request asks.
	 */
	FHyperAISpatialPlanReport Execute(UWorld& World, const FHyperAISpatialPlanRequest& Request, const TArray<FMove>& Checked, const FString& OperationId,
		bool bRecheck = true);

#if WITH_DEV_AUTOMATION_TESTS
	/** Checks run so far, so tests see how often one call computes its plan. */
	int32 GetCheckCountForTests();
	/** The seconds every clear-spot search of one check may take together; unset restores 15. */
	void SetClearSpotBudgetForTests(TOptional<double> Seconds);
#endif
}
