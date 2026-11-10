// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldConditionTools59.h"

#include "StructUtils/InstancedStruct.h"
#include "WorldConditionBase.h"
#include "WorldConditionQuery.h"

FText UWorldConditionTools59::GetQueryDescription(const FWorldConditionQueryDefinition& QueryDefinition)
{
	return QueryDefinition.GetDescription();
}

FText UWorldConditionTools59::GetConditionDescription(const FInstancedStruct& Condition)
{
	if (const FWorldConditionBase* const WorldCondition = Condition.GetPtr<FWorldConditionBase>())
	{
		return WorldCondition->GetDescription();
	}
	return FText::GetEmpty();
}

bool UWorldConditionTools59::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
