// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: UE 5.9 added these to UMVVMEditorSubsystem (ResolvePropertyPath, TryAutoConvertBinding) and
// UMVVMBlueprintView (RecreateGeneratedGraphs); 5.8's classes cannot gain members, so they are free functions here,
// copied from 5.9's ModelViewViewModelEditor/Private/MVVMEditorSubsystem.cpp and
// ModelViewViewModelBlueprint/Private/MVVMBlueprintView.cpp, with the subsystem or view passed in.

#pragma once

#include "CoreMinimal.h"
#include "MVVMBlueprintViewBinding.h"
#include "Types/MVVMBindingName.h"

class UMVVMBlueprintView;
class UMVVMEditorSubsystem;
class UWidgetBlueprint;

namespace UE::MVVM::Epic59
{
	/** Resolves a dotted property path on the widget blueprint (Context null), a widget (UWidget) or a view model class (UClass). */
	FMVVMBlueprintPropertyPath ResolvePropertyPath(const UMVVMEditorSubsystem& Subsystem, UWidgetBlueprint* WidgetBlueprint, UObject* Context, const FString& PropertyPath, FString& OutError);

	/** Gives Binding a conversion function from its source to its destination type (ConversionName, or the first that fits). */
	bool TryAutoConvertBinding(UMVVMEditorSubsystem& Subsystem, UWidgetBlueprint* WidgetBlueprint, FMVVMBlueprintViewBinding& Binding, FName ConversionName);

	/** Rebuilds the wrapper graphs of every conversion function, event and condition in the view. */
	void RecreateGeneratedGraphs(UMVVMBlueprintView& View);
}
