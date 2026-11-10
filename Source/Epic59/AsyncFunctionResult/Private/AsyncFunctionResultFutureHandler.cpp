// Copyright Epic Games, Inc. All Rights Reserved.

#include "AsyncFunctionResult/AsyncFunctionResultFutureHandler.h"

#include "CoreGlobals.h"
#include "Misc/AssertionMacros.h"
#include "Templates/ValueOrError.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AsyncFunctionResultFutureHandler)

const FString UAsyncFunctionResultFutureHandler::CanceledError(TEXT("Canceled"));

UAsyncFunctionResultFutureHandler::~UAsyncFunctionResultFutureHandler()
{
	Unsubscribe(true);
}

TFuture<TValueOrError<TSharedPtr<FJsonValue>, FString>>
UAsyncFunctionResultFutureHandler::GetValueAsJson()
{
	check(!bRetrievedFuture);
	bRetrievedFuture = true;
	// Hold a strong reference to this object within the future so that it can't be garbage
	// collected until the future is complete.
	TStrongObjectPtr<UAsyncFunctionResultFutureHandler> This(this);
	return ResultPromise.GetFuture().Next(
		[This](TValueOrError<TSharedPtr<FJsonValue>, FString>&& Result) ->
			TValueOrError<TSharedPtr<FJsonValue>, FString>
		{
			return Result;
		});
}

void UAsyncFunctionResultFutureHandler::Subscribe(UAsyncFunctionResult* ResultIn)
{
	check(IsInGameThread());
	check(ResultIn);
	check(!Result);
	Result = ResultIn;
	Result->OnCompleted.AddUniqueDynamic(this, &ThisClass::OnCompleted);
	Result->BroadcastOnCompletedIfComplete();
}

void UAsyncFunctionResultFutureHandler::Unsubscribe()
{
	Unsubscribe(false);
}

void UAsyncFunctionResultFutureHandler::Unsubscribe(bool bIsDestructing)
{
	check(IsInGameThread());
	// Cancel the promise if it was never fulfilled.
	if (!bOnCompletedHandled)
	{
		ResultPromise.SetValue(MakeError(CanceledError));
		if (!bIsDestructing && Result)
		{
			Result->OnCompleted.RemoveDynamic(this, &ThisClass::OnCompleted);
		}
	}
	bOnCompletedHandled = true;
	Result = nullptr;
}

void UAsyncFunctionResultFutureHandler::OnCompleted(UToolCallAsyncResult* ResultIn) // UE 5.8: 5.8's delegate signature
{
	check(IsInGameThread());
	check(Result == ResultIn);
	check(Result->bIsComplete);
	check(!bOnCompletedHandled);
	const FString& Error = Result->Error;
	if (Error.IsEmpty())
	{
		ResultPromise.SetValue(MakeValue(Result->GetValueAsJson()));
	}
	else
	{
		ResultPromise.SetValue(MakeError(Error));
	}
	bOnCompletedHandled = true;
	Unsubscribe();
}

TStrongObjectPtr<UAsyncFunctionResultFutureHandler>
UAsyncFunctionResultFutureHandler::Create(TObjectPtr<UAsyncFunctionResult> Result)
{
	UAsyncFunctionResultFutureHandler* OnCompletedHandler =
		NewObject<UAsyncFunctionResultFutureHandler>();
	OnCompletedHandler->Subscribe(Result);
	return TStrongObjectPtr<UAsyncFunctionResultFutureHandler>(OnCompletedHandler);
}
