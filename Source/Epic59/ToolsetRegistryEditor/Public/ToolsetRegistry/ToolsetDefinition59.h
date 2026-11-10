// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: what UE 5.9's ToolsetRegistry/ToolsetDefinition.h adds over 5.8's. Ported toolsets include
// this in place of "ToolsetRegistry/ToolsetDefinition.h" (Tools/Epic59/port_toolset.py rewrites the
// include). 5.8's UToolsetDefinition is not an IToolsetProvider, so ported toolsets list
// IToolsetProvider as a second base to keep their 5.9 hook overrides (IsToolsetHidden, GetToolsToHide).
// 5.8's registry never calls those hooks; see Source/Epic59/PORTING.md.

#pragma once

#include "CoreMinimal.h"
#include "Templates/ValueOrError.h"

#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetCore/ToolsetProvider.h"
#include "ToolsetRegistry/ToolAccessMode.h"
#include "ToolsetRegistry/ToolsetPermissionProvider.h"

#define UE_API TOOLSETREGISTRYEDITOR_API

namespace UE::ToolsetRegistry
{
	// Name of the argument that names the object a tool runs on (see IToolsetProvider::UsesInstanceArgument).
	// UE 5.8: the 5.8 registry only dispatches static tools, so nothing passes this argument.
	inline constexpr const TCHAR* InstanceArgumentName = TEXT("instance");

	// Whether Function is a non-static UFunction on a class that opts into
	// IToolsetProvider::UsesInstanceArgument.
	UE_API bool TakesInstanceArgument(const UFunction* Function);

#if WITH_METADATA
	// Whether the function is tagged AICallable or AIRuntimeCallable.
	UE_API bool IsFunctionMarkedAsTool(const TObjectPtr<const UFunction>& Function);

	// 5.8's validation (static, AICallable, AIIgnore), under 5.9's name.
	UE_API TValueOrError<bool, FString> IsFunctionAICallable(const TObjectPtr<const UFunction>& Function);

	// Whether the function is tagged AIRuntimeCallable.
	UE_API bool IsFunctionAIRuntimeCallable(const TObjectPtr<const UFunction>& Function);

	// Returns the access mode declared by meta=(AIAccessMode="Read"|"Write"|"Execute"), Execute by default.
	UE_API EToolAccessMode GetFunctionAccessMode(const TObjectPtr<const UFunction>& Function);
#endif
}

#undef UE_API
