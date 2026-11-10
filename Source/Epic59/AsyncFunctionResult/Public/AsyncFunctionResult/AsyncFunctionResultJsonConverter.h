// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Features/IModularFeature.h"
#include "Misc/NotNull.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class FJsonObject;
class FJsonValue;
class FProperty;

#define UE_API ASYNCFUNCTIONRESULT_API

namespace UE::AsyncFunctionResult
{
	/// Extension point letting a plugin supply custom JSON conversion for a UAsyncFunctionResult
	/// subclass' Value property, in place of the plain JsonUtilities conversion.
	///
	/// At most one implementation should be registered at a time; UAsyncFunctionResult uses the
	/// first one available and falls back to plain conversion when none is registered.
	class IAsyncFunctionResultJsonConverter : public IModularFeature
	{
	public:
		virtual ~IAsyncFunctionResultJsonConverter() = default;

		static UE_API FName GetModularFeatureName();

		/// Converts a Value property to JSON schema.
		virtual TSharedRef<FJsonObject> PropertyToJsonSchema(
			TNotNull<const FProperty*> Property) const = 0;

		/// Converts a Value property's data on an instance to JSON.
		virtual TSharedPtr<FJsonValue> PropertyToJsonData(
			TNotNull<FProperty*> Property, TNotNull<const void*> Value) const = 0;
	};
}

#undef UE_API
