// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: the parts of UE 5.9's AsyncFunctionResult.cpp that 5.8's UToolCallAsyncResult base lacks.
// Completion, errors and broadcasting are 5.8's own (inherited); see AsyncFunctionResult.h.

#include "AsyncFunctionResult/AsyncFunctionResult.h"

#include "CoreGlobals.h"
#include "Features/IModularFeatures.h"
#include "JsonSchema/JsonSchemaGenerator.h"
#include "Misc/AssertionMacros.h"
#include "Tasks/Task.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "AsyncFunctionResult/AsyncFunctionResultJsonConverter.h"
#include "AsyncFunctionResultLog.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AsyncFunctionResult)

DEFINE_LOG_CATEGORY(LogAsyncFunctionResult);

namespace UE::AsyncFunctionResult::Private
{
	static const TCHAR* const ValuePropertyName = TEXT("Value");

	// Schema used when a class has no Value property, meaning it always completes with no
	// value (analogous to a void return).
	TSharedRef<FJsonObject> MakeNullValueJsonSchema()
	{
		TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("description"), TEXT("Always null"));
		Schema->SetStringField(TEXT("type"), TEXT("null"));
		return Schema;
	}
}

TSharedPtr<FJsonValue> UAsyncFunctionResult::GetValueAsJson() const
{
	if (!bIsComplete || !Error.IsEmpty())
	{
		return TSharedPtr<FJsonValue>();
	}
	FProperty* Property = GetClass()->FindPropertyByName(UE::AsyncFunctionResult::Private::ValuePropertyName);
	if (!Property)
	{
		return MakeShared<FJsonValueNull>();
	}

	IModularFeatures& ModularFeatures = IModularFeatures::Get();
	const FName FeatureName = UE::AsyncFunctionResult::IAsyncFunctionResultJsonConverter::GetModularFeatureName();
	if (ModularFeatures.IsModularFeatureAvailable(FeatureName))
	{
		return ModularFeatures.GetModularFeature<UE::AsyncFunctionResult::IAsyncFunctionResultJsonConverter>(FeatureName)
			.PropertyToJsonData(Property, Property->ContainerPtrToValuePtr<void>(this));
	}
	// The 5.8 base converts the Value property with the registry's own converters.
	return Super::GetValueAsJson();
}

void UAsyncFunctionResult::SetToolCallContext(
	TOptional<UE::AsyncFunctionResult::FAsyncFunctionResultContext> InContext)
{
	check(IsInGameThread());
	if (!InContext.IsSet())
	{
		return;
	}
	if (bContextInstalled.load(std::memory_order_relaxed))
	{
		UE_LOGF(
			LogAsyncFunctionResult, Error,
			"'%ls' already has a context installed, ignoring the second",
			*GetClass()->GetName());
		return;
	}
	Context = MoveTemp(*InContext);
	bContextInstalled.store(true, std::memory_order_release);
}

FString UAsyncFunctionResult::GetToolCallId() const
{
	const UE::AsyncFunctionResult::FAsyncFunctionResultContext* InstalledContext = GetToolCallContext();
	return InstalledContext ? InstalledContext->ToolCallId : FString();
}

const UE::AsyncFunctionResult::FAsyncFunctionResultContext* UAsyncFunctionResult::GetToolCallContext() const
{
	return bContextInstalled.load(std::memory_order_acquire) ? &Context : nullptr;
}

UE::AsyncFunctionResult::FCancellationToken UAsyncFunctionResult::GetCancellationToken() const
{
	const UE::AsyncFunctionResult::FAsyncFunctionResultContext* InstalledContext = GetToolCallContext();
	return InstalledContext
		? InstalledContext->CancellationToken
		: UE::AsyncFunctionResult::FCancellationToken();
}

bool UAsyncFunctionResult::IsCanceled() const
{
	const UE::AsyncFunctionResult::FAsyncFunctionResultContext* InstalledContext = GetToolCallContext();
	return InstalledContext
		&& InstalledContext->CancellationToken.IsValid()
		&& InstalledContext->CancellationToken->IsCanceled();
}

bool UAsyncFunctionResult::BeginCaptureStep()
{
	check(IsInGameThread());
	if (CaptureStep.IsValid())
	{
		return false;
	}
	const UE::AsyncFunctionResult::FAsyncFunctionResultContext* InstalledContext = GetToolCallContext();
	if (!InstalledContext)
	{
		// UE 5.8: the 5.8 registry installs no context, so this is the normal path; stay quiet.
		return false;
	}
	CaptureStep = UE::AsyncFunctionResult::BeginCaptureStep(InstalledContext->CaptureBuffer.Get());
	return CaptureStep.IsValid();
}

void UAsyncFunctionResult::EndCaptureStep()
{
	check(IsInGameThread());
	CaptureStep.Reset();
}

TSharedRef<FJsonObject> UAsyncFunctionResult::GetValueJsonSchemaImpl() const
{
	using namespace UE::AsyncFunctionResult;
	FProperty* Property = GetClass()->FindPropertyByName(Private::ValuePropertyName);
	if (!Property)
	{
		return Private::MakeNullValueJsonSchema();
	}

	IModularFeatures& ModularFeatures = IModularFeatures::Get();
	const FName FeatureName = IAsyncFunctionResultJsonConverter::GetModularFeatureName();
	if (ModularFeatures.IsModularFeatureAvailable(FeatureName))
	{
		return ModularFeatures.GetModularFeature<IAsyncFunctionResultJsonConverter>(FeatureName)
			.PropertyToJsonSchema(Property);
	}
	TSharedPtr<FJsonObject> Schema = FJsonSchemaGenerator::FPropertyToJsonSchemaObject(Property);
	check(Schema.IsValid());
	return Schema.ToSharedRef();
}

TSharedRef<FJsonObject> UAsyncFunctionResult::GetValueJsonSchema(TSubclassOf<UAsyncFunctionResult> Class)
{
	check(Class);
	return Class->GetDefaultObject<UAsyncFunctionResult>()->GetValueJsonSchemaImpl();
}

bool UAsyncFunctionResult::MaybeBroadcastCompletionWithNoValue()
{
	return MaybeBroadcastSuccessfulCompletion();
}
