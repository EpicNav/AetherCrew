// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Delegates/IDelegateInstance.h"
#include "UObject/NameTypes.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/WeakObjectPtrTemplates.h"

class IModularFeature;
class UClass;

namespace UE::Toolset
{
	// Keeps a provider class registered with the toolset registrar for as long as this object
	// lives. Hold one as a member; it registers as soon as a registrar exists, whether that is
	// before or after construction, and unregisters when destroyed.
	//
	// Game thread only, and must not outlive engine shutdown. One registration per class, so an
	// owner with several live instances must arbitrate which one holds it. Registration calls
	// IToolsetProvider::ResolveToolDispatchTarget synchronously, so that override must return an
	// existing instance or null rather than construct one.
	class FToolsetRegistration
	{
	public:
		TOOLSETCORE_API explicit FToolsetRegistration(UClass* InProviderClass);
		TOOLSETCORE_API ~FToolsetRegistration();

		// Delegates bind this address, so the owner must also keep the object where it is.
		FToolsetRegistration(const FToolsetRegistration&) = delete;
		FToolsetRegistration& operator=(const FToolsetRegistration&) = delete;

		// False while no registrar exists, and when one existed and declined the class.
		bool IsRegistered() const
		{
			return bRegistered;
		}

	private:
		void Register();
		void Unregister();
		UClass* ResolveProviderClass();
		void OnFeatureRegistered(const FName& Type, IModularFeature* Feature);
		void OnFeatureUnregistered(const FName& Type, IModularFeature* Feature);

		TWeakObjectPtr<UClass> ProviderClass;

		// Lets the class be found again after live coding replaces it and the weak pointer
		// goes stale.
		FSoftClassPath ProviderClassPath;
		bool bRegistered = false;
		FDelegateHandle RegisteredHandle;
		FDelegateHandle UnregisteredHandle;
	};
}
