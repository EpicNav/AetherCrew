// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "GpuProfilerToolset.h"
#include "Misc/AutomationTest.h"

BEGIN_DEFINE_SPEC(FGpuProfileResultSchemaSpec,
	"AIAssistant.Toolsets.PerfToolset.GpuProfileResultSchema",
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

	TArray<FString> RequiredFields(const TSharedPtr<FJsonObject>& Obj)
	{
		TArray<FString> Out;
		if (Obj.IsValid()) Obj->TryGetStringArrayField(TEXT("required"), Out);
		return Out;
	}

	TSharedPtr<FJsonObject> NodeSchema()
	{
		TSharedRef<FJsonObject> Schema = GetDefault<UGpuProfileResult>()->MakeJsonSchema();
		return SubObject(SubObject(Schema, TEXT("definitions")), TEXT("Node"));
	}

END_DEFINE_SPEC(FGpuProfileResultSchemaSpec)

void FGpuProfileResultSchemaSpec::Define()
{
	It("preserves the recursive Children $ref", [this]()
	{
		TSharedPtr<FJsonObject> Children = SubObject(SubObject(NodeSchema(), TEXT("properties")), TEXT("Children"));
		TSharedPtr<FJsonObject> ChildItems = SubObject(Children, TEXT("items"));
		TestEqual(TEXT("Children.items.$ref"), StringField(ChildItems, TEXT("$ref")), TEXT("#/definitions/Node"));
	});

	It("requires Name and InclusiveMs only", [this]()
	{
		TArray<FString> Required = RequiredFields(NodeSchema());
		TestTrue(TEXT("requires Name"), Required.Contains(TEXT("Name")));
		TestTrue(TEXT("requires InclusiveMs"), Required.Contains(TEXT("InclusiveMs")));
		TestFalse(TEXT("does not require ExclusiveMs"), Required.Contains(TEXT("ExclusiveMs")));
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
