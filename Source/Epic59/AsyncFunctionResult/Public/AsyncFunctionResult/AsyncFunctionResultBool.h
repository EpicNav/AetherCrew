// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"

#include "AsyncFunctionResultBool.generated.h"

#define UE_API ASYNCFUNCTIONRESULT_API

// Async function result that completes with a boolean.
UCLASS(BlueprintType, MinimalAPI)
class UAsyncFunctionResultBool : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	// Set the result.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	bool SetValue(const bool& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(bool(InValue), Value);
	}

	// Value of the result.
	UPROPERTY(BlueprintReadOnly, Category = "AsyncFunctionResult")
	bool Value = false;
};

#undef UE_API
