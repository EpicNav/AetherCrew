// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "Kismet/KismetSystemLibrary.h"

namespace UE::ToolsetRegistry
{
	// UE 5.8: 5.9 exports this from ToolsetRegistry; here it is inline and editor-only, the case 5.9
	// routes to UKismetSystemLibrary.
	inline void RaiseScriptError(const FString& ErrorMessage = FString(TEXT("An error occurred")))
	{
		UKismetSystemLibrary::RaiseScriptError(ErrorMessage);
	}
}
