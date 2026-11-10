// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldConditionQueryConverter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "WorldConditionQuery.h"

namespace UE::WorldConditionsToolset
{
	FString FWorldConditionQueryConverter::GetName() const
	{
		return TEXT("WorldConditionQueryConverter59"); // UE 5.8: 5.8's WorldConditionsToolset registers "WorldConditionQueryConverter", and 5.8's registry verify()s that names are unique
	}

	bool FWorldConditionQueryConverter::CanConvertProperty(TNotNull<const FProperty*> Property)
	{
		const FStructProperty* const StructProp = CastField<FStructProperty>(static_cast<const FProperty*>(Property));
		return StructProp != nullptr
			&& StructProp->Struct == FWorldConditionQueryDefinition::StaticStruct();
	}

	TSharedPtr<FJsonObject> FWorldConditionQueryConverter::PropertyToJsonSchema(TNotNull<const FProperty*> Property)
	{
		TSharedPtr<FJsonObject> Schema = ToolsetStructToJsonSchema(FWorldConditionQueryDefinition::StaticStruct());
		if (Schema.IsValid())
		{
			Schema->SetStringField(TEXT("title"), TEXT("WorldConditionQueryDefinition"));
		}
		return Schema;
	}

	TSharedPtr<FJsonValue> FWorldConditionQueryConverter::PropertyToDefault(
		TNotNull<const FProperty*> Property, const FString& DefaultString)
	{
		return MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
	}

	TSharedPtr<FJsonValue> FWorldConditionQueryConverter::PropertyToJsonData(
		TNotNull<FProperty*> Property, const void* Value)
	{
		const FWorldConditionQueryDefinition* const QueryDef =
			static_cast<const FWorldConditionQueryDefinition*>(Value);
		if (QueryDef == nullptr)
		{
			return MakeShared<FJsonValueNull>();
		}

		// Serialize all UPROPERTYs via the ToolsetJson pipeline.
		// FInstancedStruct conditions get _structType automatically from JsonObjectConverter.
		TSharedPtr<FJsonObject> Result = ToolsetStructToJsonData(
			FWorldConditionQueryDefinition::StaticStruct(), QueryDef);
		if (!Result.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}

		Result->SetStringField(TEXT("description"), QueryDef->GetDescription().ToString());
		return MakeShared<FJsonValueObject>(Result);
	}

#if 0 // UE 5.8: 5.9 converter import API (FToolsetJsonImportResult, FToolsetJsonImportContext)
	UE::ToolsetRegistry::FToolsetJsonImportResult FWorldConditionQueryConverter::JsonDataToProperty(
		const TSharedPtr<FJsonValue>& JsonValue, TNotNull<FProperty*> Property, void* OutValue, const UE::ToolsetRegistry::FToolsetJsonImportContext& Context)
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Object)
		{
			return UE::ToolsetRegistry::FToolsetJsonImportResult::Rejected(FString::Printf(
				TEXT("Property '%s' takes a JSON object describing a world condition query"), *Property->GetName()));
		}

		FWorldConditionQueryDefinition* const QueryDef =
			static_cast<FWorldConditionQueryDefinition*>(OutValue);
		if (!ensure(QueryDef != nullptr))
		{
			return UE::ToolsetRegistry::FToolsetJsonImportResult::Rejected(FString::Printf(TEXT("No value to import property '%s' into"), *Property->GetName()));
		}

		// Deserialize all UPROPERTYs via the ToolsetJson pipeline.
		// FInstancedStruct conditions are initialized from _structType automatically by JsonObjectConverter.
		const TSharedPtr<FJsonObject> JsonObject = JsonValue->AsObject();
		FText FailReason;
		if (!ToolsetJsonDataToStruct(
				JsonObject.ToSharedRef(), FWorldConditionQueryDefinition::StaticStruct(), QueryDef,
				nullptr, nullptr, EJsonObjectImportFlags::None, &FailReason))
		{
			return UE::ToolsetRegistry::FToolsetJsonImportResult::Rejected(FailReason.ToString());
		}

		// Rebuild SharedDefinition from EditableConditions.
		QueryDef->Initialize(nullptr);
		return UE::ToolsetRegistry::FToolsetJsonImportResult::Imported();
	}
#else
	// UE 5.8: 5.8 converters return bool, so the failure reasons above are not reported
	bool FWorldConditionQueryConverter::JsonDataToProperty(
		const TSharedPtr<FJsonValue>& JsonValue, TNotNull<FProperty*> Property, void* OutValue, UObject* Outer)
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Object)
		{
			return false;
		}

		FWorldConditionQueryDefinition* const QueryDef =
			static_cast<FWorldConditionQueryDefinition*>(OutValue);
		if (!ensure(QueryDef != nullptr))
		{
			return false;
		}

		const TSharedPtr<FJsonObject> JsonObject = JsonValue->AsObject();
		if (!ToolsetJsonDataToStruct(
				JsonObject.ToSharedRef(), FWorldConditionQueryDefinition::StaticStruct(), QueryDef))
		{
			return false;
		}

		// Rebuild SharedDefinition from EditableConditions.
		QueryDef->Initialize(nullptr);
		return true;
	}
#endif
}
