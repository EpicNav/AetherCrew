// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"

#include "AsyncFunctionResultInt.generated.h"

#define UE_API ASYNCFUNCTIONRESULT_API

// Async function result that completes with an integer.
UCLASS(BlueprintType, MinimalAPI)
class UAsyncFunctionResultInt : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	// Set the result.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	bool SetValue(const int32& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(int32(InValue), Value);
	}

	// Value of the result.
	UPROPERTY(BlueprintReadOnly, Category = "AsyncFunctionResult")
	int32 Value = 0;
};

#undef UE_API
