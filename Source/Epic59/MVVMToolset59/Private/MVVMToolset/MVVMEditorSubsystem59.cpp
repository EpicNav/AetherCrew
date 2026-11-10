// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: copied from UE 5.9; see MVVMEditorSubsystem59.h.

#include "MVVMToolset/MVVMEditorSubsystem59.h"

#include "Bindings/MVVMBindingHelper.h"
#include "Bindings/MVVMConversionFunctionHelper.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewCondition.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMBlueprintViewEvent.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMWidgetBlueprintExtension_View.h"
#include "WidgetBlueprint.h"

namespace UE::MVVM::Epic59
{
namespace Private
{
bool FindFieldChainFromPath(UStruct* InStruct, const FString& Path, TArray<UE::MVVM::FMVVMConstFieldVariant>& OutChain, FString& OutError)
{
	OutChain.Reset();

	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("."));

	UStruct* CurrentStruct = InStruct;

	for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
	{
		const FString& Segment = Segments[SegmentIndex];
		if (!CurrentStruct)
		{
			OutChain.Reset();
			return false;
		}

		// 1) Property first
		if (FProperty* FoundProperty = CurrentStruct->FindPropertyByName(FName(*Segment)))
		{
			OutChain.Emplace(FoundProperty);

			if (FStructProperty* StructProp = CastField<FStructProperty>(FoundProperty))
			{
				CurrentStruct = StructProp->Struct;
			}
			else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(FoundProperty))
			{
				CurrentStruct = ObjProp->PropertyClass;
			}
			else
			{
				CurrentStruct = nullptr; // leaf
			}
			continue;
		}

		// 2) Then a UFunction - only valid on a UClass, and only as the last segment.
		if (UClass* CurrentClass = Cast<UClass>(CurrentStruct))
		{
			if (UFunction* FoundFunction = CurrentClass->FindFunctionByName(FName(*Segment)))
			{
				const bool bIsLastSegment = (SegmentIndex == Segments.Num() - 1);
				if (!bIsLastSegment)
				{
					OutError = FString::Printf(TEXT("Field '%s' is a function, so it must be the last segment of the path '%s'."), *Segment, *Path);
					OutChain.Reset();
					return false;
				}

				OutChain.Emplace(FoundFunction);
				CurrentStruct = nullptr; // leaf
				continue;
			}
		}

		// 3) Not found
		if (Segment.Contains(":"))
		{
			FString LeftStr;
			FString RightStr;
			Segment.Split(":", &LeftStr, &RightStr);
			OutError = FString::Printf(TEXT("Cannot find field '%s'. If %s is meant to be a conversion function, use the ConversionName parameter instead to set it."), *Segment, *RightStr);
		}
		else if (Path.Contains(":"))
		{
			FString LeftStr;
			FString RightStr;
			Path.Split(":", &LeftStr, &RightStr);
			OutError = FString::Printf(TEXT("Cannot parse path '%s'. If %s is meant to be a conversion function, use the ConversionName parameter instead to set it."), *Path, *RightStr);
		}
		else
		{
			TArray<FString> PropertyNames;
			for (TFieldIterator<FProperty> It(CurrentStruct, EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				PropertyNames.Add(It->GetName());
			}

			TArray<FString> FunctionNames;
			if (const UClass* CurrentClass = Cast<UClass>(CurrentStruct))
			{
				for (TFieldIterator<UFunction> It(CurrentClass, EFieldIterationFlags::IncludeSuper); It; ++It)
				{
					if (It->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
					{
						FunctionNames.Add(It->GetName());
					}
				}
			}

			OutError = FString::Printf(TEXT("Cannot find field '%s'. Valid properties: %s. Valid functions: %s"),
				*Segment,
				*FString::Join(PropertyNames, TEXT(", ")),
				FunctionNames.IsEmpty() ? TEXT("(none)") : *FString::Join(FunctionNames, TEXT(", ")));
		}

		OutChain.Reset();
		return false;
	}

	return !OutChain.IsEmpty();
}
}

FMVVMBlueprintPropertyPath ResolvePropertyPath(const UMVVMEditorSubsystem& Subsystem, UWidgetBlueprint* WidgetBlueprint, UObject* Context, const FString& PropertyPath, FString& OutError)
{
	if (WidgetBlueprint == nullptr)
	{
		OutError = FString::Printf(TEXT("Null WidgetBlueprint."));
		return FMVVMBlueprintPropertyPath();
	}

	if (Context == nullptr)
	{
		// A freshly created/uncompiled blueprint may not have a GeneratedClass yet.
		UClass* SelfClass = WidgetBlueprint->GeneratedClass ? WidgetBlueprint->GeneratedClass : WidgetBlueprint->SkeletonGeneratedClass;

		TArray<UE::MVVM::FMVVMConstFieldVariant> FieldChain;
		if (!Private::FindFieldChainFromPath(SelfClass, PropertyPath, FieldChain, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Invalid property path '%s'. Property does not exist in widget '%s'"), *PropertyPath, *WidgetBlueprint->GetName());
			}
			return FMVVMBlueprintPropertyPath();
		}

		// Get path to Widget Blueprint property
		FMVVMBlueprintPropertyPath BlueprintPropertyPath;
		BlueprintPropertyPath.SetSelfContext();
		for (const UE::MVVM::FMVVMConstFieldVariant& Field : FieldChain)
		{
			BlueprintPropertyPath.AppendPropertyPath(WidgetBlueprint, Field);
		}
		return BlueprintPropertyPath;
	}

	if (UWidget* Widget = Cast<UWidget>(Context))
	{
		TArray<UE::MVVM::FMVVMConstFieldVariant> FieldChain;
		if (!Private::FindFieldChainFromPath(Widget->GetClass(), PropertyPath, FieldChain, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Invalid property path '%s'. Property does not exist in '%s'"), *PropertyPath, *Widget->GetName());
			}
			return FMVVMBlueprintPropertyPath();
		}

		// Get path to Widget property
		FMVVMBlueprintPropertyPath BlueprintPropertyPath;
		BlueprintPropertyPath.SetWidgetName(Widget->GetFName());
		for (const UE::MVVM::FMVVMConstFieldVariant& Field : FieldChain)
		{
			BlueprintPropertyPath.AppendPropertyPath(WidgetBlueprint, Field);
		}
		return BlueprintPropertyPath;
	}

	if (UClass* ViewModelClass = Cast<UClass>(Context))
	{
		TArray<UE::MVVM::FMVVMConstFieldVariant> FieldChain;
		if (!Private::FindFieldChainFromPath(ViewModelClass, PropertyPath, FieldChain, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Invalid property path '%s'. Property does not exist in '%s'"), *PropertyPath, *ViewModelClass->GetName());
			}
			return FMVVMBlueprintPropertyPath();
		}

		// Get path to Widget View Model property
		UMVVMBlueprintView* View = Subsystem.GetView(WidgetBlueprint);
		if (!IsValid(View))
		{
			OutError = FString::Printf(TEXT("Invalid MVVM View."));
			return FMVVMBlueprintPropertyPath();
		}

		FGuid ViewModelId;
		for (const FMVVMBlueprintViewModelContext& ViewModelContext : View->GetViewModels())
		{
			// A Blueprint ViewModel is also matched by its other classes (e.g. the SKEL_ class, which already contains
			// properties that were added since the last compile), not only by the generated class registered on the widget.
			const UClass* RegisteredClass = ViewModelContext.GetViewModelClass();
			if (Context == RegisteredClass
				|| (RegisteredClass != nullptr && ViewModelClass->ClassGeneratedBy != nullptr && ViewModelClass->ClassGeneratedBy == RegisteredClass->ClassGeneratedBy))
			{
				ViewModelId = ViewModelContext.GetViewModelId();
				break;
			}
		}

		if (!ViewModelId.IsValid())
		{
			OutError = FString::Printf(TEXT("ViewModel class '%s' not on widget blueprint %s."), *ViewModelClass->GetName(), *WidgetBlueprint->GetName());
			return FMVVMBlueprintPropertyPath();
		}

		FMVVMBlueprintPropertyPath BlueprintPropertyPath;
		BlueprintPropertyPath.SetViewModelId(ViewModelId);
		for (const UE::MVVM::FMVVMConstFieldVariant& Field : FieldChain)
		{
			BlueprintPropertyPath.AppendPropertyPath(WidgetBlueprint, Field);
		}
		return BlueprintPropertyPath;
	}

	OutError = FString::Printf(TEXT("Invalid Context '%s'. Must derive UWidget, UClass or be null."), *Context->GetName());
	return FMVVMBlueprintPropertyPath();
}

bool TryAutoConvertBinding(UMVVMEditorSubsystem& Subsystem, UWidgetBlueprint* WidgetBlueprint, FMVVMBlueprintViewBinding& Binding, FName ConversionName)
{
	const FMVVMBlueprintPropertyPath SourcePath = Binding.SourcePath;
	const FMVVMBlueprintPropertyPath DestinationPath = Binding.DestinationPath;

	if (!SourcePath.IsValid() || !DestinationPath.IsValid())
	{
		return false;
	}

	const FProperty* SourceProperty = nullptr;
	const FProperty* DestinationProperty = nullptr;

	const TArray<UE::MVVM::FMVVMConstFieldVariant> SourceFields = SourcePath.GetCompleteFields(WidgetBlueprint);
	if (SourceFields.Num() > 0 && SourceFields.Last().IsProperty())
	{
		SourceProperty = SourceFields.Last().GetProperty();
	}

	const TArray<UE::MVVM::FMVVMConstFieldVariant> DestinationFields = DestinationPath.GetCompleteFields(WidgetBlueprint);
	if (DestinationFields.Num() > 0 && DestinationFields.Last().IsProperty())
	{
		DestinationProperty = DestinationFields.Last().GetProperty();
	}

	if (SourceProperty == nullptr || DestinationProperty == nullptr)
	{
		// One binding is null, can not convert
		return false;
	}

	if (ConversionName.IsNone() && UE::MVVM::BindingHelper::ArePropertiesCompatible(SourceProperty, DestinationProperty))
	{
		// if properties are compatible and no conversion was explicitly requested, no work to do
		return false;
	}

	UE::MVVM::FConversionFunctionValue SourceConversionFunction;
	FMVVMBlueprintPinId SourceArgumentPin;

	const TArray<UE::MVVM::FConversionFunctionValue> ConversionFunctions = Subsystem.GetConversionFunctions(WidgetBlueprint, SourceProperty, DestinationProperty);
	if (!ConversionName.IsNone())
	{
		for (const UE::MVVM::FConversionFunctionValue& ConversionFunction : ConversionFunctions)
		{
			if (ConversionName == ConversionFunction.GetFName())
			{
				const TArray<FMVVMBlueprintPinId> ArgumentPins = Subsystem.GetConversionFunctionPins(WidgetBlueprint, ConversionFunction, SourceProperty);
				if (!ArgumentPins.IsEmpty())
				{
					SourceArgumentPin = ArgumentPins[0];
					SourceConversionFunction = ConversionFunction;
				}
				break;
			}
		}
	}
	else
	{
		for (const UE::MVVM::FConversionFunctionValue& ConversionFunction : ConversionFunctions)
		{
			// Find conversions that have a source argument type and a destination return type
			const TArray<FMVVMBlueprintPinId> ArgumentPins = Subsystem.GetConversionFunctionPins(WidgetBlueprint, ConversionFunction, SourceProperty);
			if (!ArgumentPins.IsEmpty())
			{
				SourceConversionFunction = ConversionFunction;
				SourceArgumentPin = ArgumentPins[0];
				break;
			}
		}
	}

	if (!SourceConversionFunction.IsValid())
	{
		// No usable conversion function found; leave the binding untouched.
		return false;
	}

	// Bind conversion function argument to the source path
	FMVVMBlueprintFunctionReference FunctionReference = SourceConversionFunction.IsFunction()
		? FMVVMBlueprintFunctionReference(WidgetBlueprint, SourceConversionFunction.GetFunction())
		: FMVVMBlueprintFunctionReference(SourceConversionFunction.GetNode());

	Subsystem.SetSourceToDestinationConversionFunction(WidgetBlueprint, Binding, FunctionReference);

	// SetSourceToDestinationConversionFunction validates the function against the destination path again and silently
	// assigns nothing if it is rejected, so check what was actually assigned.
	const UMVVMBlueprintViewConversionFunction* AssignedConversionFunction = Binding.Conversion.GetConversionFunction(true);
	if (AssignedConversionFunction == nullptr || AssignedConversionFunction->GetConversionFunction() != FunctionReference)
	{
		return false;
	}

	Subsystem.SetPathForConversionFunctionArgument(WidgetBlueprint, Binding, SourceArgumentPin, SourcePath, true);
	return true;
}

void RecreateGeneratedGraphs(UMVVMBlueprintView& View)
{
	UWidgetBlueprint* WidgetBlueprint = View.GetOuterUMVVMWidgetBlueprintExtension_View()->GetWidgetBlueprint();

	for (FMVVMBlueprintViewBinding& Binding : View.GetBindings())
	{
		if (UMVVMBlueprintViewConversionFunction* ConversionFunction = Binding.Conversion.SourceToDestinationConversion)
		{
			ConversionFunction->RemoveWrapperGraph(View.GetOuterUMVVMWidgetBlueprintExtension_View()->GetWidgetBlueprint());

			Binding.Conversion.SourceToDestinationConversion = DuplicateObject<UMVVMBlueprintViewConversionFunction>(ConversionFunction, WidgetBlueprint);
			Binding.Conversion.SourceToDestinationConversion->RecreateWrapperGraph(WidgetBlueprint);
		}

		if (UMVVMBlueprintViewConversionFunction* ConversionFunction = Binding.Conversion.DestinationToSourceConversion)
		{
			ConversionFunction->RemoveWrapperGraph(View.GetOuterUMVVMWidgetBlueprintExtension_View()->GetWidgetBlueprint());

			Binding.Conversion.DestinationToSourceConversion = DuplicateObject<UMVVMBlueprintViewConversionFunction>(ConversionFunction, WidgetBlueprint);
			Binding.Conversion.DestinationToSourceConversion->RecreateWrapperGraph(WidgetBlueprint);
		}
	}

	for (UMVVMBlueprintViewEvent* Event : View.GetEvents())
	{
		if (IsValid(Event))
		{
			Event->RecreateWrapperGraph();
		}

	}

	for (UMVVMBlueprintViewCondition* Condition : View.GetConditions())
	{
		if (IsValid(Condition))
		{
			Condition->RecreateWrapperGraph();
		}
	}
}
}
