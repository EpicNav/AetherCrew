// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioMeshTerrainToolset.h"

class UWorld;

/** The only code that reads or changes Mesh Partition actors and their modifiers. */
namespace HyperAIStudio::MeshTerrain::Gate
{
	UWorld* GetEditorWorld();

	/** Terrains and modifiers in the level, by name. Base mesh sections count toward their terrain, not as modifiers. */
	void Read(UWorld& World, TArray<FHyperAIMeshTerrain>& OutTerrains, TArray<FHyperAIMeshTerrainModifier>& OutModifiers);
	/** Level path plus every terrain's and modifier's identity, transform and settings. */
	FString ComputeRevision(UWorld& World);

	/** Checks every op against the level as it would be at that point; changes nothing. */
	bool ValidateOps(UWorld& World, const TArray<FHyperAIMeshTerrainOp>& Ops, TArray<FString>& OutNotes, FString& OutError);
	/** Applies ops in order as one undo step. */
	bool ApplyOps(UWorld& World, const TArray<FHyperAIMeshTerrainOp>& Ops, int32& OutChanged, FString& OutError);

	/** True while any terrain is rebuilding its preview sections. */
	bool IsBuilding(UWorld& World);
	void Check(UWorld& World, FHyperAIMeshTerrainValidateReport& Report);

	/** Splits "A=1; B=(X=1,Y=2)" at semicolons outside brackets and quotes; exposed for tests. */
	bool ParseProperties(const FString& Text, TArray<TPair<FString, FString>>& OutPairs, FString& OutError);
	/** "x,y,z; x,y,z" as points; exposed for tests. */
	bool ParsePoints(const FString& Text, TArray<FVector>& OutPoints, FString& OutError);
}
