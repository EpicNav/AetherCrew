// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Delegates/Delegate.h"
#include "UObject/Interface.h"

#include "ToolsetPermissionProvider.generated.h"

UINTERFACE(MinimalAPI, BlueprintType, meta = (CannotImplementInterfaceInBlueprint))
class UToolsetPermissionProvider : public UInterface
{
	GENERATED_BODY()
};

// Implemented by a host that wants to restrict which tools are exposed. Nothing here
// interprets what a role means; the registry relays whoever registered.
class IToolsetPermissionProvider
{
	GENERATED_BODY()

public:

	// Permission roles granted to the current session, from the host's own entitlement system.
	UFUNCTION(BlueprintCallable, Category = "Toolset")
	virtual TArray<FString> GetGrantedRoles() const = 0;

	// Whether the host is currently restricting which tools are exposed.
	UFUNCTION(BlueprintCallable, Category = "Toolset")
	virtual bool IsRestricted() const = 0;

	// Must be fired by the toolset permission provider owner when either method above changes.
	FSimpleMulticastDelegate OnPermissionsChanged;
};
