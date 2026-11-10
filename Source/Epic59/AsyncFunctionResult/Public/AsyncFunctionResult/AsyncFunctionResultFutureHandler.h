// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Async/Future.h"
#include "Containers/UnrealString.h"
#include "Delegates/Delegate.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"
#include "Templates/ValueOrError.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"

#include "AsyncFunctionResult/AsyncFunctionResult.h"

#include "AsyncFunctionResultFutureHandler.generated.h"

#define UE_API ASYNCFUNCTIONRESULT_API

// Handles UAsyncFunctionResult::OnCompleted by completing the associated future.
//
// NOTE: This is tested by AsyncFunctionResultTest.cpp.
UCLASS(MinimalAPI, NotBlueprintable, HideDropdown)
class UAsyncFunctionResultFutureHandler : public UObject
{
	GENERATED_BODY()

public:
	UE_API virtual ~UAsyncFunctionResultFutureHandler() override;

	// Resolves with the JSON result of the UAsyncFunctionResult object passed to Create().
	// NOTE: The future can only be retrieved once.
	UE_API TFuture<TValueOrError<TSharedPtr<FJsonValue>, FString>> GetValueAsJson();

	// Unsubscribe from the result and cancel the pending future returned from GetValueAsJson().
	UE_API void Unsubscribe();

private:
	// Handles a result completion.
	// UE 5.8: OnCompleted is 5.8's FToolCallAsyncResultCompleted, whose parameter is a UToolCallAsyncResult*.
	UFUNCTION()
	void OnCompleted(UToolCallAsyncResult* Result);

	// Subscribes to result completion.
	void Subscribe(UAsyncFunctionResult* Result);

	// Unsubscribe, optionally without touching the Result as it may already be garbage collected.
	void Unsubscribe(bool bIsDestructing);

private:
	TObjectPtr<UAsyncFunctionResult> Result;
	TPromise<TValueOrError<TSharedPtr<FJsonValue>, FString>> ResultPromise;
	bool bOnCompletedHandled = false;
	bool bRetrievedFuture = false;

public:
	// Create a handler and bind the OnCompleted method to the specified result instance's
	// OnCompleted delegate.
	static UE_API TStrongObjectPtr<UAsyncFunctionResultFutureHandler> Create(
		TObjectPtr<UAsyncFunctionResult> Result);

	// Error that indicates the future was canceled.
	static UE_API const FString CanceledError;
};

#undef UE_API
