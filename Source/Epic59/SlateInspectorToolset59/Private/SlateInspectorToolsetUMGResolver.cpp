// Copyright Epic Games, Inc. All Rights Reserved.

#include "SlateInspectorToolsetUMGResolver.h"

#include "Misc/Paths.h"
#include "Types/ReflectionMetadata.h"
#include "UObject/Class.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectGlobals.h"

namespace UE::SlateInspectorToolset::Private
{

/**
 * UUserWidget's UClass, looked up by path so this module need not depend on UMG.
 * Returns null when UMG is not loaded, which yields an empty outer chain rather
 * than an error -- the correct answer for UI built on another tech.
 */
UClass* FindUserWidgetClass()
{
	static UClass* UserWidgetClass = nullptr;
	if (UserWidgetClass == nullptr)
	{
		UserWidgetClass = FindObject<UClass>(FTopLevelAssetPath(TEXT("/Script/UMG"), TEXT("UserWidget")));
	}
	return UserWidgetClass;
}

}

TOptional<FSlateInspectorToolsetUMGInfo> FSlateInspectorToolsetUMGResolver::Resolve(TSharedRef<SWidget> Widget, bool bAllowParentFallback)
{
	// Prefer metadata attached directly to this widget; fall back to the nearest
	// ancestor carrying UMG metadata only when asked. GetWidgetOrParentMetaData
	// starts at the widget itself, but we already know it has none, so any hit
	// from the fallback originates on an ancestor.
	TSharedPtr<FReflectionMetaData> Meta = Widget->GetMetaData<FReflectionMetaData>();
	bool bResolvedOnParent = false;
	if (!Meta.IsValid() && bAllowParentFallback)
	{
		Meta = FReflectionMetaData::GetWidgetOrParentMetaData(&Widget.Get());
		bResolvedOnParent = Meta.IsValid();
	}

	if (!Meta.IsValid())
	{
		return TOptional<FSlateInspectorToolsetUMGInfo>();
	}

	FSlateInspectorToolsetUMGInfo Info;
	Info.bResolvedOnParent = bResolvedOnParent;
	Info.WidgetName = Meta->Name.ToString();

	if (const UClass* WidgetClass = Meta->Class.Get())
	{
		Info.WidgetClassName = WidgetClass->GetName();
	}

	if (const UObject* Asset = Meta->Asset.Get())
	{
		Info.AssetPath = Asset->GetPathName();
	}

	UObject* SourceObject = Meta->SourceObject.Get();
	Info.SourceObject = SourceObject;

	// Walk the outer chain, recording each enclosing UUserWidget innermost first.
	UClass* UserWidgetClass = UE::SlateInspectorToolset::Private::FindUserWidgetClass();
	for (UObject* OuterUserWidget = (SourceObject && UserWidgetClass) ? SourceObject->GetTypedOuter(UserWidgetClass) : nullptr;
		OuterUserWidget != nullptr;
		OuterUserWidget = OuterUserWidget->GetTypedOuter(UserWidgetClass))
	{
		Info.OuterUserWidgetChain.Emplace(OuterUserWidget->GetName(), OuterUserWidget->GetClass()->GetName());
	}

	return Info;
}

FString FSlateInspectorToolsetUMGResolver::MakeCompactAnnotation(const FSlateInspectorToolsetUMGInfo& Info)
{
	const FString AssetName = FPaths::GetBaseFilename(Info.AssetPath);

	FString Result;
	if (!AssetName.IsEmpty() && !Info.WidgetName.IsEmpty())
	{
		Result = FString::Printf(TEXT("%s.%s"), *AssetName, *Info.WidgetName);
	}
	else if (!Info.WidgetName.IsEmpty())
	{
		Result = Info.WidgetName;
	}
	else if (!AssetName.IsEmpty())
	{
		Result = AssetName;
	}

	if (!Info.WidgetClassName.IsEmpty())
	{
		Result += FString::Printf(TEXT("(%s)"), *Info.WidgetClassName);
	}

	return Result;
}
