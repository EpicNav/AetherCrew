// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"
#include "ToolsetRegistry/ToolsetImage.h"

#include "ToolCallAsyncResultImages.generated.h"

// Async tool call result that completes with an array of rendered images (camera-augmented).
UCLASS(BlueprintType)
class UToolCallAsyncResultImages : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	// Set the result.
	UFUNCTION(BlueprintCallable, Category = "ToolsetRegistry")
	bool SetValue(const TArray<FToolsetImage>& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(TArray<FToolsetImage>(InValue), Value);
	}

	// Value of the result.
	UPROPERTY(BlueprintReadOnly, Category = "ToolsetRegistry")
	TArray<FToolsetImage> Value;
};
