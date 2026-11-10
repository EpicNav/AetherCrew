// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Blueprint/UserWidget.h"
#include "WidgetConnectionMapperTestFixtures.generated.h"

/** Struct with no conversion function to bridge to it, so bindings targeting it are Unbridgeable.
 *  A struct rather than a UObject because object pin compatibility is too permissive. */
USTRUCT(BlueprintType)
struct FWidgetConnectionMapperTestExoticStruct
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Dummy = 0;
};

/** Test-only widget exposing MVVM destination properties for the misconfiguration-detection tests:
 *  IntDestination is type-compatible with TestField (Connected case); TextDestination is
 *  mismatched but has a real conversion function (Fixable); ExoticDestination is mismatched with
 *  no conversion function anywhere (Unbridgeable); ReadOnlyIntDestination is type-compatible but
 *  BlueprintReadOnly, which IsBindingValid rejects outright regardless of type (Unknown). */
UCLASS()
class UWidgetConnectionMapperTestWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, Category = "Test")
	int32 IntDestination = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Test")
	FText TextDestination;

	UPROPERTY(BlueprintReadWrite, Category = "Test")
	FWidgetConnectionMapperTestExoticStruct ExoticDestination;

	UPROPERTY(BlueprintReadOnly, Category = "Test")
	int32 ReadOnlyIntDestination = 0;
};
