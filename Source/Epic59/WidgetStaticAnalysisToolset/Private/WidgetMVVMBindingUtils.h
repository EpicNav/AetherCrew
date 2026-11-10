// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Map.h"
#include "CoreTypes.h"
#include "Misc/Guid.h"
#include "UObject/NameTypes.h"

class UClass;
class UMVVMBlueprintView;
class UWidgetBlueprint;
struct FMVVMBlueprintPropertyPath;

/** A ViewModel context's name and class, keyed by GetViewModelId(). */
struct FViewModelIdentity
{
	FString Name;
	UClass* Class = nullptr;
};

/** The resolved destination of an MVVM binding or event. */
struct FResolvedMVVMDestination
{
	bool bIsWidget = false;
	FName WidgetName;
	/** Set only when bIsWidget: the leaf property or function name. */
	FName FieldName;
	/** Only when bIsWidget: true if FieldName is a property, false if a function. */
	bool bIsProperty = false;
	/** Set only when !bIsWidget: the SelfContext/ViewModel-rooted handler name. */
	FName HandlerName;
};

class FWidgetMVVMBindingUtils
{
public:
	/** WidgetBlueprint's existing MVVM view, or nullptr. Read-only; does not create one. */
	[[nodiscard]] static UMVVMBlueprintView* FindView(const UWidgetBlueprint* WidgetBlueprint);

	[[nodiscard]] static FResolvedMVVMDestination ResolveDestination(const FMVVMBlueprintPropertyPath& DestinationPath, const UWidgetBlueprint* WidgetBlueprint);

	/** Every declared ViewModel context on View, keyed by GetViewModelId(). Empty for a null View. */
	[[nodiscard]] static TMap<FGuid, FViewModelIdentity> BuildViewModelIdentitiesById(const UMVVMBlueprintView* View);
};
