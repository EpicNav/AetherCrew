// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions
#include "WorldConditionTools59.generated.h"

struct FInstancedStruct;
struct FWorldConditionQueryDefinition;

/** Inspect WorldCondition (FWorldConditionQueryDefinition, FWorldConditionBase) structs. */
UCLASS(Blueprintable)
class UWorldConditionTools59 : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Returns a human-readable description of a world condition query.
	 * @param QueryDefinition The query definition to describe.
	 * @return Text description of all conditions in the query.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "WorldConditions")
	static FText GetQueryDescription(const FWorldConditionQueryDefinition& QueryDefinition);

	/**
	 * Returns a human-readable description of a single world condition.
	 * The condition must be passed as an FInstancedStruct containing an FWorldConditionBase-derived struct.
	 * @param Condition The instanced struct holding the world condition.
	 * @return Text description of the condition, or empty if invalid.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "WorldConditions")
	static FText GetConditionDescription(const FInstancedStruct& Condition);

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface
};
