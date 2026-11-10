// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AsyncFunctionResult/AsyncFunctionResult.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"

#include "AsyncFunctionResultJson.generated.h"

// Async function result whose value is an arbitrary JSON value.
// Subclasses describe the concrete shape via MakeJsonSchema().
UCLASS(BlueprintType, Abstract, MinimalAPI)
class UAsyncFunctionResultJson : public UAsyncFunctionResult
{
	GENERATED_BODY()

public:
	bool SetValue(TSharedPtr<FJsonValue> InValue)
	{
		return MaybeBroadcastSuccessfulCompletion(MoveTemp(InValue), Value);
	}

	virtual TSharedPtr<FJsonValue> GetValueAsJson() const override
	{
		if (!bIsComplete || !Error.IsEmpty())
		{
			return TSharedPtr<FJsonValue>();
		}
		return Value;
	}

protected:
	virtual TSharedRef<FJsonObject> GetValueJsonSchemaImpl() const final override
	{
		return MakeJsonSchema();
	}

	virtual TSharedRef<FJsonObject> MakeJsonSchema() const
		PURE_VIRTUAL(UAsyncFunctionResultJson::MakeJsonSchema,
			return MakeShared<FJsonObject>(); );

private:
	// Intentionally not a UPROPERTY: TSharedPtr<FJsonValue> isn't reflectable.
	TSharedPtr<FJsonValue> Value;
};
