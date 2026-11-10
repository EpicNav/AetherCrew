// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"
#include "ToolsetRegistry/ToolsetImage.h"

#include "ToolCallAsyncResultImageArray.generated.h"

// Async tool call result that completes with an array of ToolsetImages.
UCLASS(BlueprintType)
class UToolCallAsyncResultImageArray : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "ToolsetRegistry")
	bool SetValue(const TArray<FToolsetImage>& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(TArray<FToolsetImage>(InValue), Value);
	}

	// Takes ownership of the images to avoid copying a large batch.
	bool SetValue(TArray<FToolsetImage>&& InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(MoveTemp(InValue), Value);
	}

	UPROPERTY(BlueprintReadOnly, Category = "ToolsetRegistry")
	TArray<FToolsetImage> Value;
};
