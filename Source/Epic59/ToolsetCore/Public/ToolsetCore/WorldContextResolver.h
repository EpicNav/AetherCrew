// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Templates/Function.h"

class UObject;

namespace UE::Toolset
{
	// Installs the callback used to resolve the implicit "WorldContextObject" parameter of
	// dispatched UFunctions. Called by ToolsetRegistryEngine at startup with a GEngine-
	// backed lookup; consumers without a running UEngine (headless tools, commandlets
	// that don't need a game world) can skip the ToolsetRegistryEngine module and the
	// resolver stays empty so WorldContextObject params are left unset at dispatch.
	//
	// Lives here rather than in ToolsetRegistry so a world-scoped subsystem in an engine
	// module can be a toolset without depending on the plugin.
	//
	// Pass an empty TFunction to clear a previously installed resolver.
	TOOLSETCORE_API void SetWorldContextResolver(TFunction<UObject*()> Resolver);

	// Invokes the installed resolver and returns its result. Returns nullptr when no
	// resolver has been installed.
	TOOLSETCORE_API UObject* ResolveWorldContext();
}
