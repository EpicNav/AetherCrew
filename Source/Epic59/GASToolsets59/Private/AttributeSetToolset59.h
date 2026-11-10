// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: 5.9 ToolsetDefinition additions
#include "UObject/ScriptInterface.h"

#include "AttributeSetToolset59.generated.h"

/// Describes a gameplay attribute belonging to an AttributeSet class.
USTRUCT(BlueprintType)
struct FGameplayAttributeInfo59
{
	GENERATED_BODY()

	/// The short attribute name (e.g. "Health").
	UPROPERTY(BlueprintReadWrite, Category = "AttributeSet")
	FString AttributeName;

	/// The fully-qualified attribute name including the set class (e.g. "UMyHealthSet.Health").
	UPROPERTY(BlueprintReadWrite, Category = "AttributeSet")
	FString FullName;

	/// The name of the AttributeSet class that owns this attribute (e.g. "UMyHealthSet").
	UPROPERTY(BlueprintReadWrite, Category = "AttributeSet")
	FString SetClassName;
};

/// Describes an AttributeSet subclass found in the project.
USTRUCT(BlueprintType)
struct FAttributeSetClassInfo59
{
	GENERATED_BODY()

	/// The UClass name of the attribute set (e.g. "UMyHealthSet").
	UPROPERTY(BlueprintReadWrite, Category = "AttributeSet")
	FString ClassName;

	/// The object path to the Blueprint asset, or empty for native C++ classes.
	UPROPERTY(BlueprintReadWrite, Category = "AttributeSet")
	FString AssetPath;

	/// The gameplay attributes defined on this set.
	UPROPERTY(BlueprintReadWrite, Category = "AttributeSet")
	TArray<FGameplayAttributeInfo59> Attributes;
};

/// Provides tools for discovering AttributeSet classes and their gameplay attributes.
UCLASS(BlueprintType, Hidden)
class UAttributeSetToolset59 : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/**
	 * Returns all AttributeSet subclasses found in the project, including their attributes.
	 * Covers both native C++ subclasses and Blueprint subclasses discovered via the asset registry.
	 * @return A list of attribute set descriptors sorted by class name.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "AttributeSets")
	static TArray<FAttributeSetClassInfo59> FindAttributeSetClasses();

	/**
	 * Returns the gameplay attributes defined on a specific AttributeSet class.
	 * @param ClassName The UClass name to look up, e.g. "UMyHealthSet".
	 * @return A list of attribute descriptors defined on the class.
	 */
	UFUNCTION(meta = (AICallable, AIAccessMode = "Read"), Category = "AttributeSets")
	static TArray<FGameplayAttributeInfo59> ListAttributes(const FString& ClassName);

	friend class FAttributeSetToolsetSpec;

	//~ Begin UToolsetDefinition Interface
	virtual bool IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition Interface
};
