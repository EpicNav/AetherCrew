// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorSubsystem.h"

namespace WidgetStaticAnalysisSchemaTest
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext |
		EAutomationTestFlags::ProductFilter |
		EAutomationTestFlags::CriticalPriority;

	/** Name of the folder that specs using FTestAssetSandbox create, and of the TestMountPoint root. */
	constexpr const TCHAR* SandboxName = TEXT("WidgetStaticAnalysisSchemaTest");

	/** Shared mount point for transient test assets. Must be a single-segment root: a nested path
	 *  like "/Automation/Foo/" makes AssetRegistry's mount-liveness check treat discovered assets
	 *  as unmounted and silently drop them. */
	static const FString TestMountPoint = FString(TEXT("/")) + SandboxName + TEXT("/");

	/** Registers a transient mount point for test assets. */
	inline void RegisterTestMountPoint()
	{
		FPackageName::RegisterMountPoint(*TestMountPoint, FPaths::AutomationTransientDir());
	}

	/** Unregisters the transient mount point and forces GC. */
	inline void UnregisterTestMountPoint()
	{
		FPackageName::UnRegisterMountPoint(*TestMountPoint, FPaths::AutomationTransientDir());
		if (GEngine)
		{
			GEngine->ForceGarbageCollection(true);
		}
	}

	/** Creates an empty, unregistered widget blueprint under Folder (TestMountPoint by default; specs
	 *  using FTestAssetSandbox pass its Folder()) named NamePrefix + InOutCounter (incremented). */
	inline UWidgetBlueprint* MakeEmptyWidgetBlueprintForTest(
		const TCHAR* NamePrefix, int32& InOutCounter, const FString& Folder = TestMountPoint)
	{
		const FString Name = FString::Printf(TEXT("%s%d"), NamePrefix, InOutCounter++);
		const FString PackagePath = Folder / Name;
		UPackage* Package = CreatePackage(*PackagePath);

		return UWidgetBlueprintEditorSubsystem::CreateWidgetBlueprint(
			Package, FName(*Name), BPTYPE_Normal,
			UUserWidget::StaticClass(), nullptr, NAME_None, /*bRegisterAndCompile=*/false);
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
