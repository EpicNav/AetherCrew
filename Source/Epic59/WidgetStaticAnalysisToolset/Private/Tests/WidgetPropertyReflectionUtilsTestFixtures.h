// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "UObject/Object.h"
#include "UObject/ObjectPtr.h"
#include "WidgetPropertyReflectionUtilsTestFixtures.generated.h"

/** Test object with a fixed set of properties, instead of a real widget class's. */
UCLASS()
class UWidgetPropertyReflectionUtilsTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Test")
	int32 NumericValue = 42;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString StringValue = TEXT("DefaultString");
};

/** Owns a default subobject whose NumericValue differs from the subobject class's default, so the
 *  subobject's archetype (the owner CDO's copy) is not the class default object. */
UCLASS()
class UWidgetPropertyReflectionUtilsTestOwner : public UObject
{
	GENERATED_BODY()

public:
	static constexpr int32 SubobjectNumericValue = 7;

	UWidgetPropertyReflectionUtilsTestOwner()
	{
		Child = CreateDefaultSubobject<UWidgetPropertyReflectionUtilsTestObject>(TEXT("Child"));
		Child->NumericValue = SubobjectNumericValue;
	}

	UPROPERTY(VisibleAnywhere, Category = "Test")
	TObjectPtr<UWidgetPropertyReflectionUtilsTestObject> Child;
};
