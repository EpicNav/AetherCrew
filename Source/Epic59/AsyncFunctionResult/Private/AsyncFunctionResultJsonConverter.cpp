// Copyright Epic Games, Inc. All Rights Reserved.

#include "AsyncFunctionResult/AsyncFunctionResultJsonConverter.h"

FName UE::AsyncFunctionResult::IAsyncFunctionResultJsonConverter::GetModularFeatureName()
{
	static const FName FeatureName(TEXT("AsyncFunctionResultJsonConverter"));
	return FeatureName;
}
