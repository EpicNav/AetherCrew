// Copyright Epic Games, Inc. All Rights Reserved.

#include "ToolsetRegistry/ToolsetJsonSchema.h"

#include "Containers/StringView.h"
#include "Dom/JsonValue.h"
#include "Misc/Optional.h"

#include "Serialization/JsonSerializer.h" // UE 5.8: 5.9's private JsonConversion.h is not reachable from here

namespace UE::ToolsetRegistry
{
	// UE 5.8: local stand-in for 5.9's Internal::JsonToString (condensed JSON text).
	namespace
	{
		FString JsonToString(const TSharedRef<FJsonObject>& Object)
		{
			FString Out;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object, Writer);
			return Out;
		}
	}

	TOptional<FString> FToolsetJsonSchema::TryGetToolName(const TSharedPtr<FJsonValue>& ToolValue)
	{
		const TSharedPtr<FJsonObject>* ToolObject = nullptr;
		FString Name;
		if (ToolValue->TryGetObject(ToolObject) &&
			(*ToolObject)->TryGetStringField(TEXT("name"), Name))
		{
			return Name;
		}
		return {};
	}

	FString FToolsetJsonSchema::ToJsonString() const
	{
		if (!Object.IsValid())
		{
			return FString();
		}
		return JsonToString(Object.ToSharedRef()); // UE 5.8: local JsonToString
	}

	FString FToolsetJsonSchema::GetToolsetName() const
	{
		FString Name;
		if (Object.IsValid())
		{
			Object->TryGetStringField(TEXTVIEW("name"), Name);
		}
		return Name;
	}

	FString ToJsonArrayString(TArrayView<const FToolsetJsonSchema> Schemas)
	{
		TArray<FString> Serialised;
		Serialised.Reserve(Schemas.Num());
		for (const FToolsetJsonSchema& Schema : Schemas)
		{
			if (!Schema.IsEmpty())
			{
				Serialised.Add(Schema.ToJsonString());
			}
		}
		return TEXT("[") + FString::Join(Serialised, TEXT(",")) + TEXT("]");
	}

	TArray<FString> FToolsetJsonSchema::ListToolNames() const
	{
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("tools"), Tools))
		{
			return {};
		}

		TArray<FString> ToolNames;
		ToolNames.Reserve(Tools->Num());
		for (const TSharedPtr<FJsonValue>& ToolValue : *Tools)
		{
			if (TOptional<FString> Name = TryGetToolName(ToolValue))
			{
				ToolNames.Add(Name.GetValue());
			}
		}
		return ToolNames;
	}
}
