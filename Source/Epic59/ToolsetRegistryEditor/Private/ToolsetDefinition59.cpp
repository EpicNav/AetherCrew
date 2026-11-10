// Copyright Epic Games, Inc. All Rights Reserved.

#include "ToolsetRegistry/ToolsetDefinition59.h"

#include "ToolsetRegistryEditor/Module.h"

namespace UE::ToolsetRegistry
{

bool TakesInstanceArgument(const UFunction* Function)
{
	if (!Function || Function->HasAllFunctionFlags(FUNC_Static))
	{
		return false;
	}
	const UClass* OwnerClass = Function->GetOwnerClass();
	const IToolsetProvider* Definition =
		OwnerClass ? Cast<IToolsetProvider>(OwnerClass->GetDefaultObject()) : nullptr;
	return Definition && Definition->UsesInstanceArgument();
}

#if WITH_METADATA

bool IsFunctionMarkedAsTool(const TObjectPtr<const UFunction>& Function)
{
	check(Function);
	static const FName AICallableName(TEXT("AICallable"));
	return Function->HasMetaData(AICallableName) || IsFunctionAIRuntimeCallable(Function);
}

TValueOrError<bool, FString> IsFunctionAICallable(const TObjectPtr<const UFunction>& Function)
{
	return UToolsetDefinition::IsFunctionAICallable(Function);
}

bool IsFunctionAIRuntimeCallable(const TObjectPtr<const UFunction>& Function)
{
	check(Function);
	static const FName AIRuntimeCallableName(TEXT("AIRuntimeCallable"));
	return Function->HasMetaData(AIRuntimeCallableName);
}

EToolAccessMode GetFunctionAccessMode(const TObjectPtr<const UFunction>& Function)
{
	check(Function);
	static const FName AIAccessModeName(TEXT("AIAccessMode"));
	if (!Function->HasMetaData(AIAccessModeName))
	{
		return EToolAccessMode::Execute;
	}

	const FString Value = Function->GetMetaData(AIAccessModeName);
	const TOptional<EToolAccessMode> Mode = ParseAccessMode(Value);
	if (!Mode.IsSet())
	{
		UE_LOGF(LogToolsetRegistryEditor, Warning,
			"UFunction '%ls' has meta=(AIAccessMode=\"%ls\") which is not one of "
			"Read/Write/Execute; defaulting to Execute.",
			*Function->GetPathName(), *Value);
		return EToolAccessMode::Execute;
	}
	return Mode.GetValue();
}

#endif

}
