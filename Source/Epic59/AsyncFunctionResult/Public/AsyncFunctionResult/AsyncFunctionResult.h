// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: compatibility stand-in for UE 5.9's AsyncFunctionResult engine module, shipped inside
// HyperAIStudio so Epic's 5.9 toolsets build against the 5.8 ToolsetRegistry. UE 5.8's registry
// only treats a return value as asynchronous when its class derives from UToolCallAsyncResult,
// so UAsyncFunctionResult derives from it and adds the 5.9-only API on top. The members 5.8's
// base already declares (bIsComplete, OnCompleted, Error, SetError, BroadcastOnCompletedIfComplete,
// GetValueAsJson, GetValueAsJsonString, MaybeBroadcastCompletion, MaybeBroadcastSuccessfulCompletion)
// are inherited, not redeclared. See Source/Epic59/PORTING.md.

#pragma once

#include <atomic>

#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Optional.h"
#include "Templates/SubclassOf.h"
#include "Templates/UniquePtr.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"

#include "ToolsetRegistry/ToolCallAsyncResult.h"

#include "AsyncFunctionResult/AsyncFunctionResultContext.h"
#include "AsyncFunctionResult/AsyncFunctionResultCaptureStep.h"

#include "AsyncFunctionResult.generated.h"

#define UE_API ASYNCFUNCTIONRESULT_API

/// UE 5.8: OnCompleted is 5.8's delegate, whose parameter is a UToolCallAsyncResult*. Handlers bound
/// with AddDynamic must take a UToolCallAsyncResult* and Cast<> it.
using FAsyncFunctionResultCompleted = FToolCallAsyncResultCompleted;

/// Base class for a UFUNCTION's asynchronous return value, analogous to a promise.
///
/// Asynchronous UFUNCTIONs should *not* use this base class directly, instead they should define
/// or use an existing derivative of this class that adds a typed value property named `Value`.
///
/// When the asynchronous operation completes it should perform one of the following:
/// * If successful, use MaybeBroadcastSuccessfulCompletion() from the derived class - ideally
///   implemented in a method called `SetValue` - to notify listeners of `OnCompleted`.
/// * If an error occurs, use SetError().
UCLASS(BlueprintType, Blueprintable, MinimalAPI)
class UAsyncFunctionResult : public UToolCallAsyncResult
{
	GENERATED_BODY()

public:
	/// The default implementation converts the `Value` property; a class with no `Value` property
	/// completes with a JSON null (5.8's base logs an error instead).
	UE_API virtual TSharedPtr<FJsonValue> GetValueAsJson() const override;

	/// Install the dispatching context for the call that produced this result.
	///
	/// UE 5.8: the 5.8 registry never installs one, so tools see no id, no cancellation and no
	/// capture buffer unless a caller installs a context itself.
	UE_API void SetToolCallContext(TOptional<UE::AsyncFunctionResult::FAsyncFunctionResultContext> InContext);

	/// Identity of the call that produced this result, or an empty string.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	UE_API FString GetToolCallId() const;

	/// The context installed on this result, or null.
	UE_API const UE::AsyncFunctionResult::FAsyncFunctionResultContext* GetToolCallContext() const;

	/// The token installed on this result, or a null token.
	UE_API UE::AsyncFunctionResult::FCancellationToken GetCancellationToken() const;

	/// Whether cancellation has been requested for the call that produced this result.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	UE_API bool IsCanceled() const;

	/// Install this call's capture buffer for one step of the tool's work.
	///
	/// UE 5.8: there is no undo-capture provider in 5.8, so this opens a step only when a context
	/// with a capture buffer was installed and a provider is registered; otherwise it returns false
	/// and the work runs uncaptured, as 5.9 does without AsyncFunctionResultEditor.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	UE_API bool BeginCaptureStep();

	/// End the step BeginCaptureStep opened. Ending with no step open does nothing.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	UE_API void EndCaptureStep();

	// Get the JSON schema for an async result class' value.
	static UE_API TSharedRef<FJsonObject> GetValueJsonSchema(TSubclassOf<UAsyncFunctionResult> Class);

protected:
	/// UE 5.8: kept for subclasses that override it (UAsyncFunctionResultJson); 5.8's registry builds
	/// the value schema from the `Value` property itself and never calls this.
	UE_API virtual TSharedRef<FJsonObject> GetValueJsonSchemaImpl() const;

	/// Broadcast a successful completion while setting no value. For Blueprints, scripting
	/// subclasses and UAsyncFunctionResultVoid.
	UFUNCTION(BlueprintCallable, Category = "AsyncFunctionResult")
	UE_API bool MaybeBroadcastCompletionWithNoValue();

private:
	// The dispatching call's context. Written once on the game thread, then immutable.
	UE::AsyncFunctionResult::FAsyncFunctionResultContext Context;

	// Publishes Context.
	std::atomic<bool> bContextInstalled{false};

	// The open capture step, or null.
	TUniquePtr<UE::AsyncFunctionResult::ICaptureStep> CaptureStep;
};

#undef UE_API
