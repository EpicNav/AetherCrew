// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetPropertyOriginScanner.h"
#include "WidgetPropertyReflectionUtils.h"
#include "WidgetMVVMBindingUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMPropertyPath.h"
#include "MVVMViewModelBase.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
	// Nearest native ancestor, skipping Blueprint-generated classes.
	const UClass* FindNearestNativeClass(const UClass* Class)
	{
		while (Class && Class->ClassGeneratedBy)
		{
			Class = Class->GetSuperClass();
		}
		return Class;
	}

	// True for the direct content root of a named slot, not a descendant inside it.
	bool IsDirectNamedSlotContent(const UWidgetTree* WidgetTree, const UWidget* Widget)
	{
		for (const TPair<FName, TObjectPtr<UWidget>>& SlotEntry : WidgetTree->NamedSlotBindings)
		{
			if (SlotEntry.Value == Widget)
			{
				return true;
			}
		}
		return false;
	}

	bool PassesPropertyNameFilter(FName PropertyName, const FString& PropertyNameFilter)
	{
		return PropertyNameFilter.IsEmpty() || PropertyNameFilter == PropertyName.ToString();
	}

	void AddClassDefaultAndArchetypeOrigins(UWidget* Widget, const FProperty* Property, bool bIsNamedSlotContent,
		bool bIncludeArchetypeEntry, TArray<FTracedPropertyOrigin>& OutOrigins)
	{
		const UClass* WidgetClass = Widget->GetClass();
		const UClass* NativeClass = FindNearestNativeClass(WidgetClass);
		if (!NativeClass)
		{
			return;
		}

		// Blueprint-added properties may not exist on the native CDO.
		const UClass* PropertyOwnerClass = Property->GetOwnerClass();
		const bool bPropertyIsNative = PropertyOwnerClass && NativeClass->IsChildOf(PropertyOwnerClass);

		FString NativeValue;
		if (bPropertyIsNative)
		{
			NativeValue = FWidgetPropertyReflectionUtils::ExportPropertyValue(Property, NativeClass->GetDefaultObject());

			FTracedPropertyOrigin& NativeOrigin = OutOrigins.AddDefaulted_GetRef();
			NativeOrigin.WidgetName = Widget->GetFName();
			NativeOrigin.PropertyName = Property->GetFName();
			NativeOrigin.Kind = EOriginSourceKind::NativeClassDefault;
			NativeOrigin.ApplicationStatus = EOriginApplicationStatus::Applied;
			NativeOrigin.SourceDetail = FString::Printf(TEXT("Widget=%s; native class %s default"), *Widget->GetName(), *NativeClass->GetName());
			NativeOrigin.ExportedValue = NativeValue;
		}

		if (WidgetClass != NativeClass)
		{
			const FString GeneratedValue = FWidgetPropertyReflectionUtils::ExportPropertyValue(Property, WidgetClass->GetDefaultObject());
			if (!bPropertyIsNative || GeneratedValue != NativeValue)
			{
				FTracedPropertyOrigin& GeneratedOrigin = OutOrigins.AddDefaulted_GetRef();
				GeneratedOrigin.WidgetName = Widget->GetFName();
				GeneratedOrigin.PropertyName = Property->GetFName();
				GeneratedOrigin.Kind = EOriginSourceKind::GeneratedClassDefault;
				GeneratedOrigin.ApplicationStatus = EOriginApplicationStatus::Applied;
				GeneratedOrigin.SourceDetail = FString::Printf(TEXT("Widget=%s; generated class %s default"), *Widget->GetName(), *WidgetClass->GetName());
				GeneratedOrigin.ExportedValue = GeneratedValue;
			}
		}

		if (bIncludeArchetypeEntry)
		{
			FTracedPropertyOrigin& ArchetypeOrigin = OutOrigins.AddDefaulted_GetRef();
			ArchetypeOrigin.WidgetName = Widget->GetFName();
			ArchetypeOrigin.PropertyName = Property->GetFName();
			ArchetypeOrigin.Kind = bIsNamedSlotContent ? EOriginSourceKind::NamedSlotArchetype : EOriginSourceKind::WidgetTreeArchetype;
			ArchetypeOrigin.ApplicationStatus = EOriginApplicationStatus::Applied;
			ArchetypeOrigin.SourceDetail = FString::Printf(TEXT("Widget=%s; %s"), *Widget->GetName(),
				bIsNamedSlotContent ? TEXT("named-slot content archetype") : TEXT("widget-tree archetype"));
			ArchetypeOrigin.ExportedValue = FWidgetPropertyReflectionUtils::ExportPropertyValue(Property, Widget);
		}
	}

	// Skips pairs covered by graph writes, filtered out, or unchanged from the default.
	void ScanWidgetPropertyOrigins(UWidget* Widget, const FProperty* Property, bool bIsNamedSlotContent,
		const TSet<TPair<FName, FName>>& GraphCoveredWidgetProperties, const FString& PropertyNameFilter,
		TArray<FTracedPropertyOrigin>& OutOrigins)
	{
		if (!FWidgetPropertyReflectionUtils::ShouldEmitProperty(Property, Widget, TEXT(""), /*bSkipDefaultValues=*/false))
		{
			return;
		}

		if (!PassesPropertyNameFilter(Property->GetFName(), PropertyNameFilter))
		{
			return;
		}

		const TPair<FName, FName> WidgetPropertyKey(Widget->GetFName(), Property->GetFName());
		if (GraphCoveredWidgetProperties.Contains(WidgetPropertyKey))
		{
			return;
		}

		const bool bArchetypeOverridden = !FWidgetPropertyReflectionUtils::MatchesArchetype(Property, Widget);
		const bool bExplicitlyFiltered = !PropertyNameFilter.IsEmpty();
		if (!bArchetypeOverridden && !bExplicitlyFiltered)
		{
			return;
		}

		AddClassDefaultAndArchetypeOrigins(Widget, Property, bIsNamedSlotContent, bArchetypeOverridden, OutOrigins);
	}

	void ScanClassDefaultArchetypeAndVariantOrigins(const UWidgetBlueprint* WidgetBlueprint,
		const TSet<TPair<FName, FName>>& GraphCoveredWidgetProperties, const FString& PropertyNameFilter,
		TArray<FTracedPropertyOrigin>& OutOrigins)
	{
		const UWidgetTree* WidgetTree = WidgetBlueprint->WidgetTree;
		TArray<UWidget*> AllWidgets;
		WidgetTree->GetAllWidgets(AllWidgets);

		for (UWidget* Widget : AllWidgets)
		{
			if (!Widget)
			{
				continue;
			}

			const bool bIsNamedSlotContent = IsDirectNamedSlotContent(WidgetTree, Widget);

			for (TFieldIterator<FProperty> PropertyIt(Widget->GetClass()); PropertyIt; ++PropertyIt)
			{
				ScanWidgetPropertyOrigins(Widget, *PropertyIt, bIsNamedSlotContent, GraphCoveredWidgetProperties,
					PropertyNameFilter, OutOrigins);
			}
		}
	}

	void ScanLegacyBindingOrigins(const UWidgetBlueprint* WidgetBlueprint, const TSet<TPair<FName, FName>>& GraphCoveredWidgetProperties,
		const FString& PropertyNameFilter, TArray<FTracedPropertyOrigin>& OutOrigins)
	{
		for (const FDelegateEditorBinding& Binding : WidgetBlueprint->Bindings)
		{
			const FName WidgetName(*Binding.ObjectName);
			const FName PropertyName = Binding.PropertyName;

			if (!PassesPropertyNameFilter(PropertyName, PropertyNameFilter))
			{
				continue;
			}
			if (GraphCoveredWidgetProperties.Contains(TPair<FName, FName>(WidgetName, PropertyName)))
			{
				continue;
			}

			FTracedPropertyOrigin& Origin = OutOrigins.AddDefaulted_GetRef();
			Origin.WidgetName = WidgetName;
			Origin.PropertyName = PropertyName;
			Origin.Kind = EOriginSourceKind::LegacyBinding;
			// No enable toggle on legacy bindings.
			Origin.ApplicationStatus = EOriginApplicationStatus::Applied;
			Origin.SourceDetail = FString::Printf(TEXT("Widget=%s; legacy property binding via %s"), *Binding.ObjectName, *Binding.FunctionName.ToString());
		}
	}

	// Always reported: an MVVM binding can apply after Construct.
	void ScanMvvmBindingOrigins(const UWidgetBlueprint* WidgetBlueprint, const FString& PropertyNameFilter, TArray<FTracedPropertyOrigin>& OutOrigins)
	{
		UMVVMBlueprintView* View = FWidgetMVVMBindingUtils::FindView(WidgetBlueprint);
		if (!View)
		{
			return;
		}

		const TMap<FGuid, FViewModelIdentity> ViewModelIdentitiesById = FWidgetMVVMBindingUtils::BuildViewModelIdentitiesById(View);

		for (const FMVVMBlueprintViewBinding& Binding : View->GetBindings())
		{
			const FResolvedMVVMDestination Resolved = FWidgetMVVMBindingUtils::ResolveDestination(Binding.DestinationPath, WidgetBlueprint);
			if (!Resolved.bIsWidget || !Resolved.bIsProperty)
			{
				continue;
			}

			if (!PassesPropertyNameFilter(Resolved.FieldName, PropertyNameFilter))
			{
				continue;
			}

			const TArrayView<const FMVVMBlueprintFieldPath> SourceFieldPaths = Binding.SourcePath.GetFieldPaths();
			const FString SourceFieldName = SourceFieldPaths.Num() > 0 ? SourceFieldPaths.Last().GetRawFieldName().ToString() : FString();
			const FViewModelIdentity* ViewModelIdentity = ViewModelIdentitiesById.Find(Binding.SourcePath.GetViewModelId());

			FTracedPropertyOrigin& Origin = OutOrigins.AddDefaulted_GetRef();
			Origin.WidgetName = Resolved.WidgetName;
			Origin.PropertyName = Resolved.FieldName;
			Origin.Kind = EOriginSourceKind::MVVMBinding;
			// Disabled never pushes a value; enabled can't be confirmed statically.
			Origin.ApplicationStatus = Binding.bEnabled ? EOriginApplicationStatus::DeclaredButUnconfirmed : EOriginApplicationStatus::KnownNotApplied;
			Origin.SourceDetail = FString::Printf(TEXT("Widget=%s; MVVM binding from ViewModel=%s.%s"),
				*Resolved.WidgetName.ToString(), ViewModelIdentity ? *ViewModelIdentity->Name : TEXT(""), *SourceFieldName);
		}
	}
}

TArray<FTracedPropertyOrigin> FWidgetPropertyOriginScanner::ScanOrigins(const UWidgetBlueprint* WidgetBlueprint,
	const TSet<TPair<FName, FName>>& GraphCoveredWidgetProperties, const FString& PropertyNameFilter)
{
	TArray<FTracedPropertyOrigin> Origins;
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		return Origins;
	}

	ScanClassDefaultArchetypeAndVariantOrigins(WidgetBlueprint, GraphCoveredWidgetProperties, PropertyNameFilter, Origins);
	ScanLegacyBindingOrigins(WidgetBlueprint, GraphCoveredWidgetProperties, PropertyNameFilter, Origins);
	ScanMvvmBindingOrigins(WidgetBlueprint, PropertyNameFilter, Origins);

	return Origins;
}
