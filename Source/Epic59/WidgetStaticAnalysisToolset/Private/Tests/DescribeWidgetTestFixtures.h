// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "DescribeWidgetTestFixtures.generated.h"

/**
 * Test-only native C++ UUserWidget subclass declaring one UPROPERTY whose name
 * (InheritedText) a child WidgetBlueprint's own tree can reuse for a widget instance name --
 * gives DescribeWidgetTest a known, controlled parent class to exercise the bInherited=true path
 * (a widget-typed property on WidgetBlueprint->ParentClass with the widget's name) without
 * depending on any game-specific parent class.
 */
UCLASS(NotBlueprintable, HideDropdown, Hidden)
class UDescribeWidgetTestParentWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TObjectPtr<UTextBlock> InheritedText = nullptr;
};

/**
 * Test-only named-slot host with one "Content" slot. UUserWidget only lists the slots of a compiled
 * Widget Blueprint class, so this overrides the lookup to avoid compiling one in the test.
 */
UCLASS(NotBlueprintable, HideDropdown, Hidden)
class UDescribeWidgetTestSlotHostWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void GetSlotNames(TArray<FName>& SlotNames) const override
	{
		SlotNames.Add(FName("Content"));
	}

	virtual UWidget* GetContentForSlot(FName SlotName) const override
	{
		return SlotName == FName("Content") ? SlotContent.Get() : nullptr;
	}

	UPROPERTY()
	TObjectPtr<UWidget> SlotContent = nullptr;
};
