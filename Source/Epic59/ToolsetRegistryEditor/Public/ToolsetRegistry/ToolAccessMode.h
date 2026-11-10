// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

#include "ToolAccessMode.generated.h"

/// How much authority a tool exercises when invoked. Baked into each tool's schema as
/// annotations.accessMode and used by hosts to decide whether a call needs confirmation.
/// Set on a tool's UFunction via meta=(AIAccessMode="Read"|"Write"|"Execute"). Exposed to
/// Python as unreal.ToolAccessMode.
UENUM(BlueprintType)
enum class EToolAccessMode : uint8
{
	/// Only reads data within clear parameters;
	Read,
	/// Reads and writes within clear parameters.
	Write,
	/// Could do just about anything.
	Execute,
};

namespace UE::ToolsetRegistry
{
	/// Canonical wire strings emitted in annotations.accessMode, indexed by EToolAccessMode.
	/// Single source of truth for both conversion directions. EToolAccessMode::Execute must
	/// remain the last enumerator so the static_assert keeps this in sync with the enum.
	inline constexpr const TCHAR* GAccessModeWireStrings[] = {
		TEXT("read"),     // EToolAccessMode::Read
		TEXT("write"),    // EToolAccessMode::Write
		TEXT("execute"),  // EToolAccessMode::Execute
	};
	static_assert(UE_ARRAY_COUNT(GAccessModeWireStrings) == static_cast<int32>(EToolAccessMode::Execute) + 1,
		"GAccessModeWireStrings must have one entry per EToolAccessMode value, in declaration order.");

	/// Returns the canonical wire string for a mode ("read", "write", "execute").
	inline const TCHAR* ToAccessModeString(EToolAccessMode Mode)
	{
		return GAccessModeWireStrings[static_cast<int32>(Mode)];
	}

	/// Parses a wire string (case-insensitive) into a mode. Returns an unset optional when
	/// the value is not one of the recognized modes, leaving the caller to decide the fallback.
	inline TOptional<EToolAccessMode> ParseAccessMode(const FString& Value)
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(GAccessModeWireStrings); ++Index)
		{
			if (Value.Equals(GAccessModeWireStrings[Index], ESearchCase::IgnoreCase))
			{
				return static_cast<EToolAccessMode>(Index);
			}
		}
		return {};
	}
}
