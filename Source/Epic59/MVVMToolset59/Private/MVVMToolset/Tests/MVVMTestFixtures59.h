// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MVVMViewModelBase.h"
#include "MVVMTestFixtures59.generated.h"

/**
 * Minimal ViewModel for use as a custom parent class in CreateViewModel tests.
 * Lets tests verify that CreateViewModel respects a non-default parent class
 * without depending on any game-specific Blueprint assets.
 */
UCLASS(Transient, Hidden)
class UMVVMTestBaseViewModel59 : public UMVVMViewModelBase
{
	GENERATED_BODY()
};

/**
 * ViewModel exposing one plain (non-notify) property, one notify-capable property, and one
 * genuinely write-only property, for testing MVVM bindable-field enumeration
 * (UMVVMToolset59::ListClassBindableFields / ListPropertyPathBindableFields).
 */
UCLASS(Transient, Hidden)
class UMVVMTestBindableFieldsViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	/** Plain, non-notify, readable+writable field. */
	UPROPERTY(BlueprintReadWrite, Category = "Test")
	float PlainFloat = 0.f;

	/** Notify-capable, readable+writable field. */
	UPROPERTY(BlueprintReadWrite, FieldNotify, Category = "Test")
	FText NotifyText;

	/** Genuinely write-only field: a setter-only UFUNCTION with no paired getter. */
	UFUNCTION(BlueprintCallable, Category = "Test")
	void SetWriteOnlyBool(bool bNewValue)
	{
		bWriteOnlyBoolValue = bNewValue;
	}

protected:
	/** Native protected field. */
	UPROPERTY(BlueprintReadWrite, Category = "Test")
	float ProtectedFloat = 0.f;

private:
	bool bWriteOnlyBoolValue = false;

	/** Native private field. */
	UPROPERTY(BlueprintReadWrite, Category = "Test", meta = (AllowPrivateAccess = "true"))
	float PrivateFloat = 0.f;
};
