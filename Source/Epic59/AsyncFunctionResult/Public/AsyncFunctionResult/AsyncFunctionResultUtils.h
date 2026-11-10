// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Concepts/DerivedFrom.h"
#include "Containers/Ticker.h"
#include "Containers/UnrealString.h"
#include "Internationalization/Text.h"
#include "Templates/Function.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

#include "AsyncFunctionResult/AsyncFunctionResult.h"

#define UE_API ASYNCFUNCTIONRESULT_API

namespace UE::AsyncFunctionResult
{
	/// Returns a result that has already failed with Error, for an async function that rejects
	/// its arguments before starting any work.
	template<UE::CDerivedFrom<UAsyncFunctionResult> ResultT>
	[[nodiscard]] ResultT* MakeErrorResult(const FString& Error)
	{
		ResultT* Result = NewObject<ResultT>();
		Result->SetError(Error);
		return Result;
	}

	/// Returns a pending result and runs Function with it on the next core ticker update, for work
	/// that is unsafe on the stack of the call that started it, such as loading or unloading
	/// plugins. Function must complete the result, now or later; the reference it receives keeps
	/// the result alive for as long as it is held.
	///
	/// @note Game thread only.
	template<UE::CDerivedFrom<UAsyncFunctionResult> ResultT>
	[[nodiscard]] ResultT* RunOnNextTick(TFunction<void(const TStrongObjectPtr<ResultT>&)>&& Function)
	{
		ResultT* Result = NewObject<ResultT>();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[StrongResult = TStrongObjectPtr<ResultT>(Result), Function = MoveTemp(Function)](float) -> bool
			{
				Function(StrongResult);
				return false;
			}));
		return Result;
	}

	/// The error of an operation that failed without giving a reason.
	UE_API extern const FString GenericError;

	/// The error of an operation that was canceled.
	UE_API extern const FString CancelError;

	/// Returns the error of an operation that failed for Reason, or GenericError when Reason is empty. The overloads
	/// taking bCanceled return CancelError when it is true.
	[[nodiscard]] inline const FString& MakeFailureError(const FString& Reason)
	{
		return Reason.IsEmpty() ? GenericError : Reason;
	}

	const FString& MakeFailureError(FString&& Reason) = delete;

	[[nodiscard]] inline const FString& MakeFailureError(const FText& Reason)
	{
		return MakeFailureError(Reason.ToString());
	}

	const FString& MakeFailureError(FText&& Reason) = delete;

	[[nodiscard]] inline const FString& MakeFailureError(bool bCanceled, const FString& Reason)
	{
		return bCanceled ? CancelError : MakeFailureError(Reason);
	}

	const FString& MakeFailureError(bool bCanceled, FString&& Reason) = delete;

	[[nodiscard]] inline const FString& MakeFailureError(bool bCanceled, const FText& Reason)
	{
		return MakeFailureError(bCanceled, Reason.ToString());
	}

	const FString& MakeFailureError(bool bCanceled, FText&& Reason) = delete;
}

#undef UE_API
