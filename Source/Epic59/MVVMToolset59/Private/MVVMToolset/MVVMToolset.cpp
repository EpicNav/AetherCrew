// Copyright Epic Games, Inc. All Rights Reserved.

#include "MVVMToolset/MVVMToolset59.h"

#include "AssetRegistry/AssetRegistryHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "BlueprintEditorLibrary.h"
#include "Bindings/MVVMBindingHelper.h"
#include "Bindings/MVVMConversionFunctionHelper.h"
#include "Blueprint/BlueprintSupport.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Editor/EditorEngine.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Extensions/WidgetBlueprintGeneratedClassExtension.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h" // UE 5.8: 5.9 renamed it KismetNameValidators.h
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Templates/ValueOrError.h"
#include "ScopedTransaction.h"
#include "MVVMBlueprintViewEvent.h"
#include "MVVMBlueprintViewCondition.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMBlueprintViewEvent.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMSubsystem.h"
#include "MVVMViewModelBase.h"
#include "MVVMWidgetBlueprintExtension_View.h"
#include "Types/MVVMAvailableBinding.h"
#include "Types/MVVMConditionOperation.h"
#include "Types/MVVMFieldVariant.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#include "ToolsetRegistry/ToolsetLibrary.h"
#include "ToolsetRegistry/ToolsetLibrary59.h" // UE 5.8: the UToolsetLibrary functions 5.9 added
#include "ToolsetRegistry/ToolsetPermissionProvider.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MVVMToolset59)

DEFINE_LOG_CATEGORY_STATIC(LogMVVMToolset, Log, All);

extern UNREALED_API UEditorEngine* GEditor;

namespace
{
	UMVVMBlueprintView* GetWidgetView(UWidgetBlueprint* WidgetBlueprint)
	{
		if (!IsValid(WidgetBlueprint))
		{
			UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to get MVVM view for invalid widget blueprint."));
			return nullptr;
		}

		UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
		if (!IsValid(MVVMEditorSubsystem))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get MVVM View for widget %s. UMVVMEditorSubsystem not loaded."), *WidgetBlueprint->GetName()));
			return nullptr;

		}

		UMVVMBlueprintView* View = MVVMEditorSubsystem->RequestView(WidgetBlueprint);
		if (!IsValid(View))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get MVVM view for widget blueprint '%s'. UMVVMWidgetBlueprintExtension_View missing Blueprint View instance"), *WidgetBlueprint->GetName()));
			return nullptr;
		}

		return View;
	}

	// A Blueprint ViewModel is backed by several UClass objects: the authoritative GeneratedClass, the SKEL_ skeleton
	// class and transient REINST_/TRASH_ copies left behind by recompiles. Only the GeneratedClass can be registered on
	// a widget and bound against (the widget fails to compile otherwise), so map the others back to it.
	UClass* GetAuthoritativeViewModelClass(UClass* Class)
	{
		if (Class == nullptr)
		{
			return nullptr;
		}

		if (const UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(Class))
		{
			return Blueprint->GeneratedClass;
		}

		return Class->HasAnyClassFlags(CLASS_NewerVersionExists) ? nullptr : Class;
	}

	// Properties added to a Blueprint ViewModel (e.g. via AddViewModelProperty) only regenerate its skeleton class, the
	// GeneratedClass does not contain them until the Blueprint is compiled. Compile on demand so the ViewModel can be
	// bound against without requiring a manual compile step.
	// Non-class contexts (widgets, null) and native ViewModels are ignored.
	bool EnsureViewModelCompiled(const UObject* ViewModelContext, const TCHAR* Caller)
	{
		UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(Cast<UClass>(ViewModelContext));
		if (Blueprint == nullptr || Blueprint->IsUpToDate())
		{
			return true;
		}

		if (GEditor->IsPlayingSessionInEditor())
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s: ViewModel '%s' has uncompiled changes and cannot be compiled during PIE."), Caller, *Blueprint->GetPathName()));
			return false;
		}

		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
		if (Blueprint->Status == BS_Error)
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s: ViewModel '%s' failed to compile. Fix its compile errors before using it."), Caller, *Blueprint->GetPathName()));
			return false;
		}
		return true;
	}

	// Normalizes a binding context that refers to a ViewModel class (see GetAuthoritativeViewModelClass). Non-class contexts
	// (widgets, null for the widget blueprint itself) pass through unchanged. This has no side effects, so callers should
	// normalize all their contexts before compiling any of them via EnsureViewModelCompiled.
	bool NormalizeViewModelContext(UObject*& Context, const TCHAR* Caller)
	{
		UClass* ContextClass = Cast<UClass>(Context);
		if (ContextClass == nullptr)
		{
			return true;
		}

		UClass* ViewModelClass = GetAuthoritativeViewModelClass(ContextClass);
		if (ViewModelClass == nullptr)
		{
			const UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(ContextClass);
			if (Blueprint != nullptr && Blueprint->GeneratedClass == nullptr)
			{
				UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s: ViewModel '%s' has no generated class. Compile it and fix its compile errors before using it."), Caller, *Blueprint->GetPathName()));
			}
			else
			{
				UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s: Class '%s' is a stale class left over from a recompile. Use ListViewModels to get the current class."), Caller, *ContextClass->GetName()));
			}
			return false;
		}

		Context = ViewModelClass;
		return true;
	}

	// Read-only tools must not compile a ViewModel (unlike CreateViewBinding), so they resolve a Blueprint ViewModel context
	// against its skeleton class instead. It already contains properties added since the last compile (e.g. via
	// AddViewModelProperty), and ResolvePropertyPath matches it to the ViewModel registered on the widget.
	UObject* GetNewestViewModelContext(UObject* Context)
	{
		if (const UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(Cast<UClass>(Context)))
		{
			if (Blueprint->SkeletonGeneratedClass != nullptr)
			{
				return Blueprint->SkeletonGeneratedClass;
			}
		}
		return Context;
	}

	void BuildPropertyChains_Recursive(const UStruct* Struct, TArray<const FProperty*>& Property, TArray<TArray<const FProperty*>>& OutPropertyChain)
      {
          for (TFieldIterator<FProperty> It(Struct); It; ++It)
          {
              const FProperty* CurrentProperty = *It;
              Property.Add(CurrentProperty);
              OutPropertyChain.Add(Property);

              if (const FStructProperty* StructProp = CastField<FStructProperty>(CurrentProperty))
              {
                  BuildPropertyChains_Recursive(StructProp->Struct, Property, OutPropertyChain);
              }

              Property.Pop();
          }
      }

	// Resolves a property path's leaf field to a comparable FProperty*, for ArePropertiesCompatible/
	// GetConversionFunctions.
	const FProperty* ResolvePropertyForConversion(UWidgetBlueprint* WidgetBlueprint, const FMVVMBlueprintPropertyPath& Path)
	{
		if (!Path.IsValid())
		{
			return nullptr;
		}

		const TArray<UE::MVVM::FMVVMConstFieldVariant> Fields = Path.GetCompleteFields(WidgetBlueprint);
		if (Fields.IsEmpty() || Fields.Last().IsEmpty())
		{
			return nullptr;
		}

		const UE::MVVM::FMVVMConstFieldVariant& LeafField = Fields.Last();
		if (!LeafField.IsProperty())
		{
			return nullptr;
		}

		return LeafField.GetProperty();
	}

	// GetCPPType() alone drops container inner types (e.g. "TArray" instead of "TArray<int32>").
	FString GetFullCPPTypeName(const FProperty* Property)
	{
		if (!Property)
		{
			return FString();
		}

		FString InnerType;
		FString CppType = Property->GetCPPType(&InnerType);
		if (InnerType.Len())
		{
			CppType.Append(InnerType);
		}
		return CppType;
	}

	// The source types a condition's Value pin links to.
	bool IsSupportedConditionSourceType(const FProperty* Property)
	{
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum == nullptr;
		}
		return CastField<FBoolProperty>(Property) != nullptr
			|| CastField<FIntProperty>(Property) != nullptr
			|| CastField<FInt64Property>(Property) != nullptr
			|| CastField<FFloatProperty>(Property) != nullptr
			|| CastField<FDoubleProperty>(Property) != nullptr;
	}
}

FMVVMBindableFieldDescription UMVVMToolset59::DescribeBindableField(const UE::MVVM::FMVVMConstFieldVariant& Field, bool bIsReadable, bool bIsWritable, bool bHasNotify)
{
	FMVVMBindableFieldDescription Description;
	Description.FieldName = Field.GetName().ToString();
	Description.bIsReadable = bIsReadable;
	Description.bIsWritable = bIsWritable;
	Description.bHasNotify = bHasNotify;

	// Informational only -- doesn't gate what's returned.
	if (Field.IsProperty())
	{
		if (const FProperty* Property = Field.GetProperty())
		{
			Description.bIsPrivate = Property->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPrivate);
			Description.bIsProtected = Property->HasAnyPropertyFlags(CPF_NativeAccessSpecifierProtected);
		}
	}
	else if (Field.IsFunction())
	{
		if (const UFunction* Function = Field.GetFunction())
		{
			Description.bIsPrivate = Function->HasAnyFunctionFlags(FUNC_Private);
			Description.bIsProtected = Function->HasAnyFunctionFlags(FUNC_Protected);
		}
	}

	// Prefers the readable (getter) side for function-backed fields with independently-typed getter/setter.
	const TValueOrError<const FProperty*, FText> PropertyTypeResult = bIsReadable
		? UE::MVVM::BindingHelper::TryGetPropertyTypeForSourceBinding(Field)
		: UE::MVVM::BindingHelper::TryGetPropertyTypeForDestinationBinding(Field);
	if (PropertyTypeResult.HasValue())
	{
		Description.FieldType = GetFullCPPTypeName(PropertyTypeResult.GetValue());
	}

	// Getters have no argument (FunctionArgumentType stays empty); setters return void (FunctionReturnType
	// stays empty). Both stay empty for a plain property.
	if (Field.IsFunction())
	{
		if (const FProperty* ArgumentProperty = UE::MVVM::BindingHelper::GetFirstArgumentProperty(Field.GetFunction()))
		{
			Description.FunctionArgumentType = GetFullCPPTypeName(ArgumentProperty);
		}
		if (const FProperty* ReturnProperty = UE::MVVM::BindingHelper::GetReturnProperty(Field.GetFunction()))
		{
			Description.FunctionReturnType = GetFullCPPTypeName(ReturnProperty);
		}
	}

	return Description;
}

// Re-resolves Field via FindFieldByName, then delegates to the overload above. FieldName is kept from
// the argument rather than the resolved field, so a resolution failure still reports the name looked up.
FMVVMBindableFieldDescription UMVVMToolset59::DescribeBindableField(UClass* OwningClass, FName FieldName, bool bIsReadable, bool bIsWritable, bool bHasNotify)
{
	if (OwningClass == nullptr)
	{
		FMVVMBindableFieldDescription Description;
		Description.FieldName = FieldName.ToString();
		Description.bIsReadable = bIsReadable;
		Description.bIsWritable = bIsWritable;
		Description.bHasNotify = bHasNotify;
		return Description;
	}

	const UE::MVVM::FMVVMFieldVariant Field = UE::MVVM::BindingHelper::FindFieldByName(OwningClass, FMVVMBindingName(FieldName));
	FMVVMBindableFieldDescription Description = DescribeBindableField(UE::MVVM::FMVVMConstFieldVariant(Field), bIsReadable, bIsWritable, bHasNotify);
	Description.FieldName = FieldName.ToString();
	return Description;
}

TArray<FName> UMVVMToolset59::GetToolsToHide_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	if (!Provider->IsRestricted())
	{
		return {};
	}

	// CreateViewModel authors a new asset, which a restricted host shouldn't offer.
	return { GET_FUNCTION_NAME_CHECKED(UMVVMToolset59, CreateViewModel) };
}

void UMVVMToolset59::FixupMVVMData(UWidgetBlueprint* WidgetBlueprint)
{
	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to Fixup MVVM data widget '%s'. Invalid MVVM View."), *WidgetBlueprint->GetPathName()));
		return;
	}
		
	View->RecreateGeneratedGraphs();
}

UBlueprint* UMVVMToolset59::CreateViewModel(const FString& ViewModelName, const FString& PackagePath, UClass* ParentClass)
{
	if (ParentClass == nullptr)
	{
		ParentClass = UMVVMViewModelBase::StaticClass();
	}

	if (!ParentClass  || !ParentClass->IsChildOf(UMVVMViewModelBase::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create ViewModel '%s'. ParentClassName '%s' does not refer to a UMVVMViewModelBase subclass."), *ViewModelName, ParentClass ? *ParentClass->GetName() : TEXT("null")));
		return nullptr;
	}

	if (!FToolsetLibrary59::IsClassAllowed(ParentClass)) // UE 5.8: 5.9 UToolsetLibrary addition
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("CreateViewModel: ParentClass '%s' is not permitted in this project."), *ParentClass->GetName()));
		return nullptr;
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("CreateViewModel: Not supported during PIE.")));
		return nullptr;
	}

	if (!FToolsetLibrary59::ValidateAssetPath(PackagePath)) // UE 5.8: 5.9 UToolsetLibrary addition
	{
		return nullptr;
	}

	const FString FullPackageName = PackagePath / ViewModelName;
	FText PackageNameError;
	if(!FPackageName::IsValidLongPackageName(FullPackageName, false, &PackageNameError))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create ViewModel '%s'. Invalid package path '%s'. %s"), *ViewModelName, *FullPackageName, *(PackageNameError.ToString())));
		return nullptr;
	}

	if (FPackageName::DoesPackageExist(FullPackageName))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create ViewModel '%s'. An asset already exists at '%s'."), *ViewModelName, *FullPackageName));
		return nullptr;
	}

	UPackage* Package = CreatePackage(*FullPackageName);
	if (!IsValid(Package))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create ViewModel '%s'. Failed to create package at '%s'."), *ViewModelName, *FullPackageName));
		return nullptr;
	}

	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = ParentClass;
	UBlueprint* Blueprint = Cast<UBlueprint>(Factory->FactoryCreateNew(UBlueprint::StaticClass(), Package, FName(*ViewModelName), RF_Public | RF_Standalone, nullptr, GWarn));
	if (!IsValid(Blueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create ViewModel '%s'. Blueprint '%s' could not be created."), *ViewModelName, *ParentClass->GetName()));
		Package->MarkAsGarbage();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(Blueprint);
	return Blueprint;
}

void UMVVMToolset59::AddViewModelProperty(UBlueprint* ViewModel, const FString& PropertyName, const FString& PropertyType, const FString& DefaultValue)
{
	if (!IsValid(ViewModel))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add ViewModel property '%s'. Invalid ViewModel."), *PropertyName));
		return;
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddViewModelProperty: Not supported during PIE."));
		return;
	}

	FKismetNameValidator Validator(ViewModel);
	EValidatorResult Result = Validator.IsValid(PropertyName);
	if (Result != EValidatorResult::Ok)
	{
		const FString ErrorMessage = FKismetNameValidator::GetErrorString(PropertyName, Result);
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add ViewModel property '%s' on ViewModel '%s'. %s"), *PropertyName, *ViewModel->GetPathName(), *ErrorMessage));
		return;
	}

	FName PinTypeCategory = *PropertyType;
	FName PinTypeSubCategory = NAME_None;

	// Add support for float/double. PC_Real is the pin category, Float and Double are the SubCategory
	if (PropertyType == UEdGraphSchema_K2::PC_Float || PropertyType == UEdGraphSchema_K2::PC_Double)
	{
		PinTypeCategory = UEdGraphSchema_K2::PC_Real;
		PinTypeSubCategory = *PropertyType;
	}

	FEdGraphPinType PinType = UBlueprintEditorLibrary::GetBasicTypeByName(PinTypeCategory);
	if (!PinTypeSubCategory.IsNone())
	{
		PinType.PinSubCategory = PinTypeSubCategory;
	}
	
	// GetBasicTypeByName defaults to int. If int was not provided, fail
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int && PropertyType != UEdGraphSchema_K2::PC_Int)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add ViewModel property '%s' on ViewModel '%s'. Unsupported property type '%s'"), *PropertyName, *ViewModel->GetPathName(), *PropertyType));
		return;
	}

	if (!FBlueprintEditorUtils::AddMemberVariable(ViewModel, *PropertyName, PinType, DefaultValue))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add ViewModel property '%s' on ViewModel '%s'."), *PropertyName, *ViewModel->GetPathName()));
		return;
	}

	// Mark variable with FieldNotify metadata
	FBlueprintEditorUtils::SetBlueprintVariableMetaData(ViewModel, *PropertyName, nullptr, FBlueprintMetadata::MD_FieldNotify, TEXT(""));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ViewModel);
}

TArray<UClass*> UMVVMToolset59::ListViewModels(const FString& SearchPath)
{
	// Callers may include a trailing slash (e.g. "/Game/UI/"), but package paths on the asset registry
	// and on UClass packages never do, so normalize it away before comparing.
	FString NormalizedSearchPath = SearchPath;
	NormalizedSearchPath.RemoveFromEnd(TEXT("/"));

	auto MatchesSearchPath = [&NormalizedSearchPath](const UClass* Class) -> bool
	{
		if (Class == nullptr)
		{
			return false;
		}
		if (NormalizedSearchPath.IsEmpty())
		{
			return true;
		}
		const FString PackagePath = FPackageName::GetLongPackagePath(Class->GetPackage()->GetName());
		return PackagePath == NormalizedSearchPath || PackagePath.StartsWith(NormalizedSearchPath + TEXT("/"));
	};

	// GetDerivedClasses walks the in-memory class hierarchy and has no filter option, so
	// we need to filter its results by the search path ourselves. It also returns the SKEL_/REINST_
	// classes of Blueprint ViewModels, which can't be used on a widget, so only keep the authoritative ones.
	TArray<UClass*> Results;
	{
		TArray<UClass*> LoadedClasses;
		GetDerivedClasses(UMVVMViewModelBase::StaticClass(), LoadedClasses, true);
		for (UClass* Class : LoadedClasses)
		{
			// A leftover SKEL_/REINST_/TRASH_ class can still derive from the ViewModel base after its Blueprint was
			// reparented, so check the mapped class again.
			UClass* ViewModelClass = GetAuthoritativeViewModelClass(Class);
			if (ViewModelClass != nullptr && ViewModelClass->IsChildOf(UMVVMViewModelBase::StaticClass()) && MatchesSearchPath(ViewModelClass))
			{
				Results.AddUnique(ViewModelClass);
			}
		}
	}

	FARFilter Filter;
	Filter.ClassPaths.Add(UMVVMViewModelBase::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	if (!NormalizedSearchPath.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*NormalizedSearchPath));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> AssetList;
	UAssetRegistryHelpers::GetBlueprintAssets(Filter, AssetList);
	for (const FAssetData& AssetData : AssetList)
	{
		// We resolve each asset's generated class (the actual ViewModel class) rather than the UBlueprint wrapper that
		// FAssetData::GetClass() would return.
		FString GeneratedClassPath;
		if (AssetData.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClassPath))
		{
			const FString ClassObjectPath = FPackageName::ExportTextPathToObjectPath(GeneratedClassPath);
			if (UClass* GeneratedClass = LoadClass<UMVVMViewModelBase>(nullptr, *ClassObjectPath))
			{
				Results.AddUnique(GeneratedClass);
			}
		}
	}

	Results.RemoveAll([](const UClass* Class) { return !FToolsetLibrary59::IsClassAllowed(Class); }); // UE 5.8: 5.9 UToolsetLibrary addition
	return Results;
}

TArray<UClass*> UMVVMToolset59::ListWidgetViewModels(UWidgetBlueprint* WidgetBlueprint)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to list ViewModels. Invalid widget blueprint."));
		return TArray<UClass*>();
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to list ViewModels used by widget '%s'. Invalid MVVM View."), *WidgetBlueprint->GetPathName()));
		return TArray<UClass*>();
	}

	TArray<UClass*> Results;
	for(const FMVVMBlueprintViewModelContext& ViewModelContext : View->GetViewModels())
	{
		if (UClass* Class = ViewModelContext.GetViewModelClass())
		{
			Results.Add(Class);
		}
	}
	return Results;
}

void UMVVMToolset59::AddViewModelToWidget(UWidgetBlueprint* WidgetBlueprint, UClass* ViewModelClass)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to add ViewModel to widget. Invalid widget blueprint."));
		return;
	}

	if (!ViewModelClass)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddViewModelToWidget: ViewModelClass is required."));
		return;
	}

	// Registering a SKEL_ class would make the widget fail to compile, so register the authoritative class instead.
	UObject* ViewModelContext = ViewModelClass;
	if (!NormalizeViewModelContext(ViewModelContext, TEXT("AddViewModelToWidget")))
	{
		return;
	}
	ViewModelClass = CastChecked<UClass>(ViewModelContext);

	if (!FToolsetLibrary59::IsClassAllowed(ViewModelClass)) // UE 5.8: 5.9 UToolsetLibrary addition
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AddViewModelToWidget: ViewModelClass '%s' is not permitted in this project."), *ViewModelClass->GetName()));
		return;
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddViewModelToWidget: Not supported during PIE."));
		return;
	}

	if (!EnsureViewModelCompiled(ViewModelClass, TEXT("AddViewModelToWidget")))
	{
		return;
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add ViewModel to widget '%s'. Invalid MVVM View."), *WidgetBlueprint->GetPathName()));
		return;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to remove binding. UMVVMEditorSubsystem not loaded.")));
		return;
	}

	MVVMEditorSubsystem->AddViewModel(WidgetBlueprint, ViewModelClass);
}

namespace
{
	// Fills one direction of a binding's conversion (function name + input-pin sources).
	// When bSourceToDestination is true this extracts the forward (source -> destination)
	// conversion; otherwise the backward (destination -> source) conversion, covering TwoWay and
	// OneWayToSource bindings. Returns true when a conversion function is present for that
	// direction, in which case the matching direct property path (SourcePath / DestinationPath) is
	// emptied and the real inputs live on the conversion function's argument pins. Mirrors the
	// per-direction extraction in UE::MVVM::Private::GetBindingPathName so a multi-argument
	// conversion is represented correctly.
	bool PopulateBindingConversion(const UWidgetBlueprint* WidgetBlueprint, const FMVVMBlueprintViewBinding& Binding, bool bSourceToDestination, FMVVMViewBindingConversion& OutConversion)
	{
		const UMVVMBlueprintViewConversionFunction* ConversionFunction = Binding.Conversion.GetConversionFunction(bSourceToDestination);
		if (!ConversionFunction)
		{
			return false;
		}

		// An unresolved function reference (e.g. a Node whose class is gone) has no name. Report it the
		// same way GetBindingPathName does instead of the "None" string of an empty FName.
		const FName FunctionName = ConversionFunction->GetConversionFunction().GetName();
		OutConversion.FunctionName = FunctionName.IsNone() ? TEXT("<ERROR>") : FunctionName.ToString();
		for (const FMVVMBlueprintPin& Pin : ConversionFunction->GetPins())
		{
			// A split struct pin carries no value of its own; its inputs are the sub-pins that follow it.
			if (Pin.IsSplit())
			{
				continue;
			}

			FMVVMViewBindingSource& Source = OutConversion.Sources.Emplace_GetRef();
			Source.ArgumentName = Pin.GetId().ToString();
			if (Pin.UsedPathAsValue())
			{
				Source.Path = Pin.GetPath().ToString(WidgetBlueprint, false, false);
			}
			else
			{
				// Unbound pin: report its literal default value instead of a property path.
				Source.Path = Pin.GetValueAsString(WidgetBlueprint->SkeletonGeneratedClass);
				Source.bIsLiteralValue = true;
			}
		}
		return true;
	}
}

TArray<FMVVMViewBindingDescription> UMVVMToolset59::ListWidgetViewBindings(UWidgetBlueprint* WidgetBlueprint)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to list view bindings. Invalid widget blueprint."));
		return TArray<FMVVMViewBindingDescription>();
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get View for widget '%s'."), *WidgetBlueprint->GetPathName()));
		return TArray<FMVVMViewBindingDescription>();
	}

	TArray<FMVVMViewBindingDescription> Results;
	for (const FMVVMBlueprintViewBinding& Binding : View->GetBindings())
	{
		FMVVMViewBindingDescription& Description = Results.Emplace_GetRef();
		Description.BindingId = Binding.BindingId;
		Description.DisplayName = Binding.GetDisplayNameString(WidgetBlueprint, true);
		Description.BindingType = Binding.BindingType;
		Description.bEnabled = Binding.bEnabled;
		Description.bCompile = Binding.bCompile;

		// Source (forward) endpoint: a source-to-destination conversion, or a direct property path.
		if (!PopulateBindingConversion(WidgetBlueprint, Binding, /*bSourceToDestination*/true, Description.SourceToDestinationConversion)
			&& Binding.SourcePath.IsValid())
		{
			Description.SourcePath = Binding.SourcePath.ToString(WidgetBlueprint, false, false);
		}

		// Destination (backward) endpoint: a destination-to-source conversion, or a direct property path.
		if (!PopulateBindingConversion(WidgetBlueprint, Binding, /*bSourceToDestination*/false, Description.DestinationToSourceConversion)
			&& Binding.DestinationPath.IsValid())
		{
			Description.DestinationPath = Binding.DestinationPath.ToString(WidgetBlueprint, false, false);
		}
	}

	return Results;
}

void UMVVMToolset59::RemoveWidgetViewBinding(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingId)
{
	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveWidgetViewBinding: Not supported during PIE."));
		return;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to remove binding. UMVVMEditorSubsystem not loaded.")));
		return;
	}

	FString ErrorMessage;
	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to remove binding. Invalid MVVM View")));
		return;
	}

	FMVVMBlueprintViewBinding* Binding = View->GetBinding(BindingId);
	if (Binding == nullptr)
	{	
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to remove binding. Invalid BindingId '%s'."), *BindingId.ToString()));
		return;
	}

	MVVMEditorSubsystem->RemoveBinding(WidgetBlueprint, *Binding);
	if (View->GetBinding(BindingId) != nullptr)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to remove binding due to an unknown reason.")));
	}
}

void UMVVMToolset59::SetWidgetViewBindingEnabled(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingID, bool bEnabled, bool bCompile)
{
	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set binding enabled state. UMVVMEditorSubsystem not loaded.")));
		return;
	}
	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetWidgetViewBindingEnabled: Not supported during PIE."));
		return;
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set binding enabled state. Invalid MVVM View")));
		return;
	}

	FMVVMBlueprintViewBinding* Binding = View->GetBinding(BindingID);
	if (Binding == nullptr)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set binding enabled state. Invalid BindingId '%s'."), *BindingID.ToString()));
		return;
	}

	MVVMEditorSubsystem->SetEnabledForBinding(WidgetBlueprint, *Binding, bEnabled);
	MVVMEditorSubsystem->SetCompileForBinding(WidgetBlueprint, *Binding, bCompile);
}

bool UMVVMToolset59::SetBindingMode(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingID, EMVVMBindingMode Mode)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to set binding mode. Invalid widget blueprint."));
		return false;
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetBindingMode: Not supported during PIE."));
		return false;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set binding mode. UMVVMEditorSubsystem not loaded.")));
		return false;
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set binding mode. Invalid MVVM View")));
		return false;
	}

	FMVVMBlueprintViewBinding* Binding = View->GetBinding(BindingID);
	if (Binding == nullptr)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set binding mode. Invalid BindingId '%s'."), *BindingID.ToString()));
		return false;
	}

	MVVMEditorSubsystem->SetBindingTypeForBinding(WidgetBlueprint, *Binding, Mode);
	return true;
}

void UMVVMToolset59::SetConversionFunctionArgumentLiteral(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingID, const FString& ArgumentName, const FString& Value, bool bSourceToDestination)
{
	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetConversionFunctionArgumentLiteral: Not supported during PIE."));

		return;
	}

	const TCHAR* FailurePrefix = TEXT("Failed to set conversion function argument");

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. UMVVMEditorSubsystem not loaded."), FailurePrefix, *ArgumentName));
		
		return;
	}

	FText Error;
	if (!MVVMEditorSubsystem->SetConversionFunctionArgumentLiteral(WidgetBlueprint, BindingID, ArgumentName, Value, bSourceToDestination, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. %s"), FailurePrefix, *ArgumentName, *Error.ToString()));
		
		return;
	}

	// Only an edit marks the blueprint as modified. A failed compile keeps the edit; the caller reads the errors with CompileWidgetBlueprint.
	if (!WidgetBlueprint->IsUpToDate())
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
		if (WidgetBlueprint->Status == BS_Error)
		{
			UE_LOGF(LogMVVMToolset, Warning, "Conversion function argument '%ls' was edited, but widget blueprint '%ls' failed to compile. Use CompileWidgetBlueprint to read the errors.", *ArgumentName, *WidgetBlueprint->GetPathName());
		}
	}
}

void UMVVMToolset59::ClearConversionFunctionArgumentPath(UWidgetBlueprint* WidgetBlueprint, const FGuid& BindingID, const FString& ArgumentName, bool bSourceToDestination)
{
	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("ClearConversionFunctionArgumentPath: Not supported during PIE."));

		return;
	}

	const TCHAR* FailurePrefix = TEXT("Failed to clear the path of conversion function argument");

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. UMVVMEditorSubsystem not loaded."), FailurePrefix, *ArgumentName));

		return;
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. Invalid MVVM View."), FailurePrefix, *ArgumentName));

		return;
	}

	FMVVMBlueprintViewBinding* Binding = View->GetBinding(BindingID);
	if (Binding == nullptr)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. Invalid BindingId '%s'."), FailurePrefix, *ArgumentName, *BindingID.ToString()));

		return;
	}

	if (Binding->Conversion.GetConversionFunction(bSourceToDestination) == nullptr)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. The binding has no %s conversion function."), FailurePrefix, *ArgumentName, bSourceToDestination ? TEXT("source to destination") : TEXT("destination to source")));

		return;
	}

	TArray<FString> Segments;
	ArgumentName.ParseIntoArray(Segments, TEXT("."));
	TArray<FName> Names;
	for (const FString& Segment : Segments)
	{
		Names.Add(FName(*Segment));
	}
	const FMVVMBlueprintPinId PinId(MoveTemp(Names));

	const UEdGraphPin* Pin = PinId.GetNames().Num() > 0 ? MVVMEditorSubsystem->GetConversionFunctionArgumentPin(WidgetBlueprint, *Binding, PinId, bSourceToDestination) : nullptr;
	if (Pin == nullptr)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. The conversion function has no such input pin."), FailurePrefix, *ArgumentName));

		return;
	}

	if (Pin->LinkedTo.Num() == 0)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. The pin is not bound to a property path."), FailurePrefix, *ArgumentName));

		return;
	}

	MVVMEditorSubsystem->SetPathForConversionFunctionArgument(WidgetBlueprint, *Binding, PinId, FMVVMBlueprintPropertyPath(), bSourceToDestination);

	// A failed compile keeps the edit; the caller reads the errors with CompileWidgetBlueprint.
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
	if (WidgetBlueprint->Status == BS_Error)
	{
		UE_LOGF(LogMVVMToolset, Warning, "Conversion function argument '%ls' was edited, but widget blueprint '%ls' failed to compile. Use CompileWidgetBlueprint to read the errors.", *ArgumentName, *WidgetBlueprint->GetPathName());
	}
}

TArray<FString> UMVVMToolset59::ListBindableWidgetProperties(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to list bindable widget properties. Invalid widget blueprint."));
		return TArray<FString>();
	}

	if (!IsValid(Widget))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to list bindable widget properties. Invalid widget."));
		return TArray<FString>();
	}

	// Mirror the Accessor fallback used elsewhere in this file (e.g. ListWidgetViewEvents):
	// freshly created/uncompiled blueprints may not have a GeneratedClass yet.
	const UClass* AccessorClass = WidgetBlueprint->GeneratedClass ? WidgetBlueprint->GeneratedClass : WidgetBlueprint->SkeletonGeneratedClass;

	TArray<FString> Results;
	const TArray<FMVVMAvailableBinding> Bindings = UMVVMSubsystem::GetAvailableBindings(Widget->GetClass(), AccessorClass);
	Results.Reserve(Bindings.Num());
	for (const FMVVMAvailableBinding& Binding : Bindings)
	{
		Results.Add(Binding.GetBindingName().ToString());
	}
	return Results;
}

FMVVMBindableFieldListResult UMVVMToolset59::ListClassBindableFields(UClass* TargetClass, UClass* AccessorClass)
{
	if (TargetClass == nullptr)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to list bindable fields. Invalid TargetClass."));
		return FMVVMBindableFieldListResult();
	}

	// A class with no INotifyFieldValueChanged (e.g. AActor) isn't an error; GetAvailableBindings already
	// reports bHasNotify = false for every field.
	const TArray<FMVVMAvailableBinding> AvailableBindings = UMVVMSubsystem::GetAvailableBindings(TargetClass, AccessorClass);

	FMVVMBindableFieldListResult Result;
	Result.AvailableFieldCount = AvailableBindings.Num();

	Result.Fields.Reserve(AvailableBindings.Num());
	for (const FMVVMAvailableBinding& Binding : AvailableBindings)
	{
		Result.Fields.Add(DescribeBindableField(TargetClass, Binding.GetBindingName().ToName(), Binding.IsReadable(), Binding.IsWritable(), Binding.HasNotify()));
	}
	return Result;
}

FMVVMBindableFieldListResult UMVVMToolset59::ListPropertyPathBindableFields(UWidgetBlueprint* WidgetBlueprint, UObject* Context, const FString& PropertyPath)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to list bindable fields. Invalid widget blueprint."));
		return FMVVMBindableFieldListResult();
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to list bindable fields for widget '%s'. UMVVMEditorSubsystem not loaded."), *WidgetBlueprint->GetName()));
		return FMVVMBindableFieldListResult();
	}

	Context = GetNewestViewModelContext(Context);

	FString Error;
	const FMVVMBlueprintPropertyPath ResolvedPath = MVVMEditorSubsystem->ResolvePropertyPath(WidgetBlueprint, Context, PropertyPath, Error);
	if (!Error.IsEmpty())
	{
		// An unresolved PropertyPath is a normal "not found" outcome, not a caller mistake -- return an
		// empty, non-error result, but keep the reason.
		FMVVMBindableFieldListResult Result;
		Result.ErrorMessage = Error;
		return Result;
	}

	// Uses GetCompleteFields, not GetFields -- see ResolvePropertyForConversion's comment above.
	const TArray<UE::MVVM::FMVVMConstFieldVariant> ResolvedFields = ResolvedPath.GetCompleteFields(WidgetBlueprint);
	if (ResolvedFields.IsEmpty() || ResolvedFields.Last().IsEmpty())
	{
		return FMVVMBindableFieldListResult();
	}

	const UE::MVVM::FMVVMConstFieldVariant& LeafField = ResolvedFields.Last();
	// GetOwner() returns a UScriptStruct (not UClass) for a struct-nested leaf (e.g. "ColorAndOpacity.R");
	// OwningClass legitimately stays null then. Only used for the notify-capable lookup below.
	UClass* OwningClass = Cast<UClass>(LeafField.GetOwner());
	const FName FieldName = LeafField.GetName();

	// Readable/writable stand in for source/destination binding validity.
	const bool bIsReadable = UE::MVVM::BindingHelper::TryGetPropertyTypeForSourceBinding(LeafField).HasValue();
	const bool bIsWritable = UE::MVVM::BindingHelper::TryGetPropertyTypeForDestinationBinding(LeafField).HasValue();

	// OwningClass is passed as both Container and Accessor, granting full access for this read-only lookup.
	bool bHasNotify = false;
	if (OwningClass != nullptr)
	{
		const FMVVMBindingName LeafBindingName(FieldName);
		for (const FMVVMAvailableBinding& AvailableBinding : UMVVMSubsystem::GetAvailableBindings(OwningClass, OwningClass))
		{
			if (AvailableBinding.GetBindingName() == LeafBindingName)
			{
				bHasNotify = AvailableBinding.HasNotify();
				break;
			}
		}
	}

	FMVVMBindableFieldListResult Result;
	Result.Fields.Add(DescribeBindableField(LeafField, bIsReadable, bIsWritable, bHasNotify));
	Result.AvailableFieldCount = Result.Fields.Num();
	return Result;
}

FGuid UMVVMToolset59::CreateViewBinding(UWidgetBlueprint* WidgetBlueprint, UObject* SourceContext, const FString& SourcePropertyPath, UObject* DestinationContext, const FString& DestinationPropertyPath, FName ConversionName)
{
	FString Error;

	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to create view binding. Invalid widget blueprint."));
		return FGuid();
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CreateViewBinding: Not supported during PIE."));
		return FGuid();
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create binding. UMVVMEditorSubsystem not loaded.")));
		return FGuid();
	}

	if (!NormalizeViewModelContext(SourceContext, TEXT("CreateViewBinding")) || !NormalizeViewModelContext(DestinationContext, TEXT("CreateViewBinding")))
	{
		return FGuid();
	}

	if (!EnsureViewModelCompiled(SourceContext, TEXT("CreateViewBinding")) || !EnsureViewModelCompiled(DestinationContext, TEXT("CreateViewBinding")))
	{
		return FGuid();
	}

	const FMVVMBlueprintPropertyPath SourcePath = MVVMEditorSubsystem->ResolvePropertyPath(WidgetBlueprint, SourceContext, SourcePropertyPath, Error);
	if(!SourcePath.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create view binding. Invalid SourcePropertyPath. Reason: %s. If the field only exists on a subclass of an intermediate object's declared type, bind to that object and cast in a Pure Const conversion function on the WidgetBlueprint."), *Error));
		return FGuid();
	}

	const FMVVMBlueprintPropertyPath DestinationPath = MVVMEditorSubsystem->ResolvePropertyPath(WidgetBlueprint, DestinationContext, DestinationPropertyPath, Error);
	if(!DestinationPath.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create view binding. Invalid DestinationPropertyPath. Reason: %s"), *Error));
		return FGuid();
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create binding. Missing View.")));
		return FGuid();
	}

	// The subsystem calls below each open their own transaction. Group them so the binding is created with a single
	// undo step, and so a rejected binding (added, then removed again) doesn't leave an undo step that restores it.
	FScopedTransaction Transaction(NSLOCTEXT("MVVMToolset", "CreateViewBinding", "MVVM Create Binding"));

	FMVVMBlueprintViewBinding& NewBinding = MVVMEditorSubsystem->AddBinding(WidgetBlueprint);
	MVVMEditorSubsystem->SetSourcePathForBinding(WidgetBlueprint, NewBinding, SourcePath);
	MVVMEditorSubsystem->SetDestinationPathForBinding(WidgetBlueprint, NewBinding, DestinationPath, true);

	if (!MVVMEditorSubsystem->TryAutoConvertBinding(WidgetBlueprint, NewBinding, ConversionName) && !ConversionName.IsNone())
	{
		// Don't leave a binding behind that silently ignores the requested conversion
		MVVMEditorSubsystem->RemoveBinding(WidgetBlueprint, NewBinding);
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create view binding. Conversion function '%s' does not exist or can't convert from '%s' to '%s'. See ListConversionFunctions. WidgetBlueprint functions must be Pure and Const, take the source type as input, return the destination type, and be compiled first."),
			*ConversionName.ToString(), *SourcePropertyPath, *DestinationPropertyPath));
		return FGuid();
	}
	return NewBinding.BindingId;
}

FMVVMViewConfig UMVVMToolset59::GetWidgetViewConfig(UWidgetBlueprint* WidgetBlueprint)
{
	FMVVMViewConfig OutConfiguration;
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to get View Config. Invalid widget blueprint."));
		return OutConfiguration;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get View Config for widget '%s'. UMVVMEditorSubsystem not loaded."), *WidgetBlueprint->GetPathName()));
		return OutConfiguration;
	}

	UMVVMBlueprintView* View = MVVMEditorSubsystem->GetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get View Config for widget '%s'. No MVVM View on the widget blueprint."), *WidgetBlueprint->GetPathName()));
		return OutConfiguration;
	}

	UMVVMBlueprintViewSettings* ViewSettings = View->GetSettings();
	if (!IsValid(ViewSettings))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get View Config for widget '%s'. Configuration missing on View."), *WidgetBlueprint->GetPathName()));
		return OutConfiguration;
	}

	OutConfiguration.bInitializeSourcesOnConstruct = ViewSettings->bInitializeSourcesOnConstruct;
	OutConfiguration.bInitializeBindingsOnConstruct = ViewSettings->bInitializeBindingsOnConstruct;
	OutConfiguration.bInitializeEventsOnConstruct = ViewSettings->bInitializeEventsOnConstruct;
	OutConfiguration.bCreateViewWithoutBindings = ViewSettings->bCreateViewWithoutBindings;
	return OutConfiguration;
}

void UMVVMToolset59::SetWidgetViewConfig(UWidgetBlueprint* WidgetBlueprint, const FMVVMViewConfig& Configuration)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to set View Config. Invalid widget blueprint."));
		return;
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetWidgetViewConfig: Not supported during PIE."));
		return;
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set View Config for widget '%s'. No MVVM View on the widget blueprint."), *WidgetBlueprint->GetPathName()));
		return;
	}

	UMVVMBlueprintViewSettings* ViewSettings = View->GetSettings();
	if (!IsValid(ViewSettings))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set View Config for widget '%s'. Configuration missing on View."), *WidgetBlueprint->GetPathName()));
		return;
	}

	// Mirror UMVVMBlueprintViewSettings' EditCondition: Bindings cannot auto-init if Sources won't.
	const bool bClampedInitializeBindings = Configuration.bInitializeBindingsOnConstruct && Configuration.bInitializeSourcesOnConstruct;
	if (bClampedInitializeBindings != Configuration.bInitializeBindingsOnConstruct)
	{
		UE_LOGF(LogMVVMToolset, Warning, "SetWidgetViewConfig on '%ls': bInitializeBindingsOnConstruct forced to false because bInitializeSourcesOnConstruct is false.", *WidgetBlueprint->GetPathName());
	}

	WidgetBlueprint->Modify();
	View->Modify();
	ViewSettings->Modify();

	ViewSettings->bInitializeSourcesOnConstruct = Configuration.bInitializeSourcesOnConstruct;
	ViewSettings->bInitializeBindingsOnConstruct = bClampedInitializeBindings;
	ViewSettings->bInitializeEventsOnConstruct = Configuration.bInitializeEventsOnConstruct;
	ViewSettings->bCreateViewWithoutBindings = Configuration.bCreateViewWithoutBindings;

	// Only notify property-change listeners when the UMG Designer is opened to update the detail panel
	if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
	{
		if (AssetEditorSubsystem->FindEditorForAsset(WidgetBlueprint, /*bFocusIfOpen=*/false) != nullptr)
		{
			FPropertyChangedEvent ChangedEvent(nullptr, EPropertyChangeType::ValueSet);
			ViewSettings->PostEditChangeProperty(ChangedEvent);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
}

TArray<FMVVMViewConversionFunctionDescription59> UMVVMToolset59::ListConversionFunctions(UWidgetBlueprint* WidgetBlueprint)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to get available conversion functions for widget. Invalid widget blueprint."));
		return TArray<FMVVMViewConversionFunctionDescription59>();
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get available conversion functions for widget %s. UMVVMEditorSubsystem not loaded."), *WidgetBlueprint->GetName()));
		return TArray<FMVVMViewConversionFunctionDescription59>();
	}
	
	TArray<FMVVMViewConversionFunctionDescription59> Results;
	for (const UE::MVVM::FConversionFunctionValue& ConversionFunction : MVVMEditorSubsystem->GetConversionFunctions(WidgetBlueprint, nullptr, nullptr))
	{
		FMVVMViewConversionFunctionDescription59& Description = Results.Emplace_GetRef();
		Description.ConversionFunction = ConversionFunction.GetFunction();
		Description.ConversionNode = ConversionFunction.GetNode();
	}
	return Results;
}

FMVVMBindingConversionResult UMVVMToolset59::ListBindingConversionOptions(UWidgetBlueprint* WidgetBlueprint, UObject* SourceContext, const FString& SourcePropertyPath, UObject* DestinationContext, const FString& DestinationPropertyPath)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to list binding conversion options. Invalid widget blueprint."));
		return FMVVMBindingConversionResult();
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to list binding conversion options for widget %s. UMVVMEditorSubsystem not loaded."), *WidgetBlueprint->GetName()));
		return FMVVMBindingConversionResult();
	}

	SourceContext = GetNewestViewModelContext(SourceContext);
	DestinationContext = GetNewestViewModelContext(DestinationContext);

	FString Error;
	const FMVVMBlueprintPropertyPath SourcePath = MVVMEditorSubsystem->ResolvePropertyPath(WidgetBlueprint, SourceContext, SourcePropertyPath, Error);
	const FMVVMBlueprintPropertyPath DestinationPath = MVVMEditorSubsystem->ResolvePropertyPath(WidgetBlueprint, DestinationContext, DestinationPropertyPath, Error);

	// An unresolved or function-backed side isn't a caller mistake -- no script error is raised.
	const FProperty* SourceProperty = ResolvePropertyForConversion(WidgetBlueprint, SourcePath);
	const FProperty* DestinationProperty = ResolvePropertyForConversion(WidgetBlueprint, DestinationPath);
	if (SourceProperty == nullptr || DestinationProperty == nullptr)
	{
		return FMVVMBindingConversionResult();
	}

	FMVVMBindingConversionResult Result;

	// bDirectlyCompatible and ConversionOptions are always both computed independently of each other.
	Result.bDirectlyCompatible = UE::MVVM::BindingHelper::ArePropertiesCompatible(SourceProperty, DestinationProperty);

	const TArray<UE::MVVM::FConversionFunctionValue> ConversionFunctions = MVVMEditorSubsystem->GetConversionFunctions(WidgetBlueprint, SourceProperty, DestinationProperty);
	Result.AvailableConversionOptionCount = ConversionFunctions.Num();

	Result.ConversionOptions.Reserve(ConversionFunctions.Num());
	for (const UE::MVVM::FConversionFunctionValue& ConversionFunction : ConversionFunctions)
	{
		FMVVMConversionOptionDescription& Description = Result.ConversionOptions.Emplace_GetRef();
		Description.Name = ConversionFunction.GetName();
		Description.DisplayName = ConversionFunction.GetDisplayName().ToString();
		Description.Tooltip = ConversionFunction.GetTooltip().ToString();
		Description.FunctionCategory = ConversionFunction.GetCategory().ToString();
		Description.bIsFunction = ConversionFunction.IsFunction();

		// Node-backed options (bIsFunction == false) have no FProperty-based pin type; left empty
		// deliberately, not a gap.
		if (Description.bIsFunction)
		{
			// Use the first argument actually compatible with SourceProperty -- a multi-arg function's
			// first parameter isn't always the source-bound one.
			const TValueOrError<TArray<const FProperty*>, FText> ArgumentsResult = UE::MVVM::BindingHelper::TryGetArgumentsForConversionFunction(ConversionFunction.GetFunction());
			if (ArgumentsResult.HasValue())
			{
				for (const FProperty* ArgumentProperty : ArgumentsResult.GetValue())
				{
					if (UE::MVVM::BindingHelper::ArePropertiesCompatible(ArgumentProperty, SourceProperty))
					{
						Description.ExpectedArgumentType = GetFullCPPTypeName(ArgumentProperty);
						break;
					}
				}
			}
			if (const FProperty* ReturnProperty = UE::MVVM::BindingHelper::GetReturnProperty(ConversionFunction.GetFunction()))
			{
				Description.ExpectedReturnType = GetFullCPPTypeName(ReturnProperty);
			}
		}
	}

	return Result;
}

TArray<FMVVMViewEventBindingDescription> UMVVMToolset59::ListWidgetViewEventBindings(UWidgetBlueprint* WidgetBlueprint)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to list view event bindings for null widget")));
		return TArray<FMVVMViewEventBindingDescription>();
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to list view event bindings for widget '%s'. Invalid MVVM View."), *WidgetBlueprint->GetPathName()));
		return TArray<FMVVMViewEventBindingDescription>();
	}

	TArray<FMVVMViewEventBindingDescription> Results;
	for (UMVVMBlueprintViewEvent* Event : View->GetEvents())
	{
		if (!IsValid(Event))
		{
			continue;
		}

		const FMVVMBlueprintPropertyPath& EventPath = Event->GetEventPath();
		const FMVVMBlueprintPropertyPath& DestinationPath = Event->GetDestinationPath();

		FMVVMViewEventBindingDescription& Description = Results.Emplace_GetRef();
		Description.Event = Event;
		Description.DisplayName = Event->GetDisplayName(true).ToString();
		Description.EventPath = EventPath.ToString(WidgetBlueprint, false, false);
		Description.DestinationPath = DestinationPath.ToString(WidgetBlueprint, false, false);
		Description.EventWidgetName = EventPath.GetWidgetName();
		Description.EventViewModelId = EventPath.GetViewModelId();
		Description.bEnabled = Event->bEnabled;
		for (const FMVVMBlueprintPin& Pin : Event->GetPins())
		{
			// A split struct pin carries no value of its own; its inputs are the sub-pins that follow it.
			if (Pin.IsSplit())
			{
				continue;
			}

			FMVVMViewBindingSource& Source = Description.Arguments.Emplace_GetRef();
			Source.ArgumentName = Pin.GetId().ToString();
			if (Pin.UsedPathAsValue())
			{
				Source.Path = Pin.GetPath().ToString(WidgetBlueprint, false, false);
			}
			else
			{
				// Unbound pin: report its literal default value instead of a property path.
				Source.Path = Pin.GetValueAsString(WidgetBlueprint->SkeletonGeneratedClass);
				Source.bIsLiteralValue = true;
			}
		}
	}

	return Results;
}

UMVVMBlueprintViewEvent* UMVVMToolset59::CreateViewEventBinding(UWidgetBlueprint* WidgetBlueprint, UObject* EventContext, const FString& EventPropertyPath, UObject* DestinationContext, const FString& DestinationPropertyPath)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get Create view binding for null widget")));
		return nullptr;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create binding. UMVVMEditorSubsystem not loaded.")));
		return nullptr;
	}

	// Validate both contexts before compiling either, so a failing call doesn't recompile the event ViewModel.
	if (!NormalizeViewModelContext(EventContext, TEXT("CreateViewEventBinding")) || !NormalizeViewModelContext(DestinationContext, TEXT("CreateViewEventBinding")))
	{
		return nullptr;
	}

	if (!EnsureViewModelCompiled(EventContext, TEXT("CreateViewEventBinding")) || !EnsureViewModelCompiled(DestinationContext, TEXT("CreateViewEventBinding")))
	{
		return nullptr;
	}

	FString Error;
	const FMVVMBlueprintPropertyPath EventPath = MVVMEditorSubsystem->ResolvePropertyPath(WidgetBlueprint, EventContext, EventPropertyPath, Error);
	if(!EventPath.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create view binding. %s"), *Error));
		return nullptr;
	}

	const FMVVMBlueprintPropertyPath DestinationPath = MVVMEditorSubsystem->ResolvePropertyPath(WidgetBlueprint, DestinationContext, DestinationPropertyPath, Error);
	if(!DestinationPath.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create view binding. %s"), *Error));
		return nullptr;
	}

	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to create binding. Missing View.")));
		return nullptr;
	}

	UMVVMBlueprintViewEvent* Event = MVVMEditorSubsystem->AddEvent(WidgetBlueprint);
	if (!IsValid(Event))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add event binding to widget blueprint.")));
		return nullptr;
	}

	Event->SetEventPath(EventPath);
	Event->SetDestinationPath(DestinationPath);
	return Event;
}

TArray<FString> UMVVMToolset59::ListWidgetViewEvents(UWidgetBlueprint* WidgetBlueprint)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get available view events for null widget")));
		return TArray<FString>();
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get available view events for widget %s. UMVVMEditorSubsystem not loaded."), *WidgetBlueprint->GetName()));
		return TArray<FString>();
	}

	if (!IsValid(WidgetBlueprint->WidgetTree))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to get available view events for invalid widget.")));
		return TArray<FString>();
	}

	TArray<FString> OutEventProperties;

	{
		const UClass* RootClass = WidgetBlueprint->GeneratedClass ? WidgetBlueprint->GeneratedClass : WidgetBlueprint->SkeletonGeneratedClass;
		TArray<const FProperty*> Chain;
		TArray<TArray<const FProperty*>> AllChains;
		BuildPropertyChains_Recursive(RootClass, Chain, AllChains);

		for (const TArray<const FProperty*>& WidgetChain : AllChains)
		{
			FMVVMBlueprintPropertyPath BlueprintPropertyPath;
			BlueprintPropertyPath.SetSelfContext();
			for (const FProperty* Prop : WidgetChain)
			{
				BlueprintPropertyPath.AppendPropertyPath(WidgetBlueprint, UE::MVVM::FMVVMConstFieldVariant(Prop));
			}
			if (UMVVMBlueprintViewEvent::Supports(WidgetBlueprint, BlueprintPropertyPath))
			{
				OutEventProperties.Add(BlueprintPropertyPath.ToString(WidgetBlueprint, false, false));
			}
		}
	}

	WidgetBlueprint->WidgetTree->ForEachWidget([WidgetBlueprint, &OutEventProperties](UWidget* Widget)
	{
		if (!Widget)
		{
			return;
		}

		TArray<const FProperty*> Chain;
		TArray<TArray<const FProperty*>> AllChains;
		BuildPropertyChains_Recursive(Widget->GetClass(), Chain, AllChains);

		for (const TArray<const FProperty*>& WidgetChain : AllChains)
		{
			FMVVMBlueprintPropertyPath BlueprintPropertyPath;
			BlueprintPropertyPath.SetWidgetName(Widget->GetFName());
			for (const FProperty* Prop : WidgetChain)
			{
				BlueprintPropertyPath.AppendPropertyPath(WidgetBlueprint, UE::MVVM::FMVVMConstFieldVariant(Prop));
			}
			if (UMVVMBlueprintViewEvent::Supports(WidgetBlueprint, BlueprintPropertyPath))
			{
				OutEventProperties.Add(BlueprintPropertyPath.ToString(WidgetBlueprint, false, false));
			}
		}
	});


	// Add view model events
	UMVVMBlueprintView* View = GetWidgetView(WidgetBlueprint);
	if (!IsValid(View))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set list ViewModels used by widget '%s'. Invalid MVVM View."), *GetPathNameSafe(WidgetBlueprint)));
		return TArray<FString>();

	}

	TArray<UClass*> Results;
	for(const FMVVMBlueprintViewModelContext& ViewModelContext : View->GetViewModels())
	{
		
		if (UClass* Class = ViewModelContext.GetViewModelClass())
		{
			TArray<const FProperty*> Chain;
			TArray<TArray<const FProperty*>> AllChains;
			BuildPropertyChains_Recursive(Class, Chain, AllChains);

			for (const TArray<const FProperty*>& WidgetChain : AllChains)
			{
				FMVVMBlueprintPropertyPath BlueprintPropertyPath;
				BlueprintPropertyPath.SetViewModelId(ViewModelContext.GetViewModelId());
				for (const FProperty* Prop : WidgetChain)
				{
					BlueprintPropertyPath.AppendPropertyPath(WidgetBlueprint, UE::MVVM::FMVVMConstFieldVariant(Prop));
				}
				if (UMVVMBlueprintViewEvent::Supports(WidgetBlueprint, BlueprintPropertyPath))
				{
					OutEventProperties.Add(BlueprintPropertyPath.ToString(WidgetBlueprint, false, false));
				}
			}
		}
	}

	return OutEventProperties;
}

void UMVVMToolset59::SetEventBindingArgumentLiteral(UWidgetBlueprint* WidgetBlueprint, UMVVMBlueprintViewEvent* Event, const FString& ArgumentName, const FString& Value)
{
	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetEventBindingArgumentLiteral: Not supported during PIE."));

		return;
	}

	const TCHAR* FailurePrefix = TEXT("Failed to set event binding argument");

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. UMVVMEditorSubsystem not loaded."), FailurePrefix, *ArgumentName));

		return;
	}

	FText Error;
	if (!MVVMEditorSubsystem->SetEventArgumentLiteral(WidgetBlueprint, Event, ArgumentName, Value, Error))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("%s '%s'. %s"), FailurePrefix, *ArgumentName, *Error.ToString()));

		return;
	}

	// Only an edit marks the blueprint as modified. A failed compile keeps the edit; the caller reads the errors with CompileWidgetBlueprint.
	if (!WidgetBlueprint->IsUpToDate())
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
		if (WidgetBlueprint->Status == BS_Error)
		{
			UE_LOGF(LogMVVMToolset, Warning, "Event binding argument '%ls' was edited, but widget blueprint '%ls' failed to compile. Use CompileWidgetBlueprint to read the errors.", *ArgumentName, *WidgetBlueprint->GetPathName());
		}
	}
}

UMVVMBlueprintViewCondition* UMVVMToolset59::AddViewCondition(UWidgetBlueprint* WidgetBlueprint, UObject* SourceContext, const FString& SourcePropertyPath, UObject* DestinationContext, const FString& DestinationPropertyPath, EMVVMConditionOperation Operation, float Value, float MaxValue)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to add view condition. Invalid widget blueprint."));
		return nullptr;
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddViewCondition: Not supported during PIE."));
		return nullptr;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to add view condition. UMVVMEditorSubsystem not loaded."));
		return nullptr;
	}

	if (!NormalizeViewModelContext(SourceContext, TEXT("AddViewCondition")) || !NormalizeViewModelContext(DestinationContext, TEXT("AddViewCondition")))
	{
		return nullptr;
	}

	if (!EnsureViewModelCompiled(SourceContext, TEXT("AddViewCondition")) || !EnsureViewModelCompiled(DestinationContext, TEXT("AddViewCondition")))
	{
		return nullptr;
	}

	FString Error;
	const FMVVMBlueprintPropertyPath SourcePath = MVVMEditorSubsystem->ResolvePropertyPath(WidgetBlueprint, SourceContext, SourcePropertyPath, Error);
	if (!SourcePath.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add view condition. Invalid SourcePropertyPath. Reason: %s"), *Error));
		return nullptr;
	}

	const FMVVMBlueprintPropertyPath DestinationPath = MVVMEditorSubsystem->ResolvePropertyPath(WidgetBlueprint, DestinationContext, DestinationPropertyPath, Error);
	if (!DestinationPath.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add view condition. Invalid DestinationPropertyPath. Reason: %s"), *Error));
		return nullptr;
	}

	// The compiler leaves a source of any other type unlinked without an error, and the condition then compares against 0.
	const TArray<UE::MVVM::FMVVMConstFieldVariant> SourceFields = SourcePath.GetCompleteFields(WidgetBlueprint);
	const FProperty* SourceProperty = nullptr;
	if (!SourceFields.IsEmpty())
	{
		const TValueOrError<const FProperty*, FText> SourceType = UE::MVVM::BindingHelper::TryGetPropertyTypeForSourceBinding(SourceFields.Last());
		SourceProperty = SourceType.HasValue() ? SourceType.GetValue() : nullptr;
	}
	if (!IsSupportedConditionSourceType(SourceProperty))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to add view condition. Source '%s' is of type '%s'. Supported source types: bool, uint8 (not an enum), int32, int64, float and double."),
			*SourcePropertyPath, SourceProperty != nullptr ? *GetFullCPPTypeName(SourceProperty) : TEXT("unknown")));
		return nullptr;
	}

	UMVVMBlueprintViewCondition* Condition = nullptr;
	{
		// The subsystem calls below each open their own transaction. Group them so the condition is added with a single undo step.
		FScopedTransaction Transaction(NSLOCTEXT("MVVMToolset", "AddViewCondition", "MVVM Add Condition"));

		Condition = MVVMEditorSubsystem->AddCondition(WidgetBlueprint);
		if (Condition == nullptr)
		{
			UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to add view condition. Condition bindings are not allowed in this project (MVVM developer setting bAllowConditionBinding)."));
			return nullptr;
		}

		// Source before destination: once the destination is set, setting the source recompiles the skeleton while the
		// source path is still empty, which logs "The condition path is invalid.".
		MVVMEditorSubsystem->SetConditionPath(Condition, SourcePath, false);
		MVVMEditorSubsystem->SetConditionDestinationPath(Condition, DestinationPath);
		MVVMEditorSubsystem->SetConditionOperation(Condition, Operation);
		MVVMEditorSubsystem->SetConditionOperationValue(Condition, Value);
		MVVMEditorSubsystem->SetConditionOperationMaxValue(Condition, MaxValue);
	}

	// Fills the condition's compilation messages that ListViewConditions reports.
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
	return Condition;
}

TArray<FMVVMViewConditionDescription> UMVVMToolset59::ListViewConditions(UWidgetBlueprint* WidgetBlueprint)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to list view conditions. Invalid widget blueprint."));
		return TArray<FMVVMViewConditionDescription>();
	}

	const UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to list view conditions for widget '%s'. UMVVMEditorSubsystem not loaded."), *WidgetBlueprint->GetPathName()));
		return TArray<FMVVMViewConditionDescription>();
	}

	// GetView, not GetWidgetView: listing must not create a View.
	const UMVVMBlueprintView* View = MVVMEditorSubsystem->GetView(WidgetBlueprint);
	if (View == nullptr)
	{
		return TArray<FMVVMViewConditionDescription>();
	}

	TArray<FMVVMViewConditionDescription> Results;
	for (UMVVMBlueprintViewCondition* Condition : View->GetConditions())
	{
		if (!IsValid(Condition))
		{
			continue;
		}

		FMVVMViewConditionDescription& Description = Results.Emplace_GetRef();
		Description.Condition = Condition;
		Description.SourcePath = Condition->GetConditionPath().ToString(WidgetBlueprint, false, false);
		Description.DestinationPath = Condition->GetDestinationPath().ToString(WidgetBlueprint, false, false);
		Description.Operation = Condition->GetOperation();
		Description.Value = Condition->GetOperationValue();
		Description.MaxValue = Condition->GetOperationMaxValue();

		// The compiler appends a condition's messages on every compile without clearing them first.
		for (const FText& Message : Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Error))
		{
			Description.CompileErrors.AddUnique(Message.ToString());
		}
		for (const FText& Message : Condition->GetCompilationMessages(UMVVMBlueprintViewCondition::EMessageType::Warning))
		{
			Description.CompileWarnings.AddUnique(Message.ToString());
		}
	}

	return Results;
}

bool UMVVMToolset59::SetViewConditionOperation(UWidgetBlueprint* WidgetBlueprint, UMVVMBlueprintViewCondition* Condition, EMVVMConditionOperation Operation, float Value, float MaxValue)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to set view condition operation. Invalid widget blueprint."));
		return false;
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetViewConditionOperation: Not supported during PIE."));
		return false;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to set view condition operation. UMVVMEditorSubsystem not loaded."));
		return false;
	}

	const UMVVMBlueprintView* View = MVVMEditorSubsystem->GetView(WidgetBlueprint);
	if (View == nullptr || !View->GetConditions().Contains(Condition))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to set view condition operation. Condition '%s' is not in the MVVM View of widget '%s'."), *GetPathNameSafe(Condition), *WidgetBlueprint->GetPathName()));
		return false;
	}

	{
		FScopedTransaction Transaction(NSLOCTEXT("MVVMToolset", "SetViewConditionOperation", "MVVM Set Condition Operation"));
		MVVMEditorSubsystem->SetConditionOperation(Condition, Operation);
		MVVMEditorSubsystem->SetConditionOperationValue(Condition, Value);
		MVVMEditorSubsystem->SetConditionOperationMaxValue(Condition, MaxValue);
	}

	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
	return true;
}

void UMVVMToolset59::RemoveViewCondition(UWidgetBlueprint* WidgetBlueprint, UMVVMBlueprintViewCondition* Condition)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to remove view condition. Invalid widget blueprint."));
		return;
	}

	if (GEditor->IsPlayingSessionInEditor())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveViewCondition: Not supported during PIE."));
		return;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to remove view condition. UMVVMEditorSubsystem not loaded."));
		return;
	}

	const UMVVMBlueprintView* View = MVVMEditorSubsystem->GetView(WidgetBlueprint);
	if (View == nullptr || !View->GetConditions().Contains(Condition))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to remove view condition. Condition '%s' is not in the MVVM View of widget '%s'."), *GetPathNameSafe(Condition), *WidgetBlueprint->GetPathName()));
		return;
	}

	MVVMEditorSubsystem->RemoveCondition(WidgetBlueprint, Condition);
	if (View->GetConditions().Contains(Condition))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to remove view condition due to an unknown reason."));
	}
}

bool UMVVMToolset59::SetWidgetPreviewActive(UWidgetBlueprint* WidgetBlueprint, bool bActive)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to set preview active. Invalid widget blueprint."));
		return false;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to set preview active. UMVVMEditorSubsystem not loaded."));
		return false;
	}

	MVVMEditorSubsystem->SetBlueprintPreviewActive(WidgetBlueprint, bActive);
	return MVVMEditorSubsystem->IsBlueprintPreviewActive(WidgetBlueprint) == bActive;
}

bool UMVVMToolset59::GetWidgetPreviewActive(UWidgetBlueprint* WidgetBlueprint)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to get preview state. Invalid widget blueprint."));
		return false;
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to get preview state. UMVVMEditorSubsystem not loaded."));
		return false;
	}

	return MVVMEditorSubsystem->IsBlueprintPreviewActive(WidgetBlueprint);
}

FString UMVVMToolset59::ReadPreviewWidgetProperty(UWidgetBlueprint* WidgetBlueprint, FName WidgetName, FName PropertyName)
{
	if (!IsValid(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to read preview property. Invalid widget blueprint."));
		return FString();
	}

	UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
	if (!IsValid(MVVMEditorSubsystem))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("Failed to read preview property. UMVVMEditorSubsystem not loaded."));
		return FString();
	}

	UUserWidget* DesignTimeWidget = MVVMEditorSubsystem->GetDesignTimePreviewWidget(WidgetBlueprint);
	if (!IsValid(DesignTimeWidget))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to read preview property. No design-time preview widget for '%s' - is its designer open?"), *WidgetBlueprint->GetName()));
		return FString();
	}

	UObject* Container = DesignTimeWidget;
	if (!WidgetName.IsNone())
	{
		Container = DesignTimeWidget->GetWidgetFromName(WidgetName);
		if (!IsValid(Container))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to read preview property. Widget '%s' not found on the preview widget."), *WidgetName.ToString()));
			return FString();
		}
	}

	const FProperty* Property = Container->GetClass()->FindPropertyByName(PropertyName);
	if (!Property)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT("Failed to read preview property. Property '%s' not found on '%s'."), *PropertyName.ToString(), *Container->GetClass()->GetName()));
		return FString();
	}

	FString Out;
	Property->ExportText_InContainer(0, Out, Container, nullptr, nullptr, PPF_None);
	return Out;
}