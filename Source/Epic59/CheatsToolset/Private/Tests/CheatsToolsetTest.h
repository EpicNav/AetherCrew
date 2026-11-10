// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CheatManager.h"

#include "CheatsToolsetTest.generated.h"

/// Cheat manager with a known set of exec cheats, used only by automation tests.
/// SharedNameCheat intentionally collides with the test extension's cheat to exercise de-duplication.
UCLASS(Hidden, MinimalAPI)
class UCheatsToolsetTestManager : public UCheatManager
{
	GENERATED_BODY()

public:
	UFUNCTION(exec)
	void TestManagerCheat(int32 Count) {}

	UFUNCTION(exec, BlueprintAuthorityOnly)
	void TestAuthorityCheat() {}

	UFUNCTION(exec)
	void SharedNameCheat() {}
};

/// Cheat manager extension with a known set of exec cheats, used only by automation tests.
UCLASS(Hidden, MinimalAPI)
class UCheatsToolsetTestExtension : public UCheatManagerExtension
{
	GENERATED_BODY()

public:
	UFUNCTION(exec)
	void TestExtensionCheat(float Scale) {}

	UFUNCTION(exec)
	void SharedNameCheat() {}
};
