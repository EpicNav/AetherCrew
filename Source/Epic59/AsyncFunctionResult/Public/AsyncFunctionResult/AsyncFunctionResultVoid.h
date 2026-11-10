// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"

#include "AsyncFunctionResultVoid.generated.h"

// Async function result that completes with no value.
UCLASS(BlueprintType, MinimalAPI)
class UAsyncFunctionResultVoid : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	// Complete this result with no value and notify listeners of OnCompleted.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	bool SetCompleted()
	{
		return MaybeBroadcastCompletionWithNoValue();
	}
};
