// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"

#include "AsyncFunctionResultFloat.generated.h"

#define UE_API ASYNCFUNCTIONRESULT_API

// Async function result that completes with a double-precision float.
// Python `float` maps to FDoubleProperty by default.
UCLASS(BlueprintType, MinimalAPI)
class UAsyncFunctionResultFloat : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	// Set the result.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	bool SetValue(const double& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(double(InValue), Value);
	}

	// Value of the result.
	UPROPERTY(BlueprintReadOnly, Category = "AsyncFunctionResult")
	double Value = 0.0;
};

#undef UE_API
