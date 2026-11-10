// Copyright Epic Games, Inc. All Rights Reserved.

#include "ToolsetCore/WorldContextResolver.h"

namespace UE::Toolset
{
	namespace
	{
		// File-static holder for the installed resolver. Returned by reference so
		// SetWorldContextResolver can assign into it.
		TFunction<UObject*()>& GetWorldContextResolverSlot()
		{
			static TFunction<UObject*()> Resolver;
			return Resolver;
		}
	}

	void SetWorldContextResolver(TFunction<UObject*()> Resolver)
	{
		GetWorldContextResolverSlot() = MoveTemp(Resolver);
	}

	UObject* ResolveWorldContext()
	{
		const TFunction<UObject*()>& Resolver = GetWorldContextResolverSlot();
		return Resolver ? Resolver() : nullptr;
	}
}
