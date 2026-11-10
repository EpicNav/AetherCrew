// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetMVVMBindingUtils.h"
#include "MVVMBlueprintView.h"
#include "MVVMPropertyPath.h"
#include "MVVMEditorSubsystem.h"
#include "WidgetBlueprint.h"
#include "Editor.h"

UMVVMBlueprintView* FWidgetMVVMBindingUtils::FindView(const UWidgetBlueprint* WidgetBlueprint)
{
	const UMVVMEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
	return Subsystem ? Subsystem->GetView(WidgetBlueprint) : nullptr;
}

FResolvedMVVMDestination FWidgetMVVMBindingUtils::ResolveDestination(const FMVVMBlueprintPropertyPath& DestinationPath, const UWidgetBlueprint* WidgetBlueprint)
{
	FResolvedMVVMDestination Resolved;

	const TArrayView<const FMVVMBlueprintFieldPath> DestinationFieldPaths = DestinationPath.GetFieldPaths();
	const bool bDestinationIsWidget = DestinationFieldPaths.Num() > 0
		&& DestinationPath.GetSource(WidgetBlueprint) == EMVVMBlueprintFieldPathSource::Widget;

	// WidgetName is the path's root context, regardless of leaf kind.
	if (bDestinationIsWidget)
	{
		Resolved.bIsWidget = true;
		Resolved.WidgetName = DestinationPath.GetWidgetName();
		const FName LeafFieldName = DestinationFieldPaths.Last().GetRawFieldName();
		Resolved.FieldName = LeafFieldName;
		Resolved.bIsProperty = DestinationFieldPaths.Last().GetBindingKind() == EBindingKind::Property;
	}
	else
	{
		Resolved.bIsWidget = false;
		Resolved.HandlerName = DestinationFieldPaths.Num() > 0 ? DestinationFieldPaths.Last().GetRawFieldName() : NAME_None;
	}

	return Resolved;
}

TMap<FGuid, FViewModelIdentity> FWidgetMVVMBindingUtils::BuildViewModelIdentitiesById(const UMVVMBlueprintView* View)
{
	TMap<FGuid, FViewModelIdentity> IdentitiesById;
	if (!View)
	{
		return IdentitiesById;
	}

	for (const FMVVMBlueprintViewModelContext& ViewModelContext : View->GetViewModels())
	{
		FViewModelIdentity& Identity = IdentitiesById.Add(ViewModelContext.GetViewModelId());
		Identity.Name = ViewModelContext.GetViewModelName().ToString();
		Identity.Class = ViewModelContext.GetViewModelClass();
	}

	return IdentitiesById;
}
