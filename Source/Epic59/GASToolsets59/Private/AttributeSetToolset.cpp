// Copyright Epic Games, Inc. All Rights Reserved.

#include "AttributeSetToolset59.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AttributeSet.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AttributeSetToolset59)

namespace
{
	FAttributeSetClassInfo59 BuildClassInfo(UClass* Class)
	{
		FAttributeSetClassInfo59 Info;
		Info.ClassName = Class->GetName();

		// Populate AssetPath for Blueprint-generated classes.
		if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Class))
		{
			if (UBlueprint* BP = Cast<UBlueprint>(BPGC->ClassGeneratedBy))
			{
				Info.AssetPath = BP->GetPathName();
			}
		}

		TArray<FGameplayAttribute> Attributes;
		UAttributeSet::GetAttributesFromSetClass(Class, Attributes);

		for (const FGameplayAttribute& Attr : Attributes)
		{
			FGameplayAttributeInfo59 AttrInfo;
			AttrInfo.AttributeName = Attr.GetName();
			AttrInfo.SetClassName = Info.ClassName;
			AttrInfo.FullName = FString::Printf(TEXT("%s.%s"), *Info.ClassName, *AttrInfo.AttributeName);
			Info.Attributes.Add(AttrInfo);
		}

		return Info;
	}
}

TArray<FAttributeSetClassInfo59> UAttributeSetToolset59::FindAttributeSetClasses()
{
	TArray<FAttributeSetClassInfo59> Results;

	// Collect all loaded UClass objects that are strict subclasses of UAttributeSet.
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;
		if (!Class->IsChildOf(UAttributeSet::StaticClass()) ||
			Class == UAttributeSet::StaticClass() ||
			Class->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}

		Results.Add(BuildClassInfo(Class));
	}

	Results.Sort([](const FAttributeSetClassInfo59& A, const FAttributeSetClassInfo59& B)
	{
		return A.ClassName < B.ClassName;
	});

	return Results;
}

TArray<FGameplayAttributeInfo59> UAttributeSetToolset59::ListAttributes(const FString& ClassName)
{
	// Search loaded classes by name.
	UClass* FoundClass = nullptr;
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;
		if (Class->GetName() == ClassName && Class->IsChildOf(UAttributeSet::StaticClass()))
		{
			FoundClass = Class;
			break;
		}
	}

	if (!FoundClass)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("AttributeSet class '%s' not found. Use FindAttributeSetClasses to list available classes."),
			*ClassName));
		return {};
	}

	TArray<FGameplayAttribute> Attributes;
	UAttributeSet::GetAttributesFromSetClass(FoundClass, Attributes);

	TArray<FGameplayAttributeInfo59> Results;
	Results.Reserve(Attributes.Num());
	for (const FGameplayAttribute& Attr : Attributes)
	{
		FGameplayAttributeInfo59 Info;
		Info.AttributeName = Attr.GetName();
		Info.SetClassName = ClassName;
		Info.FullName = FString::Printf(TEXT("%s.%s"), *ClassName, *Info.AttributeName);
		Results.Add(Info);
	}

	return Results;
}

bool UAttributeSetToolset59::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
