// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "CoreTypes.h"

class FProperty;
class UObject;

/** Generic FProperty reflection over any UObject, shared so all callers format values the same way. */
class FWidgetPropertyReflectionUtils
{
public:
	/** True if Property is edit-visible, non-transient, matches PropertyNameFilter (if set), and,
	 *  with bSkipDefaultValues, differs from its archetype. */
	static bool ShouldEmitProperty(const FProperty* Property, const UObject* Container,
		const FString& PropertyNameFilter = TEXT(""), bool bSkipDefaultValues = true);

	/** True if Property's current value on Container is identical to its archetype's. */
	static bool MatchesArchetype(const FProperty* Property, const UObject* Container);

	/** Exports a property's current value via ExportText; empty on failure. */
	static FString ExportPropertyValue(const FProperty* Property, UObject* Container);
};
