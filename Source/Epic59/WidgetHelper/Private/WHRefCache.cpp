// Copyright Epic Games, Inc. All Rights Reserved.

#include "WHRefCache.h"
#include "Containers/Array.h"
#include "CoreGlobals.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AssertionMacros.h"
#include "Templates/Tuple.h"
#include "Widgets/SWidget.h"

namespace UE::WidgetHelper::Private
{
	constexpr int32 MinPurgeSize = 64;
}

namespace UE::WidgetHelper
{
	FWidgetRefCache& FWidgetRefCache::Get()
	{
		static FWidgetRefCache Instance;
		return Instance;
	}

	FName FWidgetRefCache::GetOrAssignRef(const TSharedRef<SWidget>& Widget, FName Prefix)
	{
		using namespace UE::WidgetHelper::Private;

		check(IsInGameThread());

		const TWeakPtr<SWidget> WeakWidget = Widget;
		if (const FName* ExistingRef = WidgetToRef.Find(WeakWidget))
		{
			return *ExistingRef;
		}

		if (WidgetToRef.Num() >= NextPurgeSize)
		{
			PurgeExpired();
			NextPurgeSize = FMath::Max(MinPurgeSize, WidgetToRef.Num() * 2);
		}

		int32& Counter = PrefixCounters.FindOrAdd(Prefix, 0);
		const FName Ref(Prefix, NAME_EXTERNAL_TO_INTERNAL(++Counter));

		WidgetToRef.Add(WeakWidget, Ref);
		RefToWidget.Add(Ref, WeakWidget);
		return Ref;
	}

	TSharedPtr<SWidget> FWidgetRefCache::ResolveRef(FName Ref) const
	{
		check(IsInGameThread());

		if (const TWeakPtr<SWidget>* WeakWidget = RefToWidget.Find(Ref))
		{
			return WeakWidget->Pin();
		}
		return nullptr;
	}

	TSharedPtr<SWidget> FWidgetRefCache::ResolveRef(FStringView Ref) const
	{
		check(IsInGameThread());

		const FName Name(Ref, FNAME_Find);
		return Name.IsNone() ? nullptr : ResolveRef(Name);
	}

	FName FWidgetRefCache::FindRef(const TSharedRef<SWidget>& Widget) const
	{
		check(IsInGameThread());

		const FName* Ref = WidgetToRef.Find(TWeakPtr<SWidget>(Widget));
		return Ref ? *Ref : NAME_None;
	}

	void FWidgetRefCache::PurgeExpired()
	{
		check(IsInGameThread());

		// Keep each ref with its key: expired weak keys all compare equal, so a lookup by key could hit another entry.
		TArray<TPair<TWeakPtr<SWidget>, FName>> Expired;
		for (const TPair<TWeakPtr<SWidget>, FName>& Pair : WidgetToRef)
		{
			if (!Pair.Key.IsValid())
			{
				Expired.Emplace(Pair.Key, Pair.Value);
			}
		}

		for (const TPair<TWeakPtr<SWidget>, FName>& Entry : Expired)
		{
			RefToWidget.Remove(Entry.Value);
			WidgetToRef.Remove(Entry.Key);
		}
	}
}
