// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "WidgetPropertyReflectionUtils.h"
#include "WidgetPropertyReflectionUtilsTestFixtures.h"
#include "WidgetStaticAnalysisSchemaTestFlags.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

BEGIN_DEFINE_SPEC(
	FWidgetPropertyReflectionUtilsTest,
	"AIAssistant.Toolsets.WidgetStaticAnalysisToolset.PropertyReflectionUtils",
	WidgetStaticAnalysisSchemaTest::Flags)

UWidgetPropertyReflectionUtilsTestObject* TestObject = nullptr;

const FProperty* FindTestProperty(const TCHAR* PropertyName);

END_DEFINE_SPEC(FWidgetPropertyReflectionUtilsTest)

const FProperty* FWidgetPropertyReflectionUtilsTest::FindTestProperty(const TCHAR* PropertyName)
{
	return TestObject ? TestObject->GetClass()->FindPropertyByName(FName(PropertyName)) : nullptr;
}

void FWidgetPropertyReflectionUtilsTest::Define()
{
	BeforeEach([this]()
	{
		TestObject = NewObject<UWidgetPropertyReflectionUtilsTestObject>(GetTransientPackage());
	});

	AfterEach([this]()
	{
		TestObject = nullptr;
	});

	Describe("ExportPropertyValue", [this]()
	{
		It("exports the full value even when it equals the class default", [this]()
		{
			const FProperty* Property = FindTestProperty(TEXT("NumericValue"));
			TestNotNull(TEXT("NumericValue property found"), Property);

			const FString Value = FWidgetPropertyReflectionUtils::ExportPropertyValue(Property, TestObject);
			TestTrue(TEXT("exported text contains the default numeric value"), Value.Contains(TEXT("42")));
		});
	});

	Describe("MatchesArchetype", [this]()
	{
		It("compares against the object's archetype, not the class default", [this]()
		{
			const FProperty* Property = FindTestProperty(TEXT("NumericValue"));
			TestNotNull(TEXT("NumericValue property found"), Property);

			const UWidgetPropertyReflectionUtilsTestOwner* Owner = NewObject<UWidgetPropertyReflectionUtilsTestOwner>(GetTransientPackage());
			const UWidgetPropertyReflectionUtilsTestObject* Child = Owner->Child;
			TestNotNull(TEXT("owner created its subobject"), Child);
			TestNotEqual(TEXT("subobject archetype is not the class default object"),
				Child->GetArchetype(), Child->GetClass()->GetDefaultObject());

			TestTrue(TEXT("subobject matches its archetype's non-default value"),
				FWidgetPropertyReflectionUtils::MatchesArchetype(Property, Child));
		});
	});

	Describe("ShouldEmitProperty", [this]()
	{
		It("omits a property left at its archetype value when bSkipDefaultValues=true", [this]()
		{
			const FProperty* Property = FindTestProperty(TEXT("NumericValue"));
			TestNotNull(TEXT("NumericValue property found"), Property);

			TestFalse(TEXT("default-valued property omitted"),
				FWidgetPropertyReflectionUtils::ShouldEmitProperty(Property, TestObject, TEXT(""), /*bSkipDefaultValues=*/true));
			TestTrue(TEXT("same property included when bSkipDefaultValues=false"),
				FWidgetPropertyReflectionUtils::ShouldEmitProperty(Property, TestObject, TEXT(""), /*bSkipDefaultValues=*/false));
		});

		It("matches PropertyNameFilter as a substring", [this]()
		{
			const FProperty* NumericProperty = FindTestProperty(TEXT("NumericValue"));
			const FProperty* StringProperty = FindTestProperty(TEXT("StringValue"));
			TestNotNull(TEXT("NumericValue property found"), NumericProperty);
			TestNotNull(TEXT("StringValue property found"), StringProperty);

			TestTrue(TEXT("NumericValue matches 'Numeric'"),
				FWidgetPropertyReflectionUtils::ShouldEmitProperty(NumericProperty, TestObject, TEXT("Numeric"), false));
			TestFalse(TEXT("StringValue does not match 'Numeric'"),
				FWidgetPropertyReflectionUtils::ShouldEmitProperty(StringProperty, TestObject, TEXT("Numeric"), false));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
