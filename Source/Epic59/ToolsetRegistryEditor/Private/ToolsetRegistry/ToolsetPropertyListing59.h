// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: the parts of UE 5.9's private ToolsetRegistry/ToolsetPropertyListing.h that back
// FToolsetLibrary59::ListObjectPropertyNames and the RootPath/MaxDepth form of ListStructProperties.
// Adapted from 5.9 (see the .cpp for each difference).

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/ValueOrError.h"

namespace UE::ToolsetRegistry::Epic59::PropertyListing
{
	/** The JSON key a property is known by (its authored name in standardized case). */
	FString ToWireKey(const FProperty* Property);

	/** Whether a listing may name Property (not deprecated; when user-visible only, visible and readable). */
	bool IsPropertyListable(const UStruct* Owner, const FProperty* Property, bool bUserVisiblePropertiesOnly);

	/** One path line per property reached below RootPath of Struct (Container is its memory, or null). */
	TValueOrError<TArray<FString>, FString> RenderInstanceNames(
		const UStruct* Struct, const void* Container, bool bUserVisiblePropertiesOnly,
		const FString& RootPath, const FString& NameFilter, int32 MaxDepth);

	/** Narrows a generated schema's "properties" object to RootPath and prunes below MaxDepth. */
	TValueOrError<TSharedPtr<FJsonObject>, FString> ExtractSchemaSubtree(
		const TSharedRef<FJsonObject>& PropertiesSchema, const UStruct* Struct,
		const FString& RootPath, int32 MaxDepth);
}
