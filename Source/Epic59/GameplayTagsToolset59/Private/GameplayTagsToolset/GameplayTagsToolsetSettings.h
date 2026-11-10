// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "GameplayTagsToolsetSettings.generated.h"

/// Policy for how UGameplayTagsToolset59::AddTag behaves when the caller passes an empty TagSource.
UENUM()
enum class EGameplayTagsToolsetEmptySourcePolicy : uint8
{
	/// Fall back to the GameplayTagsManager's default INI source.
	UseDefaultSource,

	/// Reject the call and raise a script error.
	RequireNonEmptySource,
};

/// Project settings for the GameplayTagsToolset plugin.
UCLASS(Config = Editor, DefaultConfig, meta = (DisplayName = "Gameplay Tags Toolset"))
class UGameplayTagsToolsetSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/**
	 * How AddTag should behave when invoked with an empty TagSource.
	 * UseDefaultSource (default) falls back to the GameplayTagsManager's default source.
	 * RequireNonEmptySource forces callers to specify a source explicitly.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "AddTag")
	EGameplayTagsToolsetEmptySourcePolicy EmptySourcePolicy =
		EGameplayTagsToolsetEmptySourcePolicy::UseDefaultSource;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("GameplayTagsToolsetSettings"); }
};
