// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"
#include "Templates/UnrealTemplate.h"

#define UE_API TOOLSETREGISTRYEDITOR_API // UE 5.8: in the compat ToolsetRegistryEditor module, not ToolsetRegistry

namespace UE::ToolsetRegistry
{
	/// A toolset's schema as a parsed value, so the tools it declares can be inspected without
	/// callers parsing or reassembling JSON themselves. Text is produced only at a boundary that
	/// requires it.
	///
	/// An empty schema means the toolset has nothing to offer.
	struct FToolsetJsonSchema
	{
		FToolsetJsonSchema() = default;

		explicit FToolsetJsonSchema(TSharedRef<FJsonObject> InObject)
			: Object(MoveTemp(InObject))
		{
		}

		/// Whether this schema declares nothing.
		bool IsEmpty() const { return !Object.IsValid(); }

		/// The parsed schema, or null when empty.
		const TSharedPtr<FJsonObject>& GetJsonObject() const { return Object; }

		/// Serialises the schema for a transport that needs text. Empty when the schema is empty.
		UE_API FString ToJsonString() const;

		/// The toolset name this schema carries, as its producer wrote it. Normally the name the
		/// toolset is registered under, which for a manifest-baked toolset is "Module.Class". Empty
		/// when the producer wrote no name, which nothing currently forbids.
		UE_API FString GetToolsetName() const;

		/// The name of each tool this schema declares, as its producer wrote it. The baked and MCP
		/// toolsets qualify theirs as "ToolsetName.ToolName"; ECABridge does not, so a caller matching
		/// these against a qualified name cannot assume the prefix is present.
		UE_API TArray<FString> ListToolNames() const;

		/// The name of one entry from a schema's tools array. Unset when the entry is not an object or
		/// does not name itself, which a caller treats as a tool it cannot match.
		static UE_API TOptional<FString> TryGetToolName(const TSharedPtr<FJsonValue>& ToolValue);

	private:
		TSharedPtr<FJsonObject> Object;
	};

	/// Serialises schemas as one JSON array, for a transport that carries the whole set as text.
	/// Empty schemas are omitted.
	UE_API FString ToJsonArrayString(TArrayView<const FToolsetJsonSchema> Schemas);
}

#undef UE_API
