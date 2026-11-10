// Copyright Epic Games, Inc. All Rights Reserved.

#include "DataRegistryTools59.h"

#include "DataRegistry.h"
#include "DataRegistrySource_DataTable.h"
#include "DataRegistrySubsystem.h"
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "GameplayTagsManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StructUtils/InstancedStruct.h"
#include "Templates/UniquePtr.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(FDataRegistryToolsSpec59, "AIAssistant.Toolsets.DataRegistryToolset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	TUniquePtr<UE::ToolsetRegistry::FToolCallExceptionHandler> ExceptionHandler;

	// Each test registers its own registry, backed by an in-memory data table with known rows, so the
	// tests never depend on the registries (or their id formats) a project happens to ship.
	UPackage* FixturePackage = nullptr;
	UDataRegistry* FixtureRegistry = nullptr;
	UDataTable* FixtureTable = nullptr;
	// A row struct with fields, so the registry has a non-empty schema. Looked up by path because the struct's
	// StaticStruct() is not exported from GameplayTags.
	const UScriptStruct* FixtureRowStruct = nullptr;
	FString FixtureRegistryName;
	const TArray<FString> FixtureItemNames = { TEXT("ItemA"), TEXT("ItemB") };

	bool CreateFixtureRegistry();
	void DestroyFixtureRegistry();
	TSharedPtr<FJsonObject> ParseJson(const FString& JsonString);
	void ExpectExceptionContains(const FString& Fragment);

END_DEFINE_SPEC(FDataRegistryToolsSpec59)

bool FDataRegistryToolsSpec59::CreateFixtureRegistry()
{
	UDataRegistrySubsystem* Subsystem = UDataRegistrySubsystem::Get();
	if (!TestNotNull(TEXT("DataRegistrySubsystem"), Subsystem))
	{
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FixtureRegistryName = FString::Printf(TEXT("DataRegistryToolsSpec_%s"), *UniqueSuffix);
	FixturePackage = CreatePackage(*FString::Printf(TEXT("/Temp/DataRegistryToolsSpec_%s"), *UniqueSuffix));

	FixtureRowStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/GameplayTags.GameplayTagTableRow"));
	if (!TestNotNull(TEXT("GameplayTagTableRow struct"), FixtureRowStruct))
	{
		return false;
	}

	FixtureTable = NewObject<UDataTable>(FixturePackage, TEXT("DT_DataRegistryToolsSpec"), RF_Public | RF_Standalone);
	FixtureTable->RowStruct = const_cast<UScriptStruct*>(FixtureRowStruct);
	for (const FString& ItemName : FixtureItemNames)
	{
		const FGameplayTagTableRow Row(FName(*ItemName), FString::Printf(TEXT("%s comment"), *ItemName));
		FixtureTable->AddRow(FName(*ItemName), reinterpret_cast<const uint8*>(&Row), FixtureRowStruct);
	}

	FixtureRegistry = NewObject<UDataRegistry>(FixturePackage, TEXT("DR_DataRegistryToolsSpec"), RF_Public | RF_Standalone);

	// The registry's configuration is protected and edited through the details panel, so set it
	// through reflection the way the editor does.
	FNameProperty* RegistryTypeProperty = FindFProperty<FNameProperty>(UDataRegistry::StaticClass(), TEXT("RegistryType"));
	FObjectPropertyBase* ItemStructProperty = FindFProperty<FObjectPropertyBase>(UDataRegistry::StaticClass(), TEXT("ItemStruct"));
	FArrayProperty* DataSourcesProperty = FindFProperty<FArrayProperty>(UDataRegistry::StaticClass(), TEXT("DataSources"));
	if (!TestNotNull(TEXT("RegistryType property"), RegistryTypeProperty)
		|| !TestNotNull(TEXT("ItemStruct property"), ItemStructProperty)
		|| !TestNotNull(TEXT("DataSources property"), DataSourcesProperty))
	{
		return false;
	}
	RegistryTypeProperty->SetPropertyValue_InContainer(FixtureRegistry, FName(*FixtureRegistryName));
	ItemStructProperty->SetObjectPropertyValue_InContainer(FixtureRegistry, const_cast<UScriptStruct*>(FixtureRowStruct));

	UDataRegistrySource_DataTable* Source = NewObject<UDataRegistrySource_DataTable>(FixtureRegistry);
	Source->SetSourceTable(TSoftObjectPtr<UDataTable>(FixtureTable), FDataRegistrySource_DataTableRules());
	FScriptArrayHelper DataSources(DataSourcesProperty, DataSourcesProperty->ContainerPtrToValuePtr<void>(FixtureRegistry));
	const int32 SourceIndex = DataSources.AddValue();
	CastFieldChecked<FObjectPropertyBase>(DataSourcesProperty->Inner)->SetObjectPropertyValue(DataSources.GetRawPtr(SourceIndex), Source);

	Subsystem->LoadRegistryPath(FSoftObjectPath(FixtureRegistry));
	return TestNotNull(TEXT("Fixture registry is registered"), Subsystem->GetRegistryForType(FName(*FixtureRegistryName)));
}

void FDataRegistryToolsSpec59::DestroyFixtureRegistry()
{
	if (FixtureRegistry)
	{
		if (UDataRegistrySubsystem* Subsystem = UDataRegistrySubsystem::Get())
		{
			Subsystem->IgnoreRegistryPath(FSoftObjectPath(FixtureRegistry));
		}
	}
	for (UObject* Object : TArray<UObject*>({ FixtureRegistry, FixtureTable }))
	{
		if (Object)
		{
			Object->ClearFlags(RF_Public | RF_Standalone);
			Object->MarkAsGarbage();
		}
	}
	if (FixturePackage)
	{
		FixturePackage->MarkAsGarbage();
	}
	FixtureRegistry = nullptr;
	FixtureTable = nullptr;
	FixtureRowStruct = nullptr;
	FixturePackage = nullptr;
	FixtureRegistryName.Reset();
}

TSharedPtr<FJsonObject> FDataRegistryToolsSpec59::ParseJson(const FString& JsonString)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	FJsonSerializer::Deserialize(Reader, Root);
	return Root;
}

void FDataRegistryToolsSpec59::ExpectExceptionContains(const FString& Fragment)
{
	if (!TestTrue(TEXT("ExceptionHandler is valid"), ExceptionHandler.IsValid()))
	{
		return;
	}
	TestTrue(
		*FString::Printf(TEXT("Error contains '%s'"), *Fragment),
		ExceptionHandler->GetException().Contains(Fragment));
}

void FDataRegistryToolsSpec59::Define()
{
	BeforeEach([this]()
	{
		ExceptionHandler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
		CreateFixtureRegistry();
	});

	AfterEach([this]()
	{
		DestroyFixtureRegistry();
		ExceptionHandler.Reset();
	});

	Describe(TEXT("ListRegistries"), [this]()
	{
		It(TEXT("Includes the registered registry"), [this]()
		{
			const TArray<FString> Registries = UDataRegistryTools59::ListRegistries(nullptr);
			TestTrue(TEXT("Fixture registry is listed"), Registries.Contains(FixtureRegistryName));
		});

		It(TEXT("Each entry is a non-empty name"), [this]()
		{
			for (const FString& Name : UDataRegistryTools59::ListRegistries(nullptr))
			{
				TestFalse(TEXT("name is not empty"), Name.IsEmpty());
			}
		});

		It(TEXT("Filter narrows results to matching item_struct"), [this]()
		{
			const TArray<FString> AllNames = UDataRegistryTools59::ListRegistries(nullptr);
			const TArray<FString> Filtered = UDataRegistryTools59::ListRegistries(FTableRowBase::StaticStruct());
			TestTrue(TEXT("Filtered count is <= total"), Filtered.Num() <= AllNames.Num());
			TestTrue(TEXT("Filtered list includes the fixture registry"), Filtered.Contains(FixtureRegistryName));
			for (const FString& Name : Filtered)
			{
				const FDataRegistryInfo59 Info = UDataRegistryTools59::GetRegistryInfo(Name);
				if (TestNotNull(TEXT("Filtered entry has non-null item_struct"), Info.ItemStruct.Get()))
				{
					TestTrue(TEXT("Filtered item_struct matches filter"),
						Info.ItemStruct->IsChildOf(FTableRowBase::StaticStruct()));
				}
			}
		});
	});

	Describe(TEXT("GetRegistryInfo"), [this]()
	{
		It(TEXT("Describes the registry"), [this]()
		{
			const FDataRegistryInfo59 Info = UDataRegistryTools59::GetRegistryInfo(FixtureRegistryName);
			TestEqual(TEXT("registry_name matches"), Info.RegistryName, FixtureRegistryName);
			TestEqual(TEXT("item_count matches the table"), Info.ItemCount, FixtureItemNames.Num());
			TestTrue(TEXT("item_struct is the row struct"), Info.ItemStruct.Get() == FixtureRowStruct);
		});

		It(TEXT("Invalid registry raises script error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UDataRegistryTools59::GetRegistryInfo(TEXT("NonExistentRegistryType_XYZ"));
			});
			ExpectExceptionContains(TEXT("Registry 'NonExistentRegistryType_XYZ' not found"));
		});
	});

	Describe(TEXT("GetSchema"), [this]()
	{
		It(TEXT("Returns valid JSON"), [this]()
		{
			const FString Schema = UDataRegistryTools59::GetSchema(FixtureRegistryName);
			TestFalse(TEXT("Schema not empty"), Schema.IsEmpty());
			TestNotNull(TEXT("Schema parses as JSON"), ParseJson(Schema).Get());
			TestTrue(TEXT("Schema describes the Tag field"), Schema.Contains(TEXT("Tag")));
			TestTrue(TEXT("Schema describes the DevComment field"), Schema.Contains(TEXT("DevComment")));
		});

		It(TEXT("Invalid registry raises script error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UDataRegistryTools59::GetSchema(TEXT("NonExistentRegistryType_XYZ"));
			});
			ExpectExceptionContains(TEXT("Registry 'NonExistentRegistryType_XYZ' not found"));
		});
	});

	Describe(TEXT("ListItems"), [this]()
	{
		It(TEXT("Returns one entry per registry item"), [this]()
		{
			TArray<FString> Items = UDataRegistryTools59::ListItems(FixtureRegistryName);
			Items.Sort();
			TestTrue(*FString::Printf(TEXT("Lists exactly the table's rows (got [%s])"), *FString::Join(Items, TEXT(", "))), Items == FixtureItemNames);
			TestEqual(TEXT("count matches item_count"),
				Items.Num(), UDataRegistryTools59::GetRegistryInfo(FixtureRegistryName).ItemCount);
		});

		It(TEXT("Invalid registry raises script error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UDataRegistryTools59::ListItems(TEXT("NonExistentRegistryType_XYZ"));
			});
			ExpectExceptionContains(TEXT("Registry 'NonExistentRegistryType_XYZ' not found"));
		});
	});

	Describe(TEXT("GetItems"), [this]()
	{
		It(TEXT("Returns map keyed by item name"), [this]()
		{
			const TMap<FString, FInstancedStruct> Items =
				UDataRegistryTools59::GetItems(FixtureRegistryName, FixtureItemNames);
			TestEqual(TEXT("Every requested item is returned"), Items.Num(), FixtureItemNames.Num());
			for (const FString& ItemName : FixtureItemNames)
			{
				const FInstancedStruct* Found = Items.Find(ItemName);
				if (TestNotNull(*FString::Printf(TEXT("Map contains '%s'"), *ItemName), Found))
				{
					TestTrue(TEXT("Item has the row struct"), Found->GetScriptStruct() == FixtureRowStruct);
				}
			}
		});

		It(TEXT("Missing items are omitted from result"), [this]()
		{
			const TMap<FString, FInstancedStruct> Items = UDataRegistryTools59::GetItems(
				FixtureRegistryName, TArray<FString>({ FixtureItemNames[0], TEXT("NonExistentItem_XYZ_12345") }));
			TestEqual(TEXT("Only the existing item is returned"), Items.Num(), 1);
			TestTrue(TEXT("Existing item is present"), Items.Contains(FixtureItemNames[0]));
			TestFalse(TEXT("Missing name is absent from result"), Items.Contains(TEXT("NonExistentItem_XYZ_12345")));
		});

		It(TEXT("Empty names returns empty map"), [this]()
		{
			const TMap<FString, FInstancedStruct> Items =
				UDataRegistryTools59::GetItems(FixtureRegistryName, TArray<FString>());
			TestEqual(TEXT("Map is empty"), Items.Num(), 0);
		});

		It(TEXT("Invalid registry raises script error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UDataRegistryTools59::GetItems(
					TEXT("NonExistentRegistryType_XYZ"),
					TArray<FString>({ TEXT("SomeItem") }));
			});
			ExpectExceptionContains(TEXT("Registry 'NonExistentRegistryType_XYZ' not found"));
		});
	});

	Describe(TEXT("ListDataSources"), [this]()
	{
		It(TEXT("Lists the registry's data table source"), [this]()
		{
			const TArray<FDataRegistrySourceSummary59> Sources =
				UDataRegistryTools59::ListDataSources(FixtureRegistryName);
			TestEqual(TEXT("One data source"), Sources.Num(), 1);
		});

		It(TEXT("Invalid registry raises script error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UDataRegistryTools59::ListDataSources(TEXT("NonExistentRegistryType_XYZ"));
			});
			ExpectExceptionContains(TEXT("Registry 'NonExistentRegistryType_XYZ' not found"));
		});
	});

	Describe(TEXT("ListRuntimeSources"), [this]()
	{
		It(TEXT("Lists at least the data table source"), [this]()
		{
			const TArray<FDataRegistrySourceSummary59> Sources =
				UDataRegistryTools59::ListRuntimeSources(FixtureRegistryName);
			TestTrue(TEXT("At least one runtime source"), Sources.Num() >= 1);
		});

		It(TEXT("Invalid registry raises script error"), [this]()
		{
			ExceptionHandler->CaptureErrorsIn([]()
			{
				UDataRegistryTools59::ListRuntimeSources(TEXT("NonExistentRegistryType_XYZ"));
			});
			ExpectExceptionContains(TEXT("Registry 'NonExistentRegistryType_XYZ' not found"));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
