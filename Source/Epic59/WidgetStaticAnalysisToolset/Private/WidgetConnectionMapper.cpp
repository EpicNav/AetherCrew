// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetConnectionMapper.h"
#include "WidgetMVVMBindingUtils.h"
#include "WidgetPropertyTracer.h"
#include "WidgetBlueprint.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewEvent.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "MVVMViewModelBase.h"
#include "MVVMSubsystem.h"
#include "MVVMEditorSubsystem.h"
#include "Bindings/MVVMBindingHelper.h"
#include "Types/MVVMFieldVariant.h"
#include "Templates/ValueOrError.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_VariableGet.h"
#include "INotifyFieldValueChanged.h"
#include "IFieldNotificationClassDescriptor.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "Components/Widget.h"
#include "Blueprint/WidgetTree.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"

namespace
{
	// Resolves a dot-separated field path from RootStruct; nullptr if any hop isn't a property.
	const FProperty* ResolveLeafProperty(const UStruct* RootStruct, TArrayView<const FMVVMBlueprintFieldPath> FieldPaths)
	{
		const UStruct* CurrentStruct = RootStruct;
		const FProperty* LeafProperty = nullptr;
		for (const FMVVMBlueprintFieldPath& FieldPath : FieldPaths)
		{
			if (!CurrentStruct)
			{
				return nullptr;
			}

			const FProperty* Property = CurrentStruct->FindPropertyByName(FieldPath.GetRawFieldName());
			if (!Property)
			{
				return nullptr;
			}

			LeafProperty = Property;
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				CurrentStruct = ObjectProperty->PropertyClass;
			}
			else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				CurrentStruct = StructProperty->Struct;
			}
			else
			{
				CurrentStruct = nullptr;
			}
		}
		return LeafProperty;
	}

	// Escalates Entry.Status to Misconfigured on a type mismatch. Disabled bindings are the caller's job.
	void ApplyVmBindingMisconfiguration(FMappedWidgetConnection& Entry, const FMVVMBlueprintViewBinding& Binding,
		TArrayView<const FMVVMBlueprintFieldPath> SourceFieldPaths, const UClass* ViewModelClass,
		const FResolvedMVVMDestination& Resolved, const UWidgetBlueprint* WidgetBlueprint)
	{
		Entry.Status = EMappedConnectionStatus::Connected;

		if (!ViewModelClass)
		{
			// Unresolved ViewModel class (dirty, uncompiled Blueprint): nothing to type-check against.
			Entry.Status = EMappedConnectionStatus::Misconfigured;
			Entry.Fixability = EMappedConnectionFixability::Unknown;
			Entry.Fix = TEXT("This binding's ViewModel class reference is missing or unresolved. Fix the ViewModel context in the MVVM View before this binding can be evaluated.");
			return;
		}

		if (!Resolved.bIsWidget || !Resolved.bIsProperty)
		{
			// Non-widget destination or setter-call leaf: nothing to type-check.
			return;
		}

		const UWidget* DestinationWidget = WidgetBlueprint->WidgetTree ? WidgetBlueprint->WidgetTree->FindWidget(Resolved.WidgetName) : nullptr;
		if (!DestinationWidget)
		{
			return;
		}

		const FProperty* SourceProperty = ResolveLeafProperty(ViewModelClass, SourceFieldPaths);
		const FProperty* DestinationProperty = ResolveLeafProperty(DestinationWidget->GetClass(), Binding.DestinationPath.GetFieldPaths());
		if (!SourceProperty || !DestinationProperty)
		{
			// Field no longer exists on its class (renamed or removed).
			Entry.Status = EMappedConnectionStatus::Misconfigured;
			Entry.Fixability = EMappedConnectionFixability::Unbridgeable;
			const bool bSourceMissing = !SourceProperty;
			const FString MissingFieldName = bSourceMissing
				? (SourceFieldPaths.Num() > 0 ? SourceFieldPaths.Last().GetRawFieldName().ToString() : FString())
				: Resolved.FieldName.ToString();
			Entry.Fix = FString::Printf(TEXT("Binding's %s field '%s' does not resolve to a live property on the %s. Re-point this binding at a property that still exists."),
				bSourceMissing ? TEXT("source") : TEXT("destination"), *MissingFieldName, bSourceMissing ? TEXT("ViewModel") : TEXT("widget"));
			return;
		}

		const UMVVMSubsystem* MVVMSubsystem = GEngine ? GEngine->GetEngineSubsystem<UMVVMSubsystem>() : nullptr;
		if (!MVVMSubsystem)
		{
			return;
		}

		// Honor an assigned conversion function so bridged bindings aren't flagged.
		const UMVVMBlueprintViewConversionFunction* ConversionFunctionObj = Binding.Conversion.GetConversionFunction(/*bSourceToDestination=*/true);
		const UFunction* ConversionFunction = ConversionFunctionObj && WidgetBlueprint->GeneratedClass
			? ConversionFunctionObj->GetCompiledFunction(WidgetBlueprint->GeneratedClass)
			: nullptr;

		UMVVMSubsystem::FConstDirectionalBindingArgs Args;
		Args.SourceBinding = UE::MVVM::FMVVMConstFieldVariant(SourceProperty);
		Args.DestinationBinding = UE::MVVM::FMVVMConstFieldVariant(DestinationProperty);
		Args.ConversionFunction = ConversionFunction;

		const TValueOrError<bool, FText> ValidResult = MVVMSubsystem->IsBindingValid(Args);
		const bool bValid = ValidResult.HasValue() && ValidResult.GetValue();
		if (bValid)
		{
			return;
		}

		Entry.Status = EMappedConnectionStatus::Misconfigured;

		if (UE::MVVM::BindingHelper::ArePropertiesCompatible(SourceProperty, DestinationProperty))
		{
			// Rejected for a reason other than type mismatch (binding mode or broken conversion).
			Entry.Fixability = EMappedConnectionFixability::Unknown;
			Entry.Fix = FString::Printf(TEXT("Binding from '%s' to '%s' is reported invalid for a reason other than a type mismatch (e.g. binding mode or an existing conversion function); inspect the binding's setup in the MVVM View."),
				*SourceProperty->GetName(), *DestinationProperty->GetName());
			return;
		}

		const UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
		const int32 NumConversionFunctions = MVVMEditorSubsystem
			? MVVMEditorSubsystem->GetConversionFunctions(WidgetBlueprint, SourceProperty, DestinationProperty).Num()
			: 0;

		if (NumConversionFunctions > 0)
		{
			Entry.Fixability = EMappedConnectionFixability::Fixable;
			Entry.Fix = FString::Printf(TEXT("Assign a conversion function to bridge '%s' (%s) to '%s' (%s). %d candidate(s) available."),
				*SourceProperty->GetName(), *SourceProperty->GetCPPType(), *DestinationProperty->GetName(), *DestinationProperty->GetCPPType(), NumConversionFunctions);
		}
		else
		{
			Entry.Fixability = EMappedConnectionFixability::Unbridgeable;
			// The allowlist is project-specific; may be a false negative elsewhere.
			Entry.Fix = FString::Printf(TEXT("No conversion function available in this project's allowlist to bridge '%s' (%s) to '%s' (%s)."),
				*SourceProperty->GetName(), *SourceProperty->GetCPPType(), *DestinationProperty->GetName(), *DestinationProperty->GetCPPType());
		}
	}

	void PopulateVmFieldBindings(TArray<FMappedWidgetConnection>& OutConnections, const UWidgetBlueprint* WidgetBlueprint)
	{
		UMVVMBlueprintView* View = FWidgetMVVMBindingUtils::FindView(WidgetBlueprint);
		if (!View)
		{
			return;
		}

		const TMap<FGuid, FViewModelIdentity> ViewModelIdentitiesById = FWidgetMVVMBindingUtils::BuildViewModelIdentitiesById(View);

		for (const FMVVMBlueprintViewBinding& Binding : View->GetBindings())
		{
			const TArrayView<const FMVVMBlueprintFieldPath> SourceFieldPaths = Binding.SourcePath.GetFieldPaths();
			const FString SourceProperty = SourceFieldPaths.Num() > 0 ? SourceFieldPaths.Last().GetRawFieldName().ToString() : FString();
			const FViewModelIdentity* FoundViewModelIdentity = ViewModelIdentitiesById.Find(Binding.SourcePath.GetViewModelId());

			const FResolvedMVVMDestination Resolved = FWidgetMVVMBindingUtils::ResolveDestination(Binding.DestinationPath, WidgetBlueprint);

			FMappedWidgetConnection& Entry = OutConnections.AddDefaulted_GetRef();
			Entry.Kind = EMappedConnectionKind::VMFieldBinding;
			Entry.ConnectionName = SourceProperty;
			Entry.ViewModel = FoundViewModelIdentity ? FoundViewModelIdentity->Name : FString();
			Entry.Widget = Resolved.bIsWidget ? Resolved.WidgetName.ToString() : FString();
			Entry.TargetVariable = Resolved.bIsWidget ? FString() : Resolved.HandlerName.ToString();
			Entry.DeclaredIn = TEXT("MVVM View");

			if (!Binding.bEnabled)
			{
				// Disabled takes priority over misconfiguration.
				Entry.Status = EMappedConnectionStatus::Unconnected;
			}
			else
			{
				ApplyVmBindingMisconfiguration(Entry, Binding, SourceFieldPaths, FoundViewModelIdentity ? FoundViewModelIdentity->Class : nullptr, Resolved, WidgetBlueprint);
			}
		}
	}

	void PopulateLegacyPropertyBindings(TArray<FMappedWidgetConnection>& OutConnections, const UWidgetBlueprint* WidgetBlueprint)
	{
		for (const FDelegateEditorBinding& Binding : WidgetBlueprint->Bindings)
		{
			FMappedWidgetConnection& Entry = OutConnections.AddDefaulted_GetRef();
			Entry.Kind = EMappedConnectionKind::LegacyPropertyBinding;
			Entry.ConnectionName = Binding.PropertyName.ToString();
			Entry.Widget = Binding.ObjectName;
			Entry.DeclaredIn = TEXT("Blueprint Bindings");
			// Legacy bindings have no enabled toggle.
			Entry.Status = EMappedConnectionStatus::Connected;
		}
	}

	void PopulateMvvmEventBindings(TArray<FMappedWidgetConnection>& OutConnections, const UWidgetBlueprint* WidgetBlueprint)
	{
		UMVVMBlueprintView* View = FWidgetMVVMBindingUtils::FindView(WidgetBlueprint);
		if (!View)
		{
			return;
		}

		for (const TObjectPtr<UMVVMBlueprintViewEvent>& Event : View->GetEvents())
		{
			if (!Event)
			{
				continue;
			}

			const FMVVMBlueprintPropertyPath& EventPath = Event->GetEventPath();
			const TArrayView<const FMVVMBlueprintFieldPath> EventFieldPaths = EventPath.GetFieldPaths();
			const bool bEventIsWidget = EventFieldPaths.Num() > 0
				&& EventPath.GetSource(WidgetBlueprint) == EMVVMBlueprintFieldPathSource::Widget;
			if (!bEventIsWidget)
			{
				// ViewModel/SelfContext-rooted sources aren't widget events.
				continue;
			}

			FMappedWidgetConnection& Entry = OutConnections.AddDefaulted_GetRef();
			Entry.Kind = EMappedConnectionKind::EventBinding;
			Entry.KindDetail = TEXT("mvvm");
			Entry.ConnectionName = EventFieldPaths.Last().GetRawFieldName().ToString();
			Entry.Widget = EventPath.GetWidgetName().ToString();
			Entry.DeclaredIn = TEXT("MVVM View");
			Entry.Status = Event->bEnabled ? EMappedConnectionStatus::Connected : EMappedConnectionStatus::Unconnected;
		}
	}

	void PopulateBlueprintGraphConnections(TArray<FMappedWidgetConnection>& OutConnections, const UWidgetBlueprint* WidgetBlueprint)
	{
		TArray<UEdGraph*> AllGraphs;
		FWidgetPropertyTracer::CollectAllGraphs(WidgetBlueprint, AllGraphs);

		// Compiled assets duplicate graphs as "ExecuteUbergraph_"; keep one entry per {widget, event}, preferring the real name.
		TMap<TPair<FName, FName>, FString> EventBindingGraphByKey;
		TMap<TPair<FName, FName>, FString> DelegateGraphByKey;

		auto PreferRealGraphName = [](TMap<TPair<FName, FName>, FString>& GraphByKey, const TPair<FName, FName>& Key, const FString& GraphName)
		{
			FString* ExistingGraphName = GraphByKey.Find(Key);
			const bool bExistingIsGeneratedAlias = ExistingGraphName && ExistingGraphName->StartsWith(TEXT("ExecuteUbergraph_"));
			if (!ExistingGraphName || bExistingIsGeneratedAlias)
			{
				GraphByKey.Add(Key, GraphName);
			}
		};

		for (UEdGraph* Graph : AllGraphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
				{
					const TPair<FName, FName> Key(BoundEvent->ComponentPropertyName, BoundEvent->DelegatePropertyName);
					PreferRealGraphName(EventBindingGraphByKey, Key, Graph->GetName());
				}
				else if (const UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
				{
					// Only counts when the self pin traces to a widget variable's Get node.
					const UEdGraphPin* SelfPin = DelegateNode->FindPin(UEdGraphSchema_K2::PN_Self);
					const UEdGraphPin* LinkedFromWidgetGet = SelfPin && SelfPin->LinkedTo.Num() > 0 ? SelfPin->LinkedTo[0] : nullptr;
					const UK2Node_VariableGet* WidgetGetNode = LinkedFromWidgetGet ? Cast<UK2Node_VariableGet>(LinkedFromWidgetGet->GetOwningNode()) : nullptr;
					if (!WidgetGetNode || !WidgetGetNode->VariableReference.IsSelfContext())
					{
						continue;
					}

					const TPair<FName, FName> Key(WidgetGetNode->GetVarName(), DelegateNode->GetPropertyName());
					PreferRealGraphName(DelegateGraphByKey, Key, Graph->GetName());
				}
			}
		}

		for (const TPair<TPair<FName, FName>, FString>& KeyAndGraph : EventBindingGraphByKey)
		{
			FMappedWidgetConnection& Entry = OutConnections.AddDefaulted_GetRef();
			Entry.Kind = EMappedConnectionKind::EventBinding;
			Entry.KindDetail = TEXT("blueprint_bind");
			Entry.ConnectionName = KeyAndGraph.Key.Value.ToString();
			Entry.Widget = KeyAndGraph.Key.Key.ToString();
			Entry.DeclaredIn = KeyAndGraph.Value;
			Entry.Status = EMappedConnectionStatus::Connected;
		}

		for (const TPair<TPair<FName, FName>, FString>& KeyAndGraph : DelegateGraphByKey)
		{
			FMappedWidgetConnection& Entry = OutConnections.AddDefaulted_GetRef();
			Entry.Kind = EMappedConnectionKind::Delegate;
			Entry.ConnectionName = KeyAndGraph.Key.Value.ToString();
			Entry.Widget = KeyAndGraph.Key.Key.ToString();
			Entry.DeclaredIn = KeyAndGraph.Value;
			Entry.Status = EMappedConnectionStatus::Connected;
		}
	}

	void AppendUnconnectedVmFields(TArray<FMappedWidgetConnection>& OutConnections, const UWidgetBlueprint* WidgetBlueprint)
	{
		UMVVMBlueprintView* View = FWidgetMVVMBindingUtils::FindView(WidgetBlueprint);
		if (!View)
		{
			return;
		}

		const TMap<FGuid, FViewModelIdentity> ViewModelIdentitiesById = FWidgetMVVMBindingUtils::BuildViewModelIdentitiesById(View);

		TSet<TPair<FGuid, FName>> BoundFields;
		for (const FMVVMBlueprintViewBinding& Binding : View->GetBindings())
		{
			// The ViewModel field is the first hop of a multi-hop path ("Inventory" in "Inventory.Count").
			const TArrayView<const FMVVMBlueprintFieldPath> SourceFieldPaths = Binding.SourcePath.GetFieldPaths();
			if (SourceFieldPaths.Num() > 0)
			{
				BoundFields.Add(TPair<FGuid, FName>(Binding.SourcePath.GetViewModelId(), SourceFieldPaths[0].GetRawFieldName()));
			}
		}

		for (const TPair<FGuid, FViewModelIdentity>& IdAndIdentity : ViewModelIdentitiesById)
		{
			UClass* ViewModelClass = IdAndIdentity.Value.Class;
			const UObject* ViewModelCDO = ViewModelClass ? ViewModelClass->GetDefaultObject() : nullptr;
			const INotifyFieldValueChanged* FieldNotifier = Cast<INotifyFieldValueChanged>(ViewModelCDO);
			if (!FieldNotifier)
			{
				continue;
			}

			FieldNotifier->GetFieldNotificationDescriptor().ForEachField(ViewModelClass, [&BoundFields, &IdAndIdentity, &OutConnections](UE::FieldNotification::FFieldId FieldId)
			{
				if (!BoundFields.Contains(TPair<FGuid, FName>(IdAndIdentity.Key, FieldId.GetName())))
				{
					FMappedWidgetConnection& Entry = OutConnections.AddDefaulted_GetRef();
					Entry.Kind = EMappedConnectionKind::VMFieldBinding;
					Entry.ConnectionName = FieldId.GetName().ToString();
					Entry.ViewModel = IdAndIdentity.Value.Name;
					// Names the scope that would have declared the binding.
					Entry.DeclaredIn = TEXT("MVVM View");
					Entry.Status = EMappedConnectionStatus::Unconnected;
				}
				return true;
			});
		}
	}

	// Keeps only VMFieldBinding rows bound to exactly ViewModelClass.
	TArray<FMappedWidgetConnection> NarrowToViewModelClass(TArray<FMappedWidgetConnection>&& AllConnections,
		const UWidgetBlueprint* WidgetBlueprint, TSubclassOf<UMVVMViewModelBase> ViewModelClass)
	{
		TSet<FString> MatchingViewModelNames;
		if (UMVVMBlueprintView* View = FWidgetMVVMBindingUtils::FindView(WidgetBlueprint))
		{
			for (const FMVVMBlueprintViewModelContext& ViewModelContext : View->GetViewModels())
			{
				if (ViewModelContext.GetViewModelClass() == ViewModelClass)
				{
					MatchingViewModelNames.Add(ViewModelContext.GetViewModelName().ToString());
				}
			}
		}

		TArray<FMappedWidgetConnection> Narrowed;
		Narrowed.Reserve(AllConnections.Num());
		for (FMappedWidgetConnection& Connection : AllConnections)
		{
			if (Connection.Kind == EMappedConnectionKind::VMFieldBinding && MatchingViewModelNames.Contains(Connection.ViewModel))
			{
				Narrowed.Add(MoveTemp(Connection));
			}
		}
		return Narrowed;
	}

	void AppendUnconnectedWidgetDelegates(TArray<FMappedWidgetConnection>& OutConnections, const UWidgetBlueprint* WidgetBlueprint)
	{
		if (!WidgetBlueprint->WidgetTree)
		{
			return;
		}

		// Skip pairs already described by a more specific pass.
		TSet<TPair<FName, FName>> AlreadyDescribedWidgetDelegates;
		for (const FMappedWidgetConnection& Existing : OutConnections)
		{
			if (Existing.Kind == EMappedConnectionKind::EventBinding || Existing.Kind == EMappedConnectionKind::Delegate)
			{
				AlreadyDescribedWidgetDelegates.Add(TPair<FName, FName>(FName(*Existing.Widget), FName(*Existing.ConnectionName)));
			}
		}

		WidgetBlueprint->WidgetTree->ForEachWidget([&AlreadyDescribedWidgetDelegates, &OutConnections](UWidget* Widget)
		{
			if (!Widget)
			{
				return;
			}
			for (TFieldIterator<FMulticastDelegateProperty> It(Widget->GetClass()); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_BlueprintAssignable))
				{
					continue;
				}
				if (!AlreadyDescribedWidgetDelegates.Contains(TPair<FName, FName>(Widget->GetFName(), It->GetFName())))
				{
					FMappedWidgetConnection& Entry = OutConnections.AddDefaulted_GetRef();
					Entry.Kind = EMappedConnectionKind::Delegate;
					Entry.ConnectionName = It->GetName();
					Entry.Widget = Widget->GetName();
					// No listener in this widget's graphs; not necessarily broken.
					Entry.DeclaredIn = TEXT("Widget's own graphs");
					Entry.Status = EMappedConnectionStatus::Unconnected;
				}
			}
		});
	}
}

TArray<FMappedWidgetConnection> FWidgetConnectionMapper::MapConnections(const UWidgetBlueprint* WidgetBlueprint)
{
	TArray<FMappedWidgetConnection> Connections;
	if (!WidgetBlueprint)
	{
		return Connections;
	}

	PopulateVmFieldBindings(Connections, WidgetBlueprint);
	PopulateLegacyPropertyBindings(Connections, WidgetBlueprint);
	PopulateMvvmEventBindings(Connections, WidgetBlueprint);
	PopulateBlueprintGraphConnections(Connections, WidgetBlueprint);
	AppendUnconnectedVmFields(Connections, WidgetBlueprint);
	AppendUnconnectedWidgetDelegates(Connections, WidgetBlueprint);

	return Connections;
}

TArray<FMappedViewModelWidgetConnections> FWidgetConnectionMapper::MapViewModelConnections(
	TSubclassOf<UMVVMViewModelBase> ViewModelClass)
{
	TArray<FMappedViewModelWidgetConnections> Result;
	if (!ViewModelClass)
	{
		return Result;
	}

	// Native ViewModel classes have no on-disk asset, so GetReferencers finds nothing.
	const UPackage* ViewModelPackage = ViewModelClass->GetPackage();
	if (!ViewModelPackage)
	{
		return Result;
	}

	IAssetRegistry* AssetRegistryPtr = IAssetRegistry::Get();
	if (!AssetRegistryPtr)
	{
		return Result;
	}
	IAssetRegistry& AssetRegistry = *AssetRegistryPtr;

	TArray<FName> ReferencerPackageNames;
	AssetRegistry.GetReferencers(ViewModelPackage->GetFName(), ReferencerPackageNames,
		UE::AssetRegistry::EDependencyCategory::Package);

	const FTopLevelAssetPath WidgetBlueprintClassPath = UWidgetBlueprint::StaticClass()->GetClassPathName();

	for (FName ReferencerPackageName : ReferencerPackageNames)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(ReferencerPackageName, Assets);
		for (const FAssetData& Asset : Assets)
		{
			// Widget Blueprints only.
			if (Asset.AssetClassPath != WidgetBlueprintClassPath)
			{
				continue;
			}

			const UWidgetBlueprint* ReferencingWidgetBlueprint = Cast<UWidgetBlueprint>(Asset.GetAsset());
			if (!ReferencingWidgetBlueprint)
			{
				continue;
			}

			FMappedViewModelWidgetConnections& WidgetConnections = Result.AddDefaulted_GetRef();
			WidgetConnections.WidgetPath = Asset.GetSoftObjectPath().ToString();
			WidgetConnections.Connections = NarrowToViewModelClass(MapConnections(ReferencingWidgetBlueprint), ReferencingWidgetBlueprint, ViewModelClass);
		}
	}

	return Result;
}
