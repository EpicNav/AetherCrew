// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"

namespace UE::PerfToolset
{
	// Default JSON number serialization is %.17g, which produces noise like
	// "16.500000076293945" for values that read as "16.5" — wasting tokens
	// when the schema is consumed by an LLM. Pre-format with 4 significant
	// digits (no trailing zeros, no scientific for ms-range values) and
	// emit raw via FJsonValueNumberString.
	inline TSharedRef<FJsonValueNumberString> MakeMsValue(double Ms)
	{
		return MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%.4g"), Ms));
	}
}
