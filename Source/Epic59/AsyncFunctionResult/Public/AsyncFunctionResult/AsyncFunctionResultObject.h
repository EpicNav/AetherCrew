// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"
#include "UObject/ObjectPtr.h"

#include "AsyncFunctionResultObject.generated.h"

#define UE_API ASYNCFUNCTIONRESULT_API

// Async function result that completes with an object, or with null.
UCLASS(BlueprintType, MinimalAPI)
class UAsyncFunctionResultObject : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	// Set the result. Off the game thread, the value is only stored on the next game thread
	// update, so it must stay alive until then.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	bool SetValue(UObject* InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(TObjectPtr<UObject>(InValue), Value);
	}

	// Value of the result.
	UPROPERTY(BlueprintReadOnly, Category = "AsyncFunctionResult")
	TObjectPtr<UObject> Value;
};

#undef UE_API
