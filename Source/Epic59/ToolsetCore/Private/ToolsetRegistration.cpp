// Copyright Epic Games, Inc. All Rights Reserved.

#include "ToolsetCore/ToolsetRegistration.h"

#include "Features/IModularFeatures.h"
#include "UObject/Class.h"

#include "ToolsetCore/ToolsetCoreLog.h"
#include "ToolsetCore/ToolsetProvider.h"
#include "ToolsetCore/ToolsetRegistrar.h"

namespace UE::Toolset
{
	FToolsetRegistration::FToolsetRegistration(UClass* InProviderClass)
	{
		if (!InProviderClass)
		{
			UE_LOGF(LogToolsetCore, Error, "Cannot register a null class as a toolset.");
			return;
		}

		if (!InProviderClass->ImplementsInterface(UToolsetProvider::StaticClass()))
		{
			UE_LOGF(LogToolsetCore, Error,
				"Cannot register '%ls' as a toolset: it does not implement IToolsetProvider. "
				"Add IToolsetProvider to the class's base list, or derive from UToolsetDefinition.",
				*InProviderClass->GetPathName());
			return;
		}

		ProviderClass = InProviderClass;
		ProviderClassPath = FSoftClassPath(InProviderClass);

		IModularFeatures& Features = IModularFeatures::Get();
		RegisteredHandle = Features.OnModularFeatureRegistered().AddRaw(
			this, &FToolsetRegistration::OnFeatureRegistered);
		UnregisteredHandle = Features.OnModularFeatureUnregistered().AddRaw(
			this, &FToolsetRegistration::OnFeatureUnregistered);

		Register();
		if (!bRegistered && !Features.IsModularFeatureAvailable(IToolsetRegistrar::ModularFeatureName))
		{
			UE_LOGF(LogToolsetCore, Verbose,
				"No toolset registrar yet; '%ls' will register if one appears.",
				*InProviderClass->GetPathName());
		}
	}

	FToolsetRegistration::~FToolsetRegistration()
	{
		IModularFeatures& Features = IModularFeatures::Get();
		Features.OnModularFeatureRegistered().Remove(RegisteredHandle);
		Features.OnModularFeatureUnregistered().Remove(UnregisteredHandle);

		Unregister();
	}

	UClass* FToolsetRegistration::ResolveProviderClass()
	{
		// Live coding keeps a replaced class alive until the next GC, renamed into the transient
		// package, so the weak pointer outlives the class's identity; the path already names the
		// replacement.
		UClass* Class = ProviderClass.Get();
		if (Class && !Class->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			return Class;
		}

		UClass* Reloaded = ProviderClassPath.ResolveClass();
		ProviderClass = Reloaded;
		return Reloaded;
	}

	void FToolsetRegistration::Register()
	{
		UClass* Class = ResolveProviderClass();
		if (bRegistered || !Class)
		{
			return;
		}

		IModularFeatures& Features = IModularFeatures::Get();
		if (!Features.IsModularFeatureAvailable(IToolsetRegistrar::ModularFeatureName))
		{
			return;
		}

		// Latching on a decline would make the early-return above permanent.
		bRegistered =
			Features.GetModularFeature<IToolsetRegistrar>(IToolsetRegistrar::ModularFeatureName)
				.RegisterToolsetClass(Class);
		UE_LOGF(LogToolsetCore, Verbose, "Toolset registrar %ls '%ls'.",
			bRegistered ? TEXT("accepted") : TEXT("declined"), *Class->GetPathName());
	}

	void FToolsetRegistration::Unregister()
	{
		UClass* Class = ResolveProviderClass();
		if (!bRegistered || !Class)
		{
			UE_LOGF(LogToolsetCore, Verbose, "Not unregistering '%ls': Registered=%d, ClassResolved=%d.", *ProviderClassPath.ToString(), bRegistered ? 1 : 0, Class ? 1 : 0);
			return;
		}

		IModularFeatures& Features = IModularFeatures::Get();
		const bool bRegistrarAvailable = Features.IsModularFeatureAvailable(IToolsetRegistrar::ModularFeatureName);
		if (bRegistrarAvailable)
		{
			Features.GetModularFeature<IToolsetRegistrar>(IToolsetRegistrar::ModularFeatureName)
				.UnregisterToolsetClass(Class);
		}
		bRegistered = false;
		UE_LOGF(LogToolsetCore, Verbose, "Unregistered '%ls' from the toolset registrar: RegistrarAvailable=%d.", *Class->GetPathName(), bRegistrarAvailable ? 1 : 0);
	}

	void FToolsetRegistration::OnFeatureRegistered(const FName& Type, IModularFeature* Feature)
	{
		if (Type == IToolsetRegistrar::ModularFeatureName)
		{
			Register();
		}
	}

	void FToolsetRegistration::OnFeatureUnregistered(const FName& Type, IModularFeature* Feature)
	{
		if (Type == IToolsetRegistrar::ModularFeatureName)
		{
			// The registrar is being torn down, so there is nothing to call Unregister on.
			// Drop the claim so a later registrar picks this provider up again.
			UE_LOGF(LogToolsetCore, Verbose, "Toolset registrar went away; dropping the claim on '%ls': WasRegistered=%d.", *ProviderClassPath.ToString(), bRegistered ? 1 : 0);
			bRegistered = false;
		}
	}
}
