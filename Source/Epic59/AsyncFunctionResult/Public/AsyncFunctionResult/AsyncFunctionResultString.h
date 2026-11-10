// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"

#include "AsyncFunctionResultString.generated.h"

#define UE_API ASYNCFUNCTIONRESULT_API

// Async function result that completes with a string.
UCLASS(BlueprintType, MinimalAPI)
class UAsyncFunctionResultString : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	// Set the result.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	bool SetValue(const FString& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(FString(InValue), Value);
	}

	// Value of the result.
	UPROPERTY(BlueprintReadOnly, Category = "AsyncFunctionResult")
	FString Value;
};

#undef UE_API
