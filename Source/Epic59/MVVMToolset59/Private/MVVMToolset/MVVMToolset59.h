// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MVVMBlueprintView.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetDefinition59.h" // UE 5.8: IToolsetProvider and the permission provider
#include "Types/MVVMConditionOperation.h"
#include "Types/MVVMConversionFunctionValue.h"
#include "Types/MVVMFieldVariant.h"
#include "UObject/UnrealType.h"

#include "MVVMToolset59.generated.h"

/* Information about a conversion function. Note, can either be a function or node. */
USTRUCT(BlueprintType)
struct FMVVMViewConversionFunctionDescription59
{
	GENERATED_BODY()

	/* If the conversion function is a UFunction, this value will be set */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TObjectPtr<const UFunction> ConversionFunction;

	/* If the conversion function is a UK2Node, this value will be set */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TSubclassOf<UK2Node> ConversionNode;
};

USTRUCT(BlueprintType)
struct FMVVMViewBindingSource
{
	GENERATED_BODY()

	/* True when Path holds a literal default value rather than a bound property path. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bIsLiteralValue = false;

	/* Name of the function or event input pin this feeds. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString ArgumentName;

	/*
	The source property path (e.g. "ViewModel.Health"), or a literal default value
	when the input pin is not bound to a property path (see bIsLiteralValue).
	*/
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString Path;
};

/* Information about an existing MVVM View event binding. */
USTRUCT(BlueprintType)
struct FMVVMViewEventBindingDescription
{
	GENERATED_BODY()

	/* The underlying event binding object. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TObjectPtr<UMVVMBlueprintViewEvent> Event;

	/* Human readable display name of the event binding. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString DisplayName;

	/* The event property path the binding listens to (e.g. the widget's multicast delegate). */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString EventPath;

	/* The destination property path modified when the event is triggered. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString DestinationPath;

	/* Name of the source widget the event originates from, if the event source is a widget. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FName EventWidgetName;

	/* Id of the source ViewModel the event originates from, if the event source is a ViewModel. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FGuid EventViewModelId;

	/* Whether the event binding is enabled. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bEnabled = false;

	/* One entry per event input pin; a split struct pin is listed by its sub-pins. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TArray<FMVVMViewBindingSource> Arguments;
};

/* Information about an existing MVVM View condition. */
USTRUCT(BlueprintType)
struct FMVVMViewConditionDescription
{
	GENERATED_BODY()

	/* The underlying condition object. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TObjectPtr<UMVVMBlueprintViewCondition> Condition;

	/* Display path of the compared source, prefixed with its context's name (e.g. "MyViewModel.Health"). */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString SourcePath;

	/* Display path of the destination called or set when the condition is true, prefixed with its context's name (e.g. "MyImage.SetOpacity"). */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString DestinationPath;

	/* The comparison applied to the source value. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	EMVVMConditionOperation Operation = EMVVMConditionOperation::Equal;

	/* Value the source is compared against; the lower bound of a Between operation. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	float Value = 0.0f;

	/* Upper bound of a Between operation. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	float MaxValue = 0.0f;

	/* Errors of the condition from the last compile of the widget blueprint. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TArray<FString> CompileErrors;

	/* Warnings of the condition from the last compile of the widget blueprint. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TArray<FString> CompileWarnings;
};

/* One direction of a binding's conversion (source -> destination, or destination -> source). */
USTRUCT(BlueprintType)
struct FMVVMViewBindingConversion
{
	GENERATED_BODY()

	/*
	Name of the conversion function for this direction. Empty when this direction has no
	conversion function - the matching direct property path (SourcePath / DestinationPath) is
	used instead, or the binding mode does not drive this direction. "<ERROR>" when a conversion
	function is set but its function reference cannot be resolved.
	*/
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString FunctionName;

	/*
	One entry per conversion input pin (so something like MakeVector(VM.X, VM.Y, VM.Z) would
	have three entries). Split struct pins are not listed themselves; each of their sub-pins
	gets its own entry instead (e.g. "InVector.X"). Empty when FunctionName is empty.
	*/
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TArray<FMVVMViewBindingSource> Sources;
};

/* Information about an existing MVVM View property binding. */
USTRUCT(BlueprintType)
struct FMVVMViewBindingDescription
{
	GENERATED_BODY()

	/* Unique id of the binding; use with RemoveWidgetViewBinding / SetBindingMode / SetWidgetViewBindingEnabled. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FGuid BindingId;

	/*
	Canonical display name of the binding, of the form "Destination <- Source"
	(or "<->" / "->" depending on the binding mode). Conversion bindings are rendered as
	"Function(Arg1, Arg2, ...)". Not unique: bindings with the same endpoints share the same
	display name. Use BindingId to identify a binding.
	*/
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString DisplayName;

	/*
	The source property path the binding reads from (right-hand side, e.g. "ViewModel.Health").
	Empty when a source-to-destination conversion function supplies the value instead - see
	SourceToDestinationConversion.
	*/
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString SourcePath;

	/*
	The destination property path the binding writes to (left-hand side, e.g. "MyWidget.Text").
	Empty when a destination-to-source conversion function supplies the value instead - see
	DestinationToSourceConversion.
	*/
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString DestinationPath;

	/*
	Forward (source -> destination) conversion, applied for OneTimeToDestination,
	OneWayToDestination and TwoWay bindings. FunctionName is empty when the source is a direct
	property path (see SourcePath).
	*/
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FMVVMViewBindingConversion SourceToDestinationConversion;

	/*
	Backward (destination -> source) conversion, applied for OneWayToSource and TwoWay bindings.
	FunctionName is empty when the destination is a direct property path (see DestinationPath).
	*/
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FMVVMViewBindingConversion DestinationToSourceConversion;

	/* The binding mode (OneWayToDestination, TwoWay, OneWayToSource, etc.). */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	EMVVMBindingMode BindingType = EMVVMBindingMode::OneWayToDestination;

	/* Whether the binding is enabled by default. The instance may enable it at runtime. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bEnabled = false;

	/* Whether the binding is compiled (usable at runtime). If false it is editor-visible only. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bCompile = false;
};

/* Configuration of a WidgetBlueprint's MVVM View. */
USTRUCT(BlueprintType)
struct FMVVMViewConfig
{
	GENERATED_BODY()

	/* Auto initialize the view sources when the Widget is constructed. */
	UPROPERTY(BlueprintReadWrite, Category = "MVVM")
	bool bInitializeSourcesOnConstruct = true;

	/* Auto initialize the view bindings when the Widget is constructed. Requires bInitializeSourcesOnConstruct. */
	UPROPERTY(BlueprintReadWrite, Category = "MVVM")
	bool bInitializeBindingsOnConstruct = true;

	/* Auto initialize the view events when the Widget is constructed. */
	UPROPERTY(BlueprintReadWrite, Category = "MVVM")
	bool bInitializeEventsOnConstruct = true;

	/* Create the view even when there are no view bindings or events. */
	UPROPERTY(BlueprintReadWrite, Category = "MVVM")
	bool bCreateViewWithoutBindings = false;
};

/* Describes a single field valid as an MVVM binding source or destination: its type and readable/writable/notify characteristics. */
USTRUCT(BlueprintType)
struct FMVVMBindableFieldDescription
{
	GENERATED_BODY()

	/* Name of the field as it would appear in a property path. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString FieldName;

	/* C++ type of the field (e.g. "float", "FText"). */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString FieldType;

	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bIsReadable = false;

	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bIsWritable = false;

	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bHasNotify = false;

	/* True if a native (C++) private member. Informational only -- doesn't affect bindability. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bIsPrivate = false;

	/* True if a native (C++) protected member. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bIsProtected = false;

	/* Argument type of a function-backed field's setter. Empty for a plain property or a getter-backed field. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString FunctionArgumentType;

	/* Return type of a function-backed field's getter. Empty for a plain property or a setter-backed field. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString FunctionReturnType;
};

/* Result of listing every field on a class or property path valid as an MVVM binding source or destination. */
USTRUCT(BlueprintType)
struct FMVVMBindableFieldListResult
{
	GENERATED_BODY()

	/* Keep alphabetically first: output keys are sorted, and this is the canary if Fields gets cut. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	int32 AvailableFieldCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TArray<FMVVMBindableFieldDescription> Fields;

	/* Why the lookup found nothing, if it didn't. Empty on success. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString ErrorMessage;
};

/* Describes a single candidate conversion function (or node) that could bridge a source and destination type. */
USTRUCT(BlueprintType)
struct FMVVMConversionOptionDescription
{
	GENERATED_BODY()

	/* Real name -- pass this as CreateViewBinding's ConversionName, not DisplayName. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString Name;

	/* Human-readable label. Not usable as ConversionName -- see Name. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString Tooltip;

	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString FunctionCategory;

	/* Populated only for function-backed options (bIsFunction true); empty for node-backed ones. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString ExpectedArgumentType;

	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	FString ExpectedReturnType;

	/* False = backed by a UK2Node subclass rather than a UFunction. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bIsFunction = false;
};

/* Result of checking whether/how a source and destination field can be bound. */
USTRUCT(BlueprintType)
struct FMVVMBindingConversionResult
{
	GENERATED_BODY()

	/* Keep alphabetically first: output keys are sorted, and this is the canary if ConversionOptions gets cut. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	int32 AvailableConversionOptionCount = 0;

	/* True if the source and destination types already bind directly, with no conversion function needed. */
	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	bool bDirectlyCompatible = false;

	UPROPERTY(BlueprintReadOnly, Category = "MVVM")
	TArray<FMVVMConversionOptionDescription> ConversionOptions;
};

/*
	Toolset for authoring ModelViewViewModel (MVVM) data on UMG Widget Blueprints.

	The Model-View-ViewModel pattern decouples a UMG widget's presentation (the View)
	from the data and logic that drives it (the ViewModel). Bindings wire individual
	widget properties to ViewModel properties so the UI reflects state changes
	automatically, optionally routing values through conversion functions when the
	source and destination types differ, and event bindings forward widget events
	(multicast delegates) onto the ViewModel.

	This toolset exposes an API to configure WidgetBlueprint's MVVM data:
	  - Author ViewModel Blueprints derived from UMVVMViewModelBase and add
	    typed properties to them.
	  - Discover ViewModels on disk, attach them to a WidgetBlueprint, and list
	    the ones currently registered on a widget.
	  - Create, list, and remove property-to-property view bindings between
	    widgets, ViewModels, and the WidgetBlueprint itself, optionally selecting
	    a conversion function when types don't match.
	  - Set literal values on a binding's conversion function arguments, or unbind
	    an argument from its property path.
	  - Create event bindings from widget multicast delegates to ViewModel
	    handlers, enumerate the events available to bind, and set the literal
	    values an event binding sends.
	  - Create, list, and remove conditions that run a destination when a source
	    value compares true against a constant, and change their comparison.
	  - Repair MVVM state on a WidgetBlueprint by regenerating its binding graphs
	    to match the stored binding data.
*/
UCLASS(BlueprintType, Hidden)
class UMVVMToolset59 : public UToolsetDefinition, public IToolsetProvider // UE 5.8: 5.9's UToolsetDefinition is an IToolsetProvider
{
	GENERATED_BODY()

public:
	/*
	Attempts to fixup MVVM data. This function can be called if the MVVM Generated Blueprint graphs like conversion functions report compilation errors.
	It regenerates the graphs to ensure parity with view binding data.
	@param WidgetBlueprint - WidgetBlueprint to fixup
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable), Category = "MVVM")
	static void FixupMVVMData(UWidgetBlueprint* WidgetBlueprint);

	/*
	Creates a new ViewModel asset

	@param ViewModelName - Desired ViewModel asset name.
	@param PackagePath - Target path in which the asset should reside
	@param ParentClass - Class of which the ViewModel should derive. Must derive UMVVMViewModelBase
	@return newly created view model object.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static UBlueprint* CreateViewModel(const FString& ViewModelName, const FString& PackagePath, UClass* ParentClass);

	/*
	Add a property to an existing ViewModel.
	The ViewModel is compiled automatically when it is next added to a widget or used in a binding.

	@param ViewModel - ViewModel asset to use.
	@param PropertyName - Desired name of the new property
	@param PropertyType - Desired type of the new property. Corresponds with PinType
	@param DefaultValue - If a default property is desired, string representation of the value
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static void AddViewModelProperty(UBlueprint* ViewModel, const FString& PropertyName, const FString& PropertyType, const FString& DefaultValue);

	/*
	List all ViewModel under the search path

	@param SearchPath - Mount point search path for ViewModel assets
	@return the ViewModel classes that match the path filter. For Blueprint ViewModels this is the generated class (never the SKEL_ class).
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static TArray<UClass*> ListViewModels(const FString& SearchPath);

	/*
	List all view models used by a widget Blueprint

	@param WidgetBlueprint - Blueprint to get the view models from
	@return All the view model classes used by the Blueprint
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static TArray<UClass*> ListWidgetViewModels(UWidgetBlueprint* WidgetBlueprint);

	/*
	Adds a ViewModel to the WidgetBlueprint

	@param WidgetBlueprint - Blueprint to use
	@param ViewModelClass - View model class to use.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static void AddViewModelToWidget(UWidgetBlueprint* WidgetBlueprint, UClass* ViewModelClass);

	/*
	List all view bindings on the WidgetBlueprint (including possible conversion functions).

	@param WidgetBlueprint - WidgetBlueprint to use
	@return A description of every view binding on the Blueprint
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static TArray<FMVVMViewBindingDescription> ListWidgetViewBindings(UWidgetBlueprint* WidgetBlueprint);

	/*
	Removes View Binding on the WidgetBlueprint

	@param WidgetBlueprint - WidgetBlueprint to use
	@param BindingID - The guid of the binding to remove
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static void RemoveWidgetViewBinding(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingID);

	/*
	Changes the enabled and compile flag for an existing widget view binding.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param BindingID - The guid of the binding to change
	@param bEnabled - Whether the binding is enabled or disabled by default. The instance may enable the binding at runtime.
	@param bCompile - If false then the binding is visible in the editor, but is not compiled and cannot be used at runtime.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static void SetWidgetViewBindingEnabled(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingID, bool bEnabled = true, bool bCompile = true);

	/*
	Sets the binding mode (OneTimeToDestination/OneWayToDestination/TwoWay/OneWayToSource) for an existing view binding.
	Not supported during PIE.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param BindingID - The guid of the binding to change
	@param Mode - New binding mode to apply
	@return true if the mode was applied, false if in PIE, if the MVVM subsystem isn't loaded, or if the
		widget blueprint, view, or binding could not be resolved
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static bool SetBindingMode(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingID, EMVVMBindingMode Mode);

	/*
	Sets a literal value on an input pin of a binding's conversion function, then recompiles the WidgetBlueprint.
	A pin bound to a property path must be cleared first with ClearConversionFunctionArgumentPath. If the recompile fails
	the value is kept; use CompileWidgetBlueprint to read the errors. Not supported during PIE.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param BindingID - The guid of the binding to change
	@param ArgumentName - ArgumentName from ListWidgetViewBindings. Edits to split struct sub-pins (e.g. "InVector_X") may be lost on reload.
	@param Value - "true"/"false" for bool, a decimal integer for int/int64, a number for float, any text for name/string (empty resets to default)
	@param bSourceToDestination - True for the source-to-destination conversion, false for destination-to-source
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static void SetConversionFunctionArgumentLiteral(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingID, const FString& ArgumentName, const FString& Value, bool bSourceToDestination = true);

	/*
	Removes the property path bound to an input pin of a binding's conversion function, resetting the pin to its default
	literal, then recompiles the WidgetBlueprint. The compiler does not warn if the cleared pin carried the binding's source
	value. If the recompile fails the edit is kept; use CompileWidgetBlueprint to read the errors. Not supported during PIE.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param BindingID - The guid of the binding to change
	@param ArgumentName - ArgumentName from ListWidgetViewBindings
	@param bSourceToDestination - True for the source-to-destination conversion, false for destination-to-source
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static void ClearConversionFunctionArgumentPath(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingID, const FString& ArgumentName, bool bSourceToDestination = true);

	/*
	Lists the bindable property paths available on a widget (binding source/destination candidates).
	Names aren't filtered by direction -- a read-only property can't be used as a destination and a
	write-only one can't be used as a source; CreateViewBinding reports that mismatch if it's tried.

	@param WidgetBlueprint - WidgetBlueprint the widget belongs to
	@param Widget - Widget to enumerate bindable properties on
	@return the property names on Widget usable as a view binding source or destination
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static TArray<FString> ListBindableWidgetProperties(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget);

	/*
	Lists every field on TargetClass valid as an MVVM binding source or destination.

	@param TargetClass - ViewModel (or any UClass) to enumerate bindable fields on.
	@param AccessorClass - Class the fields would be accessed from; gates private/protected visibility.
	@return the bindable fields found, led by their total count.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static FMVVMBindableFieldListResult ListClassBindableFields(UClass* TargetClass, UClass* AccessorClass);

	/*
	Resolves PropertyPath against Context and describes that single field. Unlike
	ListClassBindableFields, this doesn't gate on private/protected -- CreateViewBinding can bind those
	fields too, so we don't hide them. bIsPrivate/bIsProtected on the result say which case it is.

	@param WidgetBlueprint - WidgetBlueprint the property path is resolved within.
	@param Context - Object that contains the property. Can be the following:
		- UWidget: property is from a widget in the widget blueprint's widgettree
		- UClass: property is from a widget blueprint's ViewModel
		- Null: property is from the widget blueprint
	@param PropertyPath - Dot '.' separated path to the reflected property (path.SubField). Case-sensitive
	@return the resolved field (0 or 1 entries). If the path doesn't
		resolve, ErrorMessage carries why -- not treated as a script error.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static FMVVMBindableFieldListResult ListPropertyPathBindableFields(UWidgetBlueprint* WidgetBlueprint, UObject* Context, const FString& PropertyPath);

	/*
	Creates a View Binding from the SourceProperty to the Destination property.
	If ConversionName is given, that conversion function is used, even if the property types already match (e.g. to format a FText).
	Otherwise, if there is a type mismatch, an existing conversion function will try to be used.

	Property paths resolve against each segment's declared type, so a field that only exists on a subclass of an intermediate
	object's declared type won't resolve. Bind to the intermediate object and cast inside a conversion function instead.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param SourceContext - Source object that contains the source property.
		Can be the following:
		- UWidget: source property is from a widget in the widget blueprint's widgettree
		- UClass: source property is from a widget blueprint's ViewModel
		- Null: source property is from the widget blueprint
	@param SourcePropertyPath - Property to read data from. Contains a dot '.' separated path to the reflected property (path.SubField). Case-sensitive
	@param DestinationContext - Destination object that contains the destination property.
		Can be the following:
		- UWidget: destination property is from a widget in the widget blueprint's widgettree
		- UClass: destination property is from a widget blueprint's ViewModel
		- Null: destination property is from the widget blueprint
	@param DestinationPropertyPath - Property to write data to. Contains a dot '.' separated path to the field (path.subfield)
	@param ConversionName - Optional parameter, name of a conversion function to use. If none is provided, one will be inferred if the types don't match. See ListConversionFunctions.
		If the given conversion function doesn't exist or can't convert between the two properties, no binding is created.
	@return Valid FGuid if the binding was created, otherwise returns an invalid zero-guid
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static FGuid CreateViewBinding(UWidgetBlueprint* WidgetBlueprint, UObject* SourceContext, const FString& SourcePropertyPath, UObject* DestinationContext, const FString& DestinationPropertyPath, FName ConversionName);

	/*
	List Available Conversion functions for a WidgetBlueprint that bind a Source Property to a Destination Property
	Includes Pure and Const functions on the WidgetBlueprint itself once it has been compiled.
	@param WidgetBlueprint - WidgetBlueprint to use
	@return the FMVVMViewConversionFunctionDescription59 for available conversion functions.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static TArray<FMVVMViewConversionFunctionDescription59> ListConversionFunctions(UWidgetBlueprint* WidgetBlueprint);

	/*
	Reports whether SourcePropertyPath and DestinationPropertyPath bind directly, and/or which conversion
	functions bridge them. Parameters mirror CreateViewBinding's Source-/Destination-prefixed parameters.
	A function-backed leaf field on either side resolves as unbridgeable.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param SourceContext - Object containing the candidate source property. Same contract as CreateViewBinding's SourceContext.
	@param SourcePropertyPath - Property path to the candidate source field.
	@param DestinationContext - Object containing the candidate destination property. Same contract as CreateViewBinding's DestinationContext.
	@param DestinationPropertyPath - Property path to the candidate destination field.
	@return whether the two fields bind directly, and/or which conversion functions (if any) bridge them.
		Use each option's Name, not DisplayName, as CreateViewBinding's ConversionName.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static FMVVMBindingConversionResult ListBindingConversionOptions(UWidgetBlueprint* WidgetBlueprint, UObject* SourceContext, const FString& SourcePropertyPath, UObject* DestinationContext, const FString& DestinationPropertyPath);

	/*
	Returns the MVVM View configuration.

	@param WidgetBlueprint - WidgetBlueprint to read configuration from.
	@return the current view configuration
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static FMVVMViewConfig GetWidgetViewConfig(UWidgetBlueprint* WidgetBlueprint);

	/*
	Writes the MVVM View configuration on the WidgetBlueprint's View.

	@param WidgetBlueprint - WidgetBlueprint to write configuration to.
	@param Configuration - New view configuration.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static void SetWidgetViewConfig(UWidgetBlueprint* WidgetBlueprint, const FMVVMViewConfig& Configuration);

	/*
	List all event bindings currently configured on the WidgetBlueprint's View, along with their context information.
	@param WidgetBlueprint - WidgetBlueprint to use
	@return the descriptions for each existing event binding.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static TArray<FMVVMViewEventBindingDescription> ListWidgetViewEventBindings(UWidgetBlueprint* WidgetBlueprint);


	/*
	Creates a View Event Binding from the EventProperty to the Destination property.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param EventContext - Event object that contains the source property.
		Can be the following:
		- UWidget: source property is from a widget in the widget blueprint's widgettree
		- UClass: source property is from a widget blueprint's ViewModel
		- Null: source property is from the widget blueprint
	@param EventPropertyPath - Event property to listen to. Contains a dot '.' separated path to the reflected property (path.SubField). Case-sensitive
	@param DestinationContext - Destination object that contains the destination property.
		Can be the following:
		- UWidget: destination property is from a widget in the widget blueprint's widgettree
		- UClass: destination property is from a widget blueprint's ViewModel
		- Null: destination property is from the widget blueprint
	@param DestinationPropertyPath - Property modify when event is triggered. Contains a dot '.' separated path to the field (path.subfield)
	@return Valid UMVVMBlueprintViewEvent* if the event was created
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable), Category = "MVVM")
	static UMVVMBlueprintViewEvent* CreateViewEventBinding(UWidgetBlueprint* WidgetBlueprint, UObject* EventContext, const FString& EventPropertyPath, UObject* DestinationContext, const FString& DestinationPropertyPath);

	/*
	List Available Event Properties for a WidgetBlueprint (i.e. a Multicast Delegate)
	@param WidgetBlueprint - WidgetBlueprint to use
	@return the FString property path for the event property.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable), Category = "MVVM")
	static TArray<FString> ListWidgetViewEvents(UWidgetBlueprint* WidgetBlueprint);

	/*
	Sets a literal value on an input pin of an event binding, then recompiles the WidgetBlueprint if the edit changed it.
	A pin bound to a property path is refused. If the recompile fails the value is kept; use CompileWidgetBlueprint to
	read the errors. Not supported during PIE.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param Event - Event binding to change, from ListWidgetViewEventBindings
	@param ArgumentName - ArgumentName from ListWidgetViewEventBindings
	@param Value - Literal value (e.g. "true", "42", "0.5"); empty resets the pin to its default
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static void SetEventBindingArgumentLiteral(UWidgetBlueprint* WidgetBlueprint, UMVVMBlueprintViewEvent* Event, const FString& ArgumentName, const FString& Value);

	/*
	Adds a condition to the WidgetBlueprint's View: when the source value changes and compares true against Value (Value to
	MaxValue for a Between operation), the destination is called or set. The widget blueprint is then compiled; compile
	errors do not fail the call and are reported in the condition's CompileErrors. Not supported during PIE.

	Values are compared as 32-bit floats. A destination function is called with default parameter values, and a destination
	property is set to its type's default value, not to the source value.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param SourceContext - Object that contains the source property.
		Can be the following:
		- UClass: source property is from a widget blueprint's ViewModel
		- Null: source property is from the widget blueprint
	@param SourcePropertyPath - Property or getter to compare. Contains a dot '.' separated path to the field (path.SubField). Must be a bool, uint8 (not an enum), int32, int64, float or double. Case-sensitive
	@param DestinationContext - Object that contains the destination.
		Can be the following:
		- UWidget: destination is on a widget in the widget blueprint's widgettree
		- UClass: destination is on a widget blueprint's ViewModel
		- Null: destination is on the widget blueprint
	@param DestinationPropertyPath - Function to call or property to set when the condition is true. Contains a dot '.' separated path to the field (path.SubField)
	@param Operation - Comparison of the source value against Value
	@param Value - Value the source is compared against; the lower bound of a Between operation
	@param MaxValue - Upper bound of a Between operation
	@return The new condition
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static UMVVMBlueprintViewCondition* AddViewCondition(UWidgetBlueprint* WidgetBlueprint, UObject* SourceContext, const FString& SourcePropertyPath, UObject* DestinationContext, const FString& DestinationPropertyPath, EMVVMConditionOperation Operation, float Value, float MaxValue = 0.0f);

	/*
	List all conditions on the WidgetBlueprint's View.

	@param WidgetBlueprint - WidgetBlueprint to use
	@return A description of every condition on the Blueprint
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static TArray<FMVVMViewConditionDescription> ListViewConditions(UWidgetBlueprint* WidgetBlueprint);

	/*
	Sets the comparison of an existing condition, then compiles the widget blueprint; compile errors do not fail the call
	and are reported in the condition's CompileErrors. Not supported during PIE.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param Condition - Condition to change
	@param Operation - New comparison of the source value against Value
	@param Value - New value the source is compared against; the lower bound of a Between operation
	@param MaxValue - New upper bound of a Between operation
	@return true if the operation was applied, false on failure
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static bool SetViewConditionOperation(UWidgetBlueprint* WidgetBlueprint, UMVVMBlueprintViewCondition* Condition, EMVVMConditionOperation Operation, float Value, float MaxValue = 0.0f);

	/*
	Removes a condition from the WidgetBlueprint's View. Not supported during PIE.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param Condition - Condition to remove
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static void RemoveViewCondition(UWidgetBlueprint* WidgetBlueprint, UMVVMBlueprintViewCondition* Condition);

	/*
	Turns the View Bindings design-time preview on or off for a WidgetBlueprint, exactly like the
	eye toggle on the View Bindings panel toolbar. The WidgetBlueprint's designer must be open for
	the canvas to update; the state is stored either way. No-ops while the per-user Enable
	Design-Time View Model Preview editor preference is off.

	@param WidgetBlueprint - WidgetBlueprint to use
	@param bActive - True to enable the design-time preview, false to disable it.
	@return True if the preview state matches bActive after the call.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Write"), Category = "MVVM")
	static bool SetWidgetPreviewActive(UWidgetBlueprint* WidgetBlueprint, bool bActive);

	/*
	Reads the View Bindings design-time preview toggle state for a WidgetBlueprint.

	@param WidgetBlueprint - WidgetBlueprint to use
	@return True when the design-time preview is active.
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static bool GetWidgetPreviewActive(UWidgetBlueprint* WidgetBlueprint);

	/*
	Exports a property value from the live design-time preview widget of an open WidgetBlueprint
	designer. Use after SetWidgetPreviewActive to assert what the binding preview wrote to the canvas.

	@param WidgetBlueprint - WidgetBlueprint whose designer is open
	@param WidgetName - Name of the widget in the tree to read from; None reads from the preview UUserWidget itself.
	@param PropertyName - Name of the property to export.
	@return The property value as exported text; empty on failure (a script error is raised).
	*/
	UFUNCTION(BlueprintCallable, meta = (Experimental, AICallable, AIAccessMode = "Read"), Category = "MVVM")
	static FString ReadPreviewWidgetProperty(UWidgetBlueprint* WidgetBlueprint, FName WidgetName, FName PropertyName);

	//~ Begin UToolsetDefinition
	// Hides CreateViewModel (authors a new asset) when the host is restricted.
	virtual TArray<FName> GetToolsToHide_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const override;
	//~ End UToolsetDefinition

private:
	/*
	Resolves FieldType/FunctionArgumentType/FunctionReturnType by re-finding the field via
	BindingHelper::FindFieldByName(OwningClass, FieldName). Use the FMVVMConstFieldVariant overload
	instead when the field is already resolved (e.g. a struct-nested property path leaf, whose owner
	isn't a UClass).
	*/
	static FMVVMBindableFieldDescription DescribeBindableField(UClass* OwningClass, FName FieldName, bool bIsReadable, bool bIsWritable, bool bHasNotify);

	// Same as above, computed directly from an already-resolved Field. Works whether Field's owner
	// is a UClass or a UScriptStruct.
	static FMVVMBindableFieldDescription DescribeBindableField(const UE::MVVM::FMVVMConstFieldVariant& Field, bool bIsReadable, bool bIsWritable, bool bHasNotify);
};
