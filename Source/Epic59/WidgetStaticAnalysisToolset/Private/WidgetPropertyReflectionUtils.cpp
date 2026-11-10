// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetPropertyReflectionUtils.h"
#include "UObject/UnrealType.h"

bool FWidgetPropertyReflectionUtils::ShouldEmitProperty(const FProperty* Property, const UObject* Container,
	const FString& PropertyNameFilter, bool bSkipDefaultValues)
{
	if (!Property ||
		!Property->HasAnyPropertyFlags(CPF_Edit) ||
		Property->HasAnyPropertyFlags(CPF_Transient) ||
		Property->HasAnyPropertyFlags(CPF_DisableEditOnTemplate))
	{
		return false;
	}

	if (!PropertyNameFilter.IsEmpty() && !Property->GetName().Contains(PropertyNameFilter))
	{
		return false;
	}

	if (bSkipDefaultValues && MatchesArchetype(Property, Container))
	{
		return false;
	}

	return true;
}

bool FWidgetPropertyReflectionUtils::MatchesArchetype(const FProperty* Property, const UObject* Container)
{
	if (!Property || !IsValid(Container))
	{
		return false;
	}

	const UObject* Archetype = Container->GetArchetype();

	if (!Archetype)
	{
		return false;
	}

	return Property->Identical_InContainer(Container, Archetype, 0, PPF_None);
}

FString FWidgetPropertyReflectionUtils::ExportPropertyValue(const FProperty* Property, UObject* Container)
{
	if (!Property || !IsValid(Container))
	{
		return FString();
	}

	// Container as Data and Delta forces a full export, not a diff.
	FString ValueStr;
	const bool bExported = Property->ExportText_InContainer(0, ValueStr, Container, Container,
		Container, PPF_None);

	return bExported ? ValueStr : FString();
}
