// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"

#include "AsyncFunctionResultStringArray.generated.h"

#define UE_API ASYNCFUNCTIONRESULT_API

// Async function result that completes with an array of strings.
UCLASS(BlueprintType, MinimalAPI)
class UAsyncFunctionResultStringArray : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	// Set the result.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	bool SetValue(const TArray<FString>& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(TArray<FString>(InValue), Value);
	}

	// Value of the result.
	UPROPERTY(BlueprintReadOnly, Category = "AsyncFunctionResult")
	TArray<FString> Value;
};

#undef UE_API
