// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "StatsToolset.h"

BEGIN_DEFINE_SPEC(FStatCountersResultSchemaSpec,
	"AIAssistant.Toolsets.PerfToolset.StatCountersResultSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	TSharedPtr<FJsonObject> SubObject(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Out = nullptr;
		return (Obj.IsValid() && Obj->TryGetObjectField(Field, Out)) ? *Out : nullptr;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString Out;
		if (Obj.IsValid()) Obj->TryGetStringField(Field, Out);
		return Out;
	}

	TArray<FString> StringArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		TArray<FString> Out;
		if (Obj.IsValid()) Obj->TryGetStringArrayField(Field, Out);
		return Out;
	}

	TSharedPtr<FJsonObject> RowSchema()
	{
		TSharedRef<FJsonObject> Schema = GetDefault<UStatCountersResult>()->MakeJsonSchema();
		return SubObject(Schema, TEXT("items"));
	}

END_DEFINE_SPEC(FStatCountersResultSchemaSpec)

void FStatCountersResultSchemaSpec::Define()
{
	It("emits an array of objects", [this]()
	{
		TSharedRef<FJsonObject> Schema = GetDefault<UStatCountersResult>()->MakeJsonSchema();
		TestEqual(TEXT("root type"), StringField(Schema, TEXT("type")), TEXT("array"));
		TestEqual(TEXT("items type"), StringField(RowSchema(), TEXT("type")), TEXT("object"));
	});

	It("requires Name, Value and Unit on each row", [this]()
	{
		TArray<FString> Required = StringArrayField(RowSchema(), TEXT("required"));
		TestTrue(TEXT("requires Name"), Required.Contains(TEXT("Name")));
		TestTrue(TEXT("requires Value"), Required.Contains(TEXT("Value")));
		TestTrue(TEXT("requires Unit"), Required.Contains(TEXT("Unit")));
	});

	It("constrains Unit to count/bytes/float", [this]()
	{
		TSharedPtr<FJsonObject> UnitProp = SubObject(SubObject(RowSchema(), TEXT("properties")), TEXT("Unit"));
		TArray<FString> Allowed = StringArrayField(UnitProp, TEXT("enum"));
		TestTrue(TEXT("allows count"), Allowed.Contains(TEXT("count")));
		TestTrue(TEXT("allows bytes"), Allowed.Contains(TEXT("bytes")));
		TestTrue(TEXT("allows float"), Allowed.Contains(TEXT("float")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
