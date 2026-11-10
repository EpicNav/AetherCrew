// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include "ConfigSettingsToolsetTestObjects59.generated.h"

USTRUCT()
struct FConfigSettingsToolsetTestStruct
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 First = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Second = 0;
};

/**
 * Settings object used exclusively by ConfigSettingsToolsetTest.
 * Uses config=ConfigSettingsToolsetTest (NOT DefaultConfig) so SaveConfig()
 * writes to Saved/Config/ — a directory that is writable on all machines
 * including Horde build agents.
 */
UCLASS(config=ConfigSettingsToolsetTest)
class UConfigSettingsToolsetTestSettings59 : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(config, EditAnywhere, Category = "Test")
	bool bTestBoolProperty = false;

	UPROPERTY(config, EditAnywhere, Category = "Test")
	FConfigSettingsToolsetTestStruct TestStruct;

	/** TestStruct.First as of each change notification for TestStruct, standing in for runtime state a setting drives. */
	TArray<int32> NotifiedTestStructFirst;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
	{
		Super::PostEditChangeProperty(PropertyChangedEvent);
		if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UConfigSettingsToolsetTestSettings59, TestStruct))
		{
			NotifiedTestStructFirst.Add(TestStruct.First);
		}
	}
#endif
};
