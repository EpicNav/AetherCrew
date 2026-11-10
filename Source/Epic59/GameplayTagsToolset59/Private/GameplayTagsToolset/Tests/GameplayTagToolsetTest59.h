// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "GameplayTagToolsetTest59.generated.h"

// Minimal test asset that holds a gameplay tag container for searchable name tests.
UCLASS(Hidden)
class UGameplayTagTestAsset59 : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FGameplayTagContainer Tags;
};
