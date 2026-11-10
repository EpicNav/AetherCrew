// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "MVVMEditorSubsystem.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace MVVMToolSetTest
{
	static const auto Flags =
		EAutomationTestFlags::EditorContext |
		EAutomationTestFlags::ProductFilter |
		EAutomationTestFlags::CriticalPriority;

	inline UWidgetBlueprint* CreateTestWidgetBlueprintWithMVVM(const FString& Folder, const FString& Name)
	{
		const FString FullPackageName = Folder / Name;
		UPackage* Package = CreatePackage(*FullPackageName);
		if (!IsValid(Package))
		{
			return nullptr;
		}

		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->ParentClass = UUserWidget::StaticClass();
		UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Factory->FactoryCreateNew(UWidgetBlueprint::StaticClass(), Package, FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
		if (!IsValid(WidgetBlueprint))
		{
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(WidgetBlueprint);
		return WidgetBlueprint;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
