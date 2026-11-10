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
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "MVVMEditorSubsystem"

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

void OnBindingPreEditChange(UMVVMBlueprintView* BlueprintView, FName PropertyName)
{
	FProperty* ChangedProperty = FMVVMBlueprintViewBinding::StaticStruct()->FindPropertyByName(PropertyName);
	check(ChangedProperty != nullptr);

	FEditPropertyChain EditChain;
	EditChain.AddTail(UMVVMBlueprintView::StaticClass()->FindPropertyByName("Bindings"));
	EditChain.AddTail(ChangedProperty);
	EditChain.SetActivePropertyNode(ChangedProperty);

	BlueprintView->PreEditChange(EditChain);
}

void OnBindingPostEditChange(UMVVMBlueprintView* BlueprintView, FName PropertyName)
{
	FProperty* ChangedProperty = FMVVMBlueprintViewBinding::StaticStruct()->FindPropertyByName(PropertyName);
	check(ChangedProperty != nullptr);

	FEditPropertyChain EditChain;
	EditChain.AddTail(UMVVMBlueprintView::StaticClass()->FindPropertyByName("Bindings"));
	EditChain.AddTail(ChangedProperty);
	EditChain.SetActivePropertyNode(ChangedProperty);

	FPropertyChangedEvent ChangeEvent(ChangedProperty, EPropertyChangeType::ValueSet);
	FPropertyChangedChainEvent ChainEvent(EditChain, ChangeEvent);
	BlueprintView->PostEditChangeChainProperty(ChainEvent);
}

void OnEventPreEditChange(UMVVMBlueprintViewEvent* Event, FName PropertyName)
{
	FProperty* ChangedProperty = UMVVMBlueprintViewEvent::StaticClass()->FindPropertyByName(PropertyName);
	check(ChangedProperty != nullptr);

	FEditPropertyChain EditChain;
	EditChain.AddTail(ChangedProperty);
	EditChain.SetActivePropertyNode(ChangedProperty);

	Event->SetFlags(RF_Transactional);
	Event->PreEditChange(EditChain);
}

void OnEventPostEditChange(UMVVMBlueprintViewEvent* Event, FName PropertyName)
{
	FProperty* ChangedProperty = UMVVMBlueprintViewEvent::StaticClass()->FindPropertyByName(PropertyName);
	check(ChangedProperty != nullptr);

	FEditPropertyChain EditChain;
	EditChain.AddTail(ChangedProperty);
	EditChain.SetActivePropertyNode(ChangedProperty);

	FPropertyChangedEvent ChangeEvent(ChangedProperty, EPropertyChangeType::ValueSet);
	FPropertyChangedChainEvent ChainEvent(EditChain, ChangeEvent);
	Event->PostEditChangeChainProperty(ChainEvent);
}

UEdGraphPin* GetGraphPin(const UMVVMEditorSubsystem* Subsystem, const UWidgetBlueprint* WidgetBlueprint, const FMVVMBlueprintViewBinding& ViewBinding, const FMVVMBlueprintPinId& ParameterId, bool bSourceToDestination)
{
	if (WidgetBlueprint == nullptr)
	{
		return nullptr;
	}
	return Subsystem->GetConversionFunctionArgumentPin(WidgetBlueprint, ViewBinding, ParameterId, bSourceToDestination);
}

UEdGraphPin* GetGraphPin(const UWidgetBlueprint* WidgetBlueprint, UMVVMBlueprintViewEvent* ViewEvent,  const FMVVMBlueprintPinId& ParameterId)
{
	if (WidgetBlueprint == nullptr || ViewEvent == nullptr)
	{
		return nullptr;
	}

	return ViewEvent->GetOrCreateGraphPin(ParameterId);
}

void DoAction(const UMVVMEditorSubsystem* Subsystem, UWidgetBlueprint* WidgetBlueprint, UMVVMBlueprintViewEvent* ViewEvent, FMVVMBlueprintViewBinding* Binding,  const FMVVMBlueprintPinId& ParameterId, bool bSourceToDestination
	, FText TransactionName
	, TFunctionRef<bool(const UEdGraphSchema_K2*, UEdGraphPin*)> Test, TFunctionRef<void(const UEdGraphSchema_K2*, UEdGraphPin*)> Action)
{
	UEdGraphPin* GraphPin;
	if (ViewEvent)
	{
		GraphPin = Private::GetGraphPin(WidgetBlueprint, ViewEvent, ParameterId);
	}
	else if (Binding)
	{
		GraphPin = Private::GetGraphPin(Subsystem, WidgetBlueprint, *Binding, ParameterId, bSourceToDestination);
	}
	else
	{
		return;
	}

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Test(K2Schema, GraphPin))
	{
		return;
	}

	FScopedTransaction Transaction(TransactionName);
	if (ViewEvent)
	{
		FName NAME_SavedPin = "SavedPins";
		Private::OnEventPreEditChange(ViewEvent, NAME_SavedPin);
		Action(K2Schema, GraphPin);
		ViewEvent->SavePinValues();
		Private::OnEventPostEditChange(ViewEvent, NAME_SavedPin);
	}
	else if (Binding)
	{
		FName NAME_Conversion = GET_MEMBER_NAME_CHECKED(FMVVMBlueprintViewBinding, Conversion);
		UMVVMBlueprintView* View = Subsystem->GetView(WidgetBlueprint);
		UMVVMBlueprintViewConversionFunction* ConversionFunction = Binding->Conversion.GetConversionFunction(bSourceToDestination);
		check(View);
		check(ConversionFunction);

		Private::OnBindingPreEditChange(View, NAME_Conversion);
		Action(K2Schema, GraphPin);
		ConversionFunction->SavePinValues(WidgetBlueprint);
		Private::OnBindingPostEditChange(View, NAME_Conversion);
	}
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

bool SetConversionFunctionArgumentLiteral(const UMVVMEditorSubsystem& Subsystem, UWidgetBlueprint* WidgetBlueprint, FGuid BindingId, const FString& PinName, const FString& Value, bool bSourceToDestination, FText& OutError)
{
	UMVVMBlueprintView* View = Subsystem.GetView(WidgetBlueprint);
	FMVVMBlueprintViewBinding* Binding = View ? View->GetBinding(BindingId) : nullptr;
	if (Binding == nullptr)
	{
		OutError = FText::Format(LOCTEXT("ConversionArgumentUnknownBinding", "No binding with id '{0}' in the view."), FText::FromString(BindingId.ToString()));
		return false;
	}

	const UMVVMBlueprintViewConversionFunction* ConversionFunction = Binding->Conversion.GetConversionFunction(bSourceToDestination);
	if (ConversionFunction == nullptr || ConversionFunction->GetConversionFunction().GetType() != EMVVMBlueprintFunctionReferenceType::Function)
	{
		OutError = LOCTEXT("ConversionArgumentNoFunctionConversion", "The binding has no function conversion in that direction. Arguments of node-based conversions cannot be set.");
		return false;
	}

	const FName Name(*PinName);
	const FMVVMBlueprintPinId PinId(MakeArrayView(&Name, 1));
	UEdGraphPin* Pin = Subsystem.GetConversionFunctionArgumentPin(WidgetBlueprint, *Binding, PinId, bSourceToDestination);
	if (Pin == nullptr || Pin->bOrphanedPin || !UE::MVVM::ConversionFunctionHelper::IsInputPin(Pin))
	{
		OutError = FText::Format(LOCTEXT("ConversionArgumentUnknownPin", "The conversion function has no input pin '{0}'."), FText::FromString(PinName));
		return false;
	}

	if (Pin->LinkedTo.Num() > 0)
	{
		OutError = FText::Format(LOCTEXT("ConversionArgumentPinBoundToPath", "Pin '{0}' is bound to a property path. Clear the path before setting a literal value."), FText::FromString(PinName));
		return false;
	}

	const FName Category = Pin->PinType.PinCategory;
	const bool bSupportedType = !Pin->PinType.IsContainer()
		&& (Category == UEdGraphSchema_K2::PC_Boolean
			|| Category == UEdGraphSchema_K2::PC_Int
			|| Category == UEdGraphSchema_K2::PC_Int64
			|| Category == UEdGraphSchema_K2::PC_Real
			|| Category == UEdGraphSchema_K2::PC_Name
			|| Category == UEdGraphSchema_K2::PC_String);
	if (!bSupportedType)
	{
		OutError = FText::Format(LOCTEXT("ConversionArgumentUnsupportedPinType", "Pin '{0}' has type '{1}'. Only bool, int, int64, float, name and string pins can be set to a literal."), FText::FromString(PinName), UEdGraphSchema_K2::TypeToText(Pin->PinType));
		return false;
	}

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	FString UseDefaultValue;
	TObjectPtr<UObject> UseDefaultObject = nullptr;
	FText UseDefaultText;
	const bool bPreserveTextIdentity = false;
	K2Schema->GetPinDefaultValuesFromString(Pin->PinType, Pin->GetOwningNodeUnchecked(), Value, UseDefaultValue, UseDefaultObject, UseDefaultText, bPreserveTextIdentity);
	const FString SchemaError = K2Schema->IsPinDefaultValid(Pin, UseDefaultValue, UseDefaultObject, UseDefaultText);
	if (!SchemaError.IsEmpty())
	{
		OutError = FText::Format(LOCTEXT("ConversionArgumentInvalidValue", "'{0}' is not a valid value for pin '{1}': {2}"), FText::FromString(Value), FText::FromString(PinName), FText::FromString(SchemaError));
		return false;
	}

	// An empty value resets the pin: FMVVMBlueprintPin::CopyTo skips an empty saved value, so the autogenerated default is what a rebuild would give anyway.
	const FText TransactionName = FText::Format(LOCTEXT("SetConversionFunctionArgumentLiteral", "Set conversion function argument {0}"), FText::FromString(PinName));
	Private::DoAction(&Subsystem, WidgetBlueprint, nullptr, Binding, PinId, bSourceToDestination, TransactionName
		, [&Value](const UEdGraphSchema_K2* K2Schema, UEdGraphPin* GraphPin)->bool
		{
			return GraphPin != nullptr
				&& (Value.IsEmpty() ? !K2Schema->DoesDefaultValueMatchAutogenerated(*GraphPin) : !GraphPin->DefaultValue.Equals(Value, ESearchCase::CaseSensitive));
		}
		, [WidgetBlueprint, Binding, bSourceToDestination, &Value](const UEdGraphSchema_K2* K2Schema, UEdGraphPin* GraphPin)
		{
			// The conversion function is not transactional by default; without that, undo would not restore its SavedPins.
			UMVVMBlueprintViewConversionFunction* EditedConversionFunction = Binding->Conversion.GetConversionFunction(bSourceToDestination);
			EditedConversionFunction->SetFlags(RF_Transactional);
			EditedConversionFunction->Modify();
			GraphPin->Modify();
			if (Value.IsEmpty())
			{
				K2Schema->ResetPinToAutogeneratedDefaultValue(GraphPin);
			}
			else
			{
				const bool bMarkAsModified = false;
				K2Schema->TrySetDefaultValue(*GraphPin, Value, bMarkAsModified);
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
		});

	return true;
}

bool SetEventArgumentLiteral(const UMVVMEditorSubsystem& Subsystem, UWidgetBlueprint* WidgetBlueprint, UMVVMBlueprintViewEvent* Event, const FString& PinName, const FString& Value, FText& OutError)
{
	const UMVVMBlueprintView* View = Subsystem.GetView(WidgetBlueprint);
	if (View == nullptr || Event == nullptr || !View->GetEvents().Contains(Event))
	{
		OutError = LOCTEXT("EventArgumentUnknownEvent", "The event is not in the view.");
		return false;
	}

	const FName Name(*PinName);
	const FMVVMBlueprintPinId PinId(MakeArrayView(&Name, 1));
	UEdGraphPin* Pin = Event->GetOrCreateGraphPin(PinId);
	if (Pin == nullptr || Pin->bOrphanedPin || !UE::MVVM::ConversionFunctionHelper::IsInputPin(Pin))
	{
		OutError = FText::Format(LOCTEXT("EventArgumentUnknownPin", "The event has no input pin '{0}'."), FText::FromString(PinName));
		return false;
	}

	if (Pin->LinkedTo.Num() > 0)
	{
		OutError = FText::Format(LOCTEXT("EventArgumentPinBoundToPath", "Pin '{0}' is bound to a property path. Clear the path before setting a literal value."), FText::FromString(PinName));
		return false;
	}

	const FName Category = Pin->PinType.PinCategory;
	const bool bSupportedType = !Pin->PinType.IsContainer()
		&& (Category == UEdGraphSchema_K2::PC_Boolean
			|| Category == UEdGraphSchema_K2::PC_Int
			|| Category == UEdGraphSchema_K2::PC_Int64
			|| Category == UEdGraphSchema_K2::PC_Real
			|| Category == UEdGraphSchema_K2::PC_Name
			|| Category == UEdGraphSchema_K2::PC_String);
	if (!bSupportedType)
	{
		OutError = FText::Format(LOCTEXT("EventArgumentUnsupportedPinType", "Pin '{0}' has type '{1}'. Only bool, int, int64, float, name and string pins can be set to a literal."), FText::FromString(PinName), UEdGraphSchema_K2::TypeToText(Pin->PinType));
		return false;
	}

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	FString UseDefaultValue;
	TObjectPtr<UObject> UseDefaultObject = nullptr;
	FText UseDefaultText;
	const bool bPreserveTextIdentity = false;
	K2Schema->GetPinDefaultValuesFromString(Pin->PinType, Pin->GetOwningNodeUnchecked(), Value, UseDefaultValue, UseDefaultObject, UseDefaultText, bPreserveTextIdentity);
	const FString SchemaError = K2Schema->IsPinDefaultValid(Pin, UseDefaultValue, UseDefaultObject, UseDefaultText);
	if (!SchemaError.IsEmpty())
	{
		OutError = FText::Format(LOCTEXT("EventArgumentInvalidValue", "'{0}' is not a valid value for pin '{1}': {2}"), FText::FromString(Value), FText::FromString(PinName), FText::FromString(SchemaError));
		return false;
	}

	// An empty value resets the pin: FMVVMBlueprintPin::CopyTo skips an empty saved value, so the autogenerated default is what a rebuild would give anyway.
	const FText TransactionName = FText::Format(LOCTEXT("SetEventArgumentLiteral", "Set event argument {0}"), FText::FromString(PinName));
	Private::DoAction(&Subsystem, WidgetBlueprint, Event, nullptr, PinId, true, TransactionName
		, [&Value](const UEdGraphSchema_K2* K2Schema, UEdGraphPin* GraphPin)->bool
		{
			return GraphPin != nullptr
				&& (Value.IsEmpty() ? !K2Schema->DoesDefaultValueMatchAutogenerated(*GraphPin) : !GraphPin->DefaultValue.Equals(Value, ESearchCase::CaseSensitive));
		}
		, [WidgetBlueprint, Event, &Value](const UEdGraphSchema_K2* K2Schema, UEdGraphPin* GraphPin)
		{
			// The event may not be transactional; without that, undo would not restore its SavedPins.
			Event->SetFlags(RF_Transactional);
			Event->Modify();
			GraphPin->Modify();
			if (Value.IsEmpty())
			{
				K2Schema->ResetPinToAutogeneratedDefaultValue(GraphPin);
			}
			else
			{
				const bool bMarkAsModified = false;
				K2Schema->TrySetDefaultValue(*GraphPin, Value, bMarkAsModified);
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
		});

	return true;
}
}

#undef LOCTEXT_NAMESPACE
