// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "UObject/Interface.h"
#include "UObject/NameTypes.h"
#include "UObject/ScriptInterface.h"

#include "ToolsetCore/ToolsetPermissionProvider.h"

#include "ToolsetProvider.generated.h"

UINTERFACE(MinimalAPI, BlueprintType)
class UToolsetProvider : public UInterface
{
	GENERATED_BODY()
};

// Contract for a class that exposes AI-callable tools.
//
// Every method is called on the class default object, including when tools dispatch on a live
// instance. A Blueprint can declare the interface without a C++ implementer, so
// Cast<IToolsetProvider> can still be null on a class that reports ImplementsInterface.
class IToolsetProvider
{
	GENERATED_BODY()

public:

	// UE 5.8: GetToolsetVersion() is left to 5.8's UToolsetDefinition, which every ported toolset still
	// derives from; declaring it here too would make unqualified calls ambiguous.

	// Bare tool names (UFUNCTION names) to hide from the agent right now, called once per
	// registered permission provider.
	UFUNCTION(BlueprintNativeEvent, Category = "Toolset", meta = (AIIgnore))
	TOOLSETCORE_API TArray<FName> GetToolsToHide(
		const TScriptInterface<IToolsetPermissionProvider>& Provider) const;
	virtual TArray<FName> GetToolsToHide_Implementation(
		const TScriptInterface<IToolsetPermissionProvider>& Provider) const
	{
		return {};
	}

	// Whether to withhold the whole toolset from the agent for this permission provider.
	UFUNCTION(BlueprintNativeEvent, Category = "Toolset", meta = (AIIgnore))
	TOOLSETCORE_API bool IsToolsetHidden(
		const TScriptInterface<IToolsetPermissionProvider>& Provider) const;
	virtual bool IsToolsetHidden_Implementation(
		const TScriptInterface<IToolsetPermissionProvider>& Provider) const
	{
		return false;
	}

	// Whether AICallable UFunctions are dispatched on the object ResolveToolDispatchTarget
	// returns rather than on the class default object. Tools must be static unless this or
	// UsesInstanceArgument is true.
	//
	// Answered on the CDO during cook, so it must be a constant: never consult live state.
	virtual bool UsesInstanceDispatch() const
	{
		return false;
	}

	// Whether each call to a non-static AICallable UFunction names the object it runs on, through a
	// required "instance" argument. Unlike UsesInstanceDispatch, where the provider resolves one
	// target, the caller picks among any number of live instances. A class cannot use both.
	//
	// Answered on the CDO during cook, so it must be a constant: never consult live state.
	virtual bool UsesInstanceArgument() const
	{
		return false;
	}

	// The object AICallable UFunctions are dispatched on, consulted once per tool call when
	// UsesInstanceDispatch returns true. Null is fine before the instance exists.
	//
	// Runs on the class default object, so `this` is the CDO and returning it silently sends
	// every tool call to the wrong object - look the instance up, or have it publish itself
	// where the CDO can read it. Registration calls this synchronously, so return an existing
	// instance or null and never construct one here.
	virtual UObject* ResolveToolDispatchTarget() const
	{
		return nullptr;
	}
};
