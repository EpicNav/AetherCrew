// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Blueprint/UserWidget.h"
#include "MVVMViewModelBase.h"
#include "WidgetStaticAnalysisSchemaTestFixtures.generated.h"

/** Test-only concrete UUserWidget subclass (UUserWidget itself is abstract). Used to exercise the
 *  no-Blueprint-origin case, since it has a null ClassGeneratedBy. */
UCLASS()
class UWidgetStaticAnalysisTestNativeUserWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Test-only call target for the "SomeObject.SomeFunction(SomeObject)" pattern, where one Get
	 *  node feeds both the call's self pin and an ordinary argument pin. Exercises write-detection
	 *  dedup keyed on (node, pin) rather than node alone. */
	UFUNCTION(BlueprintCallable, Category = "Test")
	void RegisterSelfReference(UWidgetStaticAnalysisTestNativeUserWidget* SelfReference) {}
};

/** Test-only ViewModel used as the second hop of a multi-hop source path ("Inventory.Count"), so
 *  unconnected-detection tests can assert on how the *root* of such a path is classified. */
UCLASS()
class UWidgetConnectionMapperTestNestedViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, FieldNotify, Category = "Test")
	int32 Count = 0;
};

/** Test-only ViewModel with a known FieldNotify property and an Inventory field that roots a
 *  multi-hop source path. */
UCLASS()
class UWidgetConnectionMapperTestViewModel : public UMVVMViewModelBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintReadWrite, FieldNotify, Category = "Test")
	int32 TestField = 0;

	UPROPERTY(BlueprintReadWrite, FieldNotify, Category = "Test")
	TObjectPtr<UWidgetConnectionMapperTestNestedViewModel> Inventory;
};
