// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Tests/AttributeSetToolsetTest59.h"

#include "AbilitySystemInspectorToolsetTest59.generated.h"

/// Minimal actor used in automation tests.
/// Owns an AbilitySystemComponent and the test attribute set.
UCLASS(Hidden, MinimalAPI)
class AGASToolsetsTestActor59 : public AActor, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AGASToolsetsTestActor59()
	{
		AbilitySystemComponent =
			CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	}

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override
	{
		return AbilitySystemComponent;
	}

	UPROPERTY()
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;
};
