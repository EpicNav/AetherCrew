// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Features/IModularFeature.h"
#include "UObject/NameTypes.h"

class UClass;

namespace UE::Toolset
{
	// The live toolset registry. At most one is registered at a time, and none is registered
	// when no registry is loaded.
	class IToolsetRegistrar : public IModularFeature
	{
	public:
		TOOLSETCORE_API static const FName ModularFeatureName;

		virtual ~IToolsetRegistrar() = default;

		// ProviderClass must implement IToolsetProvider. Registering the same class twice is a
		// no-op rather than an error.
		//
		// Returns whether the class is registered once the call returns; an implementation may
		// decline, so a caller tracking registration state must not assume success.
		virtual bool RegisterToolsetClass(UClass* ProviderClass) = 0;

		virtual void UnregisterToolsetClass(UClass* ProviderClass) = 0;
	};
}
